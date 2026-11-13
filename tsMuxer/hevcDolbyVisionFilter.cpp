// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "hevcDolbyVisionFilter.h"

#include <algorithm>

#include <fs/systemlog.h>

#include "doviLib.h"
#include "hevc.h"
#include "nalUnits.h"
#include "vodCoreException.h"
#include "vod_common.h"

// A four byte start code. The Matroska reader hands every NAL over in Annex-B form already
// (matroskaParser.cpp writeNalHeader), so the split re-emits the same framing on both sides.
static constexpr uint8_t START_CODE[4] = {0, 0, 0, 1};

HevcDolbyVisionFilter::HevcDolbyVisionFilter(const int demuxedPID)
    : SubTrackFilter(demuxedPID),
      m_firstDemuxCall(true),
      m_blStreamIndex(-1),
      m_elStreamIndex(-1),
      m_unwrapped(0),
      m_rpu(0),
      m_haveOriginals(false),
      m_restored(0)
{
}

void HevcDolbyVisionFilter::setOriginalRpus(DvOriginalRpus&& rpus)
{
    m_originals = std::move(rpus);
    if (m_originals.offsets.size() != m_originals.pts.size())
        THROW(ERR_SOURCE, "Dolby Vision: this file preserves "
                              << m_originals.offsets.size() << " original RPUs but timestamps for "
                              << m_originals.pts.size()
                              << ". They cannot be matched to pictures, so the disc is not rebuilt from it.")

    // Presentation order is what sorts them, so the timestamps must already be in order. If they
    // are not, the file has been altered and a lookup would silently find the wrong entry.
    for (size_t i = 1; i < m_originals.pts.size(); ++i)
    {
        if (m_originals.pts[i] <= m_originals.pts[i - 1])
            THROW(ERR_SOURCE,
                  "Dolby Vision: the preserved original RPUs are not in presentation order, so this file has been "
                  "altered since it was written and the disc is not rebuilt from it.")
    }

    m_haveOriginals = !m_originals.offsets.empty();
}

// A plain binary search. The index is sorted because presentation order sorts it, which is checked
// once when it arrives rather than assumed here.
int64_t HevcDolbyVisionFilter::entryForPts(const int64_t pts) const
{
    size_t lo = 0;
    size_t hi = m_originals.pts.size();
    while (lo < hi)
    {
        const size_t mid = lo + (hi - lo) / 2;
        if (m_originals.pts[mid] < pts)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < m_originals.pts.size() && m_originals.pts[lo] == pts)
        return static_cast<int64_t>(lo);
    return -1;
}

void HevcDolbyVisionFilter::fillPids(const PIDSet& acceptedPIDs, const int pid)
{
    m_blStreamIndex = SubTrackFilter::pidToSubPid(pid, BL_SUB_TRACK);
    m_elStreamIndex = SubTrackFilter::pidToSubPid(pid, EL_SUB_TRACK);
    if (acceptedPIDs.find(m_blStreamIndex) == acceptedPIDs.end())
        m_blStreamIndex = -1;
    if (acceptedPIDs.find(m_elStreamIndex) == acceptedPIDs.end())
        m_elStreamIndex = -1;
}

// "4:35,32,33,34 3:1,39", parsed by parseStartCodeRule so there is one definition of the format.
void HevcDolbyVisionFilter::setStartCodeRule(const std::string& rule) { parseStartCodeRule(rule, m_startCodeByType); }

int64_t HevcDolbyVisionFilter::emit(const int streamIndex, const uint8_t* data, const uint8_t* dataEnd,
                                    DemuxedData& demuxedData, int64_t& discardSize, const int nalType) const
{
    if (streamIndex >= 0)
    {
        // Four bytes unless the source's own framing was recorded and says three for this type.
        // Matroska holds no start codes at all, so without that record every rebuilt disc came out
        // four byte regardless of what the source did.
        int len = static_cast<int>(sizeof(START_CODE));
        if (nalType >= 0)
        {
            const auto found = m_startCodeByType.find(nalType);
            if (found != m_startCodeByType.end())
                len = found->second;
        }
        demuxedData[streamIndex].append(START_CODE + (sizeof(START_CODE) - len), len);
        demuxedData[streamIndex].append(data, dataEnd - data);
        return len + (dataEnd - data);
    }
    // Only one of the two layers was asked for. The other one is not an error, it is simply
    // not wanted, so its bytes are discarded rather than buffered forever.
    discardSize += dataEnd - data;
    return 0;
}

