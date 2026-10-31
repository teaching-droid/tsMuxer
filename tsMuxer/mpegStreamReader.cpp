
#include "mpegStreamReader.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

#include "nalUnits.h"
#include "vodCoreException.h"
#include "vod_common.h"

static constexpr double EPSILON = 5e-5;
static constexpr int64_t MAX_PULLDOWN_ASYNC = 100000000ll;
static constexpr int UNIT_SKIPPED = 5;

using namespace std;

// The 01 byte of the start code a findNALWithStartCode result points at.
static uint8_t* markerFromResult(uint8_t* r) { return r[2] == 1 ? r + 2 : r + 3; }

void MPEGStreamReader::setBuffer(uint8_t* data, const uint32_t dataLen, const bool lastBlock)
{
    if (lastBlock)
        m_eof = true;
    const int64_t keptLen = m_tmpBufferLen;

    if (m_tmpBufferLen + dataLen > m_tmpBufferSize)
    {
        // What is kept and the new block do not fit: a bigger buffer, with room to spare for the next block and for
        // the few bytes the readers insert in place (a rewritten SPS or VPS, a repeated SEI)
        const int64_t need = m_tmpBufferLen + dataLen;
        if (need + 1024 * 1024 > MAX_TMP_BUFFER_SIZE)
            THROW(ERR_COMMON_SMALL_BUFFER,
                  "A picture of the video track, with the data that follows it, is larger than "
                      << MAX_TMP_BUFFER_SIZE / (1024 * 1024) << " MiB (picture " << m_totalFrameNum
                      << "). tsMuxeR cannot read pictures that large.")
        const int64_t newSize = (std::min)(MAX_TMP_BUFFER_SIZE, (std::max)(m_tmpBufferSize * 2, need + 1024 * 1024));
        auto* grown = new uint8_t[static_cast<size_t>(newSize)];
        if (m_tmpBufferLen > 0)
            memcpy(grown, m_tmpBuffer, static_cast<size_t>(m_tmpBufferLen));
        const std::ptrdiff_t delta = grown - m_tmpBuffer;
        onMoveBuffer(delta);
        if (m_lastDecodedPos)
            m_lastDecodedPos += delta;
        if (m_cachedMarker)
            m_cachedMarker += delta;
        if (m_scanHighWater)
            m_scanHighWater += delta;
        if (m_joinPos)
            m_joinPos += delta;
        delete[] m_retiredBuffer;
        m_retiredBuffer = m_tmpBuffer;
        m_tmpBuffer = grown;
        m_tmpBufferSize = newSize;
    }
    memcpy(m_tmpBuffer + m_tmpBufferLen, data + MAX_AV_PACKET_SIZE, dataLen);
    m_tmpBufferLen += dataLen;

    m_curPos = m_buffer = m_tmpBuffer;
    m_bufEnd = m_buffer + m_tmpBufferLen;
    if (m_joinPending)
    {
        m_joinPending = false;
        m_joinPos = m_tmpBuffer + keptLen;
    }
    m_tmpBufferLen = 0;
}

int MPEGStreamReader::flushPacket(AVPacket& avPacket)
{
    m_eof = true;
    m_cachedMarker = nullptr;  // flush repoints m_curPos/m_bufEnd directly
    m_scanHighWater = nullptr;
    avPacket.codec = this;
    avPacket.duration = 0;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.flags = m_flags;
    avPacket.stream_index = m_streamIndex;
    avPacket.codecID = getCodecInfo().codecID;

    if (m_tmpBufferLen > 0)
    {
        m_curPos = m_tmpBuffer;
        m_bufEnd = m_tmpBuffer + m_tmpBufferLen;
        countUnits(m_curPos, m_bufEnd);
        const int isNal = bufFromNAL();
        int decodeRez = 0;
        if (isNal)
        {
            m_shortStartCodes = isNal < 4;
            // decodeNal already refuses to decode anything at or before m_lastDecodedPos. The guard
            // that used to stand here duplicated that test with m_curPos as it was BEFORE
            // storeBufferRest moved the buffer tail to the front of m_tmpBuffer, compared against an
            // m_lastDecodedPos that HAD been rebased by the same move. Comparing the two therefore
            // always passed, and passing it cleared m_lastDecodedPos, which defeated the real guard
            // below as well. The result was that the last access unit of a stream was decoded a
            // second time: counted twice, and its timings advanced once too often, which split the
            // final access unit across two PES and left the track one frame too long.
            decodeRez = decodeNal(m_curPos + isNal);
        }
        if (decodeRez == 0)
        {
            avPacket.data = m_tmpBuffer;
            avPacket.size = static_cast<int>(m_tmpBufferLen);
        }
    }

    avPacket.pts = m_curPts + m_timeOffset;
    avPacket.dts = pictureDts();  // shift dts back

    string message = "Processed ";
    message += int32ToStr(m_totalFrameNum - m_framesLeftOut);
    message += " video frames";
    LTRACE(LT_DEBUG, 0, message);
    LTRACE(LT_INFO, 2, message);
    m_processedBytes += avPacket.size;
    return static_cast<int>(m_tmpBufferLen);
}

