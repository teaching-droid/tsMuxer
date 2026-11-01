// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "h264StreamReader.h"

#include <fs/systemlog.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

#include "avCodecs.h"
#include "tsMuxer.h"
#include "tsPacket.h"
#include "vodCoreException.h"
#include "vod_common.h"

using namespace std;

H264StreamReader::H264StreamReader()
{
    m_frameNum = 0;
    m_lastMessageLen = -1;
    m_forceLsbDiv = 0;
    m_fieldFirst = 0;
    prevPicOrderCntMsb = prevPicOrderCntLsb = 0;
    m_iFramePtsOffset = 0;
    m_isFirstFrame = true;
    m_forcedLevel = 0;
    m_delimiterFound = false;
    m_pict_type = 0;
    m_nextFrameFound = false;
    m_nextFrameIdr = false;
    m_firstAUDWarn = true;
    m_firstSPSWarn = true;
    m_firstSEIWarn = true;
    m_lastSlicePPS = UINT_MAX;
    m_lastSliceSPS = UINT_MAX;
    m_lastSliceIDR = false;
    m_h264SPSCont = false;
    m_spsCounter = 0;
    m_insertSEIMethod = SeiMethod::SEI_DoNotInsert;
    m_needSeiCorrection = false;
    m_firstDecodeNal = true;
    orig_hrd_parameters_present_flag = false;
    orig_vcl_parameters_present_flag = false;
    m_spsFlagsKnown = false;
    m_lastPictStruct = 0;

    m_bSliceFound = false;
    m_picOrderOffset = 0;
    m_frameDepth = 1;
    m_lastIFrame = false;
    m_idrSliceCnt = 0;
    m_firstFileFrame = false;
    m_lastDtsInc = 0;

    m_mvcSubStream = false;
    m_mvcPrimaryStream = false;
    m_blurayMode = true;
    number_of_offset_sequences = -1;
    m_bdRomMetaDataMsgPtsPos = 0;
    m_priorityNalAddr = nullptr;
    m_OffsetMetadataPtsAddr = nullptr;
    m_startPts = 0;
    m_decodedSliceHeader.resize(8 + 8);  // 8 for slice data, 8 - extra size for increase decoded data
    m_removalDelay = 0;
    m_spsChangeWarned = false;
}

H264StreamReader::~H264StreamReader()
{
    for (const auto& [index, sps] : m_spsMap) delete sps;
    for (const auto& [index, pps] : m_ppsMap) delete pps;
}

void H264StreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    if (data.fps > 0.0 && m_fps == 0.0)
        setFPS(data.fps);
}

// Whether an interlaced stream codes its pictures as frames (MBAFF) and no fields: the field_pic_flag of the first
// slice of each picture in the block read so far, read with the parameter set being read. A field, or no slice to
// tell, keeps the default rate (MPEGStreamReader::fallbackFps).
bool H264StreamReader::codesFramesOnly(void* curNalUnit)
{
    const SPSUnit* sps = curNalUnit         ? static_cast<const SPSUnit*>(curNalUnit)
                         : m_spsMap.empty() ? nullptr
                                            : m_spsMap.begin()->second;
    if (sps == nullptr || sps->frame_mbs_only_flag || m_buffer == nullptr)
        return false;
    int frames = 0;
    for (uint8_t* nal = NALUnit::findNextNAL(m_buffer, m_bufEnd); nal < m_bufEnd - 4 && frames < 16;
         nal = NALUnit::findNextNAL(nal, m_bufEnd))
    {
        const auto type = static_cast<NALUnit::NALType>(*nal & 0x1f);
        if (type != NALUnit::NALType::nuSliceIDR && type != NALUnit::NALType::nuSliceNonIDR)
            continue;
        uint8_t header[32];
        const int len = NALUnit::decodeNAL(nal + 1, (std::min)(nal + 25, m_bufEnd), header, sizeof(header));
        if (len <= 0)
            continue;
        SliceUnit::FieldHeader head;
        if (!SliceUnit::readFieldHeader(header, header + len, [sps](unsigned) { return sps; }, head))
            return false;
        if (head.firstMb != 0)
            continue;  // not the first slice of its picture
        if (head.field)
            return false;
        ++frames;
    }
    return frames > 0;
}

int H264StreamReader::secondFieldStart(const uint8_t* data, const int size) const
{
    const auto spsOf = [this](const unsigned ppsId) -> const SPSUnit*
    {
        const auto pps = m_ppsMap.find(ppsId);
        if (pps == m_ppsMap.end())
            return nullptr;
        const auto sps = m_spsMap.find(pps->second->seq_parameter_set_id);
        return sps == m_spsMap.end() ? nullptr : sps->second;
    };
    auto* const begin = const_cast<uint8_t*>(data);
    uint8_t* const end = begin + size;
    bool firstSeen = false;
    bool firstBottom = false;
    uint8_t* afterLastSlice = nullptr;  // the start code of the first unit after the slice before, until a slice
    bool sliceSeen = false;
    for (uint8_t* nal = NALUnit::findNextNAL(begin, end); nal < end; nal = NALUnit::findNextNAL(nal, end))
    {
        uint8_t* startCode = nal - 3;
        if (startCode > begin && startCode[-1] == 0)
            --startCode;
        const auto type = static_cast<NALUnit::NALType>(*nal & 0x1f);
        const bool slice = type == NALUnit::NALType::nuSliceIDR || type == NALUnit::NALType::nuSliceNonIDR;
        if (!slice)
        {
            if (sliceSeen)
            {
                afterLastSlice = startCode;
                sliceSeen = false;
            }
            continue;
        }
        sliceSeen = true;
        uint8_t* const unitsBefore = afterLastSlice;  // the units right in front of this slice, if any
        afterLastSlice = nullptr;
        uint8_t header[32];
        const int len = NALUnit::decodeNAL(nal + 1, (std::min)(nal + 25, end), header, sizeof(header));
        SliceUnit::FieldHeader head;
        if (len <= 0 || !SliceUnit::readFieldHeader(header, header + len, spsOf, head))
            return -1;
        if (head.firstMb != 0)
            continue;  // a further slice of its picture
        if (!firstSeen)
        {
            if (!head.field)
                return -1;  // a frame
            firstSeen = true;
            firstBottom = head.bottom;
            continue;
        }
        if (!head.field || head.bottom == firstBottom)
            return -1;
        // The units between the last slice of the first field and this one are the second field's: its delimiter,
        // its SEI, a parameter set sent again before it. With none, the field begins at its own start code.
        return static_cast<int>((unitsBefore ? unitsBefore : startCode) - begin);
    }
    return -1;
}

static int fieldsShownOf(int picStruct);

CheckStreamRez H264StreamReader::checkStream(uint8_t* buffer, int len)
{
    SEIUnit lastSEI;
    SliceUnit slice;
    CheckStreamRez rez;
    uint8_t* end = buffer + len;
    std::string tmpDescr;
    bool pulldownInserted = false;
    bool offsetsInserted = false;
    int64_t timedFrames = 0;  // frame pictures with a pic_struct, and the fields they are shown for
    int64_t fieldsShown = 0;
    // the slices of a stream that can code fields: coded as frames, and coded as fields
    bool interlacedFrames = false;
    bool fields = false;

    for (uint8_t* nal = NALUnit::findNextNAL(buffer, end); nal < end - 4; nal = NALUnit::findNextNAL(nal, end))
    {
        if (*nal & 0x80)
            return rez;
        auto nalType = static_cast<NALUnit::NALType>(*nal & 0x1f);
        const uint8_t* nextNal = NALUnit::findNALWithStartCode(nal, end, true);
        if (!m_eof && nextNal == end)
            break;

        switch (nalType)
        {
        case NALUnit::NALType::nuSubSPS:
            m_mvcSubStream = true;
            [[fallthrough]];
        case NALUnit::NALType::nuSPS:
        {
            if (nalType == NALUnit::NALType::nuSPS)
                m_mvcPrimaryStream = true;
            auto sps = new SPSUnit();
            sps->decodeBuffer(nal, nextNal);
            if (const int r = sps->deserialize(); r != 0)
            {
                delete sps;
                if (r == NOT_ENOUGH_BUFFER)
                    break;  // the whole unit is here and still ends early: damaged, skipped
                return rez;
            }
            if (m_spsMap.find(sps->seq_parameter_set_id) != m_spsMap.end())
            {
                delete sps;
                break;
            }
            m_spsMap.insert(make_pair(sps->seq_parameter_set_id, sps));
            if (tmpDescr.empty())
                tmpDescr = sps->getStreamDescr();
            break;
        }
        case NALUnit::NALType::nuPPS:
        {
            auto pps = new PPSUnit();
            pps->decodeBuffer(nal, nextNal);
            if (const int r = pps->deserialize(); r != 0)
            {
                delete pps;
                if (r == NOT_ENOUGH_BUFFER)
                    break;  // the whole unit is here and still ends early: damaged, skipped
                return rez;
            }
            if (m_spsMap.find(pps->seq_parameter_set_id) == m_spsMap.end() ||
                m_ppsMap.find(pps->pic_parameter_set_id) != m_ppsMap.end())
            {
                delete pps;
                break;
            }
            m_ppsMap.insert(make_pair(pps->pic_parameter_set_id, pps));
            break;
        }
        case NALUnit::NALType::nuSEI:
            if (!m_spsMap.empty())
            {
                SPSUnit* sps = m_spsMap.begin()->second;
                lastSEI.decodeBuffer(nal, nextNal);
                lastSEI.deserialize(*sps, sps->nalHrdParams.isPresent || sps->vclHrdParams.isPresent);
                if (lastSEI.hasProcessedMessage(SEI_MSG_PIC_TIMING) && lastSEI.pic_struct >= 0 &&
                    lastSEI.pic_struct != 1 && lastSEI.pic_struct != 2)  // a frame, not a field
                {
                    ++timedFrames;
                    fieldsShown += fieldsShownOf(lastSEI.pic_struct);
                }
                if (lastSEI.pic_struct == 5 || lastSEI.pic_struct == 6 || lastSEI.pic_struct == 7 ||
                    lastSEI.pic_struct == 8)
                {
                    if (!pulldownInserted)
                    {
                        pulldownInserted = true;
                        tmpDescr = sps->getStreamDescr();
                        tmpDescr += " (pulldown)";
                    }
                }
                if (!offsetsInserted && lastSEI.number_of_offset_sequences >= 0)
                {
                    offsetsInserted = true;
                    tmpDescr += "  3d-pg-planes: " + int32ToStr(lastSEI.number_of_offset_sequences);
                }
            }
            break;

        case NALUnit::NALType::nuSliceIDR:
            // m_openGOP = false;
        case NALUnit::NALType::nuSliceNonIDR:
        case NALUnit::NALType::nuSliceA:
        case NALUnit::NALType::nuSliceB:
        case NALUnit::NALType::nuSliceC:
        case NALUnit::NALType::nuSliceExt:
            if (m_ppsMap.empty() || m_spsMap.empty())
                break;
            try
            {
                uint8_t tmpBuffer[512];
                int toDecode = static_cast<int>(FFMIN(sizeof(tmpBuffer) - 8, nextNal - nal));
                int decodedLen = SliceUnit::decodeNAL(nal, nal + toDecode, tmpBuffer, sizeof(tmpBuffer));
                int nalRez = slice.deserialize(tmpBuffer, tmpBuffer + decodedLen, m_spsMap, m_ppsMap);
                if (nalRez != 0)
                    return rez;
                if (slice.getSPS()->frame_mbs_only_flag == 0)
                    (slice.m_field_pic_flag ? fields : interlacedFrames) = true;

                if (m_mvcSubStream)
                    rez.codecInfo = h264DepCodecInfo;
                else
                    rez.codecInfo = h264CodecInfo;
                rez.streamDescr = tmpDescr;
            }
            catch (BitStreamException&)
            {
                return rez;
            }
            break;
        default:
            break;
        }
    }
    if (m_mvcSubStream && m_mvcPrimaryStream)
        rez.multiSubStream = true;
    // The rate without the pulldown: delPulldown shows each picture once. Taken as 23.976 for any 29.97 stream, and
    // left at the stream's rate for 720p59.94 film, whose pictures are doubled and tripled: it played 2.5 times fast.
    if (pulldownInserted && timedFrames >= 8 && !m_spsMap.empty())
        rez.filmFps = m_spsMap.begin()->second->getFPS() * 2.0 * static_cast<double>(timedFrames) /
                      static_cast<double>(fieldsShown);
    rez.framesOnly = interlacedFrames && !fields;

    return rez;
}

const CodecInfo& H264StreamReader::getCodecInfo()
{
    if (m_mvcSubStream)
        return h264DepCodecInfo;
    return h264CodecInfo;
}

uint8_t* H264StreamReader::writeNalPrefix(uint8_t* curPos) const
{
    // What is inserted (delimiters, parameter sets, SEI) stands at the head of an access unit, where the byte stream
    // format asks for four bytes. It took the length of the stream's own start code there, three in many streams:
    // every inserted SPS and PPS of such a stream had three.
    *curPos++ = 0;
    *curPos++ = 0;
    *curPos++ = 0;
    *curPos++ = 1;
    return curPos;
}

// The start delay of the buffering period SEI written with insertSEI and forceSEI. It was a fixed 9.5 s, whatever
// the HRD the SPS gives: 12.7 times the 0.75 s the HRD tsMuxeR adds allows (H.264 C.1, initial_cpb_removal_delay is at
// most 90000 * CpbSize / BitRate), and in a field shorter than 24 bits only its low bits were kept. It is now 95 % of
// that limit, the buffer just under full at the first picture, for each schedule, kept within the field. The NAL and
// the VCL HRD get the same value, the smaller. This is the delay of the first buffering period; the later ones follow
// the buffer (hrdAccessUnit).
static void setInitialCpbRemovalDelay(SEIUnit& sei, const SPSUnit& sps)
{
    const HRDParams* params[] = {&sps.nalHrdParams, &sps.vclHrdParams};
    for (unsigned i = 0; i < 32; i++)
    {
        int64_t delay = 0;  // no HRD with this schedule: nothing is written for it
        for (const HRDParams* p : params)
        {
            if (!p->isPresent || i > p->cpb_cnt_minus1 || i >= p->bit_rate_value_minus1.size() ||
                i >= p->cpb_size_value_minus1.size())
                continue;
            const double bitRate = (p->bit_rate_value_minus1[i] + 1.0) * std::pow(2.0, 6 + p->bit_rate_scale);
            const double cpbSize = (p->cpb_size_value_minus1[i] + 1.0) * std::pow(2.0, 4 + p->cpb_size_scale);
            const int64_t fieldMax = (int64_t{1} << (p->initial_cpb_removal_delay_length_minus1 + 1)) - 1;
            int64_t v = static_cast<int64_t>(std::floor(90000.0 * cpbSize / bitRate * 0.95));
            v = (std::max)(int64_t{1}, (std::min)(v, fieldMax));
            delay = delay == 0 ? v : (std::min)(delay, v);
        }
        sei.initial_cpb_removal_delay[i] = sei.vcl_initial_cpb_removal_delay[i] = static_cast<int>(delay);
        sei.initial_cpb_removal_delay_offset[i] = sei.vcl_initial_cpb_removal_delay_offset[i] = 0;
    }
}

int H264StreamReader::readPacket(AVPacket& avPacket)
{
    const int rez = MPEGStreamReader::readPacket(avPacket);
    int added = 0;
    if (rez == 0)
    {
        added = addFrameData(avPacket);
        if (added == 0)
            added = addFieldTiming(avPacket);
    }
    avPacket.addedBytes = added;
    countHrdBytes(avPacket, added);
    return rez;
}

int H264StreamReader::flushPacket(AVPacket& avPacket)
{
    const int rez = MPEGStreamReader::flushPacket(avPacket);
    int added = addFrameData(avPacket);
    if (added == 0)
        added = addFieldTiming(avPacket);
    avPacket.addedBytes = added;
    countHrdBytes(avPacket, added);
    hrdWarning();
    return rez;
}