size_t HevcDolbyVisionFilter::wholePictures()
{
    const uint8_t* d = m_pending.data();
    const size_t n = m_pending.size();
    size_t whole = 0;
    size_t pos = m_scanPos;
    // each start code from where the last search ended; only the new bytes are looked at
    while (pos + 3 <= n)
    {
        size_t s = pos;
        while (s + 3 <= n && !(d[s] == 0 && d[s + 1] == 0 && d[s + 2] == 1)) ++s;
        if (s + 3 > n)
        {
            pos = n - 2;  // the last two bytes can be the start of a start code
            break;
        }
        if (s + 6 > n)
        {
            pos = s;  // the two bytes of the NAL header and the byte after them are not here yet
            break;
        }
        const int type = (d[s + 3] >> 1) & 0x3F;
        const bool slice = type < 32;
        // what a picture can begin with, once the one before has given a slice (H.265, 7.4.2.4.4)
        const bool begins = type == static_cast<int>(HevcUnit::NalType::AUD) || (type >= 32 && type <= 34) ||
                            type == 39 || (type >= 41 && type <= 44) || (type >= 48 && type <= 55) ||
                            (slice && (d[s + 5] & 0x80) != 0);
        if (m_seenSlice && begins)
        {
            whole = s > 0 && d[s - 1] == 0 ? s - 1 : s;  // with the zero byte of a four byte start code
            m_seenSlice = false;
        }
        if (slice)
            m_seenSlice = true;
        pos = s + 3;
    }
    m_scanPos = pos;
    return whole;
}

int HevcDolbyVisionFilter::handOnWhole(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, const size_t size)
{
    AVPacket pictures;
    pictures.data = m_pending.data();
    pictures.size = static_cast<int32_t>(size);
    pictures.pts = pictures.dts = AV_NOPTS_VALUE_INTERNAL;
    struct WholeNow
    {
        bool& flag;
        explicit WholeNow(bool& f) : flag(f) { flag = true; }
        ~WholeNow() { flag = false; }
        WholeNow(const WholeNow&) = delete;
        WholeNow& operator=(const WholeNow&) = delete;
    };
    int counted = 0;
    {
        const WholeNow whole(m_wholeNow);
        counted = demuxPacket(demuxedData, acceptedPIDs, pictures);
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(size));
    m_scanPos = m_scanPos > size ? m_scanPos - size : 0;
    return counted;
}

