#include "hevcStreamReader.h"

#include <fs/systemlog.h>

#include <cmath>
#include <memory>

#include "bitStream.h"
#include "hevc.h"
#include "nalUnits.h"
#include "tsMuxer.h"
#include "tsPacket.h"
#include "vodCoreException.h"

using namespace std;

static constexpr int MAX_SLICE_HEADER = 64;

HEVCStreamReader::HEVCStreamReader()
    : m_vps(nullptr),
      m_sps(nullptr),
      m_pps(nullptr),
      m_hdr(new HevcHdrUnit()),
      m_slice(new HevcSliceHeader()),
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
      m_vpsSizeDiff(0),
      m_forcedLevel(0),
      m_levelChangeReported(false),
      m_auHasMasteringSei(false),
      m_auHasCllSei(false),
      m_hdrSeiRepeatReported(false),
      m_auMinSliceType(-1),
      m_audInsertReported(false)
{
}

void HEVCStreamReader::applyForcedLevel(HevcUnitWithProfile* unit, uint8_t* buff, const uint8_t* nextNal)
{
    if (m_forcedLevel == 0 || unit->level_idc == m_forcedLevel)
        return;

    if (!m_levelChangeReported)
    {
        LTRACE(LT_INFO, 2,
               "Change HEVC level from " << unit->level_idc / 30 << '.' << unit->level_idc % 30 / 3 << " to "
                                         << m_forcedLevel / 30 << '.' << m_forcedLevel % 30 / 3);
        m_levelChangeReported = true;
    }

    const int oldNalSize = static_cast<int>(nextNal - buff);
    const uint8_t previousLevel = unit->level_idc;
    if (!unit->setLevel(m_forcedLevel))
        return;

    const auto tmpBuffer = std::make_unique<uint8_t[]>(unit->nalBufferLen() + 16);
    const int newNalSize = unit->serializeBuffer(tmpBuffer.get(), tmpBuffer.get() + unit->nalBufferLen() + 16);

    // The rewrite is deliberately restricted to the same-length case, and every real level fits it:
    // emulation prevention is inserted before a byte <= 3 that follows two zero bytes, and level_idc
    // is at least 30 (level 1.0), so swapping one legal level for another can neither create nor
    // remove an escape. A different length therefore means something unexpected, and shifting the
    // rest of the buffer for a cosmetic field is not worth the risk, so the stream is left alone.
    if (newNalSize != oldNalSize)
    {
        LTRACE(LT_WARN, 2,
               "HEVC level change would resize the NAL from " << oldNalSize << " to " << newNalSize
                                                              << " bytes, leaving the level unchanged");
        unit->setLevel(previousLevel);
        return;
    }
    memcpy(buff, tmpBuffer.get(), newNalSize);
}

// A source that writes mastering_display_colour_volume and content_light_level_info ONCE, at the
// head of the stream, leaves a player with no HDR10 metadata at all after a seek. Every pressed disc
// repeats both at EVERY IRAP. This puts them back.
//
// Nothing is fabricated: the bytes are the stream's own, captured when they first went past. A
// source that already repeats them is left alone, because then the per access unit flags are set.
// Only Dolby Vision is unaffected by the omission, since its RPU carries per frame metadata, which
// is exactly why the gap is easy to miss.
//
// WHERE IT GOES MATTERS. The insertion point is immediately BEFORE the first slice, not after the
// PPS. By that point every prefix NAL of this access unit has been seen, so whether the source
// already supplied them is KNOWN rather than guessed. It is still inside the prefix SEI region, so
// the order the standard requires still holds: AUD, then VPS SPS PPS, then prefix SEI, then slices.
//
// The buffer move is the one updateStreamFps already uses, with the same bound on TMP_BUFFER_SIZE.
int HEVCStreamReader::repeatHdrSeiAtIrap(uint8_t* slicePos)
{
    if (!(V3_flags & BLURAY_OUT))
        return 0;
    const bool needMaster = !m_auHasMasteringSei && m_masteringSeiBuffer.size() > 0;
    const bool needCll = !m_auHasCllSei && m_cllSeiBuffer.size() > 0;
    if (!needMaster && !needCll)
        return 0;

    const int need = static_cast<int>((needMaster ? 4 + m_masteringSeiBuffer.size() : 0) +
                                      (needCll ? 4 + m_cllSeiBuffer.size() : 0));

    // the start code in front of this slice, three or four bytes long
    uint8_t* at = slicePos - 3;
    if (at > m_tmpBuffer && at[-1] == 0)
        at--;
    if (at < m_tmpBuffer || m_bufEnd + need > m_tmpBuffer + TMP_BUFFER_SIZE)
        return 0;  // nowhere to put it, or no room: leave the stream exactly as it is

    memmove(at + need, at, m_bufEnd - at);
    m_bufEnd += need;

    uint8_t* w = at;
    for (int pass = 0; pass < 2; ++pass)
    {
        const MemoryBlock* blk =
            pass == 0 ? (needMaster ? &m_masteringSeiBuffer : nullptr) : (needCll ? &m_cllSeiBuffer : nullptr);
        if (!blk)
            continue;
        *w++ = 0;
        *w++ = 0;
        *w++ = 0;
        *w++ = 1;
        memcpy(w, blk->data(), blk->size());
        w += blk->size();
    }

    if (!m_hdrSeiRepeatReported)
    {
        m_hdrSeiRepeatReported = true;
        LTRACE(LT_INFO, 2, "HEVC HDR metadata appears only once: repeating it at every IRAP");
    }
    return need;
}

HEVCStreamReader::~HEVCStreamReader()
{
    delete m_vps;
    delete m_sps;
    delete m_pps;
    delete m_hdr;
    delete m_slice;
}