int64_t MPEGStreamReader::pictureDts()
{
    const int64_t dts = m_curDts + m_timeOffset - m_pcrIncPerFrame * getFrameDepth();
    if (m_catchUpStep == 0)
        return dts;
    if (m_curDts == m_catchUpCurDts)
        return m_catchUpDts;  // the same picture again: its packets share one time
    const int64_t next = m_catchUpDts + m_catchUpStep;
    if (dts >= next)
    {
        m_catchUpStep = 0;  // caught up
        return dts;
    }
    // Never after the time the picture is shown
    m_catchUpDts = (std::min)(next, m_curPts + m_timeOffset);
    m_catchUpCurDts = m_curDts;
    return m_catchUpDts;
}

// A deeper pyramid moves every decoding time from here on further back, behind the times the pictures before were
// already given: taken at once, the decoding time went back (by a frame for each level more), which a decoder cannot
// follow. The new period's pictures are shown no earlier than its first, and that is a frame or more after the last
// decoding time given, so the depth's growth of k frames fits there: the next k pictures take steps of 1/(k+1)
// frame, after which the times are those of the new depth.
void MPEGStreamReader::deepenAfterOutput(const int depthBefore)
{
    const int grown = getFrameDepth() - depthBefore;
    if (grown <= 0 || m_totalFrameNum == 0)
        return;  // nothing was given a time yet: the new depth holds from the first picture
    if (m_catchUpStep == 0)
    {
        m_catchUpDts = m_curDts + m_timeOffset - m_pcrIncPerFrame * depthBefore;
        m_catchUpCurDts = m_curDts;
    }
    m_catchUpStep = (std::max)(int64_t{1}, m_pcrIncPerFrame / (grown + 1));
}

void MPEGStreamReader::onShiftBuffer(int offset) {}

void MPEGStreamReader::storeBufferRest()
{
    onShiftBuffer(static_cast<int>(m_curPos - m_tmpBuffer));
    memmove(m_tmpBuffer, m_curPos, m_bufEnd - m_curPos);
    m_tmpBufferLen = static_cast<int>(m_bufEnd - m_curPos);
    if (m_lastDecodedPos > m_curPos)
        m_lastDecodedPos = m_tmpBuffer + (m_lastDecodedPos - m_curPos);
    else
        m_lastDecodedPos = nullptr;
    if (m_cachedMarker > m_curPos)
        m_cachedMarker = m_tmpBuffer + (m_cachedMarker - m_curPos);
    else
        m_cachedMarker = nullptr;
    if (m_scanHighWater > m_curPos)
        m_scanHighWater = m_tmpBuffer + (m_scanHighWater - m_curPos);
    else
        m_scanHighWater = nullptr;
    if (m_joinPos >= m_curPos)
        m_joinPos = m_tmpBuffer + (m_joinPos - m_curPos);
    else
        m_joinPos = nullptr;
    m_curPos = m_bufEnd;
}