int HevcDolbyVisionFilter::demuxPacket(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, AVPacket& avPacket)
{
    // pieces of a transport stream: gathered until they are whole pictures (setPieces)
    if (m_pieces && !m_wholeNow)
    {
        m_pending.insert(m_pending.end(), avPacket.data, avPacket.data + avPacket.size);
        const size_t whole = wholePictures();
        // No picture is this large: where no next picture is found in so much, the track is damaged past use, or it
        // is no HEVC. The pieces were gathered without an end.
        static constexpr size_t MAX_PENDING = 96 * 1024 * 1024;
        if (whole == 0 && m_pending.size() > MAX_PENDING)
            THROW(ERR_SOURCE, "Dolby Vision: no picture begins in " << MAX_PENDING / (1024 * 1024)
                                                                    << " MB of this HEVC track, so it cannot be "
                                                                       "split into its pictures. It is damaged, or it "
                                                                       "is no HEVC stream.")
        return whole > 0 ? handOnWhole(demuxedData, acceptedPIDs, whole) : 0;
    }
    if (m_firstDemuxCall)
    {
        fillPids(acceptedPIDs, m_srcPID);
        m_firstDemuxCall = false;
    }

    uint8_t* dataEnd = avPacket.data + avPacket.size;
    // What the track is, told by its first packet with a Dolby Vision NAL: a wrapped enhancement layer NAL makes it
    // a merged dual layer track; an RPU without one makes it single layer, whose second layer is made where a
    // picture for it was given (and which is refused below, as before, where none was).
    if (m_kind == Kind::undecided)
    {
        bool wrapped = false, rpu = false;
        for (uint8_t* nal = NALUnit::findNextNAL(avPacket.data, dataEnd); nal < dataEnd;)
        {
            if (dataEnd - nal >= 3)
            {
                const int type = (nal[0] >> 1) & 0x3F;
                wrapped |= type == static_cast<int>(HevcUnit::NalType::DVEL);
                rpu |= type == static_cast<int>(HevcUnit::NalType::DVRPU);
            }
            nal = NALUnit::findNextNAL(NALUnit::findNALWithStartCode(nal, dataEnd, true), dataEnd);
        }
        if (wrapped)
        {
            m_kind = Kind::dual;
            m_before.clear();
            m_deferred.clear();
            m_placedLate.clear();
        }
        else if (rpu && !m_layerPicture.empty())
            m_kind = Kind::made;
        // A picture in front of the first RPU: the layer has a picture for it as well, or every later one would
        // stand one place early. Kept, by its place, until the track is known for single layer.
        else if (!m_layerPicture.empty())
        {
            for (const PictureAt& picture : picturesIn(avPacket.data, dataEnd))
            {
                if (picture.place == DEFERRED)
                    keepBack(picture);
                else if (picture.place != NO_PLACE && m_before.size() < 64)
                    m_before.push_back(picture.place);
            }
            // (m_placedLate: the pictures kept back before, placed now; they are held with the first packet of the
            // made layer, each with no RPU of its own, as the ones of m_before are)
        }
    }
    if (m_kind == Kind::made)
        return demuxMade(demuxedData, avPacket);

    uint8_t* curNal = NALUnit::findNextNAL(avPacket.data, dataEnd);
    int64_t discardSize = 0;
    int64_t handedOn = 0;

    while (curNal < dataEnd)
    {
        uint8_t* nextNal = NALUnit::findNALWithStartCode(curNal, dataEnd, true);
        uint8_t* nalEnd = nextNal;
        if (nextNal < dataEnd)
        {
            // Back off the trailing zeroes belonging to the next start code.
            while (nalEnd > curNal && nalEnd[-1] == 0) nalEnd--;
        }

        if (nalEnd - curNal >= 2)
        {
            const int nalType = (curNal[0] >> 1) & 0x3F;
            if (nalType == static_cast<int>(HevcUnit::NalType::DVEL))
            {
                // An enhancement layer NAL inside its wrapper. Drop the two wrapper bytes and what
                // is left IS the NAL as the disc carried it, header included. Nothing is unescaped:
                // the NAL was emulation prevented before it was ever wrapped.
                handedOn +=
                    emit(m_elStreamIndex, curNal + 2, nalEnd, demuxedData, discardSize, (curNal[2] >> 1) & 0x3F);
                m_unwrapped++;
            }
            else if (nalType == static_cast<int>(HevcUnit::NalType::DVRPU))
            {
                if (m_haveOriginals)
                {
                    // A profile 8.1 carrier. What travels inline is the CONVERTED RPU; the disc's
                    // own is in the attachment, found by this picture's timestamp.
                    const int64_t ms =
                        avPacket.pts == AV_NOPTS_VALUE_INTERNAL ? -1 : avPacket.pts / INTERNAL_PTS_PER_MS;
                    const int64_t entry = ms < 0 ? -1 : entryForPts(ms);
                    if (entry < 0)
                        THROW(ERR_SOURCE,
                              "Dolby Vision: a picture at "
                                  << ms
                                  << " ms has no preserved original RPU, so this file no longer lines up with the "
                                     "metadata it carries. It has been cut or re-muxed since it was written, and a "
                                     "disc built from it would be wrong, so it is refused.")

                    // Rebuild the NAL: the attachment stores the payload with its two byte header
                    // removed, which is what an extracted RPU file holds, so the header goes back.
                    const uint8_t* payload = m_originals.data.data() + m_originals.offsets[static_cast<size_t>(entry)];
                    const uint32_t len = m_originals.lengths[static_cast<size_t>(entry)];
                    std::vector<uint8_t> nal;
                    nal.reserve(len + 2);
                    nal.push_back(curNal[0]);
                    nal.push_back(curNal[1]);
                    nal.insert(nal.end(), payload, payload + len);
                    handedOn +=
                        emit(m_elStreamIndex, nal.data(), nal.data() + nal.size(), demuxedData, discardSize, nalType);
                    m_restored++;
                }
                else
                {
                    handedOn += emit(m_elStreamIndex, curNal, nalEnd, demuxedData, discardSize, nalType);
                }
                m_rpu++;
            }
            else
            {
                handedOn += emit(m_blStreamIndex, curNal, nalEnd, demuxedData, discardSize, nalType);
            }
        }

        curNal = NALUnit::findNextNAL(nextNal, dataEnd);
    }

    // A merged dual layer track carries the enhancement layer's wrapped NALs in the same access unit as
    // its RPU. A track with an RPU and no wrapper is a SINGLE layer Dolby Vision file, profile 5 or 8,
    // and one with neither is plain HEVC: there is no enhancement layer to separate out. Splitting such
    // a track wrote an empty second track and moved every RPU out of the base layer, so a profile 8
    // disc lost its Dolby Vision, with no more than a warning after 33 RPUs. It is refused instead.
    // The first packets are read before any output is opened, so nothing has been written yet.
    ++m_packets;
    const bool singleLayer = m_rpu > 0 && m_unwrapped == 0;
    const bool noDolbyVision = m_packets > 32 && m_rpu == 0 && m_unwrapped == 0;
    if (singleLayer || noDolbyVision)
        THROW(ERR_SOURCE, (singleLayer ? "Dolby Vision: this track carries an RPU but no enhancement layer, so it is "
                                         "single layer and cannot be split into two."
                                       : "This HEVC track carries no Dolby Vision enhancement layer, so it cannot be "
                                         "split into two.")
                              << " Mux it as ONE track, without subTrack=.")

    // ** THE COUNT OF WHAT WAS READ STAYS WHOLE. ** Each NAL goes on with a start code of its own kind, and an
    // enhancement layer NAL without its two byte wrapper and the start code of the wrapper. What a split hands on is
    // therefore not what it took in, and neither the difference nor the start codes of a layer left out were counted
    // anywhere: a small file split into its two layers stopped with "the mux stopped before the source was finished"
    // at 97.8 percent, and with one layer at 95 percent, though every picture was muxed. The difference is handed to
    // the container to count as discarded. What is returned stays as it was, as the container reads by it.
    const int counted = avPacket.size - static_cast<int>(discardSize);
    m_reframed += counted - handedOn;
    return counted;
}