void HEVCStreamReader::applyDiscoveryData(const StreamDiscoveryData& data)
{
    if (data.fps > 0.0 && m_fps == 0.0)
        setFPS(data.fps);
    // Restore what the probe found. Only ever turn these ON: the probe saw more of the stream
    // than the muxer has by header-writing time, so its answer is the better one, but a flag the
    // muxer has already set for itself must not be cleared.
    if (m_hdr)
    {
        if (data.isDVRPU)
            m_hdr->isDVRPU = true;
        if (data.isDVEL)
            m_hdr->isDVEL = true;
    }
}

void HEVCStreamReader::fillVideoDiscoveryData(StreamDiscoveryData& data)
{
    MPEGStreamReader::fillVideoDiscoveryData(data);
    if (m_hdr)
    {
        data.isDVRPU = m_hdr->isDVRPU;
        data.isDVEL = m_hdr->isDVEL;
    }
}

CheckStreamRez HEVCStreamReader::checkStream(uint8_t* buffer, const int len)
{
    CheckStreamRez rez;

    uint8_t* end = buffer + len;
    for (uint8_t* nal = NALUnit::findNextNAL(buffer, end); nal < end - 4; nal = NALUnit::findNextNAL(nal, end))
    {
        if (*nal & 0x80)
            return rez;  // invalid nal
        const auto nalType = static_cast<HevcUnit::NalType>((*nal >> 1) & 0x3f);
        uint8_t* nextNal = NALUnit::findNALWithStartCode(nal, end, true);
        if (!m_eof && nextNal == end)
            break;

        switch (nalType)
        {
        case HevcUnit::NalType::VPS:
            if (!m_vps)
                m_vps = new HevcVpsUnit();
            m_vps->decodeBuffer(nal, nextNal);
            if (m_vps->deserialize() != 0)
                return rez;
            m_spsPpsFound = true;
            if (m_vps->num_units_in_tick)
            {
                // Lightweight FPS extraction for probing only. The full
                // updateFPS() (with logging and 25fps fallback) runs later
                // during actual muxing in intDecodeNAL().
                const double fps = correctFps(m_vps->getFPS());
                if (fps > 0.0 && m_fps == 0.0)
                    setFPS(fps);
            }
            break;
        case HevcUnit::NalType::SPS:
            if (!m_sps)
                m_sps = new HevcSpsUnit();
            m_sps->decodeBuffer(nal, nextNal);
            if (m_sps->deserialize() != 0)
                return rez;
            m_spsPpsFound = true;
            {
                const double fps = correctFps(getStreamFPS(m_sps));
                if (fps > 0.0 && m_fps == 0.0)
                    setFPS(fps);
            }
            break;
        case HevcUnit::NalType::PPS:
            if (!m_pps)
                m_pps = new HevcPpsUnit();
            m_pps->decodeBuffer(nal, nextNal);
            if (m_pps->deserialize() != 0)
                return rez;
            break;
        case HevcUnit::NalType::SEI_PREFIX:
            // Same guard as intDecodeNAL: one malformed SEI must not take down the probe. This
            // path runs first and is the one an "analyze" hits, so leaving it unguarded meant a
            // bad SEI still killed the whole run before muxing even started. The SEI is read
            // only to detect HDR metadata; the NAL itself passes through untouched either way.
            try
            {
                m_hdr->decodeBuffer(nal, nextNal);
                if (m_hdr->deserialize() != 0)
                    return rez;
            }
            catch (BitStreamException&)
            {
                if (m_seiParseWarns++ == 0)
                    LTRACE(LT_WARN, 2, "HEVC: malformed SEI prefix NAL ignored while probing the stream");
            }
            break;
        case HevcUnit::NalType::DVRPU:
        case HevcUnit::NalType::DVEL:
            if (nal[1] == 1)
            {
                if (nalType == HevcUnit::NalType::DVEL)
                    m_hdr->isDVEL = true;
                else
                    m_hdr->isDVRPU = true;
                V3_flags |= DV;
            }
            break;
        default:
            break;
        }

        // check Frame Depth on first slices
        if (isSlice(nalType) && (nal[2] & 0x80))
        {
            m_slice->decodeBuffer(nal, FFMIN(nal + MAX_SLICE_HEADER, nextNal));
            if (m_slice->deserialize(m_sps, m_pps))
                return rez;  // not enough buffer or error
            m_fullPicOrder = toFullPicOrder(m_slice, m_sps->log2_max_pic_order_cnt_lsb);
            incTimings();
        }
    }
    m_totalFrameNum = m_frameNum = m_fullPicOrder = 0;
    m_curDts = m_curPts = 0;

    // Set HDR10 flag if PQ detected
    if (m_vps && m_sps && m_pps && m_sps->vps_id == m_vps->vps_id && m_pps->sps_id == m_sps->sps_id)
    {
        if (m_sps->colour_primaries == 9 && m_sps->transfer_characteristics == 16 &&
            m_sps->matrix_coeffs == 9)  // SMPTE.ST.2084 (PQ)
        {
            m_hdr->isHDR10 = true;
            V3_flags |= HDR10;
        }

        rez.codecInfo = hevcCodecInfo;
        rez.streamDescr = m_sps->getDescription();
        const size_t frSpsPos = rez.streamDescr.find("Frame rate: not found");
        if (frSpsPos != string::npos)
            rez.streamDescr = rez.streamDescr.substr(0, frSpsPos) + string(" ") + m_vps->getDescription();
        // Say which track actually carries the Dolby Vision data. With profile 7 the base layer
        // is plain HDR10 and only the second track holds the RPU, which is otherwise impossible
        // to tell apart in this listing.
        // Deliberately reports presence only. Telling full from minimal enhancement layer means
        // reading disable_residual_flag, which sits about 65 bits into the RPU behind
        // emulation-prevention removal and ue(v) decoding. The tempting 4-bit field near the
        // start is vdr_rpu_profile, which distinguishes the profile family (4/5 from 7/8) and is
        // NOT the layer type, so using it labels a genuine minimal layer as full.
        if (m_hdr->isDVRPU || m_hdr->isDVEL)
        {
            rez.streamDescr += " Dolby Vision";
            if (m_hdr->isDVRPU)
                rez.streamDescr += " RPU";
            if (m_hdr->isDVEL)
                rez.streamDescr += " EL";

            // Both together means this ONE track holds a whole dual layer disc: the base layer,
            // the enhancement layer wrapped in unspecified NALs, and an RPU per picture. Saying so
            // is what lets the track be listed as its two layers and separated again without a
            // hand written meta file.
            //
            // The pair is the honest test. An RPU alone is a single layer profile 5 or 8 track,
            // which has nothing to separate; and a disc's own enhancement layer track carries its
            // NALs UNWRAPPED, so it never sets the enhancement flag here and cannot be mistaken
            // for a merged one.
            if (m_hdr->isDVRPU && m_hdr->isDVEL)
                rez.multiSubStream = true;
        }
    }

    return rez;
}