int MPEGStreamReader::readPacket(AVPacket& avPacket)
{
    // LTRACE(LT_DEBUG, 0, "MPEGStreamReader::readPacket");
    avPacket.codec = this;
    avPacket.flags = m_flags;
    avPacket.stream_index = m_streamIndex;
    avPacket.codecID = getCodecInfo().codecID;

    avPacket.duration = 0;
    avPacket.data = nullptr;
    avPacket.size = 0;
    avPacket.pts = m_curPts + m_timeOffset;
    avPacket.dts = pictureDts();  // shift dts back
    const uint8_t* prevPos = m_curPos;
    if (!m_syncToStream)
    {
        uint8_t* nal = NALUnit::findNALWithStartCode(m_curPos, m_bufEnd, m_longCodesAllowed);
        if (nal != m_bufEnd)
        {
            m_syncToStream = true;
            m_curPos = nal;
        }
        else
            m_curPos = m_bufEnd;
        const int bytesProcessed = static_cast<int>(m_curPos - prevPos);
        m_processedBytes += bytesProcessed;
        prevPos = m_curPos;
        if (!m_syncToStream)
            return NEED_MORE_DATA;
    }

    // Existence check: is a complete unit buffered? The result position is not needed,
    // only whether one exists. Cached across calls so the chunked emission of a NAL larger
    // than MAX_AV_PACKET_SIZE does not re-walk its whole tail on every call (quadratic in
    // the NAL size), and a refill only rescans the new bytes.
    uint8_t* const scanStartA = (std::min)(m_curPos + 3, m_bufEnd);
    if (!(m_cachedMarker && m_cachedMarker >= scanStartA + 2 && m_cachedMarker < m_bufEnd))
    {
        uint8_t* from = scanStartA;
        if (!m_cachedMarker && m_scanHighWater && m_scanHighWater <= m_bufEnd && m_scanHighWater - 3 > from)
            from = m_scanHighWater - 3;  // nothing before the old buffer end: rescan only the tail
        uint8_t* nextNal = NALUnit::findNALWithStartCode(from, m_bufEnd, m_longCodesAllowed);
        if (nextNal == m_bufEnd)
        {
            m_cachedMarker = nullptr;
            m_scanHighWater = m_bufEnd;
            storeBufferRest();
            return NEED_MORE_DATA;
        }
        m_cachedMarker = markerFromResult(nextNal);
        m_scanHighWater = nullptr;
    }

    int isNal = bufFromNAL();
    if (isNal)
    {
        // decodeNal can rewrite the SPS/VPS in place and SHIFT the buffer tail
        // (updateStreamFps), which would leave the cached positions stale. Drop them;
        // the cache only needs to survive the mid-NAL chunk calls, which skip this block.
        m_cachedMarker = nullptr;
        m_scanHighWater = nullptr;

        const int64_t prevDts = m_curDts;
        m_shortStartCodes = isNal < 4;
        int rez;
        while (true)
        {
            rez = decodeNal(m_curPos + isNal);
            if (rez != UNIT_SKIPPED)
                break;
            uint8_t* nal = NALUnit::findNALWithStartCode(m_curPos + isNal, m_bufEnd, m_longCodesAllowed);
            // assert(nal < findEnd); // if unit is skipped, next unit MUST be in buffer

            countUnitBytes(m_curPos, nal - m_curPos, isNal);
            m_processedBytes += nal - m_curPos;
            prevPos = m_curPos = nal;
            if (nal == m_bufEnd)
            {
                rez = NOT_ENOUGH_BUFFER;
                m_syncToStream = false;
                break;
            }
            isNal = bufFromNAL();
        }

        if (rez == NOT_ENOUGH_BUFFER)
        {
            storeBufferRest();
            m_lastDecodedPos = nullptr;
            return NEED_MORE_DATA;
        }
        if (prevDts != m_curDts)
        {
            // stream interleaving is allowed during one stream packet

            avPacket.pts = m_curPts + m_timeOffset;
            avPacket.dts = pictureDts();  // shift dts back to 1 frame;
            if (isIFrame())
                avPacket.flags |= AVPacket::IS_IFRAME;
            if (isPriorityData(&avPacket))
                avPacket.flags |= AVPacket::PRIORITY_DATA;
            if (m_spsPpsFound)
                avPacket.flags |= AVPacket::IS_SPS_PPS_IN_GOP;

            return 0;  // return zero AV packet for new frame
        }
    }
    uint8_t* findEnd = (std::min)(m_bufEnd, m_curPos + MAX_AV_PACKET_SIZE);
    uint8_t* nal = NALUnit::findNALWithStartCode(m_curPos + isNal, findEnd, m_longCodesAllowed);

    if (nal == findEnd)
    {
        if (nal[0] == 1 && nal[-1] == 0 && nal[-2] == 0)
        {
            nal -= 2;
            if (nal[-1] == 0)
                nal--;
        }
        else if (nal[0] == 0 && nal[-1] == 0)
        {
            nal--;
            if (nal[-1] == 0)
                nal--;
        }
    }

    const int bytesProcessed = static_cast<int>(nal - prevPos);
    countUnitBytes(m_curPos, nal - m_curPos, isNal);
    avPacket.data = m_curPos;
    avPacket.size = bytesProcessed;
    avPacket.pts = m_curPts + m_timeOffset;
    avPacket.dts = pictureDts();  // shift dts back
    if (isIFrame())
        avPacket.flags |= AVPacket::IS_IFRAME;
    if (isPriorityData(&avPacket))
        avPacket.flags |= AVPacket::PRIORITY_DATA;
    if (m_spsPpsFound)
        avPacket.flags |= AVPacket::IS_SPS_PPS_IN_GOP;

    m_curPos = nal;

    m_tmpBufferLen = 0;
    m_processedBytes += avPacket.size;

    return 0;
}