// How many RPUs are held before the earliest goes on: HEVC puts a picture at most 16 places out of order
static constexpr size_t HELD_PICTURES = 16;

// A place is the period in its upper bits and, below them, the order count moved up by half the room, so that the
// counts below that of the first picture of a period (the pictures shown before it) stay in order
static constexpr int PERIOD_SHIFT = 32;
static constexpr int64_t COUNT_ZERO = 1ll << 31;
// as far as a slice header is read: the order count is in its first bytes (the same reach as the HEVC reader's)
static constexpr int ORDER_SLICE_HEADER = 64;

// The place of a picture in the order they are shown in, from the first slice of the picture: its picture order
// count, in the period it is in. The count begins again at an IDR picture (at 0) and at a BLA picture (at the part
// its slice header carries); between them the slice header carries its lower bits, and the upper part goes up or
// down where those bits wrap, measured from the picture before, as the HEVC reader does it.
// A slice header that cannot be read gives NO_PLACE, but for an IDR picture, whose count is 0 without one: the
// picture gets no picture of the layer, and the place it leaves empty is filled as a picture the track leaves out
// is (stepsBefore), so the pictures after it keep their places. Counted, and said at the end.
int64_t HevcDolbyVisionFilter::placeOf(const uint8_t* nal, const uint8_t* nalEnd, const int nalType)
{
    const bool idr = nalType == static_cast<int>(HevcUnit::NalType::IDR_W_RADL) ||
                     nalType == static_cast<int>(HevcUnit::NalType::IDR_N_LP);
    const bool bla = nalType >= static_cast<int>(HevcUnit::NalType::BLA_W_LP) &&
                     nalType <= static_cast<int>(HevcUnit::NalType::BLA_N_LP);
    HevcSliceHeader slice;
    slice.decodeBuffer(nal, (std::min)(nal + ORDER_SLICE_HEADER, nalEnd));
    const bool read = slice.deserialize(m_orderSps.get(), m_orderPps.get()) == 0;
    if (!read && !idr)
    {
        ++m_orderGuessed;
        if (bla || !m_orderStarted)
        {
            // a new period all the same, from a count that is not known: the next pictures are measured from 0
            if (m_orderStarted)
                ++m_period;
            m_orderStarted = true;
            m_orderMsb = 0;
            m_prevLsb = 0;
            m_lastCount = 0;
        }
        return NO_PLACE;
    }
    const int lsb = read ? static_cast<int>(slice.pic_order_cnt_lsb) : 0;
    if (idr || bla || !m_orderStarted)
    {
        if (m_orderStarted)
            ++m_period;
        m_orderStarted = true;
        m_orderMsb = 0;
        m_prevLsb = lsb;
        m_lastCount = lsb;
    }
    else
    {
        const int range = 1 << m_orderSps->log2_max_pic_order_cnt_lsb;
        if (lsb < m_prevLsb && m_prevLsb - lsb >= range / 2)
            m_orderMsb += range;
        else if (lsb > m_prevLsb && lsb - m_prevLsb >= range / 2)
            m_orderMsb -= range;
        m_prevLsb = lsb;
        m_lastCount = lsb + m_orderMsb;
    }
    return (m_period << PERIOD_SHIFT) + COUNT_ZERO + m_lastCount;
}

