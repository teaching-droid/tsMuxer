#include "vvcStreamReader.h"

#include <fs/systemlog.h>
#include <memory>

#include "nalUnits.h"
#include "tsMuxer.h"
#include "tsPacket.h"
#include "vodCoreException.h"
#include "vvc.h"

using namespace std;

static constexpr int MAX_SLICE_HEADER = 64;

VVCStreamReader::VVCStreamReader()
    : m_vps(new VvcVpsUnit()),
      m_sps(nullptr),
      m_pps(nullptr),
      m_slice(new VvcSliceHeader()),
      m_firstFrame(true),
      m_frameNum(0),
      m_fullPicOrder(0),
      m_picOrderBase(0),
      m_frameDepth(1),
      m_picOrderMsb(0),
      m_prevPicOrder(0),
      m_lastIFrame(false),
      m_firstFileFrame(false),
      m_vpsCounter(0),
      m_vpsSizeDiff(0)
{
}

VVCStreamReader::~VVCStreamReader()
{
    delete m_vps;
    delete m_sps;
    delete m_pps;
    delete m_slice;
}

void VVCStreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    if (data.fps > 0.0 && m_fps == 0.0)
        setFPS(data.fps);
}

CheckStreamRez VVCStreamReader::checkStream(uint8_t* buffer, const int len)
{
    CheckStreamRez rez;
    // The pictures counted here are read again from the start by the mux, so the counters go back to zero on every way
    // out, not only at the end, as in the HEVC reader.
    struct ResetCounters
    {
        VVCStreamReader* r;
        ~ResetCounters()
        {
            r->m_totalFrameNum = r->m_frameNum = r->m_fullPicOrder = 0;
            r->m_curDts = r->m_curPts = 0;
        }
    } resetCounters{this};
    // A parameter set is read into a unit of its own and kept only when it is whole; a damaged one is skipped and the
    // one before it stays in use.
    const auto readWhole = [](auto*& current, auto* fresh, const uint8_t* from, const uint8_t* to) -> int
    {
        fresh->decodeBuffer(from, to);
        const int r = fresh->deserialize();
        if (r != 0)
        {
            delete fresh;
            return r;
        }
        delete current;
        current = fresh;
        return 0;
    };
    uint8_t* end = buffer + len;
    bool phPending = false;  // a picture header unit was read: the next slice begins its picture
    bool phDamaged = false;
    bool auOpen = true;  // a delimiter, SEI or parameter set came after the last slice

    for (uint8_t* nal = NALUnit::findNextNAL(buffer, end); nal < end - 4; nal = NALUnit::findNextNAL(nal, end))
    {
        if (*nal & 0x80)
            return rez;  // invalid nal
        const auto nalType = static_cast<VvcUnit::NalType>(nal[1] >> 3);
        uint8_t* nextNal = NALUnit::findNALWithStartCode(nal, end, true);
        if (!m_eof && nextNal == end)
            break;
        if ((nal[0] & 0x3f) != 0)
            continue;  // a unit of a layer above the base layer: the base layer is the one read and timed

        switch (nalType)
        {
        case VvcUnit::NalType::VPS:
        {
            const int r = readWhole(m_vps, new VvcVpsUnit(), nal, nextNal);
            if (r == UNIT_DAMAGED)
                break;
            if (r != 0)
                return rez;
            m_spsPpsFound = true;
            if (m_vps->m_vps_hrd.num_units_in_tick)
            {
                // Lightweight FPS extraction for probing only. The full
                // updateFPS() (with logging and 25fps fallback) runs later
                // during actual muxing in intDecodeNAL().
                const double fps = correctFps(m_vps->getFPS());
                if (fps > 0.0 && m_fps == 0.0)
                    setFPS(fps);
            }
            break;
        }
        case VvcUnit::NalType::SPS:
        {
            const int r = readWhole(m_sps, new VvcSpsUnit(), nal, nextNal);
            if (r == UNIT_DAMAGED)
                break;
            if (r != 0)
                return rez;
            m_spsPpsFound = true;
            const double fps = correctFps(getStreamFPS(m_sps));
            if (fps > 0.0 && m_fps == 0.0)
                setFPS(fps);
            break;
        }
        case VvcUnit::NalType::PPS:
        {
            const int r = readWhole(m_pps, new VvcPpsUnit(), nal, nextNal);
            if (r != 0 && r != UNIT_DAMAGED)
                return rez;
            break;
        }
        case VvcUnit::NalType::PH:
            m_slice->decodeBuffer(nal, FFMIN(nal + MAX_SLICE_HEADER, nextNal));
            phDamaged = m_slice->deserializePictureHeader(m_sps) != 0;
            phPending = true;
            break;
        default:
            break;
        }

        // A picture begins at its picture header: a picture header unit before its slices, or the header carried in
        // its slice (the first bit of the slice header), which is then the picture's only slice. Only the second kind
        // used to be counted.
        // As in the mux, the first slice of an access unit with no picture header at all begins its picture.
        if (!isSlice(nalType) && !isSuffix(nalType) && nalType != VvcUnit::NalType::PH)
            auOpen = true;
        else if (isSlice(nalType) && ((nal[2] & 0x80) || phPending || auOpen))
        {
            const bool noPictureHeader = !(nal[2] & 0x80) && !phPending;
            auOpen = false;
            const uint16_t phPocLsb = m_slice->pic_order_cnt_lsb;
            m_slice->decodeBuffer(nal, FFMIN(nal + MAX_SLICE_HEADER, nextNal));
            int r = m_slice->deserialize(m_sps, m_pps);
            if (noPictureHeader)
                r = UNIT_DAMAGED;
            // The same guess as the mux, so that a damaged IDR picture starts the count again here too.
            if (r == UNIT_DAMAGED || (r == 0 && !m_slice->picture_header_in_slice && phDamaged))
                m_fullPicOrder = guessPicOrder(nalType);
            else if (r)
                return rez;  // not enough buffer or error
            else
            {
                if (!m_slice->picture_header_in_slice)
                    m_slice->pic_order_cnt_lsb = phPocLsb;
                m_fullPicOrder = toFullPicOrder(m_slice, m_sps->log2_max_pic_order_cnt_lsb);
            }
            incTimings();
            phPending = false;
        }
    }

    if (m_sps && m_pps && m_pps->sps_id == m_sps->sps_id)
    {
        rez.codecInfo = vvcCodecInfo;
        rez.streamDescr = m_sps->getDescription();
        const size_t frSpsPos = rez.streamDescr.find("Frame rate: not found");
        if (frSpsPos != string::npos)
        {
            rez.streamDescr = rez.streamDescr.substr(0, frSpsPos);
            if (m_vps)
                rez.streamDescr += string(" ") + m_vps->getDescription();
        }
    }

    return rez;
}

int VVCStreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, const bool hdmvDescriptors)
{
    if (m_firstFrame)
        CheckStreamRez rez = checkStream(m_buffer, static_cast<int>(m_bufEnd - m_buffer));

    if (hdmvDescriptors)
    {
        *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::HDMV);  // descriptor tag
        *dstBuff++ = 8;                                            // descriptor length
        *dstBuff++ = 'H';
        *dstBuff++ = 'D';
        *dstBuff++ = 'M';
        *dstBuff++ = 'V';
        *dstBuff++ = 0xff;

        *dstBuff++ = static_cast<int>(StreamType::VIDEO_H266);  // stream_coding_type
        uint8_t video_format, frame_rate_index, aspect_ratio_index;
        // the rate the mux will take: before updateFPS has run, getFPS() is still 0 for a stream that names none
        M2TSStreamInfo::blurayStreamParams(headerFps(correctFps(getStreamFPS(nullptr))), getInterlaced(),
                                           getStreamWidth(), getStreamHeight(), getStreamAR(), &video_format,
                                           &frame_rate_index, &aspect_ratio_index);

        *dstBuff++ = static_cast<uint8_t>(video_format << 4 | frame_rate_index);
        *dstBuff = static_cast<uint8_t>(aspect_ratio_index << 4 | 0xf);

        return 10;  // total descriptor length
    }

    // The VVC descriptor is built from the sequence parameter set. A stream in which none was found, because it is
    // not VVC or not a kind tsMuxeR reads, has nothing to describe; it used to stop the program on the spot. It gets
    // no descriptor, and the mux goes on to the warning that the track produced no frames.
    if (m_sps == nullptr)
        return 0;

    const uint8_t* descStart = dstBuff;

    // ITU-T Rec.H.222 Table 2-133 - VVC video descriptor
    *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::VVC);
    uint8_t* descLength = dstBuff++;  // descriptor length, filled at the end
    *dstBuff++ = static_cast<uint8_t>(m_sps->profile_idc << 1 | m_sps->tier_flag);
    // Each sub-profile takes 4 bytes, and the descriptor has to fit the room kept for a track's descriptors (8 bytes
    // and 30 sub-profiles). A stream with more had its list written past that room. It is left out of the descriptor
    // then, as a count of 0, which the standard allows; the stream itself still carries it. The values are 32 bit
    // numbers in the order H.222.0 gives, most significant byte first; they were written the other way round.
    constexpr size_t MAX_SUB_PROFILES = (TS_ES_INFO_MAX - 8) / 4;
    const size_t subProfiles = m_sps->general_sub_profile_idc.size();
    if (subProfiles > MAX_SUB_PROFILES && !m_subProfilesWarned)
    {
        LTRACE(LT_WARN, 2,
               "Warning: the VVC stream names " << subProfiles << " sub-profiles, more than the " << MAX_SUB_PROFILES
                                                << " a transport stream descriptor has room for, so they are not "
                                                   "listed in it.");
        m_subProfilesWarned = true;
    }
    const size_t listed = subProfiles > MAX_SUB_PROFILES ? 0 : subProfiles;
    *dstBuff++ = static_cast<uint8_t>(listed);
    for (size_t i = 0; i < listed; ++i)
    {
        const uint32_t value = m_sps->general_sub_profile_idc[i];
        *dstBuff++ = static_cast<uint8_t>(value >> 24);
        *dstBuff++ = static_cast<uint8_t>(value >> 16);
        *dstBuff++ = static_cast<uint8_t>(value >> 8);
        *dstBuff++ = static_cast<uint8_t>(value);
    }
    *dstBuff++ =
        static_cast<uint8_t>(m_sps->progressive_source_flag << 7 | m_sps->interlaced_source_flag << 6 |
                             m_sps->non_packed_constraint_flag << 5 | m_sps->ptl_frame_only_constraint_flag << 4);
    *dstBuff++ = m_sps->level_idc;
    *dstBuff++ = 0;
    *dstBuff++ = 0xc0;

    const uint8_t* descEnd = dstBuff;
    const auto descSize = static_cast<int>(descEnd - descStart);
    *descLength = static_cast<uint8_t>(descSize - 2);  // fill descriptor length

    return descSize;
}