// insertSEI and forceSEI write an HRD into the SPS: the stream's own, or, for a stream that has none, 40 Mbit/s with a
// 30 Mbit buffer, the most a Blu-ray disc allows, whatever the stream needs. A stream denser than that HRD arrives late
// in its buffer model, and the buffering period and picture timing SEI written for it are wrong; nothing said so. Once
// per stream, at its end: how many pictures arrive late, from when, and the most the stream needs within a second.
void H264StreamReader::hrdWarning()
{
    if (m_hrdLate == 0 || m_hrdLateSaid || m_hrd.empty())
        return;
    m_hrdLateSaid = true;
    const HrdSchedule& s0 = m_hrd.front();
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << "Warning: the H.264 stream needs more than the HRD in its SPS gives ("
         << s0.bitRate / 1e6 << " Mbit/s, a buffer of " << s0.cpbSize / 1e6 << " Mbit"
         << (orig_hrd_parameters_present_flag || orig_vcl_parameters_present_flag
                 ? ")"
                 : ", the values tsMuxeR writes for a stream without an HRD)")
         << ": " << m_hrdLate << " pictures arrive after they are due, the first " << m_hrdFirstLate
         << " s after the start, and the stream needs up to " << m_hrdPeakRate / 1e6
         << " Mbit/s within one second. The buffering period and picture timing SEI that insertSEI and forceSEI write "
            "do not fit this stream. Mux it without insertSEI and forceSEI, or encode it within that rate.";
    LTRACE(LT_WARN, 2, text.str());
}

// A Matroska output takes the packets as they are and does not ask for writeAdditionData. With the SEI correction its
// SPS (in CodecPrivate) has the HRD that tsMuxeR writes, while the stream's own buffering period and picture timing
// SEI are left out (skipNal), so it had an HRD and no SEI for it. For such an output (setAddDataInPackets) the units
// writeAdditionData gives are put in front of the first packet of each frame that is not the stream's own
// delimiter. Returns the bytes put in front.
int H264StreamReader::addFrameData(AVPacket& avPacket)
{
    if (!m_addDataInPackets || !m_needSeiCorrection || m_mvcSubStream || m_mvcPrimaryStream || avPacket.size <= 0 ||
        (m_addDataDtsKnown && avPacket.dts == m_addDataDts))
        return 0;
    // The stream's own delimiter stays a packet of its own, first in the frame, and the units go in front of the
    // packet after it: so only what tsMuxeR writes is put in front of a packet (AVPacket::addedBytes), and the
    // delimiter keeps the framing the source gave it.
    const int startCode = avPacket.size >= 4 && avPacket.data[0] == 0 && avPacket.data[1] == 0
                              ? (avPacket.data[2] == 1                            ? 3
                                 : avPacket.data[2] == 0 && avPacket.data[3] == 1 ? 4
                                                                                  : 0)
                              : 0;
    if (startCode > 0 && startCode < avPacket.size)
    {
        const auto nalType = static_cast<NALUnit::NALType>(avPacket.data[startCode] & 0x1f);
        if (nalType == NALUnit::NALType::nuDelimiter || nalType == NALUnit::NALType::nuDRD)
        {
            if (!m_frameStartKnown)
            {
                m_frameStartKnown = true;
                m_frameStart = m_hrdBytes;
                m_frameStartVcl = m_hrdVclBytes;
            }
            return 0;
        }
    }
    m_addDataDts = avPacket.dts;
    m_addDataDtsKnown = true;
    // the packet is not counted yet (countHrdBytes): the frame starts here, or at its delimiter
    m_hrdBytesBefore = m_frameStartKnown ? m_frameStart : m_hrdBytes;
    m_hrdVclBytesBefore = m_frameStartKnown ? m_frameStartVcl : m_hrdVclBytes;
    m_frameStartKnown = false;
    std::vector<uint8_t> buffer(static_cast<size_t>(avPacket.size) + 65536);
    const int len = writeAdditionData(buffer.data(), buffer.data() + buffer.size(), avPacket, nullptr);
    m_hrdAuAdded = 0;  // here the units are in the packet, and counted with it
    if (len <= 0)
        return 0;
    buffer.resize(len);
    if (avPacket.size > 0)
        buffer.insert(buffer.end(), avPacket.data, avPacket.data + avPacket.size);
    m_framePacket.swap(buffer);
    avPacket.data = m_framePacket.data();
    avPacket.size = static_cast<int>(m_framePacket.size());
    return len;
}

// The second field of a frame is an access unit of its own and needs its own picture timing SEI when the SPS has an
// HRD (H.264 D.2.3). It goes into the packet of the first field, so writeAdditionData, which writes the timing of the
// first, is not called for it, and the stream's own timing SEI is left out (skipNal). So the second field had none.
// It is put in front of the first unit of the field that is not its delimiter, removed one tick after the first field
// (the clock tick is a field) and shown one tick after it. Returns the bytes put in front of the packet.
int H264StreamReader::addFieldTiming(AVPacket& avPacket)
{
    if (!m_needSeiCorrection || m_mvcSubStream || !m_secondField || m_secondFieldTimed || !m_fieldTimingKnown ||
        !avPacket.data || avPacket.size < 4 || m_lastSliceSPS == UINT_MAX)
        return 0;
    const uint8_t* data = avPacket.data;
    const int startCode = data[0] == 0 && data[1] == 0 && data[2] == 1                   ? 3
                          : data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1 ? 4
                                                                                         : 0;
    if (startCode == 0 || startCode >= avPacket.size)
        return 0;
    const auto nalType = static_cast<NALUnit::NALType>(data[startCode] & 0x1f);
    if (nalType == NALUnit::NALType::nuDelimiter || nalType == NALUnit::NALType::nuDRD)
        return 0;
    m_secondFieldTimed = true;

    const SPSUnit& sps = *m_spsMap.find(m_lastSliceSPS)->second;
    SEIUnit sei;
    sei.cpb_removal_delay = m_fieldCpbRemovalDelay;
    sei.dpb_output_delay = m_fieldDpbOutputDelay;
    sei.pic_struct = m_fieldPictStruct == 1 ? 2 : m_fieldPictStruct == 2 ? 1 : m_lastPictStruct;
    hrdAccessUnit(sps, false, m_fieldCpbRemovalDelay, sei, m_hrdBytes, m_hrdVclBytes);  // the field is a unit

    uint8_t seiBuffer[256];
    const int seiLen = writeSEIMessage(seiBuffer, seiBuffer + sizeof(seiBuffer), sei, SEI_MSG_PIC_TIMING);
    m_fieldPacket.assign(seiBuffer, seiBuffer + seiLen);
    m_fieldPacket.insert(m_fieldPacket.end(), avPacket.data, avPacket.data + avPacket.size);
    avPacket.data = m_fieldPacket.data();
    avPacket.size = static_cast<int>(m_fieldPacket.size());
    return seiLen;
}

// A packet holds one NAL unit, from its start code, or a further part of a long one, after the units addFieldTiming
// put in front of it (added bytes). The NAL HRD counts every byte of the stream, the VCL HRD the bytes of the VCL and
// filler NAL units, without their start codes (H.264 C.1.1, C-6).
void H264StreamReader::countHrdBytes(const AVPacket& avPacket, const int added)
{
    m_hrdBytesBefore = m_hrdBytes;
    m_hrdVclBytesBefore = m_hrdVclBytes;
    if (!m_needSeiCorrection || !avPacket.data || avPacket.size <= 0)
        return;
    const uint8_t* data = avPacket.data + added;
    const int size = avPacket.size - added;
    m_hrdBytes += avPacket.size;
    if (size <= 0)
        return;
    int startCode = 0;
    if (size >= 4 && data[0] == 0 && data[1] == 0)
    {
        if (data[2] == 1)
            startCode = 3;
        else if (data[2] == 0 && data[3] == 1)
            startCode = 4;
    }
    if (startCode > 0 && startCode < size)
    {
        const auto nalType = static_cast<NALUnit::NALType>(data[startCode] & 0x1f);
        m_hrdVclNal = (nalType >= NALUnit::NALType::nuSliceNonIDR && nalType <= NALUnit::NALType::nuSliceIDR) ||
                      nalType == NALUnit::NALType::nuFillerData;
    }
    if (m_hrdVclNal)
        m_hrdVclBytes += size - startCode;
}

// Called by writeAdditionData once per access unit, before its SEI units are written: counts the access unit written
// before as it arrives in the coded picture buffer, takes the nominal removal time of this one (C.1.2), and, with a
// buffering period, gives its initial delays. They were 95 % of the buffer at every buffering period, while only the
// first starts with the buffer that full: at a later one the delay must not exceed the time from the arrival of the
// access unit before to the removal of this one (C.3 item 1, C-15; for CBR also not fall below it, C-16). So it is
// that time, for VBR at most the first delay, and the offset makes up the difference, so that delay + offset stays
// the same, as the buffering period SEI semantics ask. Arrival follows C.1.1: at the bit rate, for VBR not before the
// removal time less delay + offset (C-4), or less the delay at a buffering period (C-5). A stream denser than its HRD
// allows arrives late, and then no delay fits: the first is kept, so that a decoder that starts there still waits for
// its buffer. Before the first buffering period nothing is counted, and an SPS with another HRD or clock tick starts
// the model again, as the first buffering period does.
void H264StreamReader::hrdAccessUnit(const SPSUnit& sps, const bool bufferingPeriod, const uint32_t cpbRemovalDelay,
                                     SEIUnit& sei, const int64_t start, const int64_t vclStart)
{
    if (m_hrdPending && !m_hrd.empty())
    {
        const double bytes = static_cast<double>(start - m_hrdAuStart + m_hrdAuAdded);
        const double vclBytes = static_cast<double>(vclStart - m_hrdAuVclStart);
        for (HrdSchedule& s : m_hrd)
        {
            double tai = 0;  // C-2, C-3
            if (!m_hrdPendingFirst)
                tai = s.cbr ? s.taf
                            : (std::max)(s.taf, s.tr - static_cast<double>(m_hrdPendingBp ? s.delay : s.sum) / 90000.0);
            s.taf = tai + 8.0 * (s.vcl ? vclBytes : bytes) / s.bitRate;  // C-6
        }
        // For the warning of hrdWarning: access units of the first schedule that arrive after they are due (C.3
        // item 3), and the most bits that are due within one second
        const HrdSchedule& s0 = m_hrd.front();
        if (!m_hrdLowDelay && s0.taf > s0.tr + 1e-6 && m_hrdLate++ == 0)
            m_hrdFirstLate = s0.tr;
        m_hrdWindow.emplace_back(s0.tr, 8.0 * (s0.vcl ? vclBytes : bytes));
        m_hrdWindowBits += m_hrdWindow.back().second;
        while (m_hrdWindow.front().first <= s0.tr - 1.0)
        {
            m_hrdWindowBits -= m_hrdWindow.front().second;
            m_hrdWindow.pop_front();
        }
        m_hrdPeakRate = (std::max)(m_hrdPeakRate, m_hrdWindowBits);
    }
    m_hrdPending = true;
    m_hrdPendingBp = bufferingPeriod;
    m_hrdPendingFirst = false;
    m_hrdAuStart = start;
    m_hrdAuVclStart = vclStart;
    m_hrdAuAdded = 0;

    const double tc = sps.time_scale > 0 ? static_cast<double>(sps.num_units_in_tick) / sps.time_scale : 0.0;
    if (bufferingPeriod)
    {
        std::vector<int64_t> params{sps.num_units_in_tick, sps.time_scale};
        for (const HRDParams* p : {&sps.nalHrdParams, &sps.vclHrdParams})
        {
            params.push_back(p->isPresent ? static_cast<int64_t>(p->cpb_cnt_minus1) : -1);
            params.insert(params.end(),
                          {p->bit_rate_scale, p->cpb_size_scale, p->initial_cpb_removal_delay_length_minus1});
            params.insert(params.end(), p->bit_rate_value_minus1.begin(), p->bit_rate_value_minus1.end());
            params.insert(params.end(), p->cpb_size_value_minus1.begin(), p->cpb_size_value_minus1.end());
            params.insert(params.end(), p->cbr_flag.begin(), p->cbr_flag.end());
        }
        if (m_hrd.empty() || params != m_hrdParams)
        {
            m_hrdParams = params;
            m_hrd.clear();
            m_hrdPendingFirst = true;
            m_hrdLowDelay = sps.low_delay_hrd_flag != 0;
            m_hrdWindow.clear();
            m_hrdWindowBits = 0;
            if (tc > 0 && !m_mvcSubStream)  // the dependent view of 3D: its HRD covers both views
            {
                for (const HRDParams* p : {&sps.nalHrdParams, &sps.vclHrdParams})
                    for (unsigned i = 0; p->isPresent && i <= p->cpb_cnt_minus1 && i < 32; i++)
                    {
                        if (i >= p->bit_rate_value_minus1.size() || i >= p->cpb_size_value_minus1.size() ||
                            sei.initial_cpb_removal_delay[i] <= 0)
                            continue;
                        HrdSchedule s{};
                        s.vcl = p == &sps.vclHrdParams;
                        s.index = i;
                        s.cbr = i < p->cbr_flag.size() && p->cbr_flag[i];
                        s.bitRate = (p->bit_rate_value_minus1[i] + 1.0) * std::pow(2.0, 6 + p->bit_rate_scale);
                        const double cpbSize =
                            (p->cpb_size_value_minus1[i] + 1.0) * std::pow(2.0, 4 + p->cpb_size_scale);
                        const int64_t fieldMax = (int64_t{1} << (p->initial_cpb_removal_delay_length_minus1 + 1)) - 1;
                        s.maxDelay =
                            (std::min)(fieldMax, static_cast<int64_t>(std::floor(90000.0 * cpbSize / s.bitRate)));
                        // setInitialCpbRemovalDelay has written the first delay into sei
                        s.cpbSize = cpbSize;
                        s.first = s.delay = s.sum = sei.initial_cpb_removal_delay[i];
                        s.trPeriod = s.tr = static_cast<double>(s.first) / 90000.0;  // C-7
                        m_hrd.push_back(s);
                    }
            }
            return;
        }
    }
    if (m_hrd.empty())
    {
        m_hrdPending = false;
        return;
    }
    for (HrdSchedule& s : m_hrd)
    {
        s.tr = s.trPeriod + tc * cpbRemovalDelay;  // C-8, C-9
        if (!bufferingPeriod)
            continue;
        const double gap = 90000.0 * (s.tr - s.taf);  // C-14
        int64_t delay = s.first;
        if (gap > 0)
            delay = (std::max)(int64_t{1}, (std::min)(static_cast<int64_t>(std::floor((std::min)(gap, 1e12) + 1e-6)),
                                                      s.cbr ? s.maxDelay : s.first));
        const int64_t offset = (std::max)(int64_t{0}, s.first - delay);
        s.delay = delay;
        s.sum = delay + offset;
        s.trPeriod = s.tr;
        int* delays = s.vcl ? sei.vcl_initial_cpb_removal_delay : sei.initial_cpb_removal_delay;
        int* offsets = s.vcl ? sei.vcl_initial_cpb_removal_delay_offset : sei.initial_cpb_removal_delay_offset;
        delays[s.index] = static_cast<int>(delay);
        offsets[s.index] = static_cast<int>(offset);
    }
}

int H264StreamReader::writeSEIMessage(uint8_t* dstBuffer, const uint8_t* dstEnd, const SEIUnit& sei,
                                      const uint8_t payloadType) const
{
    uint8_t* curPos = dstBuffer;

    if (dstEnd - curPos < 4)
        THROW(ERR_COMMON, "H264 stream error: Not enough buffer for write headers")

    uint8_t tmpBuffer[256];
    BitStreamWriter writer{};
    curPos = writeNalPrefix(curPos);
    writer.setBuffer(tmpBuffer, tmpBuffer + sizeof(tmpBuffer));

    uint8_t* sizeField = nullptr;
    unsigned beforeMessageLen = 0;
    if (m_mvcSubStream)
    {
        writer.putBits(8, static_cast<int>(NALUnit::NALType::nuSEI));
        writer.putBits(8, SEI_MSG_MVC_SCALABLE_NESTING);

        sizeField = writer.getBuffer() + writer.getBitsCount() / 8;
        writer.putBits(8, 0);  // skip size field
        beforeMessageLen = writer.getBitsCount();

        static constexpr uint8_t DEFAULT_MVC_SEI_HEADER[] = {192, 16};
        if (!m_lastSeiMvcHeader.empty())
        {
            for (const auto& i : m_lastSeiMvcHeader) writer.putBits(8, i);
        }
        else
        {
            writer.putBits(8, DEFAULT_MVC_SEI_HEADER[0]);
            writer.putBits(8, DEFAULT_MVC_SEI_HEADER[1]);
        }

        writer.putBits(8, payloadType);
    }
    else
    {
        writer.putBits(8, static_cast<int>(NALUnit::NALType::nuSEI));
        writer.putBits(8, payloadType);
    }

    const SPSUnit* sps = m_spsMap.find(m_lastSliceSPS)->second;
    if (sps)
    {
        if (payloadType == 0)
            sei.serialize_buffering_period_message(*sps, writer, false);

        else
            sei.serialize_pic_timing_message(*sps, writer, false);

        if (sizeField)
        {
            const unsigned msgLen = writer.getBitsCount() - beforeMessageLen;
            *sizeField = static_cast<uint8_t>(msgLen / 8);
        }
        SEIUnit::write_rbsp_trailing_bits(writer);
        writer.flushBits();

        const int sRez = SEIUnit::encodeNAL(tmpBuffer, tmpBuffer + writer.getBitsCount() / 8, curPos, dstEnd - curPos);
        if (sRez == -1)
            THROW(ERR_COMMON, "H264 stream error: Not enough buffer for write headers")
        curPos += sRez;
    }

    return static_cast<int>(curPos - dstBuffer);
}