int MPEGStreamReader::bufFromNAL() const
{
    if (m_bufEnd - m_curPos < (3 + (m_longCodesAllowed ? 1 : 0)))
        return 0;
    if (m_curPos[0] == 0 && m_curPos[1] == 0)
    {
        if (m_longCodesAllowed && m_curPos[2] == 0 && m_curPos[3] == 1)
            return 4;
        if (m_curPos[2] == 1)
            return 3;
    }
    return 0;
}

/*
int MPEGStreamReader::bufFromNAL(const uint8_t* buff, const uint8_t* bufEnd, bool longCodesAllowed) {
        if (bufEnd - buff < 4)
                return false;
        if (buff[0] == 0 && buff[1] == 0) {
                if (longCodesAllowed && buff[2] == 0 && buff[3] == 1 )
                        return 4;
                else if (buff[2] == 1)
                        return 3;
                else
                        return 0;
        }
        else
                return 0;
}
*/

int64_t MPEGStreamReader::getProcessedSize() { return m_processedBytes; }

// ** WHAT A READER CANNOT USE IS COUNTED WHERE IT GOES THROUGH THE UNITS, NOT WHERE THE DATA ARRIVES. ** A track named
// as another codec than the file holds came out as "video" when one parameter set of the right type turned up by
// chance: DTS read as HEVC gave 2,633 "pictures" and "Mux successful". Every unit passes here once, in the packets
// readPacket sends (or skips) and in the rest flushPacket sends, from whatever source: a raw file, a transport stream,
// MP4 or Matroska. The bytes before the first start code are not in a unit and are not counted at all.
void MPEGStreamReader::countUnitBytes(const uint8_t* pos, const int64_t len, const int startCodeLen)
{
    if (startCodeLen > 0)
    {
        m_unusableBytes += overLongUnitBytes();
        int maxLen = 0;
        m_unitUnusable = len > startCodeLen && unusableUnit(pos + startCodeLen, pos + len, maxLen);
        m_unitMaxLen = maxLen;
        m_unitLen = -startCodeLen;
        m_unitStartCodeLen = startCodeLen;
        m_unitOpen = true;
    }
    if (!m_unitOpen)
        return;
    m_unitBytes += len;
    m_unitLen += len;
    if (m_unitUnusable)
        m_unusableBytes += len;
}