int HEVCStreamReader::getTSDescriptor(uint8_t* dstBuff, const bool blurayMode, const bool hdmvDescriptors)
{
    if (m_firstFrame)
        CheckStreamRez rez = checkStream(m_buffer, static_cast<int>(m_bufEnd - m_buffer));

    int lenDoviDesc = 0;
    if (!blurayMode && m_hdr->isDVRPU)
    {
        // 'DOVI' registration descriptor
        *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);
        *dstBuff++ = 4;  // descriptor length
        *dstBuff++ = 'D';
        *dstBuff++ = 'O';
        *dstBuff++ = 'V';
        *dstBuff++ = 'I';
        lenDoviDesc += 6;
    }

    if (hdmvDescriptors)
    {
        // 'HDMV' registration descriptor
        *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::HDMV);  // descriptor tag
        *dstBuff++ = 8;                                            // descriptor length
        *dstBuff++ = 'H';
        *dstBuff++ = 'D';
        *dstBuff++ = 'M';
        *dstBuff++ = 'V';
        *dstBuff++ = 0xff;  // stuffing byte

        *dstBuff++ = static_cast<uint8_t>(StreamType::VIDEO_H265);  // stream_conding_type
        uint8_t video_format, frame_rate_index, aspect_ratio_index;
        M2TSStreamInfo::blurayStreamParams(getFPS(), getInterlaced(), getStreamWidth(), getStreamHeight(),
                                           getStreamAR(), &video_format, &frame_rate_index, &aspect_ratio_index);

        *dstBuff++ = static_cast<uint8_t>(video_format << 4 | frame_rate_index);
        *dstBuff++ = static_cast<uint8_t>(aspect_ratio_index << 4 | 0xf);
    }
    else
    {
        uint8_t tmpBuffer[512];

        for (uint8_t* nal = NALUnit::findNextNAL(m_buffer, m_bufEnd); nal < m_bufEnd - 4;
             nal = NALUnit::findNextNAL(nal, m_bufEnd))
        {
            const auto nalType = static_cast<HevcUnit::NalType>((*nal >> 1) & 0x3f);
            const uint8_t* nextNal = NALUnit::findNALWithStartCode(nal, m_bufEnd, true);

            if (nalType == HevcUnit::NalType::SPS)
            {
                const int toDecode = FFMIN(sizeof(tmpBuffer) - 8, (unsigned)(nextNal - nal));
                NALUnit::decodeNAL(nal, nal + toDecode, tmpBuffer, sizeof(tmpBuffer));
                break;
            }
        }

        *dstBuff++ = static_cast<int>(TSDescriptorTag::HEVC);
        *dstBuff++ = 13;  // descriptor length
        memcpy(dstBuff, tmpBuffer + 3, 12);
        dstBuff += 12;
        // flags temporal_layer_subset, HEVC_still_present,
        // HEVC_24hr_picture_present, HDR_WCG unspecified
        *dstBuff++ = m_sps->sub_pic_hrd_params_present_flag ? 0x0f : 0x1f;

        /* HEVC_timing_and_HRD_descriptor
        // mandatory for interlaced video only
        memcpy(dstBuff, "\x3f\x0f\x03\x7f\x7f", 5);
        dstBuff += 5;

        uint32_t N = 1001 * getFPS();
        uint32_t K = 27000000;
        uint32_t num_units_in_tick = 1001;
        if (N % 1000)
        {
            N = 1000 * getFPS();
            num_units_in_tick = 1000;
        }
        N = my_htonl(N);
        K = my_htonl(K);
        num_units_in_tick = my_htonl(num_units_in_tick);
        memcpy(dstBuff, &N, 4);
        dstBuff += 4;
        memcpy(dstBuff, &K, 4);
        dstBuff += 4;
        memcpy(dstBuff, &num_units_in_tick, 4);
        dstBuff += 4;
        */
    }

    if (!blurayMode && m_hdr->isDVRPU)
        lenDoviDesc += setDoViDescriptor(dstBuff);

    return (hdmvDescriptors ? 10 : 15) + lenDoviDesc;
}