// Whether the access unit that begins at auStart holds an SPS and a PPS of its own, before its first slice. The packet
// handed over is one unit, its delimiter, and the units after it are still in the buffer, not read yet.
static bool holdsSpsAndPps(uint8_t* auStart, uint8_t* end)
{
    bool sps = false;
    bool pps = false;
    for (uint8_t* nal = NALUnit::findNextNAL(auStart, end); nal < end; nal = NALUnit::findNextNAL(nal, end))
    {
        const auto type = static_cast<NALUnit::NALType>(*nal & 0x1f);
        if ((type >= NALUnit::NALType::nuSliceNonIDR && type <= NALUnit::NALType::nuSliceIDR) ||
            (type == NALUnit::NALType::nuDelimiter && nal > auStart + 4))
            break;
        sps = sps || type == NALUnit::NALType::nuSPS;
        pps = pps || type == NALUnit::NALType::nuPPS;
    }
    return sps && pps;
}

int H264StreamReader::writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                                        PriorityDataInfo* priorityData)
{
    uint8_t* curPos = dstBuffer;
    int movedBytes = 0;  // a packet moved to the front, counted already when the reader handed it over
    uint8_t* const auStart = avPacket.data;  // in the reader's buffer (holdsSpsAndPps)
    if (avPacket.size > 4 && avPacket.size < dstEnd - dstBuffer)
    {
        // A start code is three bytes or four, and both are legal, so where the NAL header sits has
        // to be worked out rather than assumed. Reading byte 4 unconditionally lands one byte INTO
        // the header of a three byte framed packet, so the type comes out wrong and an access unit
        // delimiter already present is not recognised. The HEVC and VVC readers have always
        // detected it; this one did not.
        const int offset = avPacket.data[2] == 1 ? 3 : 4;
        auto nalType = static_cast<NALUnit::NALType>(avPacket.data[offset] & 0x1f);
        if (nalType == NALUnit::NALType::nuDelimiter || nalType == NALUnit::NALType::nuDRD)
        {
            // place delimiter at first place
            m_delimiterFound = true;
            movedBytes = avPacket.size;
            memcpy(curPos, avPacket.data, avPacket.size);
            curPos += avPacket.size;
            avPacket.size = 0;
            avPacket.data = nullptr;
        }
    }

    if (!m_delimiterFound && m_lastSliceSPS != UINT_MAX)
    {
        if (m_firstAUDWarn)
        {
            m_firstAUDWarn = false;
            LTRACE(LT_INFO, 2, "H264 bitstream changed: insert nal unit delimiters");
        }
        if (dstEnd - curPos < 6)
            THROW(ERR_COMMON, "H264 stream error: Not enough buffer for write headers")
        curPos = writeNalPrefix(curPos);
        if (m_mvcSubStream && m_blurayMode)
        {
            *curPos++ = static_cast<uint8_t>(NALUnit::NALType::nuDRD);
        }
        else
        {
            *curPos++ = static_cast<uint8_t>(NALUnit::NALType::nuDelimiter);
            *curPos++ = static_cast<uint8_t>(m_pict_type << 5 | 0x10);  // primary_pic_type << 5 + rbsp bits
        }
    }

    bool needInsTimingSEI = m_needSeiCorrection && m_lastSliceSPS != UINT_MAX;
    bool srcSpsPpsFound = avPacket.flags & AVPacket::IS_SPS_PPS_IN_GOP;
    // The first picture of a new file, the next part of a split or the first picture of a cut, carries the parameter
    // sets when its group has none, as HEVC already does. A stream from MP4 or Matroska holds them once, ahead of its
    // first picture, so without them nothing in that file decoded: this used to be done only with contSPS. The
    // dependent view of a 3D stream keeps the old rule.
    const bool newFileNeedsSpsPps = m_firstFileFrame && !srcSpsPpsFound && !m_mvcSubStream;
    bool spsDiscontinue =
        newFileNeedsSpsPps ||
        (m_h264SPSCont &&
         ((m_spsCounter < 2 && ((m_totalFrameNum > 1 && m_lastSliceIDR) || (m_totalFrameNum > 250 && isIFrame()))) ||
          (m_firstFileFrame && !srcSpsPpsFound)));
    // A picture that carries its own SPS and PPS needs no copy of them. The first IDR picture of a stream taken up in
    // the middle of a group (a cut of a disc clip) came after other pictures and got the copy ahead of its own: SPS
    // and PPS twice in one access unit. Where the stream's own are replaced (replaceToOwnSPS), they are left out and
    // the copy stays.
    if (spsDiscontinue && !replaceToOwnSPS() && auStart != nullptr && auStart >= m_buffer && auStart < m_bufEnd &&
        holdsSpsAndPps(auStart, m_bufEnd))
        spsDiscontinue = false;
    bool needInsSpsPps = false;
    if (isIFrame() && m_lastSliceSPS != UINT_MAX && m_lastSlicePPS != UINT_MAX)
    {
        needInsSpsPps = spsDiscontinue || (replaceToOwnSPS() && srcSpsPpsFound);
    }
    // With the parameter sets replaced, the stream's own PPS units are left out (skipNal), and they were written again
    // only at an I picture. A stream that sends a changed PPS at another picture (a P picture in the middle of a GOP)
    // had its pictures up to the next I picture decoded with the old one: a disc stream gave hundreds of decoder
    // errors. The PPS units are now written at each picture whose own came before it.
    const bool needInsPps = !needInsSpsPps && replaceToOwnSPS() && m_ppsSinceAU && m_lastSlicePPS != UINT_MAX;
    m_ppsSinceAU = false;

    if (needInsSpsPps || needInsPps)
    {
        // Why the check for 250 : unless we see the 2nd IDR frame, we can only see that the stream runs with open GOPs
        // (open GOPs with one IDR at start for the whole movie do happen). That's why there's also a check that there's
        // a SPS before for the first I-frame in the first GOP in a normal stream. There's nothing scary about that - we
        // simply insert one more SPS - but in order to avoid outputting messages into the console, I left the check at
        // 250.
        if (needInsSpsPps && m_firstSPSWarn && m_totalFrameNum > 250)
        {
            m_firstSPSWarn = false;
            if (spsDiscontinue)
            {
                LTRACE(LT_INFO, 2, "H264 bitstream changed: insert SPS/PPS units");
            }
        }
        int sRez;
        if (needInsSpsPps)
        {
            avPacket.flags |= AVPacket::IS_SPS_PPS_IN_GOP;
            SPSUnit* sps = m_spsMap.find(m_lastSliceSPS)->second;
            if (dstEnd - curPos < 4)
                THROW(ERR_COMMON, "H264 stream error: Not enough buffer for write headers")
            curPos = writeNalPrefix(curPos);
            sRez = sps->serializeBuffer(curPos, dstEnd, false);
            if (sRez == -1)
                THROW(ERR_COMMON, "H264 stream error: Not enough buffer for write headers")
            curPos += sRez;
        }

        // PPSUnit* pps = m_ppsMap.find(m_lastSlicePPS)->second;
        // curPos = writeNalPrefix(curPos);
        // curPos += pps->serializeBuffer(curPos, false);
        for (auto [index, pps] : m_ppsMap)
        {
            if (dstEnd - curPos < 4)
                THROW(ERR_COMMON, "H264 stream error: Not enough buffer for write headers")
            curPos = writeNalPrefix(curPos);
            sRez = pps->serializeBuffer(curPos, dstEnd, false);
            if (sRez == -1)
                THROW(ERR_COMMON, "H264 stream error: Not enough buffer for write headers")
            curPos += sRez;
        }
    }

    if (needInsTimingSEI)
    {
        if (m_firstSEIWarn)
        {
            m_firstSEIWarn = false;
            LTRACE(LT_INFO, 2, "H264 bitstream changed: insert pict timing and buffering period SEI units");
        }

        const SPSUnit& sps = *m_spsMap.find(m_lastSliceSPS)->second;
        SEIUnit bpSei;
        const bool bufferingPeriod = m_lastIFrame && m_lastSliceIDR;
        if (bufferingPeriod)
            setInitialCpbRemovalDelay(bpSei, sps);
        hrdAccessUnit(sps, bufferingPeriod, m_removalDelay, bpSei, m_hrdBytesBefore, m_hrdVclBytesBefore);
        if (m_lastIFrame)
        {
            if (m_lastSliceIDR)
                curPos += writeSEIMessage(curPos, dstEnd, bpSei, SEI_MSG_BUFFERING_PERIOD);

            if (m_mvcSubStream)
            {
                if (!m_bdRomMetaDataMsg.empty())
                {
                    // we got slice. fill previous metadata SEI message
                    auto pts90k = m_curDts / INT_FREQ_TO_TS_FREQ + m_startPts;
                    uint8_t* srcData = m_bdRomMetaDataMsg.data();
                    SEIUnit::updateMetadataPts(srcData + m_bdRomMetaDataMsgPtsPos, pts90k);

                    uint8_t* prevPos = curPos;
                    curPos = writeNalPrefix(curPos);
                    curPos += NALUnit::encodeNAL(srcData, srcData + m_bdRomMetaDataMsg.size(), curPos, dstEnd - curPos);
                    if (priorityData)
                        priorityData->push_back(std::make_pair<int, int>(static_cast<int>(prevPos - dstBuffer),
                                                                         static_cast<int>(curPos - prevPos)));
                    m_bdRomMetaDataMsg.clear();
                }
            }
        }

        SEIUnit sei;
        sei.cpb_removal_delay = m_removalDelay;
        sei.dpb_output_delay =
            static_cast<int>(((m_curPts - m_curDts + m_pcrIncPerFrame * getFrameDepth()) * 2) / m_pcrIncPerFrame);
        sei.pic_struct = m_lastPictStruct;

        if (m_lastSliceIDR)
            m_removalDelay = 0;
        m_removalDelay += 2;
        curPos += writeSEIMessage(curPos, dstEnd, sei, SEI_MSG_PIC_TIMING);
        // for a second field (addFieldTiming): one tick after this picture, from the same buffering period
        m_fieldCpbRemovalDelay = m_removalDelay - 1;
        m_fieldDpbOutputDelay = sei.dpb_output_delay;
        m_fieldPictStruct = sei.pic_struct;
        m_fieldTimingKnown = true;
    }
    if (m_hrdPending)
        m_hrdAuAdded = static_cast<int>(curPos - dstBuffer) - movedBytes;
    m_firstFileFrame = false;
    return static_cast<int>(curPos - dstBuffer);
}

int H264StreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, const bool hdmvDescriptors)
{
    SliceUnit slice;
    if (m_firstDecodeNal)
    {
        additionalStreamCheck(m_buffer, m_bufEnd);
        m_firstDecodeNal = false;
    }

    if (hdmvDescriptors)
    {
        // Blu-ray core specifications Table 9-10 - HDMV_video_registration_descriptor
        *dstBuff++ = static_cast<int>(TSDescriptorTag::REGISTRATION);  // descriptor tag
        *dstBuff++ = 8;                                                // descriptor length
        *dstBuff++ = 'H';
        *dstBuff++ = 'D';
        *dstBuff++ = 'M';
        *dstBuff++ = 'V';
        *dstBuff++ = 0xff;  // stuffing byte

        uint8_t video_format, frame_rate_index, aspect_ratio_index;
        // the rate the mux will take: before updateFPS has run, getFPS() is still 0 for a stream that names none
        const double streamFps = m_spsMap.empty() ? 0.0 : correctFps(m_spsMap.begin()->second->getFPS());
        M2TSStreamInfo::blurayStreamParams(headerFps(streamFps), getInterlaced(), getStreamWidth(), getStreamHeight(),
                                           getStreamAR(), &video_format, &frame_rate_index, &aspect_ratio_index);
        *dstBuff++ = !m_mvcSubStream ? static_cast<uint8_t>(StreamType::VIDEO_H264)
                                     : static_cast<uint8_t>(StreamType::VIDEO_MVC);  // stream_coding_type
        *dstBuff++ = static_cast<uint8_t>(video_format << 4 | frame_rate_index);     // video_format + frame_rate
        *dstBuff = static_cast<uint8_t>(aspect_ratio_index << 4 | 0xf);              // aspect ratio + stuffing_bits

        return 10;  // total descriptor length
    }

    // Not HDMV => AVC video descriptor according to H.222 Table 2-92
    // find SPS
    for (uint8_t* nal = NALUnit::findNextNAL(m_buffer, m_bufEnd); nal < m_bufEnd - 4;
         nal = NALUnit::findNextNAL(nal, m_bufEnd))
    {
        const auto nalType = static_cast<NALUnit::NALType>(*nal & 0x1f);
        if (nalType == NALUnit::NALType::nuSPS || nalType == NALUnit::NALType::nuSubSPS)
        {
            processSPS(nal);
            *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::AVC);   // descriptor tag
            *dstBuff++ = 4;                                            // descriptor length
            *dstBuff++ = nal[1];                                       // profile_idc
            *dstBuff++ = nal[2];                                       // constraint flags
            *dstBuff++ = m_forcedLevel == 0 ? nal[3] : m_forcedLevel;  // level_idc
            *dstBuff = 0xbf;  // AVC_still_present, AVC_24_hour flag, Frame_Packing_SEI_not_present_flag, reserved

            return 6;
        }
    }
    // avoid compiler warning: control reaches end of non-void function
    assert(0);  // no SPS nal: exit
    return 0;
}

void H264StreamReader::updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, const int oldSpsLen)
{
    const auto sps = static_cast<SPSUnit*>(nalUnit);
    sps->setFps(m_fps);
    auto tmpBuffer = std::make_unique<uint8_t[]>(oldSpsLen + 16);
    const long newSpsLen = sps->serializeBuffer(tmpBuffer.get(), tmpBuffer.get() + oldSpsLen + 16, false);
    if (newSpsLen == -1)
        THROW(ERR_COMMON, "Track " << m_streamIndex
                                   << ": its H.264 sequence parameter set could not be written again with the frame "
                                      "rate, as it does not hold what an H.264 stream holds there. "
                                   << "The usual reason is that the codec on the meta line is not the codec the file "
                                      "holds. List the file on its own to see what tsMuxeR makes of it.")
    if (newSpsLen != oldSpsLen)
    {
        const int sizeDiff = newSpsLen - oldSpsLen;
        if (m_bufEnd + sizeDiff > m_tmpBuffer + m_tmpBufferSize)
            THROW(ERR_COMMON, "Track " << m_streamIndex
                                       << ": its H.264 sequence parameter set could not be written again with the "
                                          "frame rate, as it does not hold what an H.264 stream holds there. "
                                       << "The usual reason is that the codec on the meta line is not the codec the "
                                          "file holds. List the file on its own to see what tsMuxeR makes of it.")
        memmove(nextNal + sizeDiff, nextNal, m_bufEnd - nextNal);
        m_bufEnd += sizeDiff;
        // m_dataLen += sizeDiff;
    }
    memcpy(buff, tmpBuffer.get(), newSpsLen);
}

void H264StreamReader::updateHRDParam(SPSUnit* sps) const
{
    // correct HRD parameters here (this function is called for every SPS nal unit)
    if (!m_needSeiCorrection)
        return;

    // if (!orig_hrd_parameters_present_flag && !orig_vcl_parameters_present_flag)
    //{
    //}
    if (!sps->vui_parameters_present_flag)
        sps->setFps(m_fps);
    sps->insertHrdParameters();
}

// All the remaining checks of the stream, executed at start

// The fields a picture is shown for, by its pic_struct: a frame 2, three fields (pulldown) 3, frame doubling 4,
// frame tripling 6
static int fieldsShownOf(const int picStruct)
{
    switch (picStruct)
    {
    case 5:
    case 6:
        return 3;
    case 7:
        return 4;
    case 8:
        return 6;
    default:
        return 2;
    }
}