void VVCStreamReader::updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int)
{
    // ** THIS IS HANDED THE SPS AS WELL AS THE VPS, AND THEY ARE SIBLINGS. **
    //
    // intDecodeNAL calls updateFPS with m_sps at the SPS branch, and this used to cast whatever arrived to the VPS
    // type. The VPS rate was never read, so only an SPS with timing of its own, at a rate other than the muxing rate,
    // ever got here. Read through the wrong type, that SPS gave a position for the timing fields out of one of its
    // own unrelated members, and the new rate was written there: in the two measured streams that carry timing, bit 2
    // of every SPS, inside its header, so the output could not be decoded. HEVCStreamReader::updateStreamFps had the
    // same mistake and was fixed the same way. Each unit handed here now rewrites its own timing, at the position
    // read from it.
    m_vpsSizeDiff = 0;
    VvcUnit* unit = nullptr;
    bool rewritten = false;
    if (nalUnit == m_vps && m_vps != nullptr)
    {
        unit = m_vps;
        rewritten = m_vps->setFPS(m_fps);
    }
    else if (nalUnit == m_sps && m_sps != nullptr)
    {
        unit = m_sps;
        rewritten = m_sps->setFPS(m_fps);
    }
    else
        return;

    // The unit carries no timing to change, or the write would not fit. Either way the stream is left exactly as it
    // came in.
    if (!rewritten)
        return;

    const int oldNalSize = static_cast<int>(nextNal - buff);
    auto tmpBuffer = std::make_unique<uint8_t[]>(unit->nalBufferLen() + 16);
    const long newSpsLen = unit->serializeBuffer(tmpBuffer.get(), tmpBuffer.get() + unit->nalBufferLen() + 16);
    if (newSpsLen == -1)
        THROW(ERR_COMMON, "Not enough buffer")

    if (m_bufEnd && newSpsLen != oldNalSize)
    {
        m_vpsSizeDiff = newSpsLen - oldNalSize;
        if (m_bufEnd + m_vpsSizeDiff > m_tmpBuffer + m_tmpBufferSize)
            THROW(ERR_COMMON, "Not enough buffer")
        memmove(nextNal + m_vpsSizeDiff, nextNal, m_bufEnd - nextNal);
        m_bufEnd += m_vpsSizeDiff;
    }
    memcpy(buff, tmpBuffer.get(), newSpsLen);
}