int64_t HevcDolbyVisionFilter::placePicture(const uint8_t* nal, const uint8_t* nalEnd, const int nalType,
                                            const bool firstSlice)
{
    // a slice that begins a picture without saying so has a header that is not that of a first slice
    const int64_t place = firstSlice ? placeOf(nal, nalEnd, nalType) : NO_PLACE;
    if (!firstSlice)
        ++m_orderGuessed;
    // ** THE LAYER BEGINS WITH THE FIRST KEY PICTURE ONLY WHERE THE TRACK DOES. ** The pictures shown before the
    // first key picture get no picture of the layer (hold): the reader puts them in front of the start. That is so
    // where the track BEGINS with a key picture. Where it begins with other pictures (a recording that begins inside
    // a group), the reader starts at the earliest of them and every picture is on the timeline: the rule left the
    // pictures shown before the first key picture out all the same, and the layer was that many pictures short.
    if (!m_firstCounted)
    {
        m_firstCounted = true;
        if (nalType >= 16 && nalType <= 23 && place != NO_PLACE)
            m_firstKeyTime = place;
    }
    return place;
}

// A picture met in front of the parameter sets is kept back: the beginning of its first slice, for its place later
void HevcDolbyVisionFilter::keepBack(const PictureAt& picture)
{
    static constexpr size_t MAX_KEPT_BACK = 8192;  // far more pictures than a group has
    if (m_deferred.size() >= MAX_KEPT_BACK)
    {
        ++m_orderGuessed;
        return;
    }
    const uint8_t* end = (std::min)(picture.nal + ORDER_SLICE_HEADER, picture.nalEnd);
    m_deferred.push_back({std::vector<uint8_t>(picture.nal, end), picture.nalType, {}});
}

std::vector<HevcDolbyVisionFilter::PictureAt> HevcDolbyVisionFilter::picturesIn(uint8_t* data, uint8_t* dataEnd)
{
    std::vector<PictureAt> pictures;
    for (uint8_t* curNal = NALUnit::findNextNAL(data, dataEnd); curNal < dataEnd;)
    {
        uint8_t* nextNal = NALUnit::findNALWithStartCode(curNal, dataEnd, true);
        const uint8_t* nalEnd = nextNal;
        if (nextNal < dataEnd)
            while (nalEnd > curNal && nalEnd[-1] == 0) nalEnd--;  // the zeroes of the next start code
        if (nalEnd - curNal >= 3)
        {
            const int nalType = (curNal[0] >> 1) & 0x3F;
            if (nalType == static_cast<int>(HevcUnit::NalType::VPS))
                m_orderVps = true;
            else if (nalType == static_cast<int>(HevcUnit::NalType::SPS))
            {
                auto sps = std::make_unique<HevcSpsUnit>();
                sps->decodeBuffer(curNal, nalEnd);
                if (sps->deserialize() == 0)
                    m_orderSps = std::move(sps);
            }
            else if (nalType == static_cast<int>(HevcUnit::NalType::PPS))
            {
                auto pps = std::make_unique<HevcPpsUnit>();
                pps->decodeBuffer(curNal, nalEnd);
                if (pps->deserialize() == 0)
                    m_orderPps = std::move(pps);
            }
            // a slice, and what may follow the slices of a picture, as the HEVC reader has them
            const bool slice = nalType <= 9 || (nalType >= 16 && nalType <= 23);
            const bool suffix = nalType == 38 || nalType == 40 || (nalType >= 45 && nalType <= 47) || nalType >= 56;
            const bool firstSlice = (curNal[2] & 0x80) != 0;
            if (!(m_orderVps && m_orderSps != nullptr && m_orderPps != nullptr))
            {
                // in front of the parameter sets: placed when they have come
                if (slice && firstSlice)
                    pictures.push_back({curNal, nalEnd, nalType, DEFERRED});
            }
            else if (!slice && !suffix)
                m_auOpen = true;
            else if (slice && (firstSlice || m_auOpen))
            {
                m_auOpen = false;
                // the pictures in front of the parameter sets come first, in the order they came: those of
                // earlier packets, then those of this one
                for (Deferred& kept : m_deferred)
                {
                    const int64_t place =
                        placePicture(kept.head.data(), kept.head.data() + kept.head.size(), kept.nalType, true);
                    if (place != NO_PLACE)
                        m_placedLate.emplace_back(place, std::move(kept.rpu));
                }
                m_deferred.clear();
                for (PictureAt& earlier : pictures)
                    if (earlier.place == DEFERRED)
                        earlier.place = placePicture(earlier.nal, earlier.nalEnd, earlier.nalType, true);
                pictures.push_back({curNal, nalEnd, nalType, placePicture(curNal, nalEnd, nalType, firstSlice)});
            }
        }
        curNal = NALUnit::findNextNAL(nextNal, dataEnd);
    }
    return pictures;
}