// Split out of setDoViDescriptor so the same derivation can also build the Matroska Dolby Vision
// configuration record, which wants exactly these fields in a different container. Behaviour of
// the Blu-ray descriptor is unchanged; it just calls this first.
// The profile and compatibility table, lifted out of getDoViParams UNCHANGED so that the Matroska
// dual layer record can consult the same one table instead of carrying a second copy that would
// drift. The only edit is that the enhancement layer question is now an argument rather than
// m_hdr->isDVEL, because a merged Matroska track has to be described from the ENHANCEMENT layer's
// stream while its picture geometry comes from the base layer.
void HEVCStreamReader::doViProfileAndCompatibility(const bool isEnhancementLayer, int& profile,
                                                   int& compatibility) const
{
    // cf. "http://www.dolby.com/us/en/technologies/dolby-vision/dolby-vision-profiles-levels.pdf"
    // "For profiles 7, 8.1 and 8.4, VUI parameters are required, as bitstreams employing these profiles
    // have a non-SDR base layer. For other Dolby Vision profiles, VUI parameters are optional."
    if (m_sps->bit_depth_luma_minus8 == 2)  // 10-bit
    {
        if (isEnhancementLayer)
        {
            if (m_sps->transfer_characteristics == 16)  // PQ
            {
                if (m_sps->chroma_sample_loc_type_top_field == 2)  // Blu-ray
                {
                    profile = 7;
                    compatibility = 6;
                }
                else  // CTA HDR10
                {
                    profile = 6;
                    compatibility = 1;
                }
            }
            else  // unspecified, assumed DV IPT
            {
                profile = 4;
                compatibility = 2;
            }
        }
        else  // single BL layer
        {
            switch (m_sps->transfer_characteristics)
            {
            case 16:  // PQ
                profile = 8;
                compatibility = 1;
                break;
            case 14:  // HLG-DVB
            case 18:  // HLG-ARIB
                profile = 8;
                compatibility = 4;
                break;
            case 1:  // SDR
                profile = 8;
                compatibility = 2;
                break;
            default:  // unspecified, assumed DV IPT
                profile = 5;
                compatibility = 0;
                V3_flags |= BL_NOTCOMPAT;
            }
        }
    }
    else  // 8-bit
    {
        if (m_sps->transfer_characteristics == 1)  // SDR
        {
            profile = 2;
            compatibility = 2;
        }
        else  // unspecified, assumed DV IPT
        {
            profile = 3;
            compatibility = 0;
        }
    }
}

// The Dolby level ladder, also lifted out unchanged. A pure function of picture width and pixel
// rate, so it can be asked about the BASE layer's geometry from anywhere.
int HEVCStreamReader::doViLevelFor(const unsigned width, const uint32_t pixelRate)
{
    int level = 0;
    if (width <= 1280 && pixelRate <= 22118400)
        level = 1;
    else if (width <= 1280 && pixelRate <= 27648000)
        level = 2;
    else if (width <= 1920 && pixelRate <= 49766400)
        level = 3;
    else if (width <= 2560 && pixelRate <= 62208000)
        level = 4;
    else if (width <= 3840 && pixelRate <= 124416000)
        level = 5;
    else if (width <= 3840 && pixelRate <= 199065600)
        level = 6;
    else if (width <= 3840 && pixelRate <= 248832000)
        level = 7;
    else if (width <= 3840 && pixelRate <= 398131200)
        level = 8;
    else if (width <= 3840 && pixelRate <= 497664000)
        level = 9;
    else if (width <= 3840 && pixelRate <= 995328000)
        level = 10;
    else if (width <= 7680 && pixelRate <= 995328000)
        level = 11;
    else if (width <= 7680 && pixelRate <= 1990656000)
        level = 12;
    else if (width <= 7680 && pixelRate <= 3981312000)
        level = 13;
    return level;
}

// The picture rate this stream's own geometry implies, in the units the level ladder uses.
uint32_t HEVCStreamReader::doViPixelRate() const
{
    // The Dolby level thresholds are defined AS exact rates, e.g. 199065600 is precisely
    // 3840 x 2160 x 24. getFPS() carries a little floating point noise, 24 arrives as
    // 24.0000004, and a UHD 24p stream then computed 199065603, three over the boundary, and was
    // declared level 7 instead of 6. Rounding the frame rate to a thousandth removes the noise
    // without disturbing any real rate: 23.976 and 29.97 keep their exact values.
    const double fps = std::round(getFPS() * 1000.0) / 1000.0;
    return static_cast<uint32_t>(getStreamWidth() * getStreamHeight() * fps);
}

bool HEVCStreamReader::getDoViParams(int& profile, int& level, int& compatibility, bool& isDVBLOut) const
{
    if (!m_sps || !m_hdr)
        return false;
    const bool isDVBL = (V3_flags & BL_TRACK) == 0;
    isDVBLOut = isDVBL;
    if (!isDVBL)
        m_hdr->isDVEL = true;

    unsigned width = getStreamWidth();
    auto pixelRate = doViPixelRate();

    if (!isDVBL && V3_flags & FOUR_K)
    {
        width *= 2;
        pixelRate *= 4;
    }

    doViProfileAndCompatibility(m_hdr->isDVEL, profile, compatibility);
    level = doViLevelFor(width, pixelRate);

    return true;
}

// The Matroska Dolby Vision configuration record: dvcC for profiles up to 7, dvvC above. 24
// bytes, the same profile / level / flags the Blu-ray descriptor carries, then reserved zeroes.
// Returns the block addition ID type (the fourcc as a uint) or 0 when the stream is not DV.
uint32_t HEVCStreamReader::buildDoViConfigRecord(uint8_t* dst) const
{
    if (!m_hdr || (!m_hdr->isDVRPU && !m_hdr->isDVEL))
        return 0;
    int profile = 0;
    int level = 0;
    int compatibility = 0;
    bool isDVBL = false;
    if (!getDoViParams(profile, level, compatibility, isDVBL))
        return 0;

    memset(dst, 0, 24);
    BitStreamWriter w{};
    w.setBuffer(dst, dst + 24);
    w.putBits(8, 1);  // dv_version_major
    w.putBits(8, 0);  // dv_version_minor
    w.putBits(7, profile);
    w.putBits(6, level);
    w.putBits(1, m_hdr->isDVRPU);  // rpu_present_flag
    w.putBits(1, m_hdr->isDVEL);   // el_present_flag
    w.putBits(1, 1);               // bl_present_flag: the base layer is always in the merged track
    w.putBits(4, compatibility);   // dv_bl_signal_compatibility_id
    w.putBits(28, 0);              // reserved
    w.flushBits();

    return profile > 7 ? 0x64767643 /* dvvC */ : 0x64766343 /* dvcC */;
}