unsigned VVCStreamReader::getStreamWidth() const { return m_sps ? m_sps->pic_width_max_in_luma_samples : 0; }

unsigned VVCStreamReader::getStreamHeight() const { return m_sps ? m_sps->pic_height_max_in_luma_samples : 0; }

// The unit asked about answers with its own rate when it has one, as in HEVCStreamReader::getStreamFPS. Always
// answering with the VPS meant that once the VPS had been rewritten to the fps= rate, an SPS with timing of its own
// was compared against that new rate, matched it, and kept the old one. The fallback is for the caller that wants
// whatever rate the stream has.
double VVCStreamReader::getStreamFPS(void* curNalUnit)
{
    if (curNalUnit == m_vps && m_vps != nullptr && m_vps->getFPS() != 0.0)
        return m_vps->getFPS();
    if (curNalUnit == m_sps && m_sps != nullptr && m_sps->getFPS() != 0.0)
        return m_sps->getFPS();

    double fps = 0;
    if (m_vps)
        fps = m_vps->getFPS();
    if (fps == 0.0 && m_sps)
        fps = m_sps->getFPS();
    return fps;
}

bool VVCStreamReader::skipNal(uint8_t* nal)
{
    const auto nalType = static_cast<VvcUnit::NalType>(nal[1] >> 3);

    if (nalType == VvcUnit::NalType::FD)
        return true;

    if ((nalType == VvcUnit::NalType::EOS || nalType == VvcUnit::NalType::EOB))
    {
        if (!m_eof || m_bufEnd - nal > 4)
        {
            return true;
        }
    }

    return false;
}

bool VVCStreamReader::isSlice(const VvcUnit::NalType nalType) const
{
    if (!m_sps || !m_pps)
        return false;

    switch (nalType)
    {
    case VvcUnit::NalType::TRAIL:
    case VvcUnit::NalType::STSA:
    case VvcUnit::NalType::RADL:
    case VvcUnit::NalType::RASL:
    case VvcUnit::NalType::IDR_W_RADL:
    case VvcUnit::NalType::IDR_N_LP:
    case VvcUnit::NalType::CRA:
    case VvcUnit::NalType::GDR:
        return true;
    default:
        return false;
    }
}