// ** THE LAYER STAYS IN STEP WHERE THE TRACK LEAVES A PICTURE OUT. ** The layer is timed by counting its pictures,
// the track by the places of its pictures in the order shown (their picture order counts). Where those leave a
// picture out, as in a file cut or put together from parts, or a damaged one, the layer had no picture for the hole
// either, so every picture of the layer after it stood one picture early: the Dolby Vision data of the rest of the
// film belonged to the picture after. The layer now gets a picture for each one the track leaves out, with the data
// of the picture before, and the two stay in step.
//
// How many pictures the track leaves out before the picture at the place `time`: the counts between it and the
// picture before, in steps of one picture (m_unit: the least step two pictures of this track were met with, noted
// by hold; 1 for every encoder known). Not from one period to the next: the count begins again there. A hole of
// more than MAX_FILLED pictures is no missing picture but another part of a film: it is counted (m_gaps) and said.
int64_t HevcDolbyVisionFilter::stepsBefore(const int64_t time)
{
    static constexpr int64_t MAX_FILLED = 600;
    if (m_lastTime == AV_NOPTS_VALUE_INTERNAL || time <= m_lastTime || m_unit <= 0)
        return 0;
    if ((time >> PERIOD_SHIFT) != (m_lastTime >> PERIOD_SHIFT))
        return 0;
    const int64_t missing = (time - m_lastTime) / m_unit - 1;
    if (missing > MAX_FILLED)
    {
        ++m_gaps;
        return 0;
    }
    return missing > 0 ? missing : 0;
}

// The held RPUs beyond `keep`, earliest first, each behind the given picture: the bytes handed on
int64_t HevcDolbyVisionFilter::handOnHeld(DemuxedData& demuxedData, const size_t keep)
{
    int64_t handedOn = 0;
    while (m_held.size() > keep)
    {
        const auto first = m_held.begin();
        // a picture of the layer for each picture the track leaves out before this one (stepsBefore)
        if (const int64_t missing = stepsBefore(first->first); missing > 0)
        {
            ++m_holes;
            m_fillers += missing;
            for (int64_t i = 0; i < missing && m_elStreamIndex >= 0; ++i)
            {
                MemoryBlock& layer = demuxedData[m_elStreamIndex];
                layer.append(m_layerPicture.data(), static_cast<int>(m_layerPicture.size()));
                handedOn += static_cast<int64_t>(m_layerPicture.size());
                if (!m_lastRpu.empty())
                {
                    layer.append(START_CODE, sizeof(START_CODE));
                    layer.append(m_lastRpu.data(), static_cast<int>(m_lastRpu.size()));
                    handedOn += static_cast<int64_t>(sizeof(START_CODE) + m_lastRpu.size());
                }
                ++m_made;
            }
        }
        // a picture whose RPU is missing or could not be converted takes that of the picture before
        if (first->second.empty() && !m_lastRpu.empty())
        {
            first->second = m_lastRpu;
            ++m_withoutRpu;
        }
        if (m_elStreamIndex >= 0)
        {
            MemoryBlock& layer = demuxedData[m_elStreamIndex];
            layer.append(m_layerPicture.data(), static_cast<int>(m_layerPicture.size()));
            handedOn += static_cast<int64_t>(m_layerPicture.size());
            if (!first->second.empty())
            {
                layer.append(START_CODE, sizeof(START_CODE));
                layer.append(first->second.data(), static_cast<int>(first->second.size()));
                handedOn += static_cast<int64_t>(sizeof(START_CODE) + first->second.size());
            }
        }
        m_lastTime = first->first;
        if (!first->second.empty())
            m_lastRpu = std::move(first->second);
        ++m_made;
        m_held.erase(first);
    }
    return handedOn;
}