// The record for a MERGED dual layer track, where the base layer and the enhancement layer share
// one Matroska track. Called on the BASE layer reader, with the enhancement layer's reader.
//
// Deliberately NOT routed through getDoViParams, and that is the whole point of it existing: that
// function decides "am I an enhancement layer" from the process global V3_flags & BL_TRACK, which
// the Blu-ray muxer sets and the Matroska path never does. Asked here it answered profile 8,
// level 3, el_present 0 for a profile 7 dual layer disc, a record wrong in three ways at once.
// Everything below is instead stated from what is genuinely known at this point:
//     profile and compatibility   from the ENHANCEMENT layer's SPS, which is what makes it dual
//     level                       from the BASE layer's geometry, because that is the picture
//     bl / el / rpu present       all 1, because by construction all three are in this one track
uint32_t HEVCStreamReader::buildDoViConfigRecordDualLayer(uint8_t* dst, const HEVCStreamReader& el) const
{
    if (m_sps == nullptr || el.m_sps == nullptr)
        return 0;

    int profile = 0;
    int compatibility = 0;
    el.doViProfileAndCompatibility(true, profile, compatibility);
    const int level = doViLevelFor(getStreamWidth(), doViPixelRate());

    memset(dst, 0, 24);
    BitStreamWriter w{};
    w.setBuffer(dst, dst + 24);
    w.putBits(8, 1);  // dv_version_major
    w.putBits(8, 0);  // dv_version_minor
    w.putBits(7, profile);
    w.putBits(6, level);
    w.putBits(1, 1);              // rpu_present_flag
    w.putBits(1, 1);              // el_present_flag
    w.putBits(1, 1);              // bl_present_flag
    w.putBits(4, compatibility);  // dv_bl_signal_compatibility_id
    w.putBits(28, 0);             // reserved
    w.flushBits();

    return profile > 7 ? 0x64767643 /* dvvC */ : 0x64766343 /* dvcC */;
}

// The record for a track whose RPU has been CONVERTED to profile 8.1 (--dv-profile=8.1). Called on
// the base layer reader, like the dual layer one, but it describes something different: a single
// layer profile 8.1 stream, cross compatible with HDR10, which is what the converted RPU makes it.
//
// el_present is 0 and that is the whole point. The enhancement layer is still physically present in
// the track, wrapped in unspecified NAL type 63 so a decoder skips it, but it is no longer part of
// what the player is being asked to decode. Declaring el_present 1 here would describe a dual layer
// stream and defeat the conversion; the enhancement layer is carried for rebuilding the disc later,
// not for playback.
//
// compatibility 1 means the base layer is HDR10, which is what profile 8.1 is, and is what lets a
// display without Dolby Vision show the picture correctly.
uint32_t HEVCStreamReader::buildDoViConfigRecordProfile81(uint8_t* dst) const
{
    if (m_sps == nullptr)
        return 0;

    constexpr int profile = 8;
    constexpr int compatibility = 1;
    const int level = doViLevelFor(getStreamWidth(), doViPixelRate());

    memset(dst, 0, 24);
    BitStreamWriter w{};
    w.setBuffer(dst, dst + 24);
    w.putBits(8, 1);  // dv_version_major
    w.putBits(8, 0);  // dv_version_minor
    w.putBits(7, profile);
    w.putBits(6, level);
    w.putBits(1, 1);              // rpu_present_flag
    w.putBits(1, 0);              // el_present_flag: single layer, see above
    w.putBits(1, 1);              // bl_present_flag
    w.putBits(4, compatibility);  // dv_bl_signal_compatibility_id: HDR10 compatible
    w.putBits(28, 0);             // reserved
    w.flushBits();

    return 0x64767643;  // dvvC, which is what profile 8 uses
}

int HEVCStreamReader::setDoViDescriptor(uint8_t* dstBuff) const
{
    int profile = 0;
    int level = 0;
    int compatibility = 0;
    bool isDVBL = false;
    if (!getDoViParams(profile, level, compatibility, isDVBL))
        return 0;

    BitStreamWriter bitWriter{};
    bitWriter.setBuffer(dstBuff, dstBuff + 128);

    bitWriter.putBits(8, 0xb0);            // DoVi descriptor tag
    bitWriter.putBits(8, isDVBL ? 5 : 7);  // descriptor length
    bitWriter.putBits(8, 1);               // dv version major
    bitWriter.putBits(8, 0);               // dv version minor
    bitWriter.putBits(7, profile);         // dv profile
    bitWriter.putBits(6, level);           // dv level
    bitWriter.putBits(1, m_hdr->isDVRPU);  // rpu_present_flag
    bitWriter.putBits(1, m_hdr->isDVEL);   // el_present_flag
    bitWriter.putBits(1, isDVBL);          // bl_present_flag
    if (!isDVBL)
    {
        bitWriter.putBits(13, 0x1011);  // dependency_pid
        bitWriter.putBits(3, 7);        // reserved
    }
    bitWriter.putBits(4, compatibility);  // dv_bl_sigHevcUnit::NalType::compatibility_id
    bitWriter.putBits(4, 15);             // reserved

    bitWriter.flushBits();
    return isDVBL ? 7 : 9;
}

