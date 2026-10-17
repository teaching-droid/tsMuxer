#include "vpxStreamReader.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

#include "avCodecs.h"
#include "avPacket.h"
#include "bitStream.h"
#include "nalUnits.h"
#include "vodCoreException.h"
#include "vod_common.h"

VpxStreamReader::VpxStreamReader(const bool vp9)
    : m_vp9(vp9), m_width(0), m_height(0), m_profile(0), m_bitDepth(8), m_lastIFrame(false), m_firstFrame(true)
{
}

const CodecInfo& VpxStreamReader::getCodecInfo() { return m_vp9 ? vp9CodecInfo : vp8CodecInfo; }

std::vector<uint8_t> VpxStreamReader::unframe(const uint8_t* data, const int size)
{
    const uint8_t* p = data;
    const uint8_t* end = data + size;
    while (p < end && *p == 0) ++p;  // the start code, three or four bytes
    if (p < end && *p == 1)
        ++p;
    while (end > p && end[-1] == 0) --end;  // nothing follows the stop byte; this is only care
    if (end > p && end[-1] == 0x80)
        --end;
    std::vector<uint8_t> frame(end - p);
    if (!frame.empty())
        frame.resize(NALUnit::decodeNAL(p, end, frame.data(), frame.size()));
    return frame;
}

bool VpxStreamReader::readIvfHeader(const uint8_t* data, const int size, bool& vp9, double& fps)
{
    // "DKIF", version (2), header size (2), fourcc, width (2), height (2), time base denominator (4) and numerator (4),
    // frames (4), unused (4). Then each frame: its size (4), its time in the time base (8), the frame.
    if (size < 32 || memcmp(data, "DKIF", 4) != 0 ||
        (memcmp(data + 8, "VP80", 4) != 0 && memcmp(data + 8, "VP90", 4) != 0))
        return false;
    vp9 = data[10] == '9';
    const auto le = [data](const int at, const int bytes)
    {
        uint64_t v = 0;
        for (int i = bytes - 1; i >= 0; --i) v = v << 8 | data[at + i];
        return v;
    };
    const auto den = static_cast<double>(le(16, 4));
    const auto num = static_cast<double>(le(20, 4));
    fps = 0.0;
    if (den <= 0 || num <= 0)
        return true;
    // The time base is not the frame rate: one writer counts frames, another milliseconds. The rate is the mean spacing
    // of the first frames' times, snapped to a standard rate as for a Matroska track without DefaultDuration.
    // The frame sizes are 32 bit: added as an int, a damaged one of 2 GB or more made the position negative, and the
    // next frame's time was read before the buffer.
    uint64_t at = le(6, 2);
    int frames = 0;
    uint64_t first = 0, last = 0;
    while (frames < 240 && at + 12 <= static_cast<uint64_t>(size))
    {
        const uint64_t t = le(static_cast<int>(at) + 4, 8);
        if (frames == 0)
            first = t;
        last = t;
        ++frames;
        at += 12 + le(static_cast<int>(at), 4);
    }
    if (frames >= 2 && last > first)
        fps = den / num / (static_cast<double>(last - first) / (frames - 1));
    else
        fps = den / num;
    for (const double standard : {12.5, 15000.0 / 1001, 15.0, 24000.0 / 1001, 24.0, 25.0, 30000.0 / 1001, 30.0,
                                  48000.0 / 1001, 48.0, 50.0, 60000.0 / 1001, 60.0, 100.0, 120000.0 / 1001, 120.0})
        if (std::fabs(fps - standard) < standard * 0.0004)
            fps = standard;
    return true;
}