void MPEGStreamReader::countUnits(const uint8_t* from, const uint8_t* end)
{
    for (const uint8_t* p = from; p < end;)
    {
        int startCodeLen = 0;
        if (end - p >= 3 && p[0] == 0 && p[1] == 0)
        {
            if (p[2] == 1)
                startCodeLen = 3;
            else if (m_longCodesAllowed && end - p >= 4 && p[2] == 0 && p[3] == 1)
                startCodeLen = 4;
        }
        const uint8_t* next = NALUnit::findNALWithStartCode(const_cast<uint8_t*>(p + (startCodeLen ? startCodeLen : 1)),
                                                            const_cast<uint8_t*>(end), m_longCodesAllowed);
        countUnitBytes(p, next - p, startCodeLen);
        p = next;
    }
}

int64_t MPEGStreamReader::overLongUnitBytes() const
{
    return m_unitOpen && !m_unitUnusable && m_unitMaxLen > 0 && m_unitLen > m_unitMaxLen
               ? m_unitLen + m_unitStartCodeLen
               : 0;
}

int64_t MPEGStreamReader::getUnusableBytes() const { return m_unusableBytes + overLongUnitBytes(); }

MPEGStreamReader::UnitHead::UnitHead(const uint8_t* unit, const uint8_t* end, const int headerBytes)
{
    const uint8_t* const last = (std::min)(end, unit + sizeof(m_data));
    const int len = last > unit ? NALUnit::decodeNAL(unit, last, m_data, sizeof(m_data)) : 0;
    m_bits = len > 0 ? len * 8 : 0;
    m_pos = headerBytes * 8;
}

bool MPEGStreamReader::UnitHead::readBit(int& value)
{
    if (m_pos >= m_bits)
        return false;
    value = (m_data[m_pos / 8] >> (7 - m_pos % 8)) & 1;
    m_pos++;
    return true;
}

bool MPEGStreamReader::UnitHead::skipBits(const int n)
{
    m_pos += n;
    return m_pos <= m_bits;
}

bool MPEGStreamReader::UnitHead::readUe(uint32_t& value)
{
    int zeros = 0;
    int bit = 0;
    for (;;)
    {
        if (!readBit(bit))
            return false;
        if (bit)
            break;
        if (++zeros > 31)
            return false;
    }
    uint64_t v = 0;
    for (int i = 0; i < zeros; i++)
    {
        if (!readBit(bit))
            return false;
        v = (v << 1) | static_cast<uint64_t>(bit);
    }
    v += (uint64_t{1} << zeros) - 1;
    if (v > 0xFFFFFFFFull)
        return false;
    value = static_cast<uint32_t>(v);
    return true;
}

int MPEGStreamReader::decodeNal(uint8_t* buff)
{
    int rez = 0;
    if (buff > m_lastDecodedPos)
    {
        rez = intDecodeNAL(buff);
        if (rez != 0)
        {
            // m_skippedNal.clear();
            return rez;
        }
        if (m_lastDecodedPos < buff)
            m_lastDecodedPos = buff;
    }

    // if (!m_skippedNal.empty() && buff == m_skippedNal[0])
    if (skipNal(buff))
    {
        // m_skippedNal.erase(m_skippedNal.begin());
        return UNIT_SKIPPED;
    }
    return rez;
}