// The pictures after this one in decode order, up to 32 and until the next IDR, as (order relative to this picture,
// fields shown). A picture's pic_struct is in the SEI that comes before its first slice.
void H264StreamReader::pulldownFieldsAhead(const SliceUnit& slice, uint8_t* buff)
{
    m_fieldsAhead.clear();
    if (m_spsMap.empty())
        return;
    const int maxLsb = 1 << slice.getSPS()->log2_max_pic_order_cnt_lsb;
    int prevLsb = static_cast<int>(slice.pic_order_cnt_lsb);
    int order = 0;            // the order count from this picture's, in steps of the picture before
    Poc1State poc1 = m_poc1;  // the counts of type 1 go on from this picture's
    int picStruct = -1;
    for (uint8_t* nal = NALUnit::findNextNAL(buff + 4, m_bufEnd); nal < m_bufEnd && m_fieldsAhead.size() < 32;
         nal = NALUnit::findNextNAL(nal, m_bufEnd))
    {
        const auto type = static_cast<NALUnit::NALType>(*nal & 0x1f);
        if (type == NALUnit::NALType::nuSEI)
        {
            uint8_t* next = NALUnit::findNALWithStartCode(nal, m_bufEnd, true);
            SEIUnit sei;
            sei.pic_struct = -1;
            sei.decodeBuffer(nal, next);
            sei.deserialize(*(m_spsMap.begin()->second),
                            orig_hrd_parameters_present_flag || orig_vcl_parameters_present_flag);
            if (sei.hasProcessedMessage(SEI_MSG_PIC_TIMING))
                picStruct = sei.pic_struct;
            continue;
        }
        if (type == NALUnit::NALType::nuSliceIDR)
            return;
        if (type != NALUnit::NALType::nuSliceNonIDR)
            continue;
        SliceUnit next;
        if (deserializeSliceHeader(next, nal, m_bufEnd, &poc1) != 0)
            return;
        if (next.first_mb_in_slice != 0)
            continue;
        // Each step from the picture before, as calcPicOrder counts: taken from this picture alone, a picture more
        // than half the count's range ahead was read as one shown before it, so an IDR seemed to have pictures shown
        // before it, and a 3:2 stream began 4 frames after its start time.
        int diff = static_cast<int>(next.pic_order_cnt_lsb) - prevLsb;
        if (diff >= maxLsb / 2)
            diff -= maxLsb;
        else if (diff < -maxLsb / 2)
            diff += maxLsb;
        prevLsb = static_cast<int>(next.pic_order_cnt_lsb);
        order += diff;
        m_fieldsAhead.emplace_back(order >> m_forceLsbDiv, fieldsShownOf(picStruct));
        picStruct = -1;
    }
}

void H264StreamReader::checkPyramid(const int frameNum, int* fullPicOrder, const bool nextFrameFound)
{
    // How far a picture is shown before it would be decoded, which is how far the decoding times are moved back. It
    // was held at 3 frames, and B pictures in a deeper pyramid were shown before they were decoded: with 16 B pictures
    // between two references, a picture needs 5. 16 is the most pictures an H.264 decoder holds, as for HEVC. Coded
    // fields stay at 3: each field counts as a picture against an order count per frame, so the difference grows with
    // the GOP and is not a depth (a 1080i disc stream gave 16 and moved every decoding time back 16 frames).
    int depth = frameNum - *fullPicOrder;
    const int maxDepth = m_fieldPicturesSeen ? 3 : 16;
    if (depth > maxDepth)
        depth = maxDepth;
    if (depth > m_frameDepth)
    {
        if (nextFrameFound)
        {
            m_frameDepth = depth;
            LTRACE(LT_INFO, 2,
                   "B-pyramid level " << m_frameDepth - 1 << " detected. Shift DTS to " << m_frameDepth << " frames");
        }
        else
        {
            *fullPicOrder = frameNum - m_frameDepth;  // special case for last frame
        }
    }
}

void H264StreamReader::additionalStreamCheck(uint8_t* buff, uint8_t* end)
{
    // Make sure that the stream doesn't appear to be progressive, since pic_order_cnt_lsb doesn't reach values higher
    // than 2 in these streams.
    bool SEIFound = false;
    SliceUnit slice;
    SPSUnit tmpsps;

    PPSUnit pps1;
    bool differPPS = false;

    int rez = 0;
    SEIUnit sei;
    uint8_t* nalEnd = nullptr;
    bool tmpspsfound = false;
    for (uint8_t* nal = NALUnit::findNextNAL(buff, end); nal != end; nal = NALUnit::findNextNAL(nal, end))
    {
        switch (static_cast<NALUnit::NALType>(*nal & 0x1f))
        {
        case NALUnit::NALType::nuSubSPS:
            m_mvcSubStream = true;
            [[fallthrough]];
        case NALUnit::NALType::nuSPS:
            nalEnd = NALUnit::findNALWithStartCode(nal, m_bufEnd, true);
            tmpsps.decodeBuffer(nal, nalEnd);
            tmpsps.deserialize();
            orig_hrd_parameters_present_flag = tmpsps.nalHrdParams.isPresent;
            orig_vcl_parameters_present_flag = tmpsps.vclHrdParams.isPresent;
            m_spsFlagsKnown = true;
            tmpspsfound = true;
            break;
        case NALUnit::NALType::nuPPS:
        {
            PPSUnit pps2;
            nalEnd = NALUnit::findNALWithStartCode(nal, m_bufEnd, true);
            if (pps1.m_nalBufferLen == 0)
            {
                pps1.decodeBuffer(nal, nalEnd);
                pps1.deserialize();
            }
            pps2.decodeBuffer(nal, nalEnd);
            pps2.deserialize();
            if (pps1.pic_parameter_set_id == pps2.pic_parameter_set_id)
            {
                if (pps2.m_nalBufferLen != pps1.m_nalBufferLen ||
                    memcmp(pps2.m_nalBuffer, pps1.m_nalBuffer, pps1.m_nalBufferLen) != 0)
                    differPPS = true;
            }
            break;
        }
        case NALUnit::NALType::nuSEI:
            if (tmpspsfound)
            {
                nalEnd = NALUnit::findNALWithStartCode(nal, end, true);
                if (nalEnd == end)
                    break;
                sei.decodeBuffer(nal, nalEnd);
                sei.deserialize(tmpsps, orig_hrd_parameters_present_flag || orig_vcl_parameters_present_flag);
                if (sei.hasProcessedMessage(SEI_MSG_BUFFERING_PERIOD) || sei.hasProcessedMessage(SEI_MSG_PIC_TIMING))
                {
                    SEIFound = true;
                }
                if (sei.hasProcessedMessage(SEI_MSG_PIC_TIMING) && !m_removePulldown)
                    m_pulldownMaxFields = (std::max)(m_pulldownMaxFields, fieldsShownOf(sei.pic_struct));

                if (sei.number_of_offset_sequences >= 0)
                {
                    number_of_offset_sequences = sei.number_of_offset_sequences;
                }
            }
            break;
        default:
            break;
        }
    }

    if (m_removePulldown || m_insertSEIMethod == SeiMethod::SEI_InsertForce)
        m_needSeiCorrection = true;
    else if (m_insertSEIMethod == SeiMethod::SEI_InsertAuto)
    {
        m_needSeiCorrection =
            orig_hrd_parameters_present_flag == 0 || orig_vcl_parameters_present_flag == 0 || !SEIFound;
        if (m_needSeiCorrection && differPPS)
        {
            LTRACE(LT_INFO, 2, "H264 stream contains different PPS with same pps_id. SEI correction is turned off");
            m_needSeiCorrection = false;
        }
    }
    else
        m_needSeiCorrection = false;

    Poc1State poc1;  // the counts of type 1 from the start of the stream
    m_forceLsbDiv = -1;
    // The coded frames of a stream that can code fields: whether odd and even counts were seen
    bool oddFrame = false, evenFrame = false;
    for (uint8_t* nal = NALUnit::findNextNAL(buff, end); nal != end; nal = NALUnit::findNextNAL(nal, end))
    {
        switch (static_cast<NALUnit::NALType>(*nal & 0x1f))
        {
        case NALUnit::NALType::nuSPS:
        case NALUnit::NALType::nuSubSPS:
            processSPS(nal);
            break;
        case NALUnit::NALType::nuPPS:
            processPPS(nal);
            break;
        case NALUnit::NALType::nuSliceIDR:
        case NALUnit::NALType::nuSliceNonIDR:
        case NALUnit::NALType::nuSliceA:
        case NALUnit::NALType::nuSliceB:
        case NALUnit::NALType::nuSliceC:
        case NALUnit::NALType::nuSliceExt:
            rez = deserializeSliceHeader(slice, nal, end, &poc1);
            if (rez == 0)
            {
                // coded fields
                if (slice.getSPS()->frame_mbs_only_flag == 0 && slice.m_field_pic_flag)
                    m_forceLsbDiv = 1;
                // Coded frames of a stream that can code fields. A frame has a count for each field, and the one in
                // the slice header is the top field's: with the bottom field first it is often the odd one of the
                // two, and the counts of the frames step by two from odd numbers. Taken as steps of one, the pictures
                // came out twice as far apart as they are, and the stream twice as long. Steps of one give odd and
                // even counts both.
                else if (slice.getSPS()->frame_mbs_only_flag == 0)
                {
                    (slice.pic_order_cnt_lsb % 2 == 1 ? oddFrame : evenFrame) = true;
                    if (oddFrame && evenFrame)
                        m_forceLsbDiv = 0;
                }
                else if (slice.pic_order_cnt_lsb % 2 == 1)
                    m_forceLsbDiv = 0;
            }
            break;
        default:
            break;
        }
    }

    if (m_forceLsbDiv == -1)
        m_forceLsbDiv = 1;

    int frameNum = -1;
    // As in the mux: before the first IDR, a stream that begins with another picture counts from it, and that picture
    // comes after the pictures shown before it. Without this the depth found here was too small for such a stream,
    // grew in the mux after the first pictures were written, and two decoding times came out the same.
    int orderOffset = 0;
    bool firstPicture = true;
    // ** EACH PICTURE IS READ WITH THE PARAMETER SETS IN FORCE AT IT. ** The block can hold several SPS or PPS of one
    // id, as a stream of parts coded otherwise does, and processSPS above left the last of each in m_spsMap, with which
    // every picture of the block was read here. A stream of High, Main and High parts (all SPS id 0, the Main part
    // with pic_order_cnt_type 2 and P pictures only) had its Main pictures read as High ones: their order counts came
    // out from bits that are none, never rose, and the depth grew by one at each of them up to 16, where it needs 2.
    // Every decoding time went back 16 frames (0.64 s at 25 fps, more than --vbv-len=500), the first one
    // before the first clock reference, and 197 of its 200 pictures came after their decoding time; 2.20.0 held the
    // depth at 3, which hid it. The SPS and PPS met in the block are now read as they come, without the changes
    // processSPS makes, and a picture takes the ones met last before it; one before the first of its id in the block
    // takes the one in m_spsMap, as before.
    std::map<uint32_t, SPSUnit*> spsInForce;
    std::map<uint32_t, PPSUnit*> ppsInForce;
    std::vector<std::unique_ptr<NALUnit>> setsMet;
    const auto restartSets = [&]()
    {
        spsInForce = m_spsMap;
        ppsInForce = m_ppsMap;
    };
    const auto meetSet = [&](uint8_t* nal, const NALUnit::NALType type)
    {
        uint8_t* nalEnd = NALUnit::findNALWithStartCode(nal, end, true);
        if (type == NALUnit::NALType::nuSPS || type == NALUnit::NALType::nuSubSPS)
        {
            auto sps = std::make_unique<SPSUnit>();
            sps->decodeBuffer(nal, nalEnd);
            if (sps->deserialize() == 0)
            {
                spsInForce[sps->seq_parameter_set_id] = sps.get();
                setsMet.push_back(std::move(sps));
            }
        }
        else if (type == NALUnit::NALType::nuPPS)
        {
            auto pps = std::make_unique<PPSUnit>();
            pps->decodeBuffer(nal, nalEnd);
            if (pps->deserialize() == 0)
            {
                ppsInForce[pps->pic_parameter_set_id] = pps.get();
                setsMet.push_back(std::move(pps));
            }
        }
    };
    // Whether the stream has coded fields at all, known before the first picture: a stream can begin with a frame
    // (MBAFF) and code fields later, and the rules for a first picture without an IDR are for frames only.
    restartSets();
    for (uint8_t* nal = NALUnit::findNextNAL(buff, end); nal != end && !m_fieldPicturesSeen;
         nal = NALUnit::findNextNAL(nal, end))
    {
        const auto type = static_cast<NALUnit::NALType>(*nal & 0x1f);
        meetSet(nal, type);
        if ((type == NALUnit::NALType::nuSliceIDR || type == NALUnit::NALType::nuSliceNonIDR) &&
            deserializeSliceHeader(slice, nal, end, nullptr, &spsInForce, &ppsInForce) == 0 && slice.m_field_pic_flag)
            m_fieldPicturesSeen = true;
    }
    poc1 = Poc1State();  // the counts of type 1 from the start of the stream again
    restartSets();
    for (uint8_t* nal = NALUnit::findNextNAL(buff, end); nal != end; nal = NALUnit::findNextNAL(nal, end))
    {
        const auto type = static_cast<NALUnit::NALType>(*nal & 0x1f);
        meetSet(nal, type);
        switch (type)
        {
        case NALUnit::NALType::nuSliceIDR:
        case NALUnit::NALType::nuSliceNonIDR:
        case NALUnit::NALType::nuSliceA:
        case NALUnit::NALType::nuSliceB:
        case NALUnit::NALType::nuSliceC:
        case NALUnit::NALType::nuSliceExt:
            rez = deserializeSliceHeader(slice, nal, end, &poc1, &spsInForce, &ppsInForce);
            if (rez == 0 && slice.first_mb_in_slice == 0 && slice.getSPS()->pic_order_cnt_type != 2)
            {
                int picOrder = calcPicOrder(slice);
                if (slice.isIDR())
                {
                    frameNum = -1;
                    orderOffset = 0;
                }
                else if (firstPicture && !m_fieldPicturesSeen)
                {
                    const int leading = leadingPicturesOfFirst(slice, nal);
                    if (leading >= 0)
                        orderOffset = leading - picOrder;
                }
                firstPicture = false;
                picOrder += orderOffset;
                if (slice.isIFrame())
                    m_fieldFirst = slice.bottom_field_flag;
                if (slice.m_field_pic_flag)
                    m_fieldPicturesSeen = true;
                checkPyramid(++frameNum, &picOrder, true);
            }
            break;
        default:
            break;
        }
    }
    prevPicOrderCntMsb = prevPicOrderCntLsb = 0;
}

int H264StreamReader::calcPicOrder(const SliceUnit& slice)
{
    if (slice.getSPS()->pic_order_cnt_type == 2)
        THROW(ERR_H264_UNSUPPORTED_PARAMETER,
              "SPS picture order " << slice.getSPS()->pic_order_cnt_type << " not supported.")

    if (slice.isIDR())
        prevPicOrderCntMsb = prevPicOrderCntLsb = 0;

    const int PicOrderCntLsb = slice.pic_order_cnt_lsb;
    const int MaxPicOrderCntLsb = 1 << slice.getSPS()->log2_max_pic_order_cnt_lsb;
    int PicOrderCntMsb;

    if ((PicOrderCntLsb < prevPicOrderCntLsb) && ((prevPicOrderCntLsb - PicOrderCntLsb) >= (MaxPicOrderCntLsb / 2)))
        PicOrderCntMsb = prevPicOrderCntMsb + MaxPicOrderCntLsb;
    else if ((PicOrderCntLsb > prevPicOrderCntLsb) && ((PicOrderCntLsb - prevPicOrderCntLsb) > (MaxPicOrderCntLsb / 2)))
        PicOrderCntMsb = prevPicOrderCntMsb - MaxPicOrderCntLsb;
    else
        PicOrderCntMsb = prevPicOrderCntMsb;

    if (slice.memory_management_control_operation == 5)
    {
        prevPicOrderCntMsb = 0;
        if (!slice.bottom_field_flag)
            prevPicOrderCntLsb = PicOrderCntMsb + PicOrderCntLsb;
        else
            prevPicOrderCntLsb = 0;
    }
    else
    {
        prevPicOrderCntMsb = PicOrderCntMsb;
        prevPicOrderCntLsb = PicOrderCntLsb;
    }
    return (PicOrderCntMsb + PicOrderCntLsb) >> m_forceLsbDiv;
}