void VpxStreamReader::setBuffer(uint8_t* data, const uint32_t dataLen, const bool lastBlock)
{
    // Every block arrives behind the front pad the reader protocol puts in front of it.
    const uint8_t* const block = data + MAX_AV_PACKET_SIZE;
    if (m_input == Input::UNDECIDED && dataLen > 0)
    {
        bool vp9 = false;
        double fps = 0;
        if (readIvfHeader(block, static_cast<int>(dataLen), vp9, fps))
        {
            m_input = Input::IVF;
            m_ivfHeaderLeft = block[6] | block[7] << 8;
            if (m_fps == 0.0 && fps > 0)
                setFPS(correctFps(fps));
        }
        else
            m_input = Input::FRAMED;
    }
    if (m_input != Input::IVF)
    {
        MPEGStreamReader::setBuffer(data, dataLen, lastBlock);
        return;
    }
    if (m_ivfDamagedSize != 0)
    {
        m_ivfLeftOut += dataLen;
        if (lastBlock)
            LTRACE(LT_WARN, 2,
                   "Warning: IVF: a frame gives its size as "
                       << m_ivfDamagedSize
                       << " bytes, more than any frame can have, so the file is damaged there. The frames before it "
                          "are kept; the rest of the file, "
                       << m_ivfLeftOut << " bytes, is left out.");
        MPEGStreamReader::setBuffer(data, 0, lastBlock);
        return;
    }
    m_pending.insert(m_pending.end(), block, block + dataLen);
    size_t at = 0;
    if (m_ivfHeaderLeft > 0)
    {
        at = (std::min)(static_cast<size_t>(m_ivfHeaderLeft), m_pending.size());
        m_ivfHeaderLeft -= static_cast<int>(at);
    }
    // each frame: its size (4, little endian), its time (8), the frame
    std::vector<uint8_t> framed;
    while (at + 12 <= m_pending.size())
    {
        const uint32_t frameSize = m_pending[at] | m_pending[at + 1] << 8 | m_pending[at + 2] << 16 |
                                   static_cast<uint32_t>(m_pending[at + 3]) << 24;
        // A damaged size field asked for up to 4 GB: the reader kept every later block while it waited for a
        // frame that never came, so a large file was held in memory to its end and then left out. No frame is
        // that large; such a size ends the reading of frames at once, and the rest of the file is counted, not kept.
        constexpr uint32_t maxIvfFrameSize = 256u * 1024 * 1024;
        if (frameSize > maxIvfFrameSize)
        {
            m_ivfDamagedSize = frameSize;
            m_ivfLeftOut += static_cast<int64_t>(m_pending.size() - at);
            at = m_pending.size();
            break;
        }
        if (at + 12 + frameSize > m_pending.size())
            break;
        const size_t start = framed.size();
        framed.resize(start + 4 + frameSize + frameSize / 2 + 8);
        framed[start] = framed[start + 1] = 0;
        framed[start + 2] = 1;
        int encoded = frameSize > 0
                          ? NALUnit::encodeNAL(m_pending.data() + at + 12, m_pending.data() + at + 12 + frameSize,
                                               framed.data() + start + 3, framed.size() - start - 4)
                          : 0;
        if (encoded < 0)
            encoded = 0;
        framed[start + 3 + encoded] = 0x80;
        framed.resize(start + 4 + encoded);
        at += 12 + frameSize;
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(at));
    if (lastBlock && m_ivfDamagedSize != 0)
        LTRACE(LT_WARN, 2,
               "Warning: IVF: a frame gives its size as "
                   << m_ivfDamagedSize
                   << " bytes, more than any frame can have, so the file is damaged there. The frames before it are "
                      "kept; the rest of the file, "
                   << m_ivfLeftOut << " bytes, is left out.");
    if (lastBlock && !m_pending.empty())
    {
        LTRACE(LT_WARN, 2, "IVF: the file ends inside a frame, " << m_pending.size() << " bytes left out");
        m_pending.clear();
    }
    m_convBuffer.resize(MAX_AV_PACKET_SIZE + framed.size());
    if (!framed.empty())  // an empty vector may have no data pointer, which memcpy must not get even for 0 bytes
        memcpy(m_convBuffer.data() + MAX_AV_PACKET_SIZE, framed.data(), framed.size());
    MPEGStreamReader::setBuffer(m_convBuffer.data(), static_cast<uint32_t>(framed.size()), lastBlock);
}