void HEVCStreamReader::updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int)
{
    // ** THIS IS HANDED THE SPS AS WELL AS THE VPS, AND THEY ARE SIBLINGS. **
    //
    // intDecodeNAL calls updateFPS with m_vps at the VPS branch and with m_sps at the SPS one, and
    // the cast below used to take whatever arrived. HevcSpsUnit is not a parent or a child of
    // HevcVpsUnit, it is a SIBLING, so casting one to the other and reading
    // num_units_in_tick_bit_pos read whatever member of the SPS happens to lie at that offset.
    //
    // Measured on ONE unchanged file over eight runs: 0, 256 and 512. When the value came out 0 it
    // warned "cannot override FPS in stream" and the fps was not written; when it came out 256 or
    // 512 it said nothing and tried a 32 bit write at that bit position in the SPS. Same input,
    // same binary, different answer, which is what undefined behaviour looks like from outside.
    //
    // The SPS has no setFPS, only a getFPS, so there was never anything here for it to do. The
    // rest of updateFPS still runs for the SPS and still reads its frame rate; only the rewrite,
    // which is the VPS's alone, is skipped.
    if (nalUnit != m_vps)
        return;

    const int oldNalSize = static_cast<int>(nextNal - buff);
    m_vpsSizeDiff = 0;
    const auto vps = static_cast<HevcVpsUnit*>(nalUnit);
    if (!vps->setFPS(m_fps))
    {
        // FPS override failed (missing timing info or buffer too small).
        // Leave the stream unmodified rather than crashing.
        return;
    }
    auto tmpBuffer = std::make_unique<uint8_t[]>(vps->nalBufferLen() + 16);
    const int newSpsLen = vps->serializeBuffer(tmpBuffer.get(), tmpBuffer.get() + vps->nalBufferLen() + 16);
    if (newSpsLen == -1)
        THROW(ERR_COMMON, "Not enough buffer")

    if (m_bufEnd && newSpsLen != oldNalSize)
    {
        m_vpsSizeDiff = newSpsLen - oldNalSize;
        if (m_bufEnd + m_vpsSizeDiff > m_tmpBuffer + TMP_BUFFER_SIZE)
            THROW(ERR_COMMON, "Not enough buffer")
        memmove(nextNal + m_vpsSizeDiff, nextNal, m_bufEnd - nextNal);
        m_bufEnd += m_vpsSizeDiff;
    }
    memcpy(buff, tmpBuffer.get(), newSpsLen);
}

// Both return the DISPLAYED size, i.e. the coded size minus the conformance window, which is what
// H.264 has always done (nalUnits.h subtracts getCropX()/getCropY()). HEVC used to return the raw
// coded size, so a stream whose encoder padded 2160 lines up to 2176 was reported, and written
// into Matroska, as 16 lines taller than the picture actually is. Every consumer wants the
// displayed size: the Matroska track header, the aspect ratio derived in
// MPEGStreamReader::fillDiscoveryData, and the Dolby Vision level's pixel rate. The Blu-ray
// video_format is chosen from the WIDTH for anything above SD, so it is unaffected.
//
// Offsets are in chroma units, hence the subsampling factors.
unsigned HEVCStreamReader::getStreamWidth() const { return m_sps ? m_sps->getDisplayWidth() : 0; }

unsigned HEVCStreamReader::getStreamHeight() const { return m_sps ? m_sps->getDisplayHeight() : 0; }

// The SPS VUI fields default to 2, "unspecified", and are only overwritten when the stream
// actually carries a colour_description_present_flag. So all-2 means the stream said nothing and
// there is nothing worth declaring in the container.
bool HEVCStreamReader::getColourDesc(uint8_t& primaries, uint8_t& transfer, uint8_t& matrix) const
{
    if (!m_sps)
        return false;
    primaries = m_sps->colour_primaries;
    transfer = m_sps->transfer_characteristics;
    matrix = m_sps->matrix_coeffs;
    return primaries != 2 || transfer != 2 || matrix != 2;
}

int HEVCStreamReader::getStreamHDR() const
{
    return (m_hdr->isDVRPU || m_hdr->isDVEL) ? 4 : (m_hdr->isHDR10plus ? 16 : (m_hdr->isHDR10 ? 2 : 1));
}

double HEVCStreamReader::getStreamFPS(void* curNalUnit)
{
    double fps = 0;
    if (m_vps)
        fps = m_vps->getFPS();
    if (fps == 0.0 && m_sps)
        fps = m_sps->getFPS();
    return fps;
}

bool HEVCStreamReader::skipNal(uint8_t* nal)
{
    const auto nalType = static_cast<HevcUnit::NalType>((*nal >> 1) & 0x3f);

    if (nalType == HevcUnit::NalType::FD)
        return true;

    if ((nalType == HevcUnit::NalType::EOS || nalType == HevcUnit::NalType::EOB))
    {
        if (!m_eof || m_bufEnd - nal > 4)
        {
            return true;
        }
    }

    return false;
}

bool HEVCStreamReader::isSlice(const HevcUnit::NalType nalType) const
{
    if (!m_sps || !m_vps || !m_pps)
        return false;
    return (nalType >= HevcUnit::NalType::TRAIL_N && nalType <= HevcUnit::NalType::RASL_R) ||
           (nalType >= HevcUnit::NalType::BLA_W_LP && nalType <= HevcUnit::NalType::RSV_IRAP_VCL23);
}

bool HEVCStreamReader::isSuffix(const HevcUnit::NalType nalType) const
{
    if (!m_sps || !m_vps || !m_pps)
        return false;
    return (nalType == HevcUnit::NalType::FD || nalType == HevcUnit::NalType::SEI_SUFFIX ||
            nalType == HevcUnit::NalType::RSV_NVCL45 ||
            (nalType >= HevcUnit::NalType::RSV_NVCL45 && nalType <= HevcUnit::NalType::RSV_NVCL47) ||
            (nalType >= HevcUnit::NalType::UNSPEC56 && nalType <= HevcUnit::NalType::DVEL));
}