int H264StreamReader::nextPicOrder(const SliceUnit& slice) const
{
    const int lsb = static_cast<int>(slice.pic_order_cnt_lsb);
    const int maxLsb = 1 << slice.getSPS()->log2_max_pic_order_cnt_lsb;
    int msb = prevPicOrderCntMsb;
    if (lsb < prevPicOrderCntLsb && prevPicOrderCntLsb - lsb >= maxLsb / 2)
        msb += maxLsb;
    else if (lsb > prevPicOrderCntLsb && lsb - prevPicOrderCntLsb > maxLsb / 2)
        msb -= maxLsb;
    int order = ((msb + lsb) >> m_forceLsbDiv) + m_iFramePtsOffset;
    if (m_idrSliceCnt == 0 && m_bSliceFound)
        order += m_picOrderOffset;
    return order;
}

int H264StreamReader::getIdrPrevFrames(uint8_t* buff, const uint8_t* bufEnd)
{
    int prevPicCnt = 0;
    int deserializeRez;
    // An IDR coded as two fields: its second field is no IDR, has the IDR's frame_num, 0, and follows it. Its count
    // is the IDR's, not one shown before it, and it ended the count at once: the pictures after the IDR that are shown
    // before it got the times of the pictures before the IDR, and the IDR and every picture after it came 2 frames
    // early.
    Poc1State poc1 = m_poc1;  // the counts of type 1 go on from the IDR's
    bool firstPicture = true;
    for (uint8_t* nal = NALUnit::findNextNAL(buff + 4, m_bufEnd); nal < m_bufEnd;
         nal = NALUnit::findNextNAL(nal, m_bufEnd))
    {
        SliceUnit slice;

        int MaxPicOrderCntLsbHalf;
        switch (static_cast<NALUnit::NALType>(*nal & 0x1f))
        {
        case NALUnit::NALType::nuSliceIDR:
        case NALUnit::NALType::nuSliceNonIDR:
        case NALUnit::NALType::nuSliceA:
        case NALUnit::NALType::nuSliceB:
        case NALUnit::NALType::nuSliceC:
        case NALUnit::NALType::nuSliceExt:
            deserializeRez = deserializeSliceHeader(slice, nal, bufEnd, &poc1);
            if (deserializeRez == UNIT_DAMAGED)
                break;  // a damaged picture header says nothing about the pictures around it
            if (deserializeRez == NOT_ENOUGH_BUFFER && !m_eof)
                return -1;
            if (deserializeRez != 0)
                return 0;
            if (slice.isIDR() && slice.first_mb_in_slice == 0)
                return 0;
            if (slice.first_mb_in_slice != 0)
                break;
            if (firstPicture)
            {
                firstPicture = false;
                if (slice.m_field_pic_flag && slice.frame_num == 0)
                    break;  // the IDR's second field
            }
            // A picture whose parameter set has no order count of its own (pic_order_cnt_type 2) is shown in the
            // order it is decoded, so it is not shown before the IDR. Its log2_max_pic_order_cnt_lsb is 0, and the
            // shift below was by -1: undefined, measured on a damaged transport stream named as H.264.
            if (slice.getSPS()->log2_max_pic_order_cnt_lsb == 0)
                return prevPicCnt;
            MaxPicOrderCntLsbHalf = 1 << (slice.getSPS()->log2_max_pic_order_cnt_lsb - 1);
            if (slice.pic_order_cnt_lsb > MaxPicOrderCntLsbHalf)
            {
                if (slice.bottom_field_flag == 0)
                    prevPicCnt++;
            }
            else
                return prevPicCnt;
            break;
        default:
            break;
        }
    }
    if (m_eof)
        return prevPicCnt;
    return -1;
}

// The pictures after the first picture of a stream that does not begin with an IDR (a cut) that are shown before
// it: they follow it in decode order with a lower order count. -2 when a coded field follows: not for such a stream.
// Counted up to the first picture shown after it, as getIdrPrevFrames does after an IDR. Returns -1 when more data is
// needed and can come.
int H264StreamReader::leadingPicturesOfFirst(const SliceUnit& firstSlice, uint8_t* buff)
{
    const int maxLsb = 1 << firstSlice.getSPS()->log2_max_pic_order_cnt_lsb;
    Poc1State poc1 = m_poc1;  // the counts of type 1 go on from this picture's
    int leading = 0;
    for (uint8_t* nal = NALUnit::findNextNAL(buff + 4, m_bufEnd); nal < m_bufEnd;
         nal = NALUnit::findNextNAL(nal, m_bufEnd))
    {
        const auto type = static_cast<NALUnit::NALType>(*nal & 0x1f);
        if (type == NALUnit::NALType::nuSliceIDR)
            return leading;
        if (type != NALUnit::NALType::nuSliceNonIDR && type != NALUnit::NALType::nuSliceA)
            continue;
        SliceUnit slice;
        const int rez = deserializeSliceHeader(slice, nal, m_bufEnd, &poc1);
        if (rez == UNIT_DAMAGED)
            continue;
        if (rez == NOT_ENOUGH_BUFFER && !m_eof)
            return -1;
        if (rez != 0)
            return leading;
        if (slice.first_mb_in_slice != 0)
            continue;
        if (slice.m_field_pic_flag)
            return -2;  // coded fields: the two fields of a frame share one time, the rule is for frames only
        int diff = static_cast<int>(slice.pic_order_cnt_lsb) - static_cast<int>(firstSlice.pic_order_cnt_lsb);
        if (diff >= maxLsb / 2)
            diff -= maxLsb;
        else if (diff < -maxLsb / 2)
            diff += maxLsb;
        if (diff >= 0)
            return leading;
        if (slice.bottom_field_flag == 0)
            leading++;
    }
    return m_eof ? leading : -1;
}

// The depth checkPyramid will find in the IDR period that begins at this IDR, from the frame pictures after it: up to
// 33 of them, the next IDR, a coded field or a reset of the order count, whichever comes first. Returns -1 when more
// data is needed and can come; it is read no further than 32 MiB past the IDR.
int H264StreamReader::depthOfIdrPeriod(const SliceUnit& idrSlice, uint8_t* buff)
{
    static constexpr int MAX_PICTURES = 33;
    static constexpr std::ptrdiff_t MAX_AHEAD = 32 * 1024 * 1024;
    const int maxLsb = 1 << idrSlice.getSPS()->log2_max_pic_order_cnt_lsb;
    int prevLsb = static_cast<int>(idrSlice.pic_order_cnt_lsb);
    int order = prevLsb;      // the order count, as calcPicOrder gives it from the IDR on
    Poc1State poc1 = m_poc1;  // the counts of type 1 go on from the IDR's
    int pictures = 0;
    int depth = 0;
    for (uint8_t* nal = NALUnit::findNextNAL(buff + 4, m_bufEnd); nal < m_bufEnd;
         nal = NALUnit::findNextNAL(nal, m_bufEnd))
    {
        const auto type = static_cast<NALUnit::NALType>(*nal & 0x1f);
        if (type == NALUnit::NALType::nuSliceIDR)
            return depth;
        if (type != NALUnit::NALType::nuSliceNonIDR && type != NALUnit::NALType::nuSliceA)
            continue;
        SliceUnit slice;
        const int rez = deserializeSliceHeader(slice, nal, m_bufEnd, &poc1);
        if (rez == NOT_ENOUGH_BUFFER && !m_eof && nal - buff < MAX_AHEAD)
            return -1;
        if (rez != 0)
            return depth;
        if (slice.first_mb_in_slice != 0)
            continue;
        if (slice.m_field_pic_flag || slice.memory_management_control_operation == 5)
            return depth;
        const int lsb = static_cast<int>(slice.pic_order_cnt_lsb);
        if (lsb < prevLsb && prevLsb - lsb >= maxLsb / 2)
            order += lsb + maxLsb - prevLsb;
        else if (lsb > prevLsb && lsb - prevLsb > maxLsb / 2)
            order += lsb - maxLsb - prevLsb;
        else
            order += lsb - prevLsb;
        prevLsb = lsb;
        depth = (std::max)(depth, ++pictures - (order >> m_forceLsbDiv));
        if (pictures >= MAX_PICTURES)
            return depth;
    }
    return m_eof || m_bufEnd - buff >= MAX_AHEAD ? depth : -1;
}

// The picture order count step of the IDR period that begins with idrSlice (its NAL at buff), read from
// that period's own pictures by the rule additionalStreamCheck applies at the start of a track: 1 (halve
// the counts) for coded fields and for frames whose counts are all even, 0 when a coded frame has an odd
// count; in a stream that can code fields, only when its coded frames have odd and even counts both.
// Returns the step in use when the data cannot tell, and -1 when more data is needed and can come.
int H264StreamReader::lsbDivOfIdrPeriod(const SliceUnit& idrSlice, uint8_t* buff)
{
    // With steps of one, the picture with count 1 is shown right after the IDR, and a decoder holds back
    // at most 16 frames to reorder them, so that picture is among the next 17.
    static constexpr int PICTURES_TO_BE_SURE = 17;
    // More data is asked for only while it fits: what is kept from this picture on, plus the next block
    // of input, has to stay inside TMP_BUFFER_SIZE, or setBuffer throws.
    static constexpr int MAX_LOOKAHEAD = TMP_BUFFER_SIZE / 2;

    if (idrSlice.getSPS()->frame_mbs_only_flag == 0 && idrSlice.m_field_pic_flag)
        return 1;  // coded fields (and the second field of this IDR would look like the next IDR below)

    Poc1State poc1 = m_poc1;  // the counts of type 1 go on from the IDR's
    int pictures = 0;
    bool oddFrame = false, evenFrame = false;
    if (idrSlice.getSPS()->frame_mbs_only_flag == 0)
        (idrSlice.pic_order_cnt_lsb % 2 == 1 ? oddFrame : evenFrame) = true;
    for (uint8_t* nal = NALUnit::findNextNAL(buff, m_bufEnd); nal < m_bufEnd; nal = NALUnit::findNextNAL(nal, m_bufEnd))
    {
        const auto nalType = static_cast<NALUnit::NALType>(*nal & 0x1f);
        if (nalType != NALUnit::NALType::nuSliceIDR && nalType != NALUnit::NALType::nuSliceNonIDR &&
            nalType != NALUnit::NALType::nuSliceA && nalType != NALUnit::NALType::nuSliceB &&
            nalType != NALUnit::NALType::nuSliceC && nalType != NALUnit::NALType::nuSliceExt)
            continue;
        SliceUnit slice;
        int rez = slice.deserializeSliceType(nal, m_bufEnd);
        if (rez == 0)
        {
            if (slice.first_mb_in_slice != 0)
                continue;  // a further slice of a picture already looked at
            if (slice.isIDR())
                return pictures > 0 ? 1 : m_forceLsbDiv;  // the next period begins: every count was even
            rez = deserializeSliceHeader(slice, nal, m_bufEnd, &poc1);
        }
        if (rez == UNIT_DAMAGED)
            continue;  // a damaged picture header says nothing about the step
        if (rez == NOT_ENOUGH_BUFFER)
            break;  // the data buffered so far ends inside this slice header
        if (rez != 0)
            return m_forceLsbDiv;
        if (slice.getSPS()->frame_mbs_only_flag == 0 && slice.m_field_pic_flag)
            return 1;
        if (slice.getSPS()->frame_mbs_only_flag == 0)
        {
            (slice.pic_order_cnt_lsb % 2 == 1 ? oddFrame : evenFrame) = true;
            if (oddFrame && evenFrame)
                return 0;
        }
        else if (slice.pic_order_cnt_lsb % 2 == 1)
            return 0;
        if (++pictures >= PICTURES_TO_BE_SURE)
            return 1;
    }
    if (m_eof)
        return pictures > 0 ? 1 : m_forceLsbDiv;
    return m_bufEnd - buff < MAX_LOOKAHEAD ? -1 : m_forceLsbDiv;
}

int H264StreamReader::intDecodeNAL(uint8_t* buff)
{
    const auto nal_unit_type = static_cast<NALUnit::NALType>(*buff & 0x1f);
    int nalRez;
    m_spsPpsFound = false;

    // Whether picture order counts step by one or by two is a property of the stream, not of the
    // container being written, and until now it was decided only inside getTSDescriptor. That exists
    // to write a transport stream descriptor, so for a Matroska output the check never ran,
    // m_forceLsbDiv stayed at the 0 it is constructed with, and the counts were used unhalved. The
    // presentation times then advanced two frames per picture while the frame numbers advanced one.
    // Measured on one source: 222 presentation times landed on an already used one and 323 were left
    // empty, the timeline came out 100 frames too long, and one pair of pictures ended up sharing a
    // time and was written as a single block.
    // The check runs before the first unit is read, as it does for a transport stream, and not at the
    // first slice: it also turns on the SEI correction (insertSEI, forceSEI), and the SEI units of the
    // first picture were read with it still off. One holding the timing message together with closed
    // captions, as broadcast streams carry them, was then not cut down to the captions but dropped
    // whole, and a Matroska file lost the first picture's caption.
    if (m_firstDecodeNal)
    {
        additionalStreamCheck(m_buffer, m_bufEnd);
        m_firstDecodeNal = false;
    }

    // First NAL of Access Unit
    switch (nal_unit_type)
    {
    case NALUnit::NALType::nuPPS:
        // LTRACE(LT_DEBUG, 0, "PPS");
        nalRez = processPPS(buff);
        if (nalRez != 0)
            return nalRez;
        m_spsPpsFound = true;
        break;
    case NALUnit::NALType::nuSPS:
    case NALUnit::NALType::nuSubSPS:
        // LTRACE(LT_DEBUG, 0, "SPS");
        nalRez = processSPS(buff);
        if (nalRez != 0)
            return nalRez;
        countSps(buff);
        break;
    case NALUnit::NALType::nuSEI:
        // LTRACE(LT_DEBUG, 0, "SEI");
        nalRez = processSEI(buff);
        if (nalRez != 0)
            return nalRez;
        break;
    case NALUnit::NALType::nuDRD:
    case NALUnit::NALType::nuDelimiter:
        // LTRACE(LT_DEBUG, 0, "NAUD type " << int32ToStr(buff[1],16));
        m_delimiterFound = true;
        break;
    // Remaining NALs of Access Unit
    case NALUnit::NALType::nuSliceIDR:
    case NALUnit::NALType::nuSliceNonIDR:
    case NALUnit::NALType::nuSliceA:
    case NALUnit::NALType::nuSliceB:
    case NALUnit::NALType::nuSliceC:
    case NALUnit::NALType::nuSliceExt:
        nalRez = processSliceNal(buff);
        return nalRez != 0 ? nalRez : 0;
    default:
        return 0;
    }

    uint8_t* nextNal = NALUnit::findNextNAL(buff, m_bufEnd);
    while (true)
    {
        if (nextNal == m_bufEnd)
            return NOT_ENOUGH_BUFFER;
        switch (static_cast<NALUnit::NALType>(*nextNal & 0x1f))
        {
        case NALUnit::NALType::nuSEI:
            nalRez = processSEI(nextNal);
            if (nalRez != 0)
                return nalRez;

            break;
        case NALUnit::NALType::nuDRD:
            if (m_blurayMode)
                m_delimiterFound = true;
            break;
        case NALUnit::NALType::nuDelimiter:
            // LTRACE(LT_DEBUG, 0, "NAUD type " << int32ToStr(nextNal[1],16));
            m_delimiterFound = true;
            break;
        case NALUnit::NALType::nuSPS:
        case NALUnit::NALType::nuSubSPS:
            // LTRACE(LT_DEBUG, 0, "SPS");
            nalRez = processSPS(nextNal);
            if (nalRez != 0)
                return nalRez;
            countSps(nextNal);
            break;
        case NALUnit::NALType::nuPPS:
            // LTRACE(LT_DEBUG, 0, "PPS");
            nalRez = processPPS(nextNal);
            if (nalRez != 0)
                return nalRez;
            m_spsPpsFound = true;
            break;
        case NALUnit::NALType::nuSliceIDR:
            // m_openGOP = false;
        case NALUnit::NALType::nuSliceNonIDR:
        case NALUnit::NALType::nuSliceA:
        case NALUnit::NALType::nuSliceB:
        case NALUnit::NALType::nuSliceC:
        case NALUnit::NALType::nuSliceExt:
            nalRez = processSliceNal(nextNal, true);
            if (nalRez == 0)
            {
                m_lastDecodedPos = nextNal;
            }
            return nalRez;
        default:
            break;
        }
        nextNal = NALUnit::findNextNAL(nextNal, m_bufEnd);
    }
}