int VpxStreamReader::prepareDemuxPacket(uint8_t* data, const int size, const uint8_t** outData)
{
    // An IVF file: a 32 byte file header, then each frame behind 12 bytes of its own, its size and its time. A frame
    // larger than a packet comes in several; it is written whole at its first, which still has all of it in the
    // reader's buffer, and the packets that follow with the rest of it are left out.
    *outData = data;
    if (size <= 0)
        return 0;
    if (m_demuxSkip > 0)
    {
        const int64_t skipped = (std::min)(m_demuxSkip, static_cast<int64_t>(size));
        m_demuxSkip -= skipped;
        return 0;
    }
    if (!(size >= 3 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (size >= 4 && data[2] == 0 && data[3] == 1))))
        return 0;
    uint8_t* frameEnd = NALUnit::findNALWithStartCode(data + 3, m_bufEnd, m_longCodesAllowed);
    if (frameEnd < data + size)
        frameEnd = data + size;
    m_demuxSkip = (frameEnd - data) - size;
    const std::vector<uint8_t> frame = unframe(data, static_cast<int>(frameEnd - data));

    m_demuxBuffer.clear();
    const auto le = [this](const uint64_t v, const int bytes)
    {
        for (int i = 0; i < bytes; ++i) m_demuxBuffer.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    if (!m_ivfHeaderWritten)
    {
        m_ivfHeaderWritten = true;
        m_demuxFrames = 0;
        // the frame rate as a fraction: the NTSC rates exactly, the rest in thousandths
        const double fps = m_fps > 0 ? m_fps : 25.0;
        uint32_t rate = static_cast<uint32_t>(llround(fps * 1000)), scale = 1000;
        for (const int base : {24, 30, 48, 60, 120})
            if (std::fabs(fps - base * 1000.0 / 1001) < 0.001)
            {
                rate = base * 1000;
                scale = 1001;
            }
        for (const char c : {'D', 'K', 'I', 'F'}) m_demuxBuffer.push_back(static_cast<uint8_t>(c));
        le(0, 2);   // version
        le(32, 2);  // header size
        for (const char c : {'V', 'P', m_vp9 ? '9' : '8', '0'}) m_demuxBuffer.push_back(static_cast<uint8_t>(c));
        le(m_width, 2);
        le(m_height, 2);
        le(rate, 4);
        le(scale, 4);
        le(0, 4);  // the frame count, not known yet
        le(0, 4);
    }
    le(frame.size(), 4);
    le(static_cast<uint64_t>(m_demuxFrames++), 8);
    m_demuxBuffer.insert(m_demuxBuffer.end(), frame.begin(), frame.end());
    *outData = m_demuxBuffer.data();
    return static_cast<int>(m_demuxBuffer.size());
}

bool VpxStreamReader::parseFrameHeader(const uint8_t* frame, const int size)
{
    if (!m_vp9)
    {
        // RFC 6386, 9.1: a three byte frame tag, its lowest bit 0 for a keyframe, which goes on with the start code
        // 9D 01 2A and the size, 14 bits each with two bits of scaling above them.
        if (size < 10 || (frame[0] & 1) != 0 || frame[3] != 0x9D || frame[4] != 0x01 || frame[5] != 0x2A)
            return false;
        m_profile = frame[0] >> 1 & 7;
        m_width = (frame[6] | frame[7] << 8) & 0x3FFF;
        m_height = (frame[8] | frame[9] << 8) & 0x3FFF;
        return true;
    }

    // VP9, the uncompressed header: frame marker, profile, show_existing_frame, frame_type (0 for a keyframe),
    // show_frame and error_resilient_mode, then for a keyframe the sync code 49 83 42, the colour configuration and
    // the frame size. The first frame of a superframe stands at the start of the block.
    try
    {
        // A copy with room after it: the reader fetches four bytes at a time.
        std::vector<uint8_t> copy(frame, frame + size);
        copy.resize(size + 8, 0);
        BitStreamReader bits{};
        bits.setBuffer(copy.data(), copy.data() + size);
        if (bits.getBits(2) != 2)
            return false;
        const int profileLow = bits.getBit();
        const int profile = (bits.getBit() << 1) | profileLow;
        if (profile == 3)
            bits.skipBit();
        if (bits.getBit())  // show_existing_frame
            return false;
        if (bits.getBit())  // frame_type 1: not a keyframe
            return false;
        bits.skipBits(2);  // show_frame, error_resilient_mode
        if (bits.getBits(24) != 0x498342)
            return false;
        int bitDepth = 8;
        if (profile >= 2)
            bitDepth = bits.getBit() ? 12 : 10;
        const unsigned colourSpace = bits.getBits(3);
        if (colourSpace != 7)  // not sRGB
        {
            bits.skipBit();  // colour range
            if (profile == 1 || profile == 3)
                bits.skipBits(3);  // subsampling x and y, reserved
        }
        else if (profile == 1 || profile == 3)
            bits.skipBit();
        m_width = bits.getBits(16) + 1;
        m_height = bits.getBits(16) + 1;
        m_profile = profile;
        m_bitDepth = bitDepth;
        return true;
    }
    catch (BitStreamException&)
    {
        return false;
    }
}