#define abs_(a, b) ((a) >= (b) ? (a) - (b) : (b) - (a))
void MPEGStreamReader::updateFPS(void* curNALUnit, uint8_t* buff, uint8_t* nextNal, const int oldSPSLen)
{
    double spsFps = getStreamFPS(curNALUnit);
    spsFps = correctFps(spsFps);
    // A rate no video has, as a damaged header gives (one picture in 30 years): taken as it stood, every picture
    // lasted that long, and a transport stream was filled with clock references without end, gigabytes a minute. It
    // is set aside as if the stream gave none.
    static constexpr double MIN_STREAM_FPS = 0.1;
    static constexpr double MAX_STREAM_FPS = 1000.0;
    if (spsFps != 0.0 && !(spsFps >= MIN_STREAM_FPS && spsFps <= MAX_STREAM_FPS))
    {
        if (!m_badFpsSaid)
            LTRACE(LT_WARN, 2,
                   "Warning: the " << getCodecInfo().displayName << " stream gives a frame rate of " << spsFps
                                   << ", which no video has. It is left aside, as if the stream gave none.");
        m_badFpsSaid = true;
        spsFps = 0.0;
    }
    if (spsFps == 0.0 && m_fps == 0.0)
    {
        setFPS(fallbackFps(curNALUnit));
        if (m_packetFps > 0.0)
            LTRACE(LT_INFO, 2,
                   "This " << getCodecInfo().displayName
                           << " stream doesn't contain fps value. Muxing fps is absent too. Set muxing FPS to " << m_fps
                           << ", the rate the times of its pictures in the container give. fps= on the "
                           << "track's line sets another.");
        else
            LTRACE(LT_INFO, 2,
                   "This " << getCodecInfo().displayName
                           << " stream doesn't contain fps value. Muxing fps is absent too. Set muxing FPS to default "
                              "25.0 value.");
    }
    else if (m_fps == 0.0)
    {
        setFPS(spsFps);
        LTRACE(LT_INFO, 2,
               getCodecInfo().displayName << " muxing fps is not set. Get fps from stream. Value: " << spsFps);
    }
    else if (spsFps != 0.0 && m_fpsFromContainer && abs_(m_fps, spsFps) <= spsFps * 0.001)
    {
        // The container's rate is the stream's own as nearly as its timestamps can say it: an MP4 time scale often
        // cannot express 29.97 or 23.976 exactly. It used to be taken as if fps= had asked for it, so the stream's
        // timing was rewritten to it and every picture retimed. The stream's own rate is kept; a real difference,
        // a speed change a container states on purpose, is still followed below.
        setFPS(spsFps);
        m_fpsFromContainer = false;
    }
    else if (spsFps != 0.0 && m_fpsFromContainer && abs_(m_fps, 2 * spsFps) <= 2 * spsFps * 0.001 &&
             interlacedForRate(curNALUnit))
    {
        // ** A RATE OF FIELDS FROM THE CONTAINER. ** A Matroska writer that takes the fields of an interlaced stream
        // for its frames gives the duration of a field as DefaultDuration: twice the stream's rate, 59.94 for 29.97.
        // As a speed change stated on purpose it was followed, and the video played at twice its speed. Of an
        // interlaced stream the stream's own rate is kept (MatroskaDemuxer::getTrackFps also reads the block times).
        LTRACE(LT_INFO, 2,
               "The container gives " << m_fps << " fps for this interlaced " << getCodecInfo().displayName
                                      << " stream, the rate of its fields; its own rate, " << spsFps
                                      << " fps, is kept. fps= on the track's line sets another.");
        setFPS(spsFps);
        m_fpsFromContainer = false;
    }
    else if (spsFps != 0.0 && abs_(m_fps, spsFps) > EPSILON)
    {
        if (m_isFirstFpsWarn)
        {
            LTRACE(LT_INFO, 2,
                   getCodecInfo().displayName << " manual defined fps doesn't equal to stream fps. Change "
                                              << getCodecInfo().displayName << " fps from " << spsFps << " to "
                                              << m_fps);
            m_isFirstFpsWarn = false;
        }
        updateStreamFps(curNALUnit, buff, nextNal, oldSPSLen);
    }
    else if (spsFps == 0.0)
    {
        if (m_isFirstFpsWarn)
        {
            LTRACE(LT_INFO, 2, getCodecInfo().displayName << " stream doesn't contain fps field. Muxing fps=" << m_fps);
            m_isFirstFpsWarn = false;
        }
    }
    updateStreamAR(curNALUnit, buff, nextNal, oldSPSLen);
}

double MPEGStreamReader::fallbackFps(void* curNalUnit)
{
    if (m_packetFps < 0.0)
    {
        m_packetFps = 0.0;
        // A field coded stream may carry a PES for each field, whose times give the field rate: it keeps the default,
        // unless the demuxer can tell the fields from the frames, as it can for H.264 (a field pair is one picture
        // there, see TSDemuxer::getTrackPacketFps). An interlaced stream coded as frames (MBAFF) has one time for each
        // frame, as a progressive one: it was given 25 all the same, and an interlaced 29.97 stream played 17 % slow.
        if (m_packetFpsSource)
            m_packetFps = correctFps(m_packetFpsSource(interlacedBy(curNalUnit) && !codesFramesOnly(curNalUnit)));
    }
    return m_packetFps > 0.0 ? m_packetFps : 25.0;
}