void H264StreamReader::countSps(const uint8_t* nal)
{
    // where the unit stands in the stream: m_processedBytes counts the bytes before m_curPos
    const int64_t at = m_processedBytes + (nal - m_curPos);
    if (at > m_spsCountedTo)
    {
        m_spsCounter++;
        m_spsCountedTo = at;
    }
}

bool H264StreamReader::skipNal(uint8_t* nal)
{
    const auto nalType = static_cast<NALUnit::NALType>(*nal & 0x1f);

    if (nalType == NALUnit::NALType::nuFillerData)
        return true;

    if (nalType == NALUnit::NALType::nuEOSeq || nalType == NALUnit::NALType::nuEOStream)
    {
        if (!m_eof || m_bufEnd - nal > 4)
        {
            updatedSPSList.clear();
            return true;
        }
    }

    if (!m_needSeiCorrection)
        return false;

    // A PPS before the second field of a frame stays where it is: nothing is written in front of that field, and a
    // stream may send a changed PPS there (the second field of an IDR frame), which its slices are read with.
    if (nalType == NALUnit::NALType::nuPPS && m_secondField)
        return false;
    if (nalType == NALUnit::NALType::nuPPS || nalType == NALUnit::NALType::nuSPS ||
        nalType == NALUnit::NALType::nuSubSPS)
    {
        return true;  // SPS/PPS will be composed from our side on writePesHeader. It is required to compose SPS/PPS
                      // before SEI
    }
    // Guarded as processSEI is: the first SPS is read before any SEI in every stream measured, because the stream
    // check fills the map before the mux starts, but nothing promises it.
    if (nalType == NALUnit::NALType::nuSEI && !m_spsMap.empty())
    {
        SEIUnit sei;
        const uint8_t* nextNal = NALUnit::findNALWithStartCode(nal, m_bufEnd, true);
        sei.decodeBuffer(nal, nextNal);
        sei.deserialize(*(m_spsMap.begin()->second),
                        orig_hrd_parameters_present_flag || orig_vcl_parameters_present_flag);

        if (sei.hasProcessedMessage(SEI_MSG_BUFFERING_PERIOD) || sei.hasProcessedMessage(SEI_MSG_PIC_TIMING))
        {
            return true;
        }
    }
    return false;
}

int H264StreamReader::processSEI(uint8_t* buff)
{
    if (!m_spsMap.empty())
    {
        SEIUnit lastSEI;
        uint8_t* nextNal = NALUnit::findNALWithStartCode(buff, m_bufEnd, true);
        if (nextNal == m_bufEnd)
            return NOT_ENOUGH_BUFFER;
        if (nextNal + 4 > m_bufEnd)
            return NOT_ENOUGH_BUFFER;
        lastSEI.pic_struct = -1;
        lastSEI.decodeBuffer(buff, nextNal);
        lastSEI.deserialize(*(m_spsMap.begin()->second),
                            orig_hrd_parameters_present_flag || orig_vcl_parameters_present_flag);

        bool timingSEI = false;
        bool nonTimingSEI = false;
        for (auto& msg : lastSEI.m_processedMessages)
        {
            if (msg == SEI_MSG_BUFFERING_PERIOD)
            {
                timingSEI = true;
            }
            else if (msg == SEI_MSG_PIC_TIMING)
            {
                timingSEI = true;
                m_lastPictStruct = lastSEI.pic_struct;
                // Pulldown that begins after the first block read: additionalStreamCheck sees only that block, and
                // such a stream kept its times in whole frames (180 times for 234 pictures).
                if (!m_removePulldown)
                    m_pulldownPendingFields = (std::max)(m_pulldownPendingFields, fieldsShownOf(m_lastPictStruct));
            }
            else if (msg == SEI_MSG_MVC_SCALABLE_NESTING)
            {
            }
            else
            {
                nonTimingSEI = true;
            }
        }

        if (m_mvcSubStream)
        {
            if (lastSEI.number_of_offset_sequences >= 0)
            {
                if (m_needSeiCorrection)
                {
                    // copy to buffer, then isert sei
                    m_bdRomMetaDataMsg.resize(lastSEI.m_nalBufferLen);
                    m_bdRomMetaDataMsgPtsPos = lastSEI.metadataPtsOffset;
                    memcpy(m_bdRomMetaDataMsg.data(), lastSEI.m_nalBuffer, lastSEI.m_nalBufferLen);
                }
                else
                {
                    // update inplace
                    m_priorityNalAddr = buff;
                    m_OffsetMetadataPtsAddr = lastSEI.metadataPtsOffset + buff;
                }
            }

            if (timingSEI && lastSEI.m_mvcHeaderLen > 0)
            {
                m_lastSeiMvcHeader.resize(lastSEI.m_mvcHeaderLen);
                memcpy(m_lastSeiMvcHeader.data(), lastSEI.m_mvcHeaderStart, lastSEI.m_mvcHeaderLen);
            }
        }

        if (timingSEI && !nonTimingSEI)

            return 0;

        if (timingSEI && nonTimingSEI && m_needSeiCorrection)
        {
            // remove timing part from muxed SEI
            int oldSize = static_cast<int>(nextNal - buff);
            lastSEI.removePicTimingSEI(*(m_spsMap.begin()->second));

            // Sized from what is left, with room for every emulation prevention byte the packing can add (at most
            // one for every two bytes). A fixed 3 KB ended the job with "Not enough buffer" for a larger SEI unit.
            std::vector<uint8_t> tmpBuff(lastSEI.m_nalBufferLen + lastSEI.m_nalBufferLen / 2 + 16);
            int newSize = lastSEI.serializeBuffer(tmpBuff.data(), tmpBuff.data() + tmpBuff.size(), false);
            if (newSize == -1)
                THROW(ERR_COMMON, "Not enough buffer")

            assert(newSize > 2);

            int sizeDiff = newSize - oldSize;
            if (sizeDiff != 0)
            {
                if (m_bufEnd + sizeDiff > m_tmpBuffer + m_tmpBufferSize)
                    THROW(ERR_COMMON, "Not enough buffer")
                memmove(nextNal + sizeDiff, nextNal, m_bufEnd - nextNal);
                m_bufEnd += sizeDiff;
            }
            memcpy(buff, tmpBuff.data(), newSize);
        }
    }
    return 0;
}

int H264StreamReader::getNalHrdLen(const uint8_t* nal) const
{
    if (m_bufEnd - nal >= 3)
    {
        if (nal[0] == 0 && nal[1] == 0)
        {
            if (nal[2] == 1)
                return 3;
            if (m_bufEnd - nal >= 4 && nal[3] == 1)
                return 4;
        }
    }
    return 0;
}

bool H264StreamReader::findPPSForward(uint8_t* buff)
{
    bool spsFound = false;
    bool ppsFound = false;
    for (uint8_t* nal = NALUnit::findNextNAL(buff, m_bufEnd); nal != m_bufEnd;
         nal = NALUnit::findNextNAL(nal, m_bufEnd))
        switch (static_cast<NALUnit::NALType>(*nal & 0x1f))
        {
        case NALUnit::NALType::nuSPS:
        case NALUnit::NALType::nuSubSPS:
            processSPS(nal);
            spsFound = true;
            break;
        case NALUnit::NALType::nuPPS:
            processPPS(nal);
            ppsFound = true;
            break;
        default:
            break;
        }
    return spsFound && ppsFound;
}

void H264StreamReader::fillPoc1(SliceUnit& slice, Poc1State& state)
{
    const SPSUnit* sps = slice.getSPS();
    if (sps == nullptr || sps->pic_order_cnt_type != 1)
        return;
    if (slice.first_mb_in_slice != 0)
    {
        slice.pic_order_cnt_lsb = static_cast<uint16_t>(state.lastOrder & 0xFFFF);
        return;
    }
    const int64_t maxFrameNum = int64_t{1} << sps->log2_max_frame_num;
    const int64_t frameNumOffset = slice.isIDR()                          ? 0
                                   : state.prevFrameNum > slice.frame_num ? state.prevFrameNumOffset + maxFrameNum
                                                                          : state.prevFrameNumOffset;
    const auto cycle = static_cast<int64_t>(sps->offset_for_ref_frame.size());
    int64_t absFrameNum = cycle != 0 ? frameNumOffset + slice.frame_num : 0;
    if (slice.nal_ref_idc == 0 && absFrameNum > 0)
        --absFrameNum;
    int64_t expected = 0;
    if (absFrameNum > 0)
    {
        int64_t perCycle = 0;
        for (const int offset : sps->offset_for_ref_frame) perCycle += offset;
        expected = (absFrameNum - 1) / cycle * perCycle;
        for (int64_t i = 0; i <= (absFrameNum - 1) % cycle; ++i)
            expected += sps->offset_for_ref_frame[static_cast<size_t>(i)];
    }
    if (slice.nal_ref_idc == 0)
        expected += sps->offset_for_non_ref_pic;
    // a frame by its top field, as a count of type 0 gives it; a bottom field by its own
    const int64_t order = expected + slice.delta_pic_order_cnt[0] +
                          (slice.m_field_pic_flag && slice.bottom_field_flag ? sps->offset_for_top_to_bottom_field : 0);
    state.prevFrameNum = slice.frame_num;
    state.prevFrameNumOffset = frameNumOffset;
    state.lastOrder = static_cast<int>(order);
    slice.pic_order_cnt_lsb = static_cast<uint16_t>(order & 0xFFFF);
}

int H264StreamReader::deserializeSliceHeader(SliceUnit& slice, const uint8_t* buff, const uint8_t* sliceEnd,
                                             Poc1State* poc1, const std::map<uint32_t, SPSUnit*>* spsMap,
                                             const std::map<uint32_t, PPSUnit*>* ppsMap)
{
    // A slice header is read up to the start code of the next unit, not up to the end of the data: a header cut short
    // read the next unit's bytes as its own fields and was taken as whole, and every picture time after it was
    // shifted without a warning. A header never comes near this window; past it the unit is long and whole.
    static constexpr int UNIT_END_WINDOW = 4096;
    const uint8_t* windowEnd = sliceEnd - buff > UNIT_END_WINDOW ? buff + UNIT_END_WINDOW : sliceEnd;
    const uint8_t* unitEnd =
        NALUnit::findNALWithStartCode(const_cast<uint8_t*>(buff), const_cast<uint8_t*>(windowEnd), true);
    const bool unitWhole = unitEnd < windowEnd;
    if (unitWhole)
        sliceEnd = unitEnd;
    int nalRez, toDecode;
    const int maxHeaderSize = static_cast<int>(sliceEnd - buff);
    do
    {
        uint8_t* tmpBuffer = m_decodedSliceHeader.data();
        const int tmpBufferSize = static_cast<int>(m_decodedSliceHeader.size());
        toDecode = FFMIN(tmpBufferSize - 8, maxHeaderSize);
        const int decodedLen = SliceUnit::decodeNAL(buff, buff + toDecode, tmpBuffer, tmpBufferSize);
        nalRez = slice.deserialize(tmpBuffer, tmpBuffer + decodedLen, spsMap ? *spsMap : m_spsMap,
                                   ppsMap ? *ppsMap : m_ppsMap);
        if (nalRez == NOT_ENOUGH_BUFFER && toDecode < maxHeaderSize)
            m_decodedSliceHeader.resize(m_decodedSliceHeader.size() + 1);
    } while (nalRez == NOT_ENOUGH_BUFFER && toDecode < maxHeaderSize);

    if (nalRez == NOT_ENOUGH_BUFFER && unitWhole)
        return UNIT_DAMAGED;  // the whole unit was read and the header still ends early
    if (nalRez == 0)
    {
        Poc1State fresh;
        fillPoc1(slice, poc1 ? *poc1 : fresh);
    }
    return nalRez;
}

bool H264StreamReader::isDamagedUnit(const int nalRez, const uint8_t* nextNal) const
{
    // NOT_ENOUGH_BUFFER from a unit whose end is in the buffer: the unit itself ends before its fields do.
    return nalRez == NOT_ENOUGH_BUFFER && nextNal < m_bufEnd;
}

void H264StreamReader::warnDamagedUnit(const char* name)
{
    if (m_damagedUnitWarnings++ == 0)
        LTRACE(LT_WARN, 2,
               "Warning: H264: a damaged " << name << " at picture " << m_totalFrameNum
                                           << " is passed through as it is. The last good one stays in use.");
}