void VpxStreamReader::incTimings()
{
    if (m_totalFrameNum++ > 0)
        m_curDts += m_pcrIncPerFrame;
    m_curPts = m_curDts;  // neither codec reorders: each frame is shown when it is decoded
}

int VpxStreamReader::intDecodeNAL(uint8_t* buff)
{
    // One unit is one frame. Its header is read from its first bytes, unescaped.
    uint8_t* frameEnd = NALUnit::findNALWithStartCode(buff, m_bufEnd, m_longCodesAllowed);
    if (!m_eof && frameEnd == m_bufEnd)
        return NOT_ENOUGH_BUFFER;
    uint8_t header[96];
    const int headerLen = NALUnit::decodeNAL(buff, (std::min)(frameEnd, buff + 64), header, sizeof header);
    if (m_firstFrame)
    {
        updateFPS(nullptr, buff, frameEnd, 0);  // the container's rate, fps= or 25 with a message
        m_firstFrame = false;
    }
    const unsigned width = m_width;
    const unsigned height = m_height;
    m_lastIFrame = parseFrameHeader(header, headerLen);
    if (width != 0 && (m_width != width || m_height != height))
    {
        // A later keyframe of another size. The track is described by its first size.
        m_width = width;
        m_height = height;
    }
    m_spsPpsFound = false;
    incTimings();
    return 0;
}

CheckStreamRez VpxStreamReader::checkStream(uint8_t* buffer, const int len)
{
    CheckStreamRez rez;
    uint8_t* end = buffer + len;
    bool ivfVp9 = false;
    double ivfFps = 0;
    const bool ivf = readIvfHeader(buffer, len, ivfVp9, ivfFps);
    if (ivf && ivfVp9 != m_vp9)
        return rez;
    uint8_t* frame = NALUnit::findNextNAL(buffer, end);
    if (ivf || frame < end)
    {
        uint8_t* frameEnd = NALUnit::findNALWithStartCode(frame, end, true);
        uint8_t header[96];
        const int ivfHeader = ivf ? (buffer[6] | buffer[7] << 8) + 12 : 0;
        const int headerLen = ivf ? (std::min)(64, len - ivfHeader)
                                  : NALUnit::decodeNAL(frame, (std::min)(frameEnd, frame + 64), header, sizeof header);
        if (ivf && headerLen > 0)
            memcpy(header, buffer + ivfHeader, headerLen);
        if (headerLen > 0 && parseFrameHeader(header, headerLen) && m_width > 0 && m_height > 0)
        {
            rez.codecInfo = getCodecInfo();
            std::ostringstream str;
            str << "Profile: " << m_profile << "  Resolution: " << m_width << ":" << m_height << "p";
            if (m_vp9)
                str << "  Bit depth: " << m_bitDepth << "bit";
            if (ivf && ivfFps > 0)
                str << "  Frame rate: " << correctFps(ivfFps);
            else
                str << "  Frame rate: not found";
            rez.streamDescr = str.str();
        }
    }
    m_totalFrameNum = 0;
    m_curDts = m_curPts = 0;
    return rez;
}