void HEVCStreamReader::incTimings()
{
    if (m_totalFrameNum++ > 0)
        m_curDts += m_pcrIncPerFrame;
    const int delta = m_frameNum - m_fullPicOrder;
    m_curPts = m_curDts - delta * m_pcrIncPerFrame;
    m_frameNum++;
    m_firstFrame = false;

    if (delta > m_frameDepth)
    {
        m_frameDepth = FFMIN(4, delta);
        LTRACE(LT_INFO, 2,
               "B-pyramid level " << m_frameDepth - 1 << " detected. Shift DTS to " << m_frameDepth << " frames");
    }
}

int HEVCStreamReader::toFullPicOrder(const HevcSliceHeader* slice, const unsigned pic_bits)
{
    if (slice->isIDR())
    {
        m_picOrderBase = m_frameNum;
        m_picOrderMsb = 0;
        m_prevPicOrder = 0;
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

void HEVCStreamReader::storeBuffer(MemoryBlock& dst, const uint8_t* data, const uint8_t* dataEnd)
{
    dataEnd--;
    while (dataEnd > data && dataEnd[-1] == 0) dataEnd--;
    if (dataEnd > data)
    {
        dst.resize(static_cast<int>(dataEnd - data));
        memcpy(dst.data(), data, dataEnd - data);
    }
}

int HEVCStreamReader::intDecodeNAL(uint8_t* buff)
{
    int rez = 0;
    bool sliceFound = false;
    m_spsPpsFound = false;
    m_lastIFrame = false;
    m_auHasMasteringSei = false;
    m_auHasCllSei = false;

    const uint8_t* prevPos = nullptr;
    uint8_t* curPos = buff;
    uint8_t* nextNal = NALUnit::findNextNAL(curPos, m_bufEnd);

    if (!m_eof && nextNal == m_bufEnd)
        return NOT_ENOUGH_BUFFER;

    while (curPos < m_bufEnd)
    {
        const auto nalType = static_cast<HevcUnit::NalType>((*curPos >> 1) & 0x3f);
        if (isSlice(nalType))
        {
            if (curPos[2] & 0x80)  // slice.first_slice
            {
                if (sliceFound)
                {  // first slice of next frame: case where there is no non-VCL NAL between the two frames
                    m_lastDecodedPos = prevPos;  // next frame started
                    incTimings();
                    return 0;
                }
                // first slice of current frame. Before parsing it, put back any HDR SEI this
                // access unit should carry: every prefix NAL of it has now been seen, so what the
                // source already supplied is known rather than guessed. The slice moves forward by
                // whatever was inserted in front of it.
                if (nalType >= HevcUnit::NalType::BLA_W_LP)
                {
                    const int hdrAdded = repeatHdrSeiAtIrap(curPos);
                    curPos += hdrAdded;
                    nextNal += hdrAdded;
                }
                m_slice->decodeBuffer(curPos, FFMIN(curPos + MAX_SLICE_HEADER, nextNal));
                rez = m_slice->deserialize(m_sps, m_pps);
                if (rez)
                    return rez;  // not enough buffer or error
                if (nalType >= HevcUnit::NalType::BLA_W_LP)
                    m_lastIFrame = true;
                // B is 0, P is 1, I is 2, so the lowest value seen is the most permissive and
                // is exactly what pic_type has to declare in a generated delimiter.
                if (m_slice->slice_type <= 2 &&
                    (m_auMinSliceType < 0 || static_cast<int>(m_slice->slice_type) < m_auMinSliceType))
                    m_auMinSliceType = static_cast<int>(m_slice->slice_type);
                m_fullPicOrder = toFullPicOrder(m_slice, m_sps->log2_max_pic_order_cnt_lsb);
            }
            sliceFound = true;
        }
        else if (!isSuffix(nalType))
        {  // first non-VCL prefix NAL (AUD, SEI...) following current frame
            if (sliceFound)
            {
                incTimings();
                m_lastDecodedPos = prevPos;  // next frame started
                return 0;
            }

            uint8_t* nextNalWithStartCode = nextNal[-4] == 0 ? nextNal - 4 : nextNal - 3;

            switch (nalType)
            {
            case HevcUnit::NalType::VPS:
                if (!m_vps)
                    m_vps = new HevcVpsUnit();
                m_vps->decodeBuffer(curPos, nextNalWithStartCode);
                rez = m_vps->deserialize();
                if (rez)
                    return rez;
                m_spsPpsFound = true;
                m_vpsCounter++;
                m_vpsSizeDiff = 0;
                if (m_vps->num_units_in_tick)
                    updateFPS(m_vps, curPos, nextNalWithStartCode, 0);
                // After the fps rewrite, because that one can move the end of the NAL.
                applyForcedLevel(m_vps, curPos, nextNalWithStartCode + m_vpsSizeDiff);
                nextNal += m_vpsSizeDiff;
                storeBuffer(m_vpsBuffer, curPos, nextNalWithStartCode);
                break;
            case HevcUnit::NalType::SPS:
                if (!m_sps)
                    m_sps = new HevcSpsUnit();
                m_sps->decodeBuffer(curPos, nextNalWithStartCode);
                rez = m_sps->deserialize();
                if (rez)
                    return rez;
                m_spsPpsFound = true;
                updateFPS(m_sps, curPos, nextNalWithStartCode, 0);
                // The level lives in the SPS as well as the VPS, and a player may read either.
                applyForcedLevel(m_sps, curPos, nextNalWithStartCode);
                storeBuffer(m_spsBuffer, curPos, nextNalWithStartCode);
                break;
            case HevcUnit::NalType::PPS:
                if (!m_pps)
                    m_pps = new HevcPpsUnit();
                m_pps->decodeBuffer(curPos, nextNalWithStartCode);
                rez = m_pps->deserialize();
                if (rez)
                    return rez;
                m_spsPpsFound = true;
                storeBuffer(m_ppsBuffer, curPos, nextNalWithStartCode);
                break;
            case HevcUnit::NalType::SEI_PREFIX:
            {
                // payload_type, read straight off the bytes after the 2 byte NAL header
                const uint8_t* pt = curPos + 2;
                int payloadType = 0;
                while (pt < nextNalWithStartCode && *pt == 0xFF)
                {
                    payloadType += 255;
                    ++pt;
                }
                if (pt < nextNalWithStartCode)
                    payloadType += *pt;
                if (payloadType == 137)  // mastering_display_colour_volume
                {
                    m_auHasMasteringSei = true;
                    storeBuffer(m_masteringSeiBuffer, curPos, nextNalWithStartCode);
                }
                else if (payloadType == 144)  // content_light_level_info
                {
                    m_auHasCllSei = true;
                    storeBuffer(m_cllSeiBuffer, curPos, nextNalWithStartCode);
                }
                try
                {
                    m_hdr->decodeBuffer(curPos, nextNal);
                    if (m_hdr->deserialize() != 0)
                        return rez;
                }
                catch (BitStreamException&)
                {
                    // the SEI is parsed only to detect HDR metadata; a malformed one
                    // must not abort the mux - the NAL passes through unchanged anyway
                    if (m_seiParseWarns++ == 0)
                        LTRACE(LT_WARN, 2, "HEVC: malformed SEI prefix NAL ignored (frame " << m_totalFrameNum << ")");
                }
                break;
            }
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
        // The last access unit of the stream has no NAL after it to announce that a new frame
        // started, which is what every other access unit relies on. Without this it never gets its
        // timings: it is appended to the PREVIOUS PES, with no timestamp and no delimiter of its
        // own, and it is not counted. A stream that ends with EOS, EOB or an access unit delimiter
        // already took the branch above when it reached that NAL, and arrives here with sliceFound
        // false, so its output is unchanged.
        if (sliceFound)
            incTimings();
        m_lastDecodedPos = m_bufEnd;
        return 0;
    }
    return NEED_MORE_DATA;
}

uint8_t* HEVCStreamReader::writeNalPrefix(uint8_t* curPos) const
{
    if (!m_shortStartCodes)
        *curPos++ = 0;
    *curPos++ = 0;
    *curPos++ = 0;
    *curPos++ = 1;
    return curPos;
}

uint8_t* HEVCStreamReader::writeBuffer(MemoryBlock& srcData, uint8_t* dstBuffer, const uint8_t* dstEnd) const
{
    if (srcData.isEmpty())
        return dstBuffer;
    const int bytesLeft = static_cast<int>(dstEnd - dstBuffer);
    const int requiredBytes = static_cast<int>(srcData.size()) + 3 + (m_shortStartCodes ? 0 : 1);
    if (bytesLeft < requiredBytes)
        return dstBuffer;

    dstBuffer = writeNalPrefix(dstBuffer);
    memcpy(dstBuffer, srcData.data(), srcData.size());
    dstBuffer += srcData.size();
    return dstBuffer;
}

int HEVCStreamReader::writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                                        PriorityDataInfo* priorityData)
{
    uint8_t* curPos = dstBuffer;
    bool audFound = false;

    if (avPacket.size > 4 && avPacket.size < dstEnd - dstBuffer)
    {
        const int offset = avPacket.data[2] == 1 ? 3 : 4;
        const auto nalType = static_cast<HevcUnit::NalType>((avPacket.data[offset] >> 1) & 0x3f);
        if (nalType == HevcUnit::NalType::AUD)
        {
            // place delimiter at first place
            memcpy(curPos, avPacket.data, avPacket.size);
            curPos += avPacket.size;
            avPacket.size = 0;
            avPacket.data = nullptr;
            audFound = true;
        }
    }

    // A source without access unit delimiters is legal HEVC, where the delimiter is optional, but
    // every pressed Blu-ray carries exactly one per picture in every video layer, and the H.264
    // reader has always inserted them when a source lacked them (h264StreamReader.cpp, the
    // m_delimiterFound path). HEVC never did, so an elementary stream from a source that omits them
    // was authored to disc without any. This closes that gap.
    //
    // pic_type must not over-claim: it declares the SET of slice types the picture may contain, so
    // announcing B for an all-I picture would be wrong. m_auMinSliceType carries the lowest
    // slice_type seen for this access unit, and the mapping is exactly 2 minus that value, giving
    // the same 0x10, 0x30 and 0x50 payload bytes a pressed disc uses. If no slice was seen, 2 is
    // the safe answer because its set contains every type.
    if (!audFound)
    {
        if (dstEnd - curPos < 7)
            THROW(ERR_COMMON, "HEVC stream error: not enough buffer to write an access unit delimiter")
        if (!m_audInsertReported)
        {
            m_audInsertReported = true;
            LTRACE(LT_INFO, 2, "HEVC bitstream has no access unit delimiters: inserting them");
        }
        const int picType = m_auMinSliceType < 0 ? 2 : 2 - m_auMinSliceType;
        *curPos++ = 0;
        *curPos++ = 0;
        *curPos++ = 0;
        *curPos++ = 1;
        *curPos++ = static_cast<uint8_t>(HevcUnit::NalType::AUD) << 1;  // 0x46, layer 0
        *curPos++ = 1;                                                  // temporal id plus 1
        *curPos++ = static_cast<uint8_t>(picType << 5 | 0x10);          // pic_type + rbsp trailing
    }
    m_auMinSliceType = -1;

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