bool VVCStreamReader::isSuffix(const VvcUnit::NalType nalType) const
{
    if (!m_sps || !m_pps)
        return false;

    switch (nalType)
    {
    case VvcUnit::NalType::SUFFIX_APS:
    case VvcUnit::NalType::SUFFIX_SEI:
    case VvcUnit::NalType::EOS:
    case VvcUnit::NalType::EOB:
    case VvcUnit::NalType::FD:
    case VvcUnit::NalType::RSV_NVCL_27:
    case VvcUnit::NalType::UNSPEC_30:
    case VvcUnit::NalType::UNSPEC_31:
        return true;
    default:
        return false;
    }
}

void VVCStreamReader::incTimings()
{
    if (m_totalFrameNum++ > 0)
        m_curDts += m_pcrIncPerFrame;
    const int delta = m_frameNum - m_fullPicOrder;
    m_curPts = m_curDts - delta * m_pcrIncPerFrame;
    m_frameNum++;
    m_firstFrame = false;

    if (delta > m_frameDepth)
    {
        m_frameDepth = delta;
        LTRACE(LT_INFO, 2,
               "B-pyramid level " << m_frameDepth - 1 << " detected. Shift DTS to " << m_frameDepth << " frames");
    }
}

int VVCStreamReader::toFullPicOrder(const VvcSliceHeader* slice, const int pic_bits)
{
    // An IDR picture starts the count again, and in VVC it carries its own pic_order_cnt_lsb, which need not be 0: the
    // picture order count of an IDR picture is that value. The base used to be the IDR's place alone, so an IDR with
    // a value of 8 and every picture after it were presented 8 frames late, over the pictures of the IDR before.
    if (slice->isIDR())
    {
        m_picOrderBase = m_frameNum - static_cast<int>(slice->pic_order_cnt_lsb);
        m_picOrderMsb = 0;
        m_prevPicOrder = static_cast<int>(slice->pic_order_cnt_lsb);
    }
    else
    {
        const int range = 1 << pic_bits;

        if (slice->pic_order_cnt_lsb < m_prevPicOrder && m_prevPicOrder - slice->pic_order_cnt_lsb >= range / 2)
            m_picOrderMsb += range;
        else if (slice->pic_order_cnt_lsb > m_prevPicOrder && slice->pic_order_cnt_lsb - m_prevPicOrder >= range / 2)
            m_picOrderMsb -= range;

        m_prevPicOrder = slice->pic_order_cnt_lsb;
    }

    return slice->pic_order_cnt_lsb + m_picOrderMsb + m_picOrderBase;
}

int VVCStreamReader::guessPicOrder(const VvcUnit::NalType nalType)
{
    // As in the HEVC reader. An IDR picture starts the count again at its own place, whatever its lost
    // pic_order_cnt_lsb was (toFullPicOrder puts an IDR there), so its time is exact. Any other picture takes the order
    // of the picture at the same place in the GOP before; the next time in sequence is only the last resort.
    if (nalType == VvcUnit::NalType::IDR_W_RADL || nalType == VvcUnit::NalType::IDR_N_LP)
    {
        m_slice->pic_order_cnt_lsb = 0;
        return toFullPicOrder(m_slice, m_sps ? m_sps->log2_max_pic_order_cnt_lsb : 0);
    }
    // An IRAP picture starts a GOP, so the GOP before it is the one still open.
    const bool irap = nalType >= VvcUnit::NalType::IDR_W_RADL && nalType <= VvcUnit::NalType::CRA;
    const std::vector<int>& before = irap ? m_gopOrderOffsets : m_prevGopOrderOffsets;
    const size_t place = irap ? 0 : m_gopOrderOffsets.size();
    if (place < before.size())
        return m_frameNum + before[place];
    return m_frameNum;
}