// A picture of the track with its RPU, to be handed on in the order of the places (placeOf)
void HevcDolbyVisionFilter::hold(const int64_t time, std::vector<uint8_t>&& rpu)
{
    // ** THE LAYER BEGINS WITH THE FIRST KEY PICTURE. ** A track that begins with a key picture and pictures shown
    // before it (an open group at the start of a cut) is timed from the key picture: those pictures stand before the
    // start. The layer is timed by counting, so a picture of it for each of them moved every RPU that follows that
    // many pictures late, for the whole film. They get none.
    if (m_firstKeyTime != AV_NOPTS_VALUE_INTERNAL && time < m_firstKeyTime)
    {
        ++m_leading;
        return;
    }
    // the least step between two pictures of one period, against the pictures held and the one handed on last
    const auto note = [&](const int64_t other)
    {
        const int64_t step = other > time ? other - time : time - other;
        if (step > 0 && (other >> PERIOD_SHIFT) == (time >> PERIOD_SHIFT) && (m_unit == 0 || step < m_unit))
            m_unit = step;
    };
    if (m_lastTime != AV_NOPTS_VALUE_INTERNAL)
        note(m_lastTime);
    const auto after = m_held.lower_bound(time);
    if (after != m_held.end())
        note(after->first);
    if (after != m_held.begin())
        note(std::prev(after)->first);
    m_held.emplace(time, std::move(rpu));
}

int HevcDolbyVisionFilter::demuxMade(DemuxedData& demuxedData, AVPacket& avPacket)
{
    uint8_t* dataEnd = avPacket.data + avPacket.size;
    // the pictures of this packet as the reader counts them, in the order they are in it
    std::vector<PictureAt> pictures = picturesIn(avPacket.data, dataEnd);
    // the pictures in front of the first RPU, each with no RPU of its own
    for (const int64_t place : m_before) hold(place, {});
    m_before.clear();

    int64_t discardSize = 0;
    int64_t handedOn = 0;
    // The pictures of this packet that get a picture of the layer (one, as a rule), each with its place and its
    // RPU, which follows its slices. ** AN RPU WITHOUT A PICTURE GOES NOWHERE. ** An RPU in front of the first
    // picture of a packet (the end of a picture whose beginning a recording does not have), or behind a picture
    // that cannot be placed, made a picture of the layer of its own: one picture too many, and every RPU after it
    // one picture late.
    std::vector<std::pair<int64_t, std::vector<uint8_t>>> made;
    size_t next = 0;  // the next of `pictures` the loop comes to
    // what the NALs now belong to: a picture that gets a picture of the layer, one that is kept back, or neither
    enum class Owner
    {
        none,
        made,
        keptBack
    } owner = Owner::none;
    for (uint8_t* curNal = NALUnit::findNextNAL(avPacket.data, dataEnd); curNal < dataEnd;)
    {
        uint8_t* nextNal = NALUnit::findNALWithStartCode(curNal, dataEnd, true);
        uint8_t* nalEnd = nextNal;
        if (nextNal < dataEnd)
            while (nalEnd > curNal && nalEnd[-1] == 0) nalEnd--;  // the zeroes of the next start code
        if (next < pictures.size() && pictures[next].nal == curNal)
        {
            const PictureAt& picture = pictures[next++];
            if (picture.place == DEFERRED)
            {
                const size_t before = m_deferred.size();
                keepBack(picture);
                owner = m_deferred.size() > before ? Owner::keptBack : Owner::none;
            }
            else if (picture.place != NO_PLACE)
            {
                made.emplace_back(picture.place, std::vector<uint8_t>());
                owner = Owner::made;
            }
            else
                owner = Owner::none;
        }
        if (nalEnd - curNal >= 2)
        {
            const int nalType = (curNal[0] >> 1) & 0x3F;
            if (nalType == static_cast<int>(HevcUnit::NalType::DVRPU))
            {
                std::string why;
                std::vector<uint8_t> rpu;
                if (!DoviLib::instance().convertRpuToMel(curNal, static_cast<size_t>(nalEnd - curNal), rpu, why))
                {
                    // The first one: the library cannot do it, and nothing is written yet. A later one: one RPU
                    // of a long film, which takes that of the picture before (handOnHeld) and is counted.
                    if (m_rpu == 0)
                        THROW(ERR_SOURCE,
                              "Dolby Vision: the RPU of the first picture could not be converted for "
                              "the second layer ("
                                  << why
                                  << "), so the layer cannot be made. --no-dv-layer on the MUXOPT line "
                                     "writes the track as the one stream it is.")
                    rpu.clear();
                }
                ++m_rpu;
                if (owner == Owner::made)
                    made.back().second = std::move(rpu);
                else if (owner == Owner::keptBack)
                    m_deferred.back().rpu = std::move(rpu);
                else
                    ++m_rpuWithoutPicture;
            }
            else if (nalType == static_cast<int>(HevcUnit::NalType::DVEL))
                THROW(ERR_SOURCE,
                      "Dolby Vision: this track began as single layer and now carries an enhancement "
                      "layer. It is not one stream, and no second layer is made for it.")
            else
                handedOn += emit(m_blStreamIndex, curNal, nalEnd, demuxedData, discardSize, nalType);
        }
        curNal = NALUnit::findNextNAL(nextNal, dataEnd);
    }
    // one picture of the layer for each of them, at the place of its picture: first the pictures that were kept
    // back and have been placed with this packet
    for (auto& [place, rpu] : m_placedLate)
    {
        ++m_basePictures;
        hold(place, std::move(rpu));
    }
    m_placedLate.clear();
    for (auto& [place, rpu] : made)
    {
        ++m_basePictures;
        hold(place, std::move(rpu));
    }
    handedOn += handOnHeld(demuxedData, HELD_PICTURES);
    ++m_packets;
    // the count of what was read stays whole, as in demuxPacket: the difference goes to the container
    const int counted = avPacket.size - static_cast<int>(discardSize);
    m_reframed += counted - handedOn;
    return counted;
}