int H264StreamReader::processSliceNal(uint8_t* buff, const bool auFirst)
{
    SliceUnit slice;
    const uint8_t* sliceEnd = m_bufEnd;

    int nalRez = deserializeSliceHeader(slice, buff, sliceEnd, &m_poc1);

    if (nalRez == NALUnit::SPS_OR_PPS_NOT_READY && m_isFirstFrame)
    {
        if (findPPSForward(buff + 1))
            nalRez = deserializeSliceHeader(slice, buff, sliceEnd, &m_poc1);
    }

    // A picture whose header is damaged (a recording that lost part of a packet) is kept as it came. Its first slice
    // still says it begins a picture (first_mb_in_slice 0 is the single bit 1), and its NAL type says whether it is an
    // IDR picture; the rest is read with the parameter sets of the picture before. An IDR picture has order 0 by
    // definition, so its time is exact; any other picture takes its order from the GOP before (see below).
    // The first slice of an access unit that does not say so: either its header is damaged down to the first
    // bit, or the slice before it was lost and this one is whole. A whole header is used as it is, with its own order;
    // a damaged one is guessed as above. Before, the slice was taken for a further slice of the picture before, the
    // two pictures were written as one, and every time after them was shifted.
    if (auFirst && nalRez == 0 && slice.first_mb_in_slice != 0 && slice.nal_unit_type != NALUnit::NALType::nuSliceExt)
    {
        if (m_damagedSliceWarnings++ == 0)
            LTRACE(LT_WARN, 2,
                   "Warning: H264: picture " << m_totalFrameNum
                                             << " has lost its first slice. The rest of the picture is kept.");
        slice.first_mb_in_slice = 0;
    }
    bool guessOrder = false;
    bool damaged = false;
    if (nalRez == UNIT_DAMAGED)
    {
        const auto unitType = static_cast<NALUnit::NALType>(*buff & 0x1f);
        const bool firstSlice =
            unitType != NALUnit::NALType::nuSliceExt && (auFirst || (m_bufEnd - buff > 1 && (buff[1] & 0x80)));
        const auto pps = m_ppsMap.find(m_lastSlicePPS);
        if (!firstSlice || pps == m_ppsMap.end())
            return 0;  // a further slice of a picture, or nothing to read it with yet: passed through as it came
        const auto sps = m_spsMap.find(pps->second->seq_parameter_set_id);
        if (sps == m_spsMap.end())
            return 0;
        if (m_damagedSliceWarnings++ == 0)
            LTRACE(LT_WARN, 2,
                   "Warning: H264: the header of picture "
                       << m_totalFrameNum
                       << " is damaged. The picture is kept, with the time of the picture at its place in the GOP "
                          "before.");
        slice.assumeDamagedFirstSlice(unitType, sps->second, pps->second);
        damaged = true;
        guessOrder = !slice.isIDR();
        nalRez = 0;
    }

    if (nalRez != 0)
    {
        return nalRez;
    }

    if (slice.first_mb_in_slice != 0)
        return 0;

    // The second field of a frame goes into the packet of the first, so nothing is written in front of it, and the
    // units the stream has in front of it are kept there (skipNal)
    const bool secondField = slice.m_field_pic_flag && m_fieldFirst != slice.bottom_field_flag;
    if (secondField && !m_secondField)
    {
        m_ppsSinceAU = false;  // a PPS read since the frame was written is one of this field, and stays in place
        m_secondFieldTimed = false;
    }
    m_secondField = secondField;

    if (m_fieldFirst != slice.bottom_field_flag)  // insrease PTS/DTS only for whole frame
        return 0;

    if (detectPrimaryPicType(slice, buff) != 0)
        return NOT_ENOUGH_BUFFER;

    // One track can carry more than one stream: parts joined end to end, or the two discs of one film.
    // Whether the picture order counts step by one or by two was decided only at the start of the track,
    // so a later part that counts the other way was read with the first part's step. Counts that step by
    // one, halved, gave every second picture the time of the picture before it, left the second half of
    // every IDR period without a picture, put times before the decoding times and printed a warning for
    // every second picture (#301; a joined sample: 12,662 repeated times, 527 s of standing picture). Counts
    // that step by two, not halved, spread the times out with no warning at all.
    // A new stream begins at an IDR whose SPS is not the one of the IDR before, and there the step is
    // decided again from that IDR period's own pictures. A stream whose SPS stays the same never gets
    // past the comparison, so it is muxed exactly as before.
    if (slice.isIDR() && slice.getSPS()->pic_order_cnt_type != 2)
    {
        const SPSUnit* sps = slice.getSPS();
        if (m_idrSps.size() != sps->m_nalBufferLen ||
            memcmp(m_idrSps.data(), sps->m_nalBuffer, sps->m_nalBufferLen) != 0)
        {
            if (!m_idrSps.empty())  // the first IDR: the step was decided by additionalStreamCheck
            {
                const int lsbDiv = lsbDivOfIdrPeriod(slice, buff);
                if (lsbDiv == -1)
                    return NOT_ENOUGH_BUFFER;
                m_forceLsbDiv = lsbDiv;
            }
            m_idrSps.assign(sps->m_nalBuffer, sps->m_nalBuffer + sps->m_nalBufferLen);
        }
    }

    // forceLsbDiv check
    if (slice.getSPS()->frame_mbs_only_flag && m_forceLsbDiv != 0)
    {
        if (slice.pic_order_cnt_lsb % 2 == 1)
        {
            if (m_totalFrameNum < 250)
            {
                m_forceLsbDiv = 0;
                LTRACE(LT_WARN, 2, "H264 warn: Force frame_mbs_only_flag division invalid detected. Cancel this flag.");
            }
            else if (!m_oddLsbSaid)
            {
                // Once for the track: a stream like this has such a value in every slice from here on, and the
                // line came for each of them, hundreds of times
                m_oddLsbSaid = true;
                LTRACE(LT_WARN, 2,
                       "H264 warn: Unexpected pic_order_cnt_lsb value "
                           << slice.pic_order_cnt_lsb << ". FrameNum: " << m_totalFrameNum << " slice type: "
                           << sliceTypeStr[slice.slice_type] << ". Later slices with such a value are not listed.");
            }
        }
    }

    ////////////////////////////

    // A stream that does not begin with an IDR, as a cut does, can have pictures after its first one in decode order
    // that are shown before it: the B pictures of an open GOP behind an I picture, or the B pictures behind a P
    // picture. The first picture took the first time before they were seen, the first of them got the same time, and
    // a Matroska output wrote the two as one block. The first picture now comes after them, and every picture from it
    // on takes its time from its own order count, as after an IDR.
    int firstLeading = -1;
    // A later file of a join that does not begin with an IDR, as a cut does, is a stream of its own when its first
    // picture would take an order a picture of the file before already has: it continued that file's IDR period,
    // its pictures took times the file before had given, and the two parts overlapped (4 of 268 times repeated).
    // It begins as a stream does, by the rule above, after the pictures before it. A file that continues the
    // stream before it, as the parts of a split recording do, gives no order twice and goes on as before.
    bool joinStart = false;
    if (m_joinPos && buff >= m_joinPos)
    {
        joinStart = !slice.isIDR() && !guessOrder && !damaged && !slice.m_field_pic_flag && !m_fieldPicturesSeen &&
                    slice.getSPS()->pic_order_cnt_type != 2 && m_ptsGiven &&
                    m_periodOrders.count(nextPicOrder(slice)) > 0;
        if (!joinStart)
            m_joinPos = nullptr;
    }
    // Frame pictures only: the two fields of a frame share one time and go out as one packet (a Matroska output
    // writes each field as a block of its own, see secondFieldStart), and this count placed them apart (a 1080i
    // capture cut mid stream got its first field pair in two packets).
    if ((joinStart || (m_totalFrameNum == 0 && m_idrSliceCnt == 0)) && !guessOrder && !damaged && !slice.isIDR() &&
        !slice.m_field_pic_flag && (!m_fieldPicturesSeen || m_totalFrameNum == 0) &&
        slice.getSPS()->pic_order_cnt_type != 2)
    {
        firstLeading = leadingPicturesOfFirst(slice, buff);
        if (firstLeading == -1)
            return NOT_ENOUGH_BUFFER;
        if (firstLeading == -2)
            firstLeading = -1;  // coded fields follow: as before
    }
    if (joinStart)
    {
        m_joinPos = nullptr;
        joinStart = firstLeading >= 0;
        if (joinStart)
        {
            m_idrSliceCnt = 0;
            m_bSliceFound = false;
            m_picOrderOffset = 0;
            m_iFramePtsOffset = 0;
            m_periodOrders.clear();
            m_fieldsShown.clear();
        }
    }

    // An IDR period can have a deeper B pyramid than the pictures before it: a joined file coded otherwise, or a stream
    // whose first block holds too few pictures to show its depth. Found only at its first deep picture, the larger
    // depth moved the decoding time of that picture back behind the times already given. The period is read ahead from
    // its IDR, and the decoding times catch up over its first pictures (deepenAfterOutput).
    if (slice.isIDR() && !damaged && !slice.m_field_pic_flag && !m_fieldPicturesSeen &&
        slice.getSPS()->pic_order_cnt_type != 2)
    {
        const int ahead = (std::min)(16, depthOfIdrPeriod(slice, buff));
        if (ahead == -1)
            return NOT_ENOUGH_BUFFER;
        if (ahead > m_frameDepth)
        {
            const int depthBefore = getFrameDepth();
            m_frameDepth = ahead;
            LTRACE(LT_INFO, 2,
                   "B-pyramid level " << m_frameDepth - 1 << " detected. Shift DTS to " << m_frameDepth << " frames");
            deepenAfterOutput(depthBefore);
        }
    }

    // Times by fields begin at the next IDR, where a period begins anyway: begun at once, mid period, the pictures
    // timed by frames before and those after left a hole of 17 frames. The decoding times are moved back
    // further for the longer pictures (else 6 pictures came before their decoding time), and everything
    // from here on is moved later by as much, so the decoding times do not go back: the one hole left, 2 frames there.
    if (slice.isIDR() && m_pulldownPendingFields > m_pulldownMaxFields)
    {
        const int depthBefore = getFrameDepth();
        m_pulldownMaxFields = m_pulldownPendingFields;
        const int64_t later = static_cast<int64_t>(getFrameDepth() - depthBefore) * m_pcrIncPerFrame;
        m_curDts += later;
    }
    // Pulldown: the pictures shown before this one that come later in decode order, read before anything changes
    if (m_pulldownMaxFields > 2 && !slice.m_field_pic_flag && slice.getSPS()->pic_order_cnt_type != 2)
        pulldownFieldsAhead(slice, buff);

    // The first picture of a part cut from a clip of a playlist, when it is not an IDR picture: its order count goes on
    // from the pictures before it in its clip, which the cut left out, not from the picture before it here, and every
    // picture of the part was shown frames away from its place. The count starts again from it, as from an IDR
    // picture. The pictures shown before it were left out with the cut (TSDemuxer::keptInPart).
    const bool partStart = !guessOrder && !slice.isIDR() && slice.isIFrame() && !slice.m_field_pic_flag &&
                           slice.getSPS()->pic_order_cnt_type != 2 && isPartStart();
    if (partStart)
    {
        prevPicOrderCntMsb = 0;
        prevPicOrderCntLsb = static_cast<int>(slice.pic_order_cnt_lsb);
    }

    // A guessed order is set below from the GOP before; calcPicOrder would take the stand-in count as the stream's own.
    int fullPicOrder = 0;
    if (!guessOrder)
    {
        if (slice.getSPS()->pic_order_cnt_type != 2)
            fullPicOrder = calcPicOrder(slice);
        else
            m_frameDepth = 0;
    }

    if (slice.isIDR())
    {
        // LTRACE(LT_INFO, 2, "got idr slice");
        m_frameNum = 0;
        if (slice.getSPS()->pic_order_cnt_type != 2)
            m_iFramePtsOffset = getIdrPrevFrames(buff, m_bufEnd);
        else
            m_iFramePtsOffset = 0;
        if (m_iFramePtsOffset == -1)
            return NOT_ENOUGH_BUFFER;
    }
    else if (partStart)
    {
        m_frameNum = 0;
        m_iFramePtsOffset = -fullPicOrder;
    }
    else if (firstLeading >= 0)
        m_frameNum = 0;  // counted from the first picture as from an IDR, so the pictures shown before it fit the delay
    else
    {
        m_frameNum++;
        // m_frameNum = slice.frame_num;

        if (slice.getSPS()->pic_order_cnt_type == 2)
            fullPicOrder = m_frameNum;
    }
    fullPicOrder += m_iFramePtsOffset;

    m_isFirstFrame = false;
    m_totalFrameNum++;

    if (slice.isIDR())
    {
        m_idrSliceCnt++;
        m_picOrderOffset = 0;
        m_periodOrders.clear();
    }
    else if (partStart)
        m_picOrderOffset = 0;
    if (m_idrSliceCnt == 0)
    {
        if (firstLeading >= 0)
        {
            m_bSliceFound = true;
            m_picOrderOffset = firstLeading - fullPicOrder;
        }
        if (!m_bSliceFound && slice.slice_type == SliceUnit::B_TYPE)
        {
            m_bSliceFound = true;
            m_picOrderOffset = m_frameNum - fullPicOrder - 1;
        }
        if (m_bSliceFound)
        {
            fullPicOrder += m_picOrderOffset;
            if (fullPicOrder < 0)
                fullPicOrder = 0;
        }
        else
            fullPicOrder = m_frameNum;
    }

    // The offset of each picture from its place in decode order repeats from GOP to GOP, so a picture whose own order
    // cannot be read takes the offset of the picture at its place in the GOP before; the next time in sequence is only
    // the last resort, as with B pictures it is the time of another picture.
    {
        // A stream with one I picture only does not grow it without end.
        static constexpr size_t MAX_GOP_PICTURES = 512;
        const bool gopStart = slice.isIDR() || (!damaged && slice.isIFrame());
        if (guessOrder)
        {
            const size_t place = m_gopOrderOffsets.size();
            fullPicOrder = m_frameNum + (place < m_prevGopOrderOffsets.size() ? m_prevGopOrderOffsets[place] : 0);
        }
        if (gopStart)
        {
            m_prevGopOrderOffsets.swap(m_gopOrderOffsets);
            m_gopOrderOffsets.clear();
        }
        if (m_gopOrderOffsets.size() < MAX_GOP_PICTURES)
            m_gopOrderOffsets.push_back(fullPicOrder - m_frameNum);
    }

    // check b-pyramid
    if (slice.m_field_pic_flag)
        m_fieldPicturesSeen = true;
    checkPyramid(m_frameNum, &fullPicOrder, m_nextFrameFound);
    if (m_periodOrders.size() >= 4096)
        m_periodOrders.erase(m_periodOrders.begin());
    m_periodOrders.insert(fullPicOrder);

    // increase PTS/DTS
    m_curDts += m_lastDtsInc;
    m_curPts = m_curDts + (fullPicOrder - m_frameNum) * m_pcrIncPerFrame;
    // Pulldown: a picture is shown when the pictures before it in the order shown have been shown for their own
    // number of fields. Taking the reordering in whole frames, as without pulldown, gave pictures the same times
    // and put them out of order: with frame doubling, 60 of 144 times were taken twice.
    const bool pulldownTimes =
        m_pulldownMaxFields > 2 && !slice.m_field_pic_flag && slice.getSPS()->pic_order_cnt_type != 2;
    if (pulldownTimes)
    {
        if (slice.isIDR() || m_fieldsShown.empty())
        {
            m_fieldsShown.clear();
            int leading = 0;
            for (const auto& [rel, fields] : m_fieldsAhead)
                if (rel < 0)
                    ++leading;
            m_pulldownFirstOrder = fullPicOrder - leading;
            m_pulldownOrigin = m_curDts + (m_pulldownFirstOrder - m_frameNum) * m_pcrIncPerFrame;
            m_pulldownSumOrder = m_pulldownFirstOrder;
            m_pulldownSumFields = 0;
        }
        // The fields shown before a picture are kept as a running sum up to m_pulldownSumOrder, moved to each picture's
        // order. Adding them up from the first picture of the period for every picture took 17 s for 30000 pictures
        // after one IDR, and a film with one IDR would have taken far longer.
        const auto setFields = [this](const int order, const int fields, const bool replace)
        {
            const auto [it, added] = m_fieldsShown.emplace(order, fields);
            const int before = added ? 2 : it->second;  // an order not yet seen counts as a frame
            if (!added)
            {
                if (!replace)
                    return;
                it->second = fields;
            }
            if (order >= m_pulldownFirstOrder && order < m_pulldownSumOrder)
                m_pulldownSumFields += fields - before;
        };
        setFields(fullPicOrder, fieldsShownOf(m_lastPictStruct), true);
        for (const auto& [rel, fields] : m_fieldsAhead) setFields(fullPicOrder + rel, fields, false);
        const auto fieldsAt = [this](const int order)
        {
            const auto it = m_fieldsShown.find(order);
            return it != m_fieldsShown.end() ? it->second : 2;
        };
        const int sumTo = (std::max)(fullPicOrder, m_pulldownFirstOrder);
        for (; m_pulldownSumOrder < sumTo; ++m_pulldownSumOrder) m_pulldownSumFields += fieldsAt(m_pulldownSumOrder);
        while (m_pulldownSumOrder > sumTo) m_pulldownSumFields -= fieldsAt(--m_pulldownSumOrder);
        m_curPts = m_pulldownOrigin + m_pulldownSumFields * m_pcrIncPerField;
    }

    // The last picture before an IDR (or of the stream) is held to its decoding time, but never onto a time a picture
    // before it already has: a cut stream lacks pictures, has a hole in the order shown, and the picture held back
    // took the time of another one.
    // Coded fields keep the old rule: the two fields of a frame share one time, and a picture here is a field.
    // With pulldown the picture before it is shown for its own number of fields, not one frame: the last picture of a
    // 720p59.94 stream with frame doubling came 1/59.94 s early, on the second half of the picture before it.
    if (m_curPts > m_curDts && (!m_nextFrameFound || m_nextFrameIdr))
        m_curPts = (std::max)(
            m_curDts, m_ptsGiven && !m_fieldPicturesSeen ? (std::min)(m_curPts, m_maxPts + m_maxPtsShown) : m_curDts);

    // A new IDR period begins after the latest time already given. A stream that lacks pictures, as a cut does (the
    // pictures before its first one in decode order), has a hole in the order shown, and the IDR, placed by the count
    // of pictures decoded, took a time a picture before it already had. The decoding times skip the same gap.
    if ((slice.isIDR() || joinStart) && m_ptsGiven && !m_fieldPicturesSeen)
    {
        const int before = joinStart ? firstLeading : m_iFramePtsOffset;  // the pictures shown before this one
        const int64_t periodStart = m_curPts - static_cast<int64_t>(before) * m_pcrIncPerFrame;
        if (periodStart <= m_maxPts)
        {
            const int64_t gap = m_maxPts + m_maxPtsShown - periodStart;
            m_curDts += gap;
            m_curPts += gap;
            // With pulldown the pictures after the IDR take their times from m_pulldownOrigin: moved with the IDR
            // alone, they came before it (a cut 3:2 stream: PTS below DTS).
            if (pulldownTimes)
                m_pulldownOrigin += gap;
        }
    }
    if (!m_ptsGiven || m_curPts >= m_maxPts)
    {
        m_maxPts = m_curPts;
        m_maxPtsShown = pulldownTimes ? fieldsShownOf(m_lastPictStruct) * m_pcrIncPerField : m_pcrIncPerFrame;
    }
    m_ptsGiven = true;

    if (m_OffsetMetadataPtsAddr)
    {
        const auto pts90k = m_curDts / INT_FREQ_TO_TS_FREQ + m_startPts;
        SEIUnit::updateMetadataPts(m_OffsetMetadataPtsAddr, pts90k);
        m_OffsetMetadataPtsAddr = nullptr;
    }
    // LTRACE(LT_INFO, 2, "delta=" << fullPicOrder - m_frameNum << " m_lastDtsInc=" << m_lastDtsInc);

    // How long this picture occupies. A pulldown flag says a picture is held on screen for longer
    // than one frame, and the coded stream carries no extra picture for that time, so the time has
    // to come from here or the file ends up short by the pulldown ratio.
    //
    // These three branches had never run. They tested a member one letter away from the one the
    // parser fills in, and that member was set to -1 in the constructor and never written again.
    if (m_lastPictStruct == 5 || m_lastPictStruct == 6)  // 3 fields per frame. Used in pulldown
        m_lastDtsInc = m_pcrIncPerFrame + m_pcrIncPerField;
    else if (m_lastPictStruct == 7)  // frame doubling. Used in pulldown
        m_lastDtsInc = m_pcrIncPerFrame * 2;
    else if (m_lastPictStruct == 8)  // frame tripling. Used in pulldown
        m_lastDtsInc = m_pcrIncPerFrame * 3;
    else
        m_lastDtsInc = m_pcrIncPerFrame;

    if (m_removePulldown)
    {
        if (m_lastDtsInc != m_pcrIncPerFrame)
            m_pulldownMarkSeen = true;
        checkPulldownSync();
        m_testPulldownDts += m_lastDtsInc;
        m_lastDtsInc = m_pcrIncPerFrame;
    }

    m_lastSlicePPS = slice.pic_parameter_set_id;
    if (slice.getPPS() != nullptr)
        m_lastSliceSPS = slice.getPPS()->seq_parameter_set_id;
    else
        m_lastSliceSPS = -1;
    m_lastSliceIDR = slice.isIDR();
    m_lastIFrame = slice.isIFrame() || (damaged && slice.isIDR());

    return 0;
}

