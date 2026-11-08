// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "hevcStreamReader.h"

#include <fs/systemlog.h>

#include <cmath>
#include <memory>
#include <vector>

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
      m_cllInMasteringSei(false),
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
// head of the stream, leaves a player with no HDR10 metadata at all after a seek. A UHD Blu-ray
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
    const bool needCll = !m_auHasCllSei && m_cllSeiBuffer.size() > 0 && !(needMaster && m_cllInMasteringSei);
    if (!needMaster && !needCll)
        return 0;

    const int need = static_cast<int>((needMaster ? 4 + m_masteringSeiBuffer.size() : 0) +
                                      (needCll ? 4 + m_cllSeiBuffer.size() : 0));

    // the start code in front of this slice, three or four bytes long
    uint8_t* at = slicePos - 3;
    if (at > m_tmpBuffer && at[-1] == 0)
        at--;
    if (at < m_tmpBuffer || m_bufEnd + need > m_tmpBuffer + m_tmpBufferSize)
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
    delete m_elVps;
    delete m_elSps;
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
    // The container's word that this is an enhancement layer (the layer half of a dual layer track) counts over the
    // first RPU read, which a profile 8.1 file has converted to single layer.
    if (m_dvRpuElPresent < 0 || data.dvRpuElPresent == 1)
        m_dvRpuElPresent = data.dvRpuElPresent;
}

void HEVCStreamReader::fillVideoDiscoveryData(StreamDiscoveryData& data)
{
    MPEGStreamReader::fillVideoDiscoveryData(data);
    if (m_hdr)
    {
        data.isDVRPU = m_hdr->isDVRPU;
        data.isDVEL = m_hdr->isDVEL;
    }
    data.dvRpuElPresent = m_dvRpuElPresent;
}

// A merged dual layer track carries the enhancement layer's NALs inside wrapper NALs. Two bytes
// off the front and what is left IS the NAL as the disc carried it, header included, still
// emulation prevented, which is the same unwrapping HevcDolbyVisionFilter does when it separates
// the layers for real. Only the parameter sets are taken, and only so the enhancement layer can be
// described with its own picture size rather than the base layer's.
//
// A failure here must cost nothing: the caller checks for a null and falls back to the base
// layer's description, which is what was always shown before.
void HEVCStreamReader::readElParameterSet(const uint8_t* nal, const uint8_t* nextNal)
{
    if (nextNal - nal < 4)
        return;
    const auto innerType = static_cast<HevcUnit::NalType>((nal[2] >> 1) & 0x3f);
    if (innerType != HevcUnit::NalType::VPS && innerType != HevcUnit::NalType::SPS)
        return;
    if (innerType == HevcUnit::NalType::VPS ? m_elVps != nullptr : m_elSps != nullptr)
        return;
    try
    {
        if (innerType == HevcUnit::NalType::VPS)
        {
            m_elVps = new HevcVpsUnit();
            m_elVps->decodeBuffer(nal + 2, nextNal);
            if (m_elVps->deserialize() != 0)
            {
                delete m_elVps;
                m_elVps = nullptr;
            }
        }
        else
        {
            m_elSps = new HevcSpsUnit();
            m_elSps->decodeBuffer(nal + 2, nextNal);
            if (m_elSps->deserialize() != 0)
            {
                delete m_elSps;
                m_elSps = nullptr;
            }
        }
    }
    catch (BitStreamException&)
    {
        // What the bit reader throws when a NAL runs out under it. Nothing is reported: this is a
        // probe, and the only thing lost is a resolution in a listing.
        if (innerType == HevcUnit::NalType::VPS)
        {
            delete m_elVps;
            m_elVps = nullptr;
        }
        else
        {
            delete m_elSps;
            m_elSps = nullptr;
        }
    }
}

CheckStreamRez HEVCStreamReader::checkStream(uint8_t* buffer, const int len)
{
    CheckStreamRez rez;
    // The pictures counted here are read again from the start by the mux, so the counters go back to zero on every way
    // out, not only at the end: an early return left them where the check stopped, and every picture of the mux came
    // out that many frames late.
    struct ResetCounters
    {
        HEVCStreamReader* r;
        ~ResetCounters()
        {
            r->m_totalFrameNum = r->m_frameNum = r->m_fullPicOrder = 0;
            r->m_curDts = r->m_curPts = 0;
            r->m_picOrderStarted = false;
            r->m_maxFullPicOrder = -1;
        }
    } resetCounters{this};
    // A parameter set is read into a unit of its own and kept only when it is whole: a damaged one (see
    // keepLastGoodUnit) is skipped here, and the one before it stays in use.
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
    bool auOpen = true;  // a delimiter, SEI or parameter set came after the last slice: the next slice begins a picture
    for (uint8_t* nal = NALUnit::findNextNAL(buffer, end); nal < end - 4; nal = NALUnit::findNextNAL(nal, end))
    {
        if (*nal & 0x80)
            return rez;  // invalid nal
        const auto nalType = static_cast<HevcUnit::NalType>((*nal >> 1) & 0x3f);
        uint8_t* nextNal = NALUnit::findNALWithStartCode(nal, end, true);
        if (!m_eof && nextNal == end)
            break;
        // A unit of a layer above the base layer (nuh_layer_id): multi-layer HEVC, which cannot be muxed
        // (refuseMultiLayer). It is not read: its pictures were counted as pictures of the stream, and the listing
        // gave hundreds of lines of B-pyramid depth for them.
        if ((nal[0] & 1) || (nal[1] >> 3))
        {
            if (static_cast<int>(nalType) < 32)  // VCL
                m_multiLayer = true;
            continue;
        }

        switch (nalType)
        {
        case HevcUnit::NalType::VPS:
        {
            const int r = readWhole(m_vps, new HevcVpsUnit(), nal, nextNal);
            if (r == UNIT_DAMAGED)
                break;
            if (r != 0)
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
        }
        case HevcUnit::NalType::SPS:
        {
            const int r = readWhole(m_sps, new HevcSpsUnit(), nal, nextNal);
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
        case HevcUnit::NalType::PPS:
        {
            const int r = readWhole(m_pps, new HevcPpsUnit(), nal, nextNal);
            if (r != 0 && r != UNIT_DAMAGED)
                return rez;
            break;
        }
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
                markHdr10ByMasteringSei();
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
                {
                    m_hdr->isDVEL = true;
                    readElParameterSet(nal, nextNal);
                }
                else
                {
                    m_hdr->isDVRPU = true;
                    if (m_dvRpuElPresent < 0)
                    {
                        HevcDvRpuUnit rpu;
                        rpu.decodeBuffer(nal, nextNal);
                        if (rpu.deserialize() == 0)
                            m_dvRpuElPresent = rpu.elPresent ? 1 : 0;
                    }
                }
                V3_flags |= DV;
            }
            break;
        default:
            break;
        }

        // check Frame Depth on first slices. As in the mux, the first slice of an access unit begins its picture even
        // when it does not say so.
        if (!isSlice(nalType) && !isSuffix(nalType))
            auOpen = true;
        else if (isSlice(nalType) && ((nal[2] & 0x80) || auOpen))
        {
            auOpen = false;
            m_slice->decodeBuffer(nal, FFMIN(nal + MAX_SLICE_HEADER, nextNal));
            int r = m_slice->deserialize(m_sps, m_pps);
            if (!(nal[2] & 0x80))
                r = UNIT_DAMAGED;
            // The same guess as the mux: an IDR picture starts the count again. Taking the next time in sequence for it
            // left the count of the IDR period before running, every later picture looked a dozen frames deep, and that
            // depth, which the mux keeps, moved every decoding time of the file.
            if (r == UNIT_DAMAGED)
                m_fullPicOrder = guessPicOrder(nalType);
            else if (r)
                return rez;  // not enough buffer or error
            else
            {
                if (!m_picOrderStarted && nalType < HevcUnit::NalType::BLA_W_LP)
                    m_firstLeading = (std::max)(0, picturesShownBeforeFirst(nal, end));
                if (m_picOrderStarted && nalType >= HevcUnit::NalType::BLA_W_LP &&
                    nalType <= HevcUnit::NalType::IDR_N_LP)
                    m_restartLeading = (std::max)(0, placesOfLeadingPictures(nal, end));
                m_fullPicOrder = toFullPicOrder(m_slice, m_sps->log2_max_pic_order_cnt_lsb);
            }
            incTimings();
        }
    }

    // Set HDR10 flag if PQ detected
    if (m_vps && m_sps && m_pps && m_sps->vps_id == m_vps->vps_id && m_pps->sps_id == m_sps->sps_id)
    {
        markHdr10ByMasteringSei();  // for a mastering display SEI read before the SPS
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
            {
                rez.multiSubStream = true;
                // The listing splits this one track into two rows, and the second row is a
                // DIFFERENT picture: a profile 7 disc pairs a 3840x2160 base layer with a
                // 1920x1080 enhancement layer. Both rows used to be described from m_sps, which
                // is the BASE layer's, so the enhancement layer was reported at the base layer's
                // resolution. Left empty when the enhancement layer's own SPS was not found, and
                // the caller then falls back to the description built above.
                if (m_elSps)
                {
                    std::string elDescr = m_elSps->getDescription();
                    const size_t frElPos = elDescr.find("Frame rate: not found");
                    if (frElPos != string::npos && m_elVps)
                        elDescr = elDescr.substr(0, frElPos) + string(" ") + m_elVps->getDescription();
                    // The same tail as the row above, because the separated track carries the RPU
                    // and the enhancement layer just as this one does.
                    rez.elStreamDescr = elDescr + " Dolby Vision RPU EL";
                }
            }
        }
        if (m_multiLayer && m_sps)
            rez.streamDescr += " Multi-layer (MV-HEVC): cannot be muxed";
    }

    return rez;
}