void VVCStreamReader::notePicOrder(const VvcUnit::NalType nalType)
{
    static constexpr size_t MAX_GOP_PICTURES = 512;  // a stream with one IRAP only does not grow it without end
    if (nalType >= VvcUnit::NalType::IDR_W_RADL && nalType <= VvcUnit::NalType::CRA)
    {
        m_prevGopOrderOffsets.swap(m_gopOrderOffsets);
        m_gopOrderOffsets.clear();
    }
    if (m_gopOrderOffsets.size() < MAX_GOP_PICTURES)
        m_gopOrderOffsets.push_back(m_fullPicOrder - m_frameNum);
}

// A parameter set that ends before its fields do is passed through as it came and the last good one stays in use;
// before the first good one, the pictures wait for it as they always did.
template <typename Unit>
void VVCStreamReader::keepLastGoodUnit(Unit*& unit, MemoryBlock& lastGood, const char* name)
{
    if (m_damagedUnitWarnings++ == 0)
        LTRACE(LT_WARN, 2,
               "Warning: VVC: a damaged " << name << " at picture " << m_totalFrameNum
                                          << " is passed through as it is. The last good one stays in use.");
    if (lastGood.size() > 0)
    {
        unit->decodeBuffer(lastGood.data(), lastGood.data() + lastGood.size());
        unit->deserialize();
    }
    else
    {
        delete unit;
        unit = nullptr;
    }
}

// dataEnd is where the start code of the next NAL begins, so the NAL is everything before it, less the zero bytes a
// stream may put in front of a start code. It used to step back one byte first, as if dataEnd were past the next
// start code, and so every stored parameter set and SEI lost its last byte: the VPS, SPS and PPS written at the
// start of a split part or a cut, and the HDR metadata repeated at every IRAP, could not be read.
void VVCStreamReader::storeBuffer(MemoryBlock& dst, const uint8_t* data, const uint8_t* dataEnd)
{
    while (dataEnd > data && dataEnd[-1] == 0) dataEnd--;
    if (dataEnd > data)
    {
        dst.resize(static_cast<int>(dataEnd - data));
        memcpy(dst.data(), data, dataEnd - data);
    }
}