void HevcDolbyVisionFilter::flush(DemuxedData& demuxedData, const PIDSet& acceptedPIDs)
{
    // the last picture of a source that comes in pieces: nothing follows it to say that it is whole. What it gives
    // is counted as handed on beyond what was read, as the pictures held below are.
    if (m_pieces && !m_pending.empty())
        m_reframed -= handOnWhole(demuxedData, acceptedPIDs, m_pending.size());
    if (m_kind != Kind::made)
        return;
    m_reframed -= handOnHeld(demuxedData, 0);
    LTRACE(LT_INFO, 2,
           "Dolby Vision: the second layer was made with " << m_made << " pictures for the " << m_basePictures
                                                           << " pictures of the track.");
    if (m_leading > 0)
        LTRACE(LT_INFO, 2,
               "Dolby Vision: " << m_leading
                                << " pictures of the track are shown before its first key picture; the "
                                   "layer begins with that key picture.");
    if (m_orderGuessed > 0)
        LTRACE(LT_WARN, 2,
               "Warning: the place of " << m_orderGuessed << (m_orderGuessed == 1 ? " picture" : " pictures")
                                        << " of the track among the others could not be read from the video. The "
                                           "second layer has no picture of its own for "
                                        << (m_orderGuessed == 1 ? "it" : "them") << ".");
    if (m_rpuWithoutPicture > 0)
        LTRACE(LT_INFO, 2,
               "Dolby Vision: " << m_rpuWithoutPicture
                                << " RPU of the track belong to no picture (the track begins inside a picture, or a "
                                   "picture has lost its beginning); they are left out.");
    if (m_withoutRpu > 0)
        LTRACE(LT_WARN, 2,
               "Warning: " << m_withoutRpu
                           << " pictures of the track have no Dolby Vision data that could be used; "
                              "in the second layer they have the data of the picture before.");
    if (m_fillers > 0)
        LTRACE(LT_WARN, 2,
               "Warning: the track's pictures leave "
                   << m_fillers << (m_fillers == 1 ? " picture" : " pictures") << " out, at " << m_holes
                   << (m_holes == 1 ? " place" : " places")
                   << ". The second layer has a picture for each of them, with the Dolby Vision data of the picture "
                      "before, so the two layers stay in step.");
    if (m_gaps > 0)
        LTRACE(LT_WARN, 2,
               "Warning: the order of the track's pictures jumps over more than 600 pictures at "
                   << m_gaps << (m_gaps == 1 ? " place" : " places")
                   << ". If pictures are missing there, the Dolby Vision data of the second layer is out of step "
                      "with the picture after such a place.");
}