int HEVCStreamReader::getTSDescriptor(uint8_t* dstBuff, const bool blurayMode, const bool hdmvDescriptors)
{
    if (m_firstFrame)
        CheckStreamRez rez = checkStream(m_buffer, static_cast<int>(m_bufEnd - m_buffer));
    if (m_multiLayer)
        refuseMultiLayer();

    // A disc takes HEVC as progressive pictures only: coded as fields, a player cannot decode it. A demux asks with
    // blurayMode set as well, and was given the warning about a disc it does not write.
    if (blurayMode && !m_demuxMode && fieldCoded() && !m_fieldsOnDiscSaid)
    {
        LTRACE(LT_WARN, 2,
               "Warning: this HEVC stream is coded as fields (interlaced). A Blu-ray or UHD disc takes HEVC as "
               "progressive pictures only, and a disc player cannot play it.");
        m_fieldsOnDiscSaid = true;
    }

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
        // the rate the mux will take: before updateFPS has run, getFPS() is still 0 for a stream that names none
        M2TSStreamInfo::blurayStreamParams(headerFps(correctFps(getStreamFPS(nullptr))), getInterlaced(),
                                           getStreamWidth(), getStreamHeight(), getStreamAR(), &video_format,
                                           &frame_rate_index, &aspect_ratio_index);

        *dstBuff++ = static_cast<uint8_t>(video_format << 4 | frame_rate_index);
        *dstBuff++ = static_cast<uint8_t>(aspect_ratio_index << 4 | 0xf);
    }
    else if (m_sps == nullptr)
    {
        // The HEVC descriptor is built from the sequence parameter set. A stream in which none was found, because
        // it is not HEVC or not a kind tsMuxeR reads, has nothing to describe: it used to stop the program on the
        // spot. It gets no HEVC descriptor, and the mux goes on to the warning that the track produced no frames.
        return lenDoviDesc;
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
        // The last of the 12 is the level. A level given with level= is written into the stream only as it is muxed,
        // after this descriptor is made, and the descriptor kept the source's level.
        if (m_forcedLevel)
            dstBuff[11] = m_forcedLevel;
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
// configuration record, which wants exactly these fields in a different container. The transport
// stream descriptor now takes the profile table below directly, with the layers it carries.
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

// The Dolby Vision video stream descriptor of a transport stream that is not a disc. It describes the layers that
// travel on THIS PID: the base layer unless the muxer placed the stream as an enhancement layer beside its base
// (setDoViBaseLayerPid), and the enhancement layer when it is that one, or when it is wrapped into the same stream.
//
// It used to be built through getDoViParams, which takes the layer from V3_flags & BL_TRACK. The muxer sets that flag
// for the first base layer, and the descriptor is made again at every PMT, so a single layer stream (profile 5 or 8)
// was then described as an enhancement layer of itself: profile 7 or 4, no base layer, dependency_pid its own PID, and
// a level for twice its width. The last four bits were all 1, which readers of the current layout take as metadata
// compression 3, a value profile 7 does not allow.
int HEVCStreamReader::setDoViDescriptor(uint8_t* dstBuff) const
{
    if (!m_sps || !m_hdr)
        return 0;

    const bool blPresent = m_doViBaseLayerPid == 0;
    // A merged dual layer track keeps its enhancement layer in this stream, unless its RPU was made single layer
    const bool elPresent = !blPresent || (m_hdr->isDVEL && m_dvRpuElPresent != 0);
    int profile = 0;
    int compatibility = 0;
    doViProfileAndCompatibility(elPresent, profile, compatibility);
    // The level is that of the whole picture: a 4K disc's enhancement layer is a quarter of its base
    unsigned width = getStreamWidth();
    uint32_t pixelRate = doViPixelRate();
    if (!blPresent && V3_flags & FOUR_K)
    {
        width *= 2;
        pixelRate *= 4;
    }
    const int level = doViLevelFor(width, pixelRate);

    BitStreamWriter bitWriter{};
    bitWriter.setBuffer(dstBuff, dstBuff + 128);

    bitWriter.putBits(8, 0xb0);               // DoVi descriptor tag
    bitWriter.putBits(8, blPresent ? 5 : 7);  // descriptor length
    bitWriter.putBits(8, 1);                  // dv version major
    bitWriter.putBits(8, 0);                  // dv version minor
    bitWriter.putBits(7, profile);            // dv profile
    bitWriter.putBits(6, level);              // dv level
    bitWriter.putBits(1, m_hdr->isDVRPU);     // rpu_present_flag
    bitWriter.putBits(1, elPresent);          // el_present_flag
    bitWriter.putBits(1, blPresent);          // bl_present_flag
    if (!blPresent)
    {
        bitWriter.putBits(13, m_doViBaseLayerPid);  // dependency_pid
        bitWriter.putBits(3, 7);                    // reserved
    }
    bitWriter.putBits(4, compatibility);  // dv_bl_signal_compatibility_id
    bitWriter.putBits(2, 0);              // dv_md_compression: none, the RPUs are carried as they are
    bitWriter.putBits(2, 3);              // reserved

    bitWriter.flushBits();
    return blPresent ? 7 : 9;
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
    // Both units carry a copy of the timing and both are handed here, so each is asked to rewrite
    // its own. The SPS could not be until now: it had no setFPS at all, which is why a stream whose
    // VPS declares no timing kept its original rate whatever fps= asked for.
    m_vpsSizeDiff = 0;
    HevcUnit* unit = nullptr;
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

    // The unit carries no timing to change, or the write would not fit. Either way the stream is
    // left exactly as it came in.
    if (!rewritten)
        return;

    const int oldNalSize = static_cast<int>(nextNal - buff);
    auto tmpBuffer = std::make_unique<uint8_t[]>(unit->nalBufferLen() + 16);
    const int newSpsLen = unit->serializeBuffer(tmpBuffer.get(), tmpBuffer.get() + unit->nalBufferLen() + 16);
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

// Both return the DISPLAYED size, i.e. the coded size minus the conformance window, which is what
// H.264 has always done (nalUnits.h subtracts getCropX()/getCropY()). HEVC used to return the raw
// coded size, so a stream whose encoder padded 2160 lines up to 2176 was reported, and written
// into Matroska, as 16 lines taller than the picture actually is. Every consumer wants the
// displayed size: the Matroska track header, the aspect ratio derived in
// MPEGStreamReader::fillDiscoveryData, and the Dolby Vision level's pixel rate. The Blu-ray
// video_format is chosen from the WIDTH for anything above SD, so it is unaffected.
//
// Offsets are in chroma units, hence the subsampling factors.
double HEVCStreamReader::getSampleAspectRatio() const
{
    // the shape of the first SPS (noteSampleAspectRatio), or of the one known now where the mux has read none yet
    return m_sampleARKnown ? m_sampleAR : m_sps ? m_sps->sar : 0;
}

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

// Profiles 7, 8.1, 8.2 and 8.4 all leave a base layer that a plain player can show: HDR10, SDR or
// HLG. Profile 5 leaves nothing of the kind, because its picture is coded in IPT, so a player that
// does not understand Dolby Vision shows wrong colours rather than a plain picture. Nothing here can
// change that; reporting success without a word about it leaves it to be discovered on a television.
//
// The profile comes from the same table the descriptors use rather than from a second copy of its
// conditions. Its only side effect for profile 5 is V3_flags |= BL_NOTCOMPAT, which is true, and whose
// one use at tsMuxer.cpp:420 is commented out and left that way.
void HEVCStreamReader::warnIfDvHasNoFallback()
{
    if (m_warnedDvProfile5 || m_sps == nullptr || m_hdr == nullptr)
        return;
    // Only the RPU is asked about here. Whether there is an enhancement layer is the CALLER's question
    // and it has already answered it: this is asked only for a stream being written as the primary
    // video. Asking again was wrong as well as redundant, because a stream folded from a dual layer
    // source still reports an enhancement layer to the reader while being muxed as a single one.
    if (!m_hdr->isDVRPU)
        return;

    int profile = 0;
    int compatibility = 0;
    doViProfileAndCompatibility(false, profile, compatibility);
    if (compatibility != 0)  // 1 is HDR10, 2 is SDR, 4 is HLG: there is a picture to fall back on
        return;

    m_warnedDvProfile5 = true;
    LTRACE(LT_WARN, 2,
           "Warning: this is Dolby Vision profile "
               << profile
               << ", which carries no HDR10 or SDR fallback. A player that understands Dolby Vision will "
                  "be correct. A player that does not will show wrong colours rather than a plain "
                  "picture, because the picture itself is coded in IPT. The mux is written as asked: "
                  "nothing here can add a fallback that the stream does not have.");
}

// A mastering display SEI (137) marks the stream as HDR10, unless the VUI of its SPS gives a transfer of SDR video:
// BT.709 (1), BT.601 (6) or BT.2020 (14, 15). A UHD disc may carry SDR BT.709 video with such a SEI, and marks it
// as SDR with BT.709 colours; it was marked as HDR10 with BT.2020 colours, in its playlist, its clip information and
// its index. Nor does it mark HLG (18) as HDR10: HLG is a high dynamic range of its own, and a player that takes it
// for HDR10 shows it wrong. It is marked as the same stream without the SEI is, as hdr10-override leaves it. A stream
// that gives no transfer (2), or PQ (16), is HDR10 by the SEI as before: an HDR10 stream whose VUI says nothing keeps
// its marking. Asked again until the SPS is there.
void HEVCStreamReader::markHdr10ByMasteringSei()
{
    if (!m_hdr->hasMasteringDisplay || m_hdr->isHDR10 || !m_sps)
        return;
    const uint8_t transfer = m_sps->transfer_characteristics;
    if (transfer == 1 || transfer == 6 || transfer == 14 || transfer == 15 || transfer == 18)
        return;
    m_hdr->isHDR10 = true;
    V3_flags |= HDR10;
}

// ** MULTI-LAYER HEVC IS REFUSED. ** A picture of a layer above the base layer (nuh_layer_id above 0) belongs to
// multi-layer HEVC, MV-HEVC, as Dolby Vision profile 20 and stereoscopic 3D HEVC are coded: two views in each access
// unit. tsMuxeR reads single layer HEVC. Each view was muxed as a picture of its own, with times that could not be
// right (presented before decoded), at exit 0, and the Dolby Vision descriptor called it profile 8. A transport
// stream, a disc and Matroska have no way here to carry the second view, so the stream is refused.
// Data that is not HEVC at all, H.264, AC-3 or DTS on a V_MPEGH/ISO/HEVC line, has such a "layer" by chance in its
// first bytes, and was refused with this text about MV-HEVC and Dolby Vision. A stream with layers has a sequence
// parameter set of its base layer before any picture: without one, the data is refused as not HEVC.
void HEVCStreamReader::refuseMultiLayer() const
{
    if (!m_sps)
        THROW(ERR_COMMON, "Track " << m_streamIndex
                                   << " is not HEVC: no sequence parameter set came before its first picture, as "
                                      "one does in an HEVC stream. The usual reason is that the codec on the meta "
                                      "line is not the codec the file holds. List the file on its own to see what "
                                      "tsMuxeR makes of it.")
    THROW(ERR_COMMON, "Track " << m_streamIndex
                               << ": the HEVC stream has more than one layer (multi-layer HEVC, MV-HEVC, as Dolby "
                                  "Vision profile 20 and 3D HEVC are coded). tsMuxeR can mux single layer HEVC only, "
                                  "so it is not muxed.")
}

// A disc clip has one picture size. A stream whose size changes inside it (1080p, then 720p) went onto a disc as one
// clip with the first size in its clip information, without a word. Said once, for a disc only.
void HEVCStreamReader::warnSizeChangeOnDisc()
{
    const unsigned width = getStreamWidth();
    const unsigned height = getStreamHeight();
    if (m_firstSpsWidth == 0)
    {
        m_firstSpsWidth = width;
        m_firstSpsHeight = height;
        return;
    }
    if (m_sizeChangeSaid || (width == m_firstSpsWidth && height == m_firstSpsHeight) || !(V3_flags & BLURAY_OUT) ||
        m_demuxMode)
        return;
    m_sizeChangeSaid = true;
    LTRACE(LT_WARN, 2,
           "Warning: the HEVC stream changes its picture size from "
               << m_firstSpsWidth << "x" << m_firstSpsHeight << " to " << width << "x" << height
               << " within the stream. A disc clip has one picture size, and a disc player may show the pictures "
                  "after the change wrongly or stop. Mux the parts of different sizes as titles of their own.");
}

// An HEVC stream without a VPS has its SPS and its pictures, and none of them can be written without it. It was refused
// with the usual reason for an empty track, a codec on the meta line that is not the codec of the file, which is wrong.
std::string HEVCStreamReader::emptyTrackReason() const
{
    if (m_vps == nullptr && m_sps != nullptr)
        return "The stream is HEVC, but it has no video parameter set (VPS), which an HEVC stream carries before its "
               "first picture, so not one picture could be written. The file is damaged or was cut out of a stream "
               "without its parameter sets; take the stream again from its source.";
    return {};
}

int HEVCStreamReader::getStreamHDR() const
{
    const int hdr = (m_hdr->isDVRPU || m_hdr->isDVEL) ? 4 : (m_hdr->isHDR10plus ? 16 : (m_hdr->isHDR10 ? 2 : 1));
    // hdr10-override (#6): the stream says SDR, and the disc marks it as HDR10 all the same, in its playlist, its clip
    // information and its index. Only for SDR: HDR10, HDR10+ and Dolby Vision streams are marked as they are. The
    // picture is not changed: a player switches the screen to HDR, and the stream itself still says SDR.
    // Asked before the SPS is read, the stream is not known yet, and nothing is marked or said: a PQ stream got the
    // warning that it says SDR, "transfer characteristics -1", and was marked right later on.
    if (!m_hdr10Override || hdr != 1 || !m_sps)
        return hdr;
    // HLG is not SDR: it is a high dynamic range of its own, and a player that takes it for HDR10 shows it wrong. It
    // was marked as HDR10, with a warning that it said SDR (transfer characteristics 18).
    if (m_sps->transfer_characteristics == 18)
    {
        static bool saidHlg = false;  // once a run, as below
        if (!saidHlg)
            LTRACE(LT_WARN, 2,
                   "Warning: hdr10-override is ignored: the HEVC stream is HLG (transfer characteristics 18), not "
                   "SDR, and the disc marks it as it is.");
        saidHlg = true;
        return hdr;
    }
    static bool said = false;  // once a run: more than one reader of the track asks
    if (!said)
    {
        said = true;
        // PQ without the BT.2020 colours and matrix HDR10 names is not SDR either: it is said as it is. Neither is
        // 2, which means unspecified (a stream without a colour description has it): it was called SDR.
        const bool pq = m_sps->transfer_characteristics == 16;
        const bool unspecified = m_sps->transfer_characteristics == 2;
        LTRACE(LT_WARN, 2,
               "Warning: hdr10-override: the HEVC stream "
                   << (pq            ? "is PQ without the BT.2020 colours of HDR10"
                       : unspecified ? "does not say how its picture is coded"
                                     : "says SDR")
                   << " (transfer characteristics " << static_cast<int>(m_sps->transfer_characteristics)
                   << (unspecified ? ", unspecified" : "")
                   << "), and the disc marks it as HDR10 as asked. A player will switch the screen to HDR while the "
                      "picture is coded "
                   << (pq            ? "otherwise"
                       : unspecified ? "as the stream does not say"
                                     : "for SDR")
                   << "; check how it looks before using the disc.");
    }
    V3_flags |= HDR10;
    return 2;
}

// ** ANSWER FOR THE UNIT THAT WAS ASKED ABOUT. **
//
// The VPS and the SPS each carry a copy of the timing, and updateFPS compares this answer with the
// rate the user asked for to decide whether that unit needs rewriting. This ignored its argument
// and always answered with the VPS.
//
// The VPS comes first in the stream, so by the time the SPS arrived the VPS had already been
// rewritten to the new rate. The SPS was then compared against that new rate, matched it, and was
// left alone. The file went out carrying two timings that disagreed: the VPS said 25 and the SPS
// still said 23.976. A reader that takes the VPS reported 25, tsMuxeR reads the SPS and reported
// 23.976, and neither was wrong about what it had read.
//
// The fallback is kept for the other caller, which passes the SPS only as a formality and wants
// whatever rate the stream has. Only a unit that has a rate of its own now answers with it.
double HEVCStreamReader::getStreamFPS(void* curNalUnit)
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
    m_maxFullPicOrder = (std::max)(m_maxFullPicOrder, m_fullPicOrder);
    m_firstFrame = false;

    // How far a picture can be presented before it would be decoded, which is how far the decoding times are moved
    // back. It was held at 4 frames, and a stream that begins on a CRA picture needs more: the leading pictures behind
    // that CRA are decoded after it but shown before it, which adds their number to every later picture, and the
    // pictures past 4 were presented before they were decoded. 16 is the most pictures an HEVC decoder holds.
    if (delta > m_frameDepth)
    {
        m_frameDepth = FFMIN(16, delta);
        LTRACE(LT_INFO, 2,
               "B-pyramid level " << m_frameDepth - 1 << " detected. Shift DTS to " << m_frameDepth << " frames");
    }
}

// The pictures after the first picture of a stream that does not begin with an IRAP picture (a cut inside a run of B
// pictures) that are shown before it: they follow it in decode order with a lower order count. Counted up to the
// first picture shown after it. Returns -1 when more data is needed and can come.
int HEVCStreamReader::picturesShownBeforeFirst(uint8_t* nal, uint8_t* end) const
{
    const int range = 1 << m_sps->log2_max_pic_order_cnt_lsb;
    const int firstLsb = m_slice->pic_order_cnt_lsb;
    HevcSliceHeader slice;
    int leading = 0;
    for (uint8_t* cur = NALUnit::findNextNAL(nal, end); cur < end - 4; cur = NALUnit::findNextNAL(cur, end))
    {
        const auto type = static_cast<HevcUnit::NalType>((*cur >> 1) & 0x3f);
        if (!isSlice(type) || !(cur[2] & 0x80))
            continue;  // not the first slice of a picture
        if (type >= HevcUnit::NalType::BLA_W_LP)
            return leading;  // a random access picture: the count goes on from it
        uint8_t* next = NALUnit::findNALWithStartCode(cur, end, true);
        if (next == end && !m_eof)
            return -1;
        slice.decodeBuffer(cur, FFMIN(cur + MAX_SLICE_HEADER, next));
        if (slice.deserialize(m_sps, m_pps) != 0)
            return leading;
        int diff = static_cast<int>(slice.pic_order_cnt_lsb) - firstLsb;
        if (diff >= range / 2)
            diff -= range;
        else if (diff < -range / 2)
            diff += range;
        if (diff >= 0)
            return leading;
        leading++;
    }
    return m_eof ? leading : -1;
}

// The places in time the leading pictures of an IDR or BLA picture take before it: they follow it in decode order and
// are shown before it (RADL pictures, and the RASL pictures of a BLA picture), the first of them shown as many places
// before it as its order count is below the IRAP picture's. Counted up to the first picture that is not a leading
// picture. Returns -1 when more data is needed and can come; it is read no further than 32 MiB past the picture.
int HEVCStreamReader::placesOfLeadingPictures(uint8_t* nal, uint8_t* end) const
{
    static constexpr std::ptrdiff_t MAX_AHEAD = 32 * 1024 * 1024;
    const int range = 1 << m_sps->log2_max_pic_order_cnt_lsb;
    HevcSliceHeader slice;
    int irapLsb = 0;  // an IDR picture's order count
    const auto irapType = static_cast<HevcUnit::NalType>((*nal >> 1) & 0x3f);
    if (irapType >= HevcUnit::NalType::BLA_W_LP && irapType <= HevcUnit::NalType::BLA_N_LP)
    {
        uint8_t* next = NALUnit::findNALWithStartCode(nal, end, true);
        if (next == end && !m_eof)
            return -1;
        slice.decodeBuffer(nal, FFMIN(nal + MAX_SLICE_HEADER, next));
        if (slice.deserialize(m_sps, m_pps) != 0)
            return 0;
        irapLsb = static_cast<int>(slice.pic_order_cnt_lsb);
    }
    int places = 0;
    for (uint8_t* cur = NALUnit::findNextNAL(nal, end); cur < end - 4; cur = NALUnit::findNextNAL(cur, end))
    {
        const auto type = static_cast<HevcUnit::NalType>((*cur >> 1) & 0x3f);
        if (!isSlice(type) || !(cur[2] & 0x80))
            continue;  // not the first slice of a picture
        if (type < HevcUnit::NalType::RADL_N || type > HevcUnit::NalType::RASL_R)
            return places;
        uint8_t* next = NALUnit::findNALWithStartCode(cur, end, true);
        if (next == end && !m_eof && cur - nal < MAX_AHEAD)
            return -1;
        slice.decodeBuffer(cur, FFMIN(cur + MAX_SLICE_HEADER, next));
        if (slice.deserialize(m_sps, m_pps) != 0)
            return places;
        int diff = static_cast<int>(slice.pic_order_cnt_lsb) - irapLsb;
        if (diff >= range / 2)
            diff -= range;
        else if (diff < -range / 2)
            diff += range;
        places = (std::max)(places, -diff);
    }
    return m_eof || end - nal >= MAX_AHEAD ? places : -1;
}

// The depth incTimings will find in the period that begins at this IDR picture, from the pictures after it: up to 33
// of them or the next IDR or BLA picture, whichever comes first. Returns -1 when more data is needed and can come;
// it is read no further than 32 MiB past the picture.
int HEVCStreamReader::depthOfPeriod(uint8_t* nal, uint8_t* end) const
{
    static constexpr int MAX_PICTURES = 33;
    static constexpr std::ptrdiff_t MAX_AHEAD = 32 * 1024 * 1024;
    const int range = 1 << m_sps->log2_max_pic_order_cnt_lsb;
    int prevLsb = 0;  // an IDR picture's order count
    int order = 0;    // the order count from this picture's, as toFullPicOrder steps it
    int pictures = 0;
    int depth = 0;
    HevcSliceHeader slice;
    for (uint8_t* cur = NALUnit::findNextNAL(nal, end); cur < end - 4; cur = NALUnit::findNextNAL(cur, end))
    {
        const auto type = static_cast<HevcUnit::NalType>((*cur >> 1) & 0x3f);
        if (!isSlice(type) || !(cur[2] & 0x80))
            continue;  // not the first slice of a picture
        if (type >= HevcUnit::NalType::BLA_W_LP && type <= HevcUnit::NalType::IDR_N_LP)
            return depth;  // the next period
        uint8_t* next = NALUnit::findNALWithStartCode(cur, end, true);
        if (next == end && !m_eof && cur - nal < MAX_AHEAD)
            return -1;
        slice.decodeBuffer(cur, FFMIN(cur + MAX_SLICE_HEADER, next));
        if (slice.deserialize(m_sps, m_pps) != 0)
            return depth;
        int diff = static_cast<int>(slice.pic_order_cnt_lsb) - prevLsb;
        if (diff <= -range / 2)
            diff += range;
        else if (diff >= range / 2)
            diff -= range;
        prevLsb = static_cast<int>(slice.pic_order_cnt_lsb);
        order += diff;
        depth = (std::max)(depth, ++pictures - order);
        if (pictures >= MAX_PICTURES)
            return depth;
    }
    return m_eof || end - nal >= MAX_AHEAD ? depth : -1;
}

// The picture order count, on a scale where the first picture of the stream is at its own position. The count starts
// again at an IDR picture, whose count is 0, and at a BLA picture, whose count is its pic_order_cnt_lsb alone. It
// used to start only at an IDR, so a stream that begins on another picture, as a broadcast capture that begins on a
// CRA picture does, kept that picture's count as an offset in every presentation time until the first IDR, and to
// the end where there is none: on a stream measured here every picture was presented before it was decoded.
int HEVCStreamReader::toFullPicOrder(const HevcSliceHeader* slice, const unsigned pic_bits)
{
    const bool bla =
        slice->nal_unit_type >= HevcUnit::NalType::BLA_W_LP && slice->nal_unit_type <= HevcUnit::NalType::BLA_N_LP;
    // The first picture of a part cut from a clip of a playlist (a CRA picture, where the part does not start at an IDR
    // picture) starts the count again as a BLA picture does: its count goes on from the pictures before it in its
    // clip, which the cut left out, as do the pictures shown before it, which went with them (TSDemuxer::keptInPart)
    const bool partStart = slice->nal_unit_type >= HevcUnit::NalType::BLA_W_LP &&
                           slice->nal_unit_type <= HevcUnit::NalType::RSV_IRAP_VCL23 && isPartStart();
    if (slice->isIDR() || bla || !m_picOrderStarted || partStart || m_joinRestart)
    {
        // A Dolby Vision enhancement layer may lack a picture its base layer has (a disc's stream was measured with
        // one missing). The pictures were placed by counting them, so from the next IDR on every picture of the
        // enhancement layer sat one picture early: matched with the wrong base layer picture, all of the second file
        // of a join. The next IDR now goes where the pictures before it end, one past the latest place given, and
        // the decoding times skip the same gap. For a stream that lacks nothing that place is the count itself.
        // The same holds for any stream that lacks pictures, as a cut does: a recording that begins after its I
        // picture lacks the pictures that came before in decode order, a hole in the order shown, and the next IDR
        // was given a time a picture before it already had.
        if (m_picOrderStarted && m_maxFullPicOrder + 1 > m_frameNum)
        {
            const int gap = m_maxFullPicOrder + 1 - m_frameNum;
            if (!m_dvGapReported && !m_joinRestart)
                LTRACE(LT_INFO, 2,
                       (m_hdr->isDVRPU ? "Dolby Vision: the stream lacks " : "HEVC: the stream lacks ")
                           << gap << (gap == 1 ? " picture" : " pictures") << " before picture " << m_totalFrameNum
                           << "; the pictures after it keep their place in time.");
            m_dvGapReported = true;
            m_frameNum += gap;
            m_curDts += static_cast<int64_t>(gap) * m_pcrIncPerFrame;
        }
        // A first picture that is not IRAP goes after the pictures shown before it (picturesShownBeforeFirst). An IDR
        // or BLA picture after the first goes after its own leading pictures (placesOfLeadingPictures): the count of
        // an IDR picture with RADL pictures used to start again at its own place, so its RADL pictures took the times
        // of the last pictures before it, and the last times of the stream were given to no picture.
        int leading = (!m_picOrderStarted && slice->nal_unit_type < HevcUnit::NalType::BLA_W_LP) ||
                              (m_joinRestart && slice->nal_unit_type != HevcUnit::NalType::BLA_W_LP &&
                               slice->nal_unit_type != HevcUnit::NalType::BLA_W_RADL &&
                               slice->nal_unit_type != HevcUnit::NalType::BLA_N_LP)
                          ? m_firstLeading
                          : 0;
        if (m_picOrderStarted && !m_joinRestart && (slice->isIDR() || bla) && !partStart)
            leading = m_restartLeading;
        m_restartLeading = 0;
        m_joinRestart = false;
        m_picOrderStarted = true;
        m_picOrderBase = m_frameNum + leading - static_cast<int>(slice->pic_order_cnt_lsb);
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

int HEVCStreamReader::nextFullPicOrder(const HevcSliceHeader* slice) const
{
    const int range = 1 << m_sps->log2_max_pic_order_cnt_lsb;
    const int lsb = static_cast<int>(slice->pic_order_cnt_lsb);
    int msb = m_picOrderMsb;
    if (lsb < m_prevPicOrder && m_prevPicOrder - lsb >= range / 2)
        msb += range;
    else if (lsb > m_prevPicOrder && lsb - m_prevPicOrder >= range / 2)
        msb -= range;
    return lsb + msb + m_picOrderBase;
}

int HEVCStreamReader::guessPicOrder(const HevcUnit::NalType nalType)
{
    // A picture whose header is cut has no order to read. An IDR picture has none to read anyway: it is 0. Any other
    // picture takes the order of the picture at the same place in the GOP before, as an encoder repeats one GOP
    // pattern; the next time in sequence is only the last resort, as with B pictures it is the time of another picture.
    if (nalType == HevcUnit::NalType::IDR_W_RADL || nalType == HevcUnit::NalType::IDR_N_LP)
    {
        m_slice->pic_order_cnt_lsb = 0;
        return toFullPicOrder(m_slice, m_sps ? m_sps->log2_max_pic_order_cnt_lsb : 0);
    }
    // An IRAP picture starts a GOP, so the GOP before it is the one still open.
    const bool irap = nalType >= HevcUnit::NalType::BLA_W_LP && nalType <= HevcUnit::NalType::RSV_IRAP_VCL23;
    const std::vector<int>& before = irap ? m_gopOrderOffsets : m_prevGopOrderOffsets;
    const size_t place = irap ? 0 : m_gopOrderOffsets.size();
    if (place < before.size())
        return m_frameNum + before[place];
    return m_frameNum;
}

void HEVCStreamReader::notePicOrder(const HevcUnit::NalType nalType)
{
    static constexpr size_t MAX_GOP_PICTURES = 512;  // a stream with one IRAP only does not grow it without end
    if (nalType >= HevcUnit::NalType::BLA_W_LP && nalType <= HevcUnit::NalType::RSV_IRAP_VCL23)
    {
        m_prevGopOrderOffsets.swap(m_gopOrderOffsets);
        m_gopOrderOffsets.clear();
    }
    if (nalType >= HevcUnit::NalType::BLA_W_LP && nalType <= HevcUnit::NalType::IDR_N_LP)
        m_periodOrders.clear();
    if (m_periodOrders.size() >= 4096)
        m_periodOrders.erase(m_periodOrders.begin());
    m_periodOrders.insert(m_fullPicOrder);
    if (m_gopOrderOffsets.size() < MAX_GOP_PICTURES)
        m_gopOrderOffsets.push_back(m_fullPicOrder - m_frameNum);
}

// Keeps the mastering display (137) and content light level (144) SEI of a prefix SEI unit for
// repeatHdrSeiAtIrap. A unit may carry several sei_message()s (H.265 7.3.5), and these two may be
// any of them, so every message is walked, on the unit with its emulation prevention bytes removed.
//
// A unit that carries nothing but these two is kept whole, exactly as it is in the stream. A unit
// that also carries other messages (buffering period, picture timing, user data, ...) is not: those
// belong to the access unit they came with and must not be repeated elsewhere. Each of the two
// messages is then kept as a unit of its own: the same NAL header, the message's own bytes, and the
// rbsp trailing bits.
void HEVCStreamReader::captureHdrSei(const uint8_t* nal, const uint8_t* nalEnd)
{
    while (nalEnd > nal && nalEnd[-1] == 0) nalEnd--;
    if (nalEnd - nal < 4)
        return;
    std::vector<uint8_t> rbsp(nalEnd - nal);
    const int rbspLen = NALUnit::decodeNAL(nal, nalEnd, rbsp.data(), rbsp.size());
    if (rbspLen < 4)
        return;
    struct Msg
    {
        int type;
        size_t start, end;  // the whole sei_message: payload type and size bytes, then the payload
    };
    std::vector<Msg> msgs;
    bool others = false;
    size_t end = rbspLen;
    while (end > 2 && rbsp[end - 1] == 0) end--;
    size_t pos = 2;  // after the NAL header
    // more_rbsp_data(): what is left is more than the rbsp_stop_one_bit and its alignment zeros
    while (pos < end && !(pos + 1 == end && rbsp[pos] == 0x80))
    {
        const size_t start = pos;
        int type = 0;
        while (pos < end && rbsp[pos] == 0xFF)
        {
            type += 255;
            pos++;
        }
        size_t size = 0;
        if (pos < end)
            type += rbsp[pos++];
        while (pos < end && rbsp[pos] == 0xFF)
        {
            size += 255;
            pos++;
        }
        if (pos >= end || static_cast<size_t>(rbsp[pos]) + size > end - pos - 1)
        {
            // damaged from here on: the messages before it are kept, the rest is not repeated
            others = true;
            break;
        }
        size += rbsp[pos++];
        pos += size;
        if (type == 137 || type == 144)
            msgs.push_back({type, start, pos});
        else
            others = true;
    }
    if (msgs.empty())
        return;
    if (!others)
    {
        bool has137 = false, has144 = false;
        for (const auto& m : msgs)
        {
            has137 |= m.type == 137;
            has144 |= m.type == 144;
        }
        m_auHasMasteringSei |= has137;
        m_auHasCllSei |= has144;
        if (has137)
        {
            storeBuffer(m_masteringSeiBuffer, nal, nalEnd);
            m_cllInMasteringSei = has144;
        }
        else
            storeBuffer(m_cllSeiBuffer, nal, nalEnd);
        return;
    }
    for (const auto& m : msgs)
    {
        std::vector<uint8_t> unit;
        unit.push_back(rbsp[0]);
        unit.push_back(rbsp[1]);
        unit.insert(unit.end(), rbsp.begin() + m.start, rbsp.begin() + m.end);
        unit.push_back(0x80);  // rbsp_trailing_bits
        std::vector<uint8_t> coded(unit.size() * 3 / 2 + 4);
        const int codedLen = NALUnit::encodeNAL(unit.data(), unit.data() + unit.size(), coded.data(), coded.size());
        if (codedLen <= 0)
            continue;
        if (m.type == 137)
        {
            m_auHasMasteringSei = true;
            storeBuffer(m_masteringSeiBuffer, coded.data(), coded.data() + codedLen);
            m_cllInMasteringSei = false;
        }
        else
        {
            m_auHasCllSei = true;
            storeBuffer(m_cllSeiBuffer, coded.data(), coded.data() + codedLen);
        }
    }
}

// dataEnd is where the start code of the next NAL begins, so the NAL is everything before it, less the zero bytes a
// stream may put in front of a start code. It used to step back one byte first, as if dataEnd were past the next
// start code, and so every stored parameter set and SEI lost its last byte: the VPS, SPS and PPS written at the
// start of a split part or a cut, and the HDR metadata repeated at every IRAP, could not be read.
void HEVCStreamReader::storeBuffer(MemoryBlock& dst, const uint8_t* data, const uint8_t* dataEnd)
{
    while (dataEnd > data && dataEnd[-1] == 0) dataEnd--;
    if (dataEnd > data)
    {
        dst.resize(static_cast<int>(dataEnd - data));
        memcpy(dst.data(), data, dataEnd - data);
    }
}

// A parameter set that ends before its fields do (a recording that lost part of a packet) ended the whole job with
// "Bitstream exception", and nothing was written. Broadcasts and discs repeat their parameter sets, so the damaged one
// is passed through as it came and the last good one stays in use; before the first good one, the pictures wait for
// it as they always did.
template <typename Unit>
void HEVCStreamReader::keepLastGoodUnit(Unit*& unit, MemoryBlock& lastGood, const char* name)
{
    if (m_damagedUnitWarnings++ == 0)
        LTRACE(LT_WARN, 2,
               "Warning: HEVC: a damaged " << name << " at picture " << m_totalFrameNum
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
        if (static_cast<int>(nalType) < 32 && ((curPos[0] & 1) || (curPos[1] >> 3)))
            refuseMultiLayer();  // a picture of a layer above the base layer
        if (isSlice(nalType))
        {
            // The first slice of an access unit must say so (first_slice_segment_in_pic_flag). One that does not, after
            // the delimiter or SEI that began the access unit, has lost the flag with the rest of its header, or the
            // slice before it was lost. It used to be taken for a further slice of the picture before: the two
            // pictures were written as one, and every decoding time after them moved. It begins its picture now.
            const bool firstSliceFlag = curPos[2] & 0x80;
            if (firstSliceFlag || !sliceFound)
            {
                if (sliceFound)
                {  // first slice of next frame: case where there is no non-VCL NAL between the two frames
                    m_lastDecodedPos = prevPos;  // next frame started
                    incTimings();
                    return 0;
                }
                // An IDR or BLA picture after the first one starts the count again after its leading pictures, which
                // it has to know before it is given its order. Its period is that many places less deep than the
                // pictures after it are in decode order (depthOfPeriod counts from the IDR picture).
                const bool restart = nalType >= HevcUnit::NalType::BLA_W_LP && nalType <= HevcUnit::NalType::IDR_N_LP;
                m_restartLeading = 0;
                if (restart && m_picOrderStarted && !isPartStart())
                {
                    const int places = placesOfLeadingPictures(curPos, m_bufEnd);
                    if (places == -1)
                        return NOT_ENOUGH_BUFFER;
                    m_restartLeading = places;
                }
                // A period can have a deeper B pyramid than the pictures before it: a joined file coded otherwise.
                // Found only at its first deep picture, the larger depth moved the decoding time of that picture
                // back behind the times already given. The period is read ahead from its IDR picture, and the
                // decoding times catch up over its first pictures (deepenAfterOutput).
                if (nalType == HevcUnit::NalType::IDR_W_RADL || nalType == HevcUnit::NalType::IDR_N_LP)
                {
                    int ahead = depthOfPeriod(curPos, m_bufEnd);
                    if (ahead == -1)
                        return NOT_ENOUGH_BUFFER;
                    ahead = (std::min)(16, ahead - m_restartLeading);
                    if (ahead > m_frameDepth)
                    {
                        const int depthBefore = m_frameDepth;
                        m_frameDepth = ahead;
                        LTRACE(LT_INFO, 2,
                               "B-pyramid level " << m_frameDepth - 1 << " detected. Shift DTS to " << m_frameDepth
                                                  << " frames");
                        deepenAfterOutput(depthBefore);
                    }
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
                // nextNal points past the next start code: the header is read up to that start code, as the check
                // does, or a header cut short read the start code's zero bytes as its own fields and was taken
                // as whole.
                const uint8_t* sliceEnd = nextNal;
                if (nextNal < m_bufEnd)
                    sliceEnd = nextNal[-4] == 0 ? nextNal - 4 : nextNal - 3;
                m_slice->decodeBuffer(curPos, FFMIN(curPos + MAX_SLICE_HEADER, sliceEnd));
                rez = m_slice->deserialize(m_sps, m_pps);
                if (!firstSliceFlag)
                    rez = UNIT_DAMAGED;  // its own order is not to be trusted: a dependent segment carries none
                // A slice header that ends before its fields do (a recording that lost part of a packet) ended
                // the whole job with "Bitstream exception", and nothing was written. The picture is kept, as it
                // came; its own order cannot be read, so guessPicOrder gives it one.
                if (rez == UNIT_DAMAGED)
                {
                    if (m_damagedSliceWarnings++ == 0)
                        LTRACE(LT_WARN, 2,
                               "Warning: HEVC: the header of picture "
                                   << m_totalFrameNum << " is damaged. The picture "
                                   << "is kept, with the time of the picture at its place in the GOP before.");
                    if (nalType >= HevcUnit::NalType::BLA_W_LP)
                        m_lastIFrame = true;
                    m_fullPicOrder = guessPicOrder(nalType);
                }
                else
                {
                    if (rez)
                        return rez;  // not enough buffer or error
                    if (nalType >= HevcUnit::NalType::BLA_W_LP)
                        m_lastIFrame = true;
                    // B is 0, P is 1, I is 2, so the lowest value seen is the most permissive and
                    // is exactly what pic_type has to declare in a generated delimiter.
                    if (m_slice->slice_type <= 2 &&
                        (m_auMinSliceType < 0 || static_cast<int>(m_slice->slice_type) < m_auMinSliceType))
                        m_auMinSliceType = static_cast<int>(m_slice->slice_type);
                    // A later file of a join that does not begin with an IDR or BLA picture, as a cut does, is a
                    // stream of its own when its first picture would take an order a picture of the file before
                    // already has: it continued the count of the file before, its pictures took times that file had
                    // given, and the two parts overlapped. It begins as a stream does, after the pictures before it,
                    // its leading pictures with it. A file that continues the stream before it gives no order twice.
                    bool joinStart = false;
                    if (m_joinPos && curPos >= m_joinPos)
                    {
                        joinStart = m_picOrderStarted &&
                                    (nalType < HevcUnit::NalType::BLA_W_LP || nalType == HevcUnit::NalType::CRA) &&
                                    m_periodOrders.count(nextFullPicOrder(m_slice)) > 0;
                        if (!joinStart)
                            m_joinPos = nullptr;
                    }
                    if ((!m_picOrderStarted && nalType < HevcUnit::NalType::BLA_W_LP) || joinStart)
                    {
                        const int leading = picturesShownBeforeFirst(curPos, m_bufEnd);
                        if (leading == -1)
                            return NOT_ENOUGH_BUFFER;
                        m_firstLeading = leading;
                    }
                    if (joinStart)
                    {
                        m_joinPos = nullptr;
                        m_joinRestart = true;
                    }
                    m_fullPicOrder = toFullPicOrder(m_slice, m_sps->log2_max_pic_order_cnt_lsb);
                }
                notePicOrder(nalType);
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
                if (m_vps->num_units_in_tick)
                    updateFPS(m_vps, curPos, nextNalWithStartCode, 0);
                // After the fps rewrite, because that one can move the end of the NAL.
                applyForcedLevel(m_vps, curPos, nextNalWithStartCode + m_vpsSizeDiff);
                nextNal += m_vpsSizeDiff;
                // The copy kept for repeating the VPS ends where the unit now ends, as the SPS one below does. It
                // used to end at the old end, so a VPS the rewrite had made longer lost its last byte in the copy
                // written at the start of a split part or a cut.
                storeBuffer(m_vpsBuffer, curPos, nextNalWithStartCode + m_vpsSizeDiff);
                break;
            case HevcUnit::NalType::SPS:
                if (!m_sps)
                    m_sps = new HevcSpsUnit();
                m_sps->decodeBuffer(curPos, nextNalWithStartCode);
                rez = m_sps->deserialize();
                if (rez == UNIT_DAMAGED)
                {
                    keepLastGoodUnit(m_sps, m_spsBuffer, "SPS");
                    break;
                }
                if (rez)
                    return rez;
                noteSampleAspectRatio(m_sps->sar);
                warnSizeChangeOnDisc();
                m_spsPpsFound = true;
                m_vpsSizeDiff = 0;
                updateFPS(m_sps, curPos, nextNalWithStartCode, 0);
                // m_vpsSizeDiff is named for the VPS and now carries either, because rewriting the
                // timing moves the end of whichever NAL was rewritten: the new values need a
                // different number of emulation prevention bytes from the old ones. Everything
                // after this point has to use the NEW end or it reads the wrong bytes.
                //
                // The level lives in the SPS as well as the VPS, and a player may read either.
                applyForcedLevel(m_sps, curPos, nextNalWithStartCode + m_vpsSizeDiff);
                nextNal += m_vpsSizeDiff;
                storeBuffer(m_spsBuffer, curPos, nextNalWithStartCode + m_vpsSizeDiff);
                break;
            case HevcUnit::NalType::PPS:
                if (!m_pps)
                    m_pps = new HevcPpsUnit();
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
            case HevcUnit::NalType::SEI_PREFIX:
            {
                captureHdrSei(curPos, nextNalWithStartCode);
                try
                {
                    m_hdr->decodeBuffer(curPos, nextNal);
                    if (m_hdr->deserialize() != 0)
                        return rez;
                    markHdr10ByMasteringSei();
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
    // What is inserted (delimiters, parameter sets, SEI) stands at the head of an access unit, where the byte stream
    // format asks for four bytes. It took the length of the stream's own start code there, three in many streams:
    // every inserted SPS and PPS of such a stream had three.
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
    const int requiredBytes = static_cast<int>(srcData.size()) + 4;
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
    // the Blu-ray format has exactly one per picture in every video layer, and the H.264
    // reader has always inserted them when a source lacked them (h264StreamReader.cpp, the
    // m_delimiterFound path). HEVC never did, so an elementary stream from a source that omits them
    // was authored to disc without any. This closes that gap.
    //
    // pic_type must not over-claim: it declares the SET of slice types the picture may contain, so
    // announcing B for an all-I picture would be wrong. m_auMinSliceType carries the lowest
    // slice_type seen for this access unit, and the mapping is exactly 2 minus that value, giving
    // the same 0x10, 0x30 and 0x50 payload bytes Blu-ray discs carry. If no slice was seen, 2 is
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

    // A decoder starts on an IRAP picture only with the parameter sets in hand, and a disc player starts there after
    // every chapter jump and seek. A stream from MP4 or Matroska holds them once, in the file's header, so a disc made
    // from one had them only at its first picture: started anywhere else, not one picture decoded. A disc repeats
    // them at every IRAP, as H.264 on a disc already gets them (muxerManager.cpp, setH264SPSCont). An access unit that
    // carries its own is left exactly as it is.
    const bool irapWithoutSets = (V3_flags & BLURAY_OUT) && (avPacket.flags & AVPacket::IS_IFRAME);
    const bool needInsSpsPps = (m_firstFileFrame || irapWithoutSets) && !(avPacket.flags & AVPacket::IS_SPS_PPS_IN_GOP);
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

// ** THE UNITS THAT CANNOT BE HEVC (see MPEGStreamReader::unusableUnit). ** ITU-T H.265 7.3.1.2 and Table 7-1: the
// forbidden_zero_bit, nuh_temporal_id_plus1 0, the reserved types 10 to 15, 22 to 31 and 41 to 47, and the unspecified
// 48 to 61 (62 and 63 carry Dolby Vision and stay). A slice whose slice_pic_parameter_set_id is above 63 (7.4.7.1) is
// a slice by its type alone. The longest unit of a type is the size its syntax comes to, with room to spare.
bool HEVCStreamReader::unusableUnit(const uint8_t* unit, const uint8_t* end, int& maxLen)
{
    maxLen = 0;
    if (end - unit < 2)
        return false;
    if ((unit[0] & 0x80) || (unit[1] & 7) == 0)
        return true;
    const int type = (unit[0] >> 1) & 0x3f;
    if ((type >= 10 && type <= 15) || (type >= 22 && type <= 31) || (type >= 41 && type <= 61))
        return true;
    if (type <= 9 || (type >= 16 && type <= 21))
    {
        UnitHead head(unit, end, 2);
        // first_slice_segment_in_pic_flag, and no_output_of_prior_pics_flag in an IRAP picture
        uint32_t ppsId = 0;
        if (!head.skipBits(type >= 16 ? 2 : 1) || !head.readUe(ppsId))
            return true;
        return ppsId > 63;
    }
    switch (type)
    {
    case 32:  // VPS
    case 34:  // PPS
        maxLen = 1024;
        break;
    case 33:  // SPS
        maxLen = 2048;
        break;
    case 35:  // access unit delimiter
    case 36:  // end of sequence
    case 37:  // end of bitstream
        maxLen = 64;
        break;
    default:
        break;
    }
    return false;
}