double MPEGStreamReader::headerFps(const double streamFps)
{
    if (m_fps != 0.0)
        return m_fps;
    return streamFps > 0.0 ? streamFps : fallbackFps();
}

void MPEGStreamReader::checkPulldownSync()
{
    // Film reaches 29.97 with fields repeated, five fields for four pictures (1.25), and 59.94 with frames repeated,
    // five frames for two pictures (2.5). Only the first was checked, so film at 59.94 gave a warning every few
    // seconds. The cadence is the one the two times are nearer to: above 1.875 it is the second.
    // Both times count from the first picture: the decoding time does not start at 0, and its start counted as a
    // mistiming of its own. They count again when the frame step changes, as it does when the film rate is found:
    // the times before were in steps of the other rate.
    if (m_pcrIncPerFrame != m_pulldownStep)
    {
        m_pulldownStep = m_pcrIncPerFrame;
        m_testPulldownDts = 0;
    }
    if (m_testPulldownDts == 0)
        m_pulldownDtsStart = m_curDts;
    const int64_t curDts = m_curDts - m_pulldownDtsStart;
    if (curDts < 2 * INTERNAL_PTS_FREQ)
        return;  // the cadence of the first pictures is not yet the stream's
    const bool repeatedFrames = m_testPulldownDts * 8 > curDts * 15;
    int64_t asyncValue = repeatedFrames ? curDts * 5 - m_testPulldownDts * 2 : curDts * 5 - m_testPulldownDts * 4;
    if (asyncValue < 0)
        asyncValue = -asyncValue;
    // a picture shown two or three times is half a picture away from the average of the two, either way
    if (repeatedFrames)
        asyncValue = (std::max)(int64_t{0}, asyncValue - 5 * 2 * m_pcrIncPerFrame);

    // A stream without one pulldown mark has nothing irregular to warn of: its pictures are shown for one frame each,
    // the times never part, and delPulldown leaves it as it is. Its times were compared with fps/1.25 all the same, and
    // a stream of plain 23.976 or 29.97 got up to 5 warnings of irregular pulldown marks.
    if (m_pulldownMarkSeen && m_testPulldownDts != 0 && asyncValue > 5 * MAX_PULLDOWN_ASYNC * m_pulldownWarnCnt)
    {
        LTRACE(LT_ERROR, 2,
               "Warning! Source stream contain irregular pulldown marks. Mistiming between original fps and "
               "fps/"
                   << (repeatedFrames ? "2.5" : "1.25") << " (without pulldown) exceeds " << (asyncValue / 5000000ll)
                   << "ms.");
        m_pulldownWarnCnt *= 2;
    }
}

void MPEGStreamReader::noteSampleAspectRatio(const double sar)
{
    if (!m_sampleARKnown)
    {
        m_sampleARKnown = true;
        m_sampleAR = sar;
    }
    else if (m_laterSampleAR < 0 && std::fabs(sar - m_sampleAR) > 1e-6)
        m_laterSampleAR = sar;
}

void MPEGStreamReader::fillAspectBySAR(const double sar)
{
    if (m_streamAR == VideoAspectRatio::AR_KEEP_DEFAULT)
    {
        const double ar = getStreamWidth() * sar / static_cast<double>(getStreamHeight());
        static constexpr double base_ar[] = {0.0, 1.0, 4.0 / 3.0, 16.0 / 9.0, 221.0 / 100.0};
        double minEps = INT_MAX;
        m_streamAR = VideoAspectRatio::AR_KEEP_DEFAULT;
        for (unsigned i = 0; i < std::size(base_ar); ++i)
        {
            if (fabs(ar - base_ar[i]) < minEps)
            {
                minEps = fabs(ar - base_ar[i]);
                m_streamAR = static_cast<VideoAspectRatio>(i);
            }
        }
    }
}

void MPEGStreamReader::fillVideoDiscoveryData(StreamDiscoveryData& data)
{
    data.width = static_cast<int>(getStreamWidth());
    data.height = static_cast<int>(getStreamHeight());
    data.fps = m_fps;
    data.interlaced = getInterlaced();
    data.hdrType = getStreamHDR();
}