int H264StreamReader::detectPrimaryPicType(const SliceUnit& firstSlice, uint8_t* buff)
{
    SliceUnit slice;
    m_nextFrameFound = false;
    m_nextFrameIdr = false;
    m_pict_type = -1;
    m_pict_type = (std::max)(m_pict_type, sliceTypeToPictType(firstSlice.slice_type));

    uint8_t* nextSlice = NALUnit::findNextNAL(buff, m_bufEnd);
    if (nextSlice == m_bufEnd)
    {
        return m_eof ? 0 : NOT_ENOUGH_BUFFER;
    }

    while (true)
    {
        const int nalUnitType = *nextSlice;
        switch (static_cast<NALUnit::NALType>(nalUnitType & 0x1f))
        {
        case NALUnit::NALType::nuSliceIDR:
        case NALUnit::NALType::nuSliceNonIDR:
        case NALUnit::NALType::nuSliceA:
        case NALUnit::NALType::nuSliceB:
        case NALUnit::NALType::nuSliceC:
        case NALUnit::NALType::nuSliceExt:
            if (slice.deserializeSliceType(nextSlice, m_bufEnd) != 0)
            {
                return m_eof ? 0 : NOT_ENOUGH_BUFFER;
            }

            if (slice.first_mb_in_slice == 0)
            {
                m_nextFrameFound = true;
                m_nextFrameIdr = slice.isIDR();
                return 0;  // next frame found
            }
            m_pict_type = (std::max)(m_pict_type, sliceTypeToPictType(slice.slice_type));
            break;
        default:
            break;
        }
        nextSlice = NALUnit::findNextNAL(nextSlice, m_bufEnd);
        if (nextSlice == m_bufEnd)
        {
            return m_eof ? 0 : NOT_ENOUGH_BUFFER;
        }
    }
}

int H264StreamReader::sliceTypeToPictType(const uint32_t slice_type) const
{
    switch (slice_type)
    {
    case SliceUnit::P_TYPE:
    {
        return (m_pict_type < 3) ? 1 : 6;
    }
    case SliceUnit::B_TYPE:
    {
        return (m_pict_type < 3) ? 2 : 7;
    }
    case SliceUnit::I_TYPE:
    {
        return (m_pict_type < 3) ? 0 : 5;
    }
    case SliceUnit::SP_TYPE:
    {
        if (m_pict_type == -1 || m_pict_type == 3)
            return 4;
        return (m_pict_type == 2) ? 7 : 6;
    }
    case SliceUnit::SI_TYPE:
    {
        if (m_pict_type == -1)
            return 3;
        if (m_pict_type == 0)
            return 5;
        return (m_pict_type == 2) ? 7 : 6;
    }
    default:;
    }
    return 0;
}

bool H264StreamReader::replaceToOwnSPS() const { return m_needSeiCorrection; }

int H264StreamReader::processSPS(uint8_t* buff)
{
    if (replaceToOwnSPS())
    {
        // do not updat SPS in stream because of we are going to insert own sps
        if (m_bufEnd - buff < 8)
            return NOT_ENOUGH_BUFFER;
        const int seq_parameter_set_id = static_cast<int>(NALUnit::extractUEGolombCode(buff + 4, m_bufEnd));
        if (updatedSPSList.find(seq_parameter_set_id) != updatedSPSList.end())
        {
            // The same SPS again is passed over: ours is written in its place. One with the same id and other
            // content, from a part of the stream encoded again, as a cutting tool does with the start of a file, was
            // passed over as well, and every picture after it was decoded with the first one's settings: the picture
            // broke up from there on (#813). It is read now, and ours follows it.
            // A new rate (fps=, delPulldown) is written into the SPS where it lies in the buffer, and the stream check
            // and the mux read that same buffer: the second reading met the rewritten SPS and called it a change.
            const uint8_t* end = NALUnit::findNALWithStartCode(buff, m_bufEnd, true);
            const auto sameAs = [&](const std::map<uint32_t, std::vector<uint8_t>>& kept)
            {
                const auto known = kept.find(seq_parameter_set_id);
                return known != kept.end() && known->second.size() == static_cast<size_t>(end - buff) &&
                       std::equal(known->second.begin(), known->second.end(), buff);
            };
            if (sameAs(m_spsSource) || sameAs(m_spsRewritten))
                return 0;
            if (!m_spsChangeSaid)
                LTRACE(LT_INFO, 2,
                       "H264 stream (track " << m_streamIndex
                                             << "): its SPS changes within the stream; the SPS written follows it.");
            m_spsChangeSaid = true;
        }
    }

    auto* sps = new SPSUnit();
    uint8_t* nextNal = NALUnit::findNALWithStartCode(buff, m_bufEnd, true);
    const int oldSpsLen = static_cast<int>(nextNal - buff);
    std::vector<uint8_t> source(buff, nextNal);  // as it came, before the frame rate and HRD are written into it
    sps->decodeBuffer(buff, nextNal);
    const int nalRez = sps->deserialize();
    // A damaged SPS asked for more data for ever, and the job ended with "Not enough buffer" after the pictures before
    // it. Discs and broadcasts repeat their SPS, so it is passed through as it came and the last good one stays in use.
    if (isDamagedUnit(nalRez, nextNal))
    {
        delete sps;
        warnDamagedUnit("SPS");
        return 0;
    }

    // The FIRST sequence parameter set is adopted, not compared. These two flags start false
    // in the constructor, and checkStream is what normally replaces them with the stream's own
    // values. checkStream is reached from getTSDescriptor, so a transport stream output gets
    // it and a Matroska one never did: the defaults were still in place when the first real
    // SPS arrived, every stream carrying HRD parameters compared unequal, and the warning
    // fired BECAUSE the stream was being seen for the first time.
    //
    // Measured on a 3D source: to a disc, stored 1,1 against an SPS of 1,1 and silence; to
    // Matroska, stored 0,0 against the same 1,1, one warning per stream, and 1,1 against 1,1
    // for every SPS after it. The stream was identical throughout.
    if (!m_spsFlagsKnown)
    {
        orig_hrd_parameters_present_flag = sps->nalHrdParams.isPresent;
        orig_vcl_parameters_present_flag = sps->vclHrdParams.isPresent;
        m_spsFlagsKnown = true;
    }
    else if (orig_hrd_parameters_present_flag != sps->nalHrdParams.isPresent ||
             orig_vcl_parameters_present_flag != sps->vclHrdParams.isPresent)
    {
        orig_hrd_parameters_present_flag = sps->nalHrdParams.isPresent;
        orig_vcl_parameters_present_flag = sps->vclHrdParams.isPresent;
        if (!m_spsChangeWarned)
        {
            LTRACE(LT_WARN, 2, "H264 warn: SPS parameters is not consistent throughout a stream");
            m_spsChangeWarned = true;
        }
    }

    // long sps fields can cause 00 00 0x code and it is required decode slice header
    if (nalRez != 0)
    {
        delete sps;
        return nalRez;  // not enough buffer
    }

    // A clip of a disc has one picture size. A join of files of other sizes changed it in the middle of the clip, and
    // the job ended "Mux successful" without a word.
    if (m_firstSpsWidth == 0)
    {
        m_firstSpsWidth = static_cast<int>(sps->getWidth());
        m_firstSpsHeight = static_cast<int>(sps->getHeight());
    }
    else if ((static_cast<int>(sps->getWidth()) != m_firstSpsWidth ||
              static_cast<int>(sps->getHeight()) != m_firstSpsHeight) &&
             !m_sizeChangeSaid && (V3_flags & BLURAY_OUT) && !m_demuxMode)
    {
        m_sizeChangeSaid = true;
        LTRACE(LT_WARN, 2,
               "Warning: H264 stream (track " << m_streamIndex << ") changes its picture size from " << m_firstSpsWidth
                                              << "x" << m_firstSpsHeight << " to " << sps->getWidth() << "x"
                                              << sps->getHeight()
                                              << ", as a join of files of different sizes does. A clip of a Blu-ray "
                                                 "disc has one picture size, so a player may not play the part after "
                                                 "the change. Join files of one size only.");
    }
    if (m_spsMap.find(sps->seq_parameter_set_id) == m_spsMap.end())
    {
        LTRACE(LT_INFO, 2, "Decoding H264 stream (track " << m_streamIndex << "): " << sps->getStreamDescr());
        if (m_forcedLevel != 0)
            LTRACE(LT_INFO, 2, "Change H264 level from " << sps->level_idc / 10.0 << " to " << m_forcedLevel / 10.0);
    }
    // update profile if needed
    if (m_forcedLevel != 0 && m_bufEnd - buff >= 4)
    {
        buff[3] = m_forcedLevel;
        sps->m_nalBuffer[3] = m_forcedLevel;
        sps->level_idc = m_forcedLevel;
    }

    updateFPS(sps, buff, nextNal, oldSpsLen);
    m_spsRewritten[sps->seq_parameter_set_id].assign(buff, NALUnit::findNALWithStartCode(buff, m_bufEnd, true));
    updateHRDParam(sps);
    if (sps->nalHrdParams.isPresent)
        updatedSPSList.insert(sps->seq_parameter_set_id);

    delete m_spsMap[sps->seq_parameter_set_id];
    m_spsMap[sps->seq_parameter_set_id] = sps;
    m_spsSource[sps->seq_parameter_set_id] = std::move(source);

    double sarAR = 1.0;
    double sampleAR = 0;
    if (sps->aspect_ratio_info_present_flag)
    {
        if (sps->aspect_ratio_idc >= 1 && sps->aspect_ratio_idc <= 16)
            sampleAR = sarAR = h264_ar_coeff[sps->aspect_ratio_idc];
        else if (sps->aspect_ratio_idc == Extended_SAR && sps->sar_width && sps->sar_height)
            sampleAR = sarAR = sps->sar_width / static_cast<double>(sps->sar_height);
    }
    noteSampleAspectRatio(sampleAR);
    fillAspectBySAR(sarAR);

    return 0;
}

int H264StreamReader::processPPS(uint8_t* buff)
{
    auto* pps = new PPSUnit();
    const uint8_t* nextNal = NALUnit::findNALWithStartCode(buff, m_bufEnd, true);

    pps->decodeBuffer(buff, nextNal);
    const int nalRez = pps->deserialize();
    if (isDamagedUnit(nalRez, nextNal))
    {
        delete pps;
        warnDamagedUnit("PPS");
        return 0;  // as an SPS: passed through, the last good one stays in use
    }
    if (nalRez != 0)
    {
        delete pps;
        return nalRez;  // not enough buffer
    }
    delete m_ppsMap[pps->pic_parameter_set_id];
    m_ppsMap[pps->pic_parameter_set_id] = pps;
    m_ppsSinceAU = true;
    return 0;
}

unsigned H264StreamReader::getStreamWidth() const
{
    return !m_spsMap.empty() ? m_spsMap.begin()->second->getWidth() : 0;
}

unsigned H264StreamReader::getStreamHeight() const
{
    return !m_spsMap.empty() ? m_spsMap.begin()->second->getHeight() : 0;
}

// The colour the stream's VUI gives. It said none for H.264, so the colour box of an MP4 decided all three values,
// also where the stream gives its own: a VUI with matrix 1 (BT.709) went out with the container's 9 (BT.2020). As for
// HEVC, all 2 (unspecified) is no description; a single 2 is filled from the container (writeColourInfo).
bool H264StreamReader::getColourDesc(uint8_t& primaries, uint8_t& transfer, uint8_t& matrix) const
{
    if (m_spsMap.empty())
        return false;
    const SPSUnit* sps = m_spsMap.begin()->second;
    primaries = sps->colour_primaries;
    transfer = sps->transfer_characteristics;
    matrix = sps->matrix_coefficients;
    return primaries != 2 || transfer != 2 || matrix != 2;
}

bool H264StreamReader::getInterlaced()
{
    return !m_spsMap.empty() ? !m_spsMap.begin()->second->frame_mbs_only_flag : true;
}

void H264StreamReader::onMoveBuffer(const std::ptrdiff_t delta)
{
    if (m_priorityNalAddr)
        m_priorityNalAddr += delta;
    if (m_OffsetMetadataPtsAddr)
        m_OffsetMetadataPtsAddr += delta;
}

void H264StreamReader::onShiftBuffer(const int offset)
{
    MPEGStreamReader::onShiftBuffer(offset);
    if (m_lastDecodedPos && m_priorityNalAddr >= m_lastDecodedPos)
        m_priorityNalAddr -= offset;
    else
        m_priorityNalAddr = nullptr;
    if (m_OffsetMetadataPtsAddr > m_lastDecodedPos)
        m_OffsetMetadataPtsAddr -= offset;
    else
        m_OffsetMetadataPtsAddr = nullptr;
}

bool H264StreamReader::isPriorityData(AVPacket* packet)
{
    // A packet with no data is never the priority NAL. Adding to its null pointer, as the test below does, is
    // undefined behaviour, and a build that checks for it stopped there.
    if (packet->data == nullptr || m_priorityNalAddr == nullptr)
        return false;
    if (packet->data + 3 == m_priorityNalAddr || packet->data + 4 == m_priorityNalAddr)
    {
        m_priorityNalAddr = nullptr;
        m_OffsetMetadataPtsAddr = nullptr;
        return true;
    }
    return false;
}

// ** THE UNITS THAT CANNOT BE H.264 (see MPEGStreamReader::unusableUnit). ** ITU-T H.264 7.3.1 and Table 7-1: the
// forbidden_zero_bit, type 0 and 25 to 31 (unspecified; 24 is the delimiter of an MVC view on a Blu-ray disc and
// stays), and the types reserved for later (16 to 18, 22, 23). A slice whose first fields are out of range (7.4.3:
// slice_type 0 to 9, pic_parameter_set_id 0 to 255, an IDR picture coded intra only) is a slice by its type alone. The
// longest unit of a type is the size its syntax comes to, with room to spare: an SPS or PPS of a real stream is a few
// dozen bytes, one with scaling lists a few hundred. Other data read as H.264 has many units of these types of 2 KB.
bool H264StreamReader::unusableUnit(const uint8_t* unit, const uint8_t* end, int& maxLen)
{
    maxLen = 0;
    if (*unit & 0x80)
        return true;
    const int type = *unit & 0x1f;
    switch (type)
    {
    case 0:
    case 16:
    case 17:
    case 18:
    case 22:
    case 23:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
        return true;
    case 7:  // SPS
        maxLen = 1024;
        return false;
    case 8:  // PPS
        maxLen = 512;
        return false;
    case 15:  // subset SPS
        maxLen = 2048;
        return false;
    case 9:   // access unit delimiter
    case 10:  // end of sequence
    case 11:  // end of stream
    case 13:  // SPS extension
    case 14:  // prefix
    case 24:  // the delimiter of an MVC view on a Blu-ray disc
        maxLen = 64;
        return false;
    case 1:
    case 2:
    case 5:
    {
        UnitHead head(unit, end, 1);
        uint32_t firstMb = 0;
        uint32_t sliceType = 0;
        uint32_t ppsId = 0;
        if (!head.readUe(firstMb) || !head.readUe(sliceType) || !head.readUe(ppsId))
            return true;
        if (sliceType > 9 || ppsId > 255)
            return true;
        return type == 5 && sliceType % 5 != 2 && sliceType % 5 != 4;  // an IDR picture is coded intra only
    }
    default:
        return false;
    }
}