int VVCStreamReader::intDecodeNAL(uint8_t* buff)
{
    int rez;
    bool sliceFound = false;
    m_spsPpsFound = false;
    m_lastIFrame = false;

    bool phInAu = false;  // this access unit has a picture header unit
    const uint8_t* prevPos = nullptr;
    uint8_t* curPos = buff;
    uint8_t* nextNal = NALUnit::findNextNAL(curPos, m_bufEnd);

    if (!m_eof && nextNal == m_bufEnd)
        return NOT_ENOUGH_BUFFER;

    while (curPos < m_bufEnd)
    {
        // The unit type is the 5 bits after the layer id in the SECOND byte. It used to be read with the HEVC formula,
        // from the first byte, so most units were taken for something else and pictures were split or merged: 11 of
        // 15 test streams came out with the wrong number of pictures, and a two-layer stream with none at all.
        const auto nalType = static_cast<VvcUnit::NalType>(curPos[1] >> 3);
        if ((curPos[0] & 0x3f) != 0)
            ;  // a unit of a layer above the base layer belongs to the access unit around it
        else if (isSlice(nalType))
        {
            // A picture begins at its picture header. When the slice carries it (the first bit of the slice header),
            // the slice is the picture's only one, so a second such slice begins the next picture; otherwise the
            // picture header unit before the slices began it, and is handled with the other prefix units below.
            const bool phInSlice = curPos[2] & 0x80;
            if (sliceFound && phInSlice)
            {  // first slice of next frame: case where there is no non-VCL NAL between the two frames
                m_lastDecodedPos = prevPos;  // next frame started
                incTimings();
                return 0;
            }
            if (!sliceFound)
            {
                // first slice of current frame. nextNal points past the next start code: the header is read up to
                // that start code, or a header cut short would read the start code's zero bytes as its own fields.
                const uint8_t* sliceEnd = nextNal;
                if (nextNal < m_bufEnd)
                    sliceEnd = nextNal[-4] == 0 ? nextNal - 4 : nextNal - 3;
                m_slice->decodeBuffer(curPos, FFMIN(curPos + MAX_SLICE_HEADER, sliceEnd));
                rez = m_slice->deserialize(m_sps, m_pps);
                const bool irap = nalType >= VvcUnit::NalType::IDR_W_RADL && nalType <= VvcUnit::NalType::CRA;
                // A damaged picture header, as for HEVC, keeps its picture, as it came, and gives it an order.
                // That includes the first slice of an access unit with no picture header at all, neither its own
                // unit nor in the slice: the flag that said it carries one was lost with the rest of its header.
                if (!phInSlice && !phInAu)
                    rez = UNIT_DAMAGED;
                if (rez == UNIT_DAMAGED || (rez == 0 && !m_slice->picture_header_in_slice && m_phDamaged))
                {
                    if (m_damagedSliceWarnings++ == 0)
                        LTRACE(LT_WARN, 2,
                               "Warning: VVC: the header of picture "
                                   << m_totalFrameNum << " is damaged. The picture "
                                   << "is kept, with the time of the picture at its place in the GOP before.");
                    if (irap)
                        m_lastIFrame = true;
                    m_fullPicOrder = guessPicOrder(nalType);
                }
                else
                {
                    if (rez)
                        return rez;  // not enough buffer or error
                    if (irap)
                        m_lastIFrame = true;
                    if (!m_slice->picture_header_in_slice)
                        m_slice->pic_order_cnt_lsb = m_phPocLsb;
                    m_fullPicOrder = toFullPicOrder(m_slice, m_sps->log2_max_pic_order_cnt_lsb);
                }
                notePicOrder(nalType);
            }
            sliceFound = true;
        }
        else if (!isSuffix(nalType))
        {  // first non-VCL prefix NAL (AUD, SEI, picture header...) following current frame
            if (sliceFound)
            {
                incTimings();
                m_lastDecodedPos = prevPos;  // next frame started
                return 0;
            }

            uint8_t* nextNalWithStartCode = nextNal[-4] == 0 ? nextNal - 4 : nextNal - 3;

            switch (nalType)
            {
            case VvcUnit::NalType::VPS:
                if (!m_vps)
                    m_vps = new VvcVpsUnit();  // keepLastGoodUnit drops a damaged first one
                m_vps->decodeBuffer(curPos, nextNalWithStartCode);
                rez = m_vps->deserialize();
                if (rez == UNIT_DAMAGED)
                {
                    keepLastGoodUnit(m_vps, m_vpsBuffer, "VPS");
                    break;
                }
                if (rez)
                    return rez;
                m_spsPpsFound = true;
                m_vpsCounter++;
                m_vpsSizeDiff = 0;
                if (m_vps->m_vps_hrd.num_units_in_tick)
                    updateFPS(m_vps, curPos, nextNalWithStartCode, 0);
                // A rewritten unit can end elsewhere: the new timing may need a different number of emulation
                // prevention bytes. The copy kept for repeating the unit has to end where the unit now ends.
                nextNal += m_vpsSizeDiff;
                storeBuffer(m_vpsBuffer, curPos, nextNalWithStartCode + m_vpsSizeDiff);
                break;
            case VvcUnit::NalType::SPS:
                if (!m_sps)
                    m_sps = new VvcSpsUnit();
                m_sps->decodeBuffer(curPos, nextNalWithStartCode);
                rez = m_sps->deserialize();
                if (rez == UNIT_DAMAGED)
                {
                    keepLastGoodUnit(m_sps, m_spsBuffer, "SPS");
                    break;
                }
                if (rez)
                    return rez;
                m_spsPpsFound = true;
                m_vpsSizeDiff = 0;  // named for the VPS, it carries the size change of whichever unit was rewritten
                updateFPS(m_sps, curPos, nextNalWithStartCode, 0);
                nextNal += m_vpsSizeDiff;
                storeBuffer(m_spsBuffer, curPos, nextNalWithStartCode + m_vpsSizeDiff);
                break;
            case VvcUnit::NalType::PPS:
                if (!m_pps)
                    m_pps = new VvcPpsUnit();
                m_pps->decodeBuffer(curPos, nextNalWithStartCode);
                rez = m_pps->deserialize();
                if (rez == UNIT_DAMAGED)
                {
                    keepLastGoodUnit(m_pps, m_ppsBuffer, "PPS");
                    break;
                }
                if (rez)
                    return rez;
                m_spsPpsFound = true;
                storeBuffer(m_ppsBuffer, curPos, nextNalWithStartCode);
                break;
            case VvcUnit::NalType::PH:
                // The slices after it take their picture order count from it.
                phInAu = true;
                m_slice->decodeBuffer(curPos, FFMIN(curPos + MAX_SLICE_HEADER, nextNalWithStartCode));
                m_phDamaged = m_slice->deserializePictureHeader(m_sps) != 0;
                m_phPocLsb = m_slice->pic_order_cnt_lsb;
                break;
            default:
                break;
            }
        }
        prevPos = curPos;
        curPos = nextNal;
        nextNal = NALUnit::findNextNAL(curPos, m_bufEnd);

        if (!m_eof && nextNal == m_bufEnd)
            return NOT_ENOUGH_BUFFER;
    }
    if (m_eof)
    {
        // The last access unit of the stream has no NAL after it to announce that a new frame started, so it was
        // never timed or counted (the same step the HEVC reader has).
        if (sliceFound)
            incTimings();
        m_lastDecodedPos = m_bufEnd;
        return 0;
    }
    return NEED_MORE_DATA;
}

uint8_t* VVCStreamReader::writeNalPrefix(uint8_t* curPos) const
{
    if (!m_shortStartCodes)
        *curPos++ = 0;
    *curPos++ = 0;
    *curPos++ = 0;
    *curPos++ = 1;
    return curPos;
}

uint8_t* VVCStreamReader::writeBuffer(MemoryBlock& srcData, uint8_t* dstBuffer, const uint8_t* dstEnd) const
{
    if (srcData.isEmpty())
        return dstBuffer;
    const size_t bytesLeft = dstEnd - dstBuffer;
    const size_t requiredBytes = srcData.size() + 3 + (m_shortStartCodes ? 0 : 1);
    if (bytesLeft < requiredBytes)
        return dstBuffer;

    dstBuffer = writeNalPrefix(dstBuffer);
    memcpy(dstBuffer, srcData.data(), srcData.size());
    dstBuffer += srcData.size();
    return dstBuffer;
}

int VVCStreamReader::writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                                       PriorityDataInfo* priorityData)
{
    uint8_t* curPos = dstBuffer;

    if (avPacket.size > 4 && avPacket.size < dstEnd - dstBuffer)
    {
        const int offset = avPacket.data[2] == 1 ? 3 : 4;
        const auto nalType = static_cast<VvcUnit::NalType>(avPacket.data[offset + 1] >> 3);
        if (nalType == VvcUnit::NalType::AUD)
        {
            // place delimiter at first place
            memcpy(curPos, avPacket.data, avPacket.size);
            curPos += avPacket.size;
            avPacket.size = 0;
            avPacket.data = nullptr;
        }
    }

    const bool needInsSpsPps = m_firstFileFrame && !(avPacket.flags & AVPacket::IS_SPS_PPS_IN_GOP);
    if (needInsSpsPps)
    {
        avPacket.flags |= AVPacket::IS_SPS_PPS_IN_GOP;

        curPos = writeBuffer(m_vpsBuffer, curPos, dstEnd);
        curPos = writeBuffer(m_spsBuffer, curPos, dstEnd);
        curPos = writeBuffer(m_ppsBuffer, curPos, dstEnd);
    }

    m_firstFileFrame = false;
    return static_cast<int>(curPos - dstBuffer);
}
