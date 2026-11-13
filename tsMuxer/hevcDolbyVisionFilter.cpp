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
            m_queue.clear();
        }
        else if (rpu && !m_layerPicture.empty())
            m_kind = Kind::made;
        // A picture in front of the first RPU: the layer has a picture for it as well, or every later one would
        // stand one slot early. It waits in the queue, with no RPU of its own, until the track is known for single
        // layer.
        else if (!m_layerPicture.empty())
        {
            for (uint8_t* nal = NALUnit::findNextNAL(avPacket.data, dataEnd); nal < dataEnd;)
            {
                uint8_t* next = NALUnit::findNALWithStartCode(nal, dataEnd, true);
                const uint8_t* nalEnd = next;
                if (next < dataEnd)
                    while (nalEnd > nal && nalEnd[-1] == 0) nalEnd--;
                if (nalEnd - nal >= 2)
                    notePicture(nal, nalEnd, (nal[0] >> 1) & 0x3F);
                nal = NALUnit::findNextNAL(next, dataEnd);
            }
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

// How many slots of the layer are held before the earliest goes on: HEVC puts a picture at most 16 places out of
// order, so a picture of an earlier slot can still come while fewer are held
static constexpr size_t HELD_SLOTS = 16;
// as far as a slice header is read: the order count is in its first bytes (the same reach as the HEVC reader's)
static constexpr int ORDER_SLICE_HEADER = 64;
// a picture waits in the queue for the pictures that follow it, no longer than this many
static constexpr size_t MAX_AHEAD = 64;
// more pictures than this the track does not leave out in one place: a jump further is no hole to fill
static constexpr int64_t MAX_FILLED = 600;
// the pictures that wait for the parameter sets: far more than a group has
static constexpr size_t MAX_QUEUED = 8192;

void HevcDolbyVisionFilter::readHeader(Picture& picture) const
{
    picture.parsed = true;
    picture.readable = false;
    picture.refused = false;
    picture.lsb = 0;
    picture.range = 1 << m_orderSps->log2_max_pic_order_cnt_lsb;
    // a slice that begins a picture without saying so has a header that is not that of a first slice
    if (!picture.firstSlice || picture.head.size() < 3)
        return;
    HevcSliceHeader slice;
    slice.decodeBuffer(picture.head.data(), picture.head.data() + picture.head.size());
    const int rez = slice.deserialize(m_orderSps.get(), m_orderPps.get());
    if (rez == 0)
    {
        picture.readable = true;
        picture.lsb = static_cast<int>(slice.pic_order_cnt_lsb);
    }
    else if (rez != UNIT_DAMAGED)
        picture.refused = true;  // a value no stream has: the reader passes the slice on and begins no picture
}

bool HevcDolbyVisionFilter::notePicture(const uint8_t* nal, const uint8_t* nalEnd, const int nalType)
{
    // A damaged parameter set leaves the last whole one of its kind in use. Before the first whole one the reader
    // has none of that kind left to read a slice with, and counts no picture until one comes: the layer has none
    // for those pictures either.
    const auto noteSet = [this](const int rez, const bool had, const unsigned kind)
    {
        if (rez != UNIT_DAMAGED)
            m_lostSets &= ~kind;
        else if (!had)
            m_lostSets |= kind;
    };
    if (nalType == static_cast<int>(HevcUnit::NalType::VPS))
    {
        HevcVpsUnit vps;
        vps.decodeBuffer(nal, nalEnd);
        const int rez = vps.deserialize();
        noteSet(rez, m_orderVps, 1);
        if (rez != UNIT_DAMAGED)
            m_orderVps = true;
    }
    else if (nalType == static_cast<int>(HevcUnit::NalType::SPS))
    {
        auto sps = std::make_unique<HevcSpsUnit>();
        sps->decodeBuffer(nal, nalEnd);
        const int rez = sps->deserialize();
        noteSet(rez, m_orderSps != nullptr, 2);
        if (rez == 0)
            m_orderSps = std::move(sps);
    }
    else if (nalType == static_cast<int>(HevcUnit::NalType::PPS))
    {
        auto pps = std::make_unique<HevcPpsUnit>();
        pps->decodeBuffer(nal, nalEnd);
        const int rez = pps->deserialize();
        noteSet(rez, m_orderPps != nullptr, 4);
        if (rez == 0)
            m_orderPps = std::move(pps);
    }
    // a slice, and what may follow the slices of a picture, as the HEVC reader has them (isSlice, isSuffix)
    const bool slice = nalType <= 9 || (nalType >= 16 && nalType <= 23);
    const bool suffix = nalType == 38 || nalType == 40 || (nalType >= 45 && nalType <= 47) || nalType >= 56;
    if (!slice || m_lostSets != 0 || nalEnd - nal < 3)
    {
        if (!suffix || m_lostSets != 0)
        {
            m_auOpen = true;  // something of the next picture
            m_currentQueued = false;
        }
        return false;
    }
    const bool firstSlice = (nal[2] & 0x80) != 0;
    if (!firstSlice && !m_auOpen)
    {
        if (m_currentQueued && !m_queue.empty())
            m_queue.back().moreSlices = true;
        return false;  // a further slice of the picture
    }
    m_auOpen = false;
    m_currentQueued = false;
    ++m_basePictures;
    if (m_queue.size() >= MAX_QUEUED)
    {
        ++m_orderGuessed;  // the parameter sets never came: no stream the reader takes either
        return false;
    }
    Picture picture;
    picture.head.assign(nal, (std::min)(nal + ORDER_SLICE_HEADER, nalEnd));
    picture.nalType = nalType;
    picture.firstSlice = firstSlice;
    // read with the parameter sets that stand in front of it; one in front of all of them is read when they come
    if (m_orderVps && m_orderSps != nullptr && m_orderPps != nullptr)
        readHeader(picture);
    m_queue.push_back(std::move(picture));
    m_currentQueued = true;
    return true;
}

void HevcDolbyVisionFilter::slotQueued(const bool last)
{
    const auto isIdr = [](const int type)
    {
        return type == static_cast<int>(HevcUnit::NalType::IDR_W_RADL) ||
               type == static_cast<int>(HevcUnit::NalType::IDR_N_LP);
    };
    const auto isBla = [](const int type)
    {
        return type >= static_cast<int>(HevcUnit::NalType::BLA_W_LP) &&
               type <= static_cast<int>(HevcUnit::NalType::BLA_N_LP);
    };
    while (!m_queue.empty())
    {
        const bool sets = m_orderVps && m_orderSps != nullptr && m_orderPps != nullptr;
        if (!sets)
        {
            if (!last)
                return;
            // the stream ended without its parameter sets: no picture has a slot
            m_orderGuessed += static_cast<int64_t>(m_queue.size());
            m_queue.clear();
            return;
        }
        for (Picture& queued : m_queue)
            if (!queued.parsed)
                readHeader(queued);
        // A first slice the reader does not take: it begins the picture at the next slice, which has no order of its
        // own to read, and a picture of that one slice is no picture to it
        if (m_queue.front().refused)
        {
            Picture& refused = m_queue.front();
            if (!refused.moreSlices)
            {
                --m_basePictures;
                if (!refused.rpu.empty())
                    ++m_rpuWithoutPicture;
                m_queue.pop_front();
                continue;
            }
            refused.refused = false;
            refused.firstSlice = false;
        }
        const int range = m_queue.front().range;
        // the difference of two order counts as their lower parts give it, to half the room either way
        const auto apart = [range](const int lsb, const int from)
        {
            int diff = lsb - from;
            if (diff >= range / 2)
                diff -= range;
            else if (diff < -range / 2)
                diff += range;
            return diff;
        };
        Picture& picture = m_queue.front();
        const int type = picture.nalType;
        const bool idr = isIdr(type);
        const bool bla = isBla(type);
        const bool irap = type >= 16 && type <= 23;
        const bool damaged = !picture.readable;
        // the pictures behind it that the slot of this one depends on: has their run ended in the queue?
        const bool enough = last || m_queue.size() > MAX_AHEAD;
        // An IDR or BLA picture after the first: the places of its leading pictures (RADL, and the RASL of a BLA
        // picture), which follow it and are shown before it (placesOfLeadingPictures)
        int64_t restartLeading = 0;
        if ((idr || bla) && m_started)
        {
            bool ended = false;
            if (bla && damaged)
                ended = true;  // its own count is not known: no places
            const int irapLsb = bla ? picture.lsb : 0;
            for (size_t i = 1; !ended && i < m_queue.size(); ++i)
            {
                const Picture& next = m_queue[i];
                if (!next.firstSlice)
                    continue;
                if (next.nalType < 6 || next.nalType > 9 || !next.readable)
                    ended = true;
                else
                    restartLeading = (std::max)(restartLeading, static_cast<int64_t>(-apart(next.lsb, irapLsb)));
            }
            if (!ended && !enough)
                return;
        }
        // A first picture that is no key picture: the pictures right behind it that are shown before it
        // (picturesShownBeforeFirst)
        int64_t firstLeading = 0;
        if (!m_started && !damaged && type < 16)
        {
            bool ended = false;
            for (size_t i = 1; !ended && i < m_queue.size(); ++i)
            {
                const Picture& next = m_queue[i];
                if (!next.firstSlice)
                    continue;
                if (next.nalType >= 16 || !next.readable || apart(next.lsb, picture.lsb) >= 0)
                    ended = true;
                else
                    ++firstLeading;
            }
            if (!ended && !enough)
                return;
        }

        int64_t slot;
        if (damaged && !idr)
        {
            // guessPicOrder: the slot of the picture at the same place of the group before, or the picture count
            ++m_orderGuessed;
            const std::vector<int64_t>& before = irap ? m_gopOffsets : m_prevGopOffsets;
            const size_t place = irap ? 0 : m_gopOffsets.size();
            slot = m_frameNum + (place < before.size() ? before[place] : 0);
        }
        else
        {
            // toFullPicOrder. An IDR picture has the count 0, read or not.
            const int lsb = idr ? 0 : picture.lsb;
            if (idr || bla || !m_started)
            {
                // A period begins at the count of the pictures, less those shown before the start, and never at
                // a slot already given: the count moves on to the first free slot where pictures are missing
                // (HEVCStreamReader::periodStart)
                if (m_started && m_maxSlot + 1 > m_frameNum)
                    m_frameNum = m_maxSlot + 1;
                const int64_t start = m_started ? (std::max)(m_maxSlot + 1, m_frameNum - m_leading) : m_frameNum;
                int64_t leading = !m_started && type < 16 ? firstLeading : 0;
                if (m_started && (idr || bla))
                    leading = restartLeading;
                m_started = true;
                m_base = start + leading - lsb;
                m_msb = 0;
                m_prevLsb = lsb;
            }
            else
            {
                if (lsb < m_prevLsb && m_prevLsb - lsb >= range / 2)
                    m_msb += range;
                else if (lsb > m_prevLsb && lsb - m_prevLsb >= range / 2)
                    m_msb -= range;
                m_prevLsb = lsb;
            }
            slot = lsb + m_msb + m_base;
        }
        // notePicOrder, incTimings
        if (irap)
        {
            m_prevGopOffsets.swap(m_gopOffsets);
            m_gopOffsets.clear();
        }
        if (m_gopOffsets.size() < 512)
            m_gopOffsets.push_back(slot - m_frameNum);
        ++m_frameNum;
        m_maxSlot = (std::max)(m_maxSlot, slot);

        // the layer's picture of that slot
        if (slot < 0)
            ++m_leading;
        else if (slot < m_nextSlot || !m_slots.emplace(slot, std::move(picture.rpu)).second)
            ++m_sameSlot;
        m_queue.pop_front();
    }
}

int64_t HevcDolbyVisionFilter::writeLayerPicture(DemuxedData& demuxedData, const std::vector<uint8_t>& rpu)
{
    ++m_made;
    if (m_elStreamIndex < 0)
        return 0;
    MemoryBlock& layer = demuxedData[m_elStreamIndex];
    layer.append(m_layerPicture.data(), static_cast<int>(m_layerPicture.size()));
    auto written = static_cast<int64_t>(m_layerPicture.size());
    if (!rpu.empty())
    {
        layer.append(START_CODE, sizeof(START_CODE));
        layer.append(rpu.data(), static_cast<int>(rpu.size()));
        written += static_cast<int64_t>(sizeof(START_CODE) + rpu.size());
    }
    return written;
}

int64_t HevcDolbyVisionFilter::handOnSlots(DemuxedData& demuxedData, const size_t keep)
{
    int64_t handedOn = 0;
    while (m_slots.size() > keep)
    {
        const auto first = m_slots.begin();
        // ** THE LAYER STAYS IN STEP WHERE THE TRACK LEAVES A PICTURE OUT. ** A slot the track has no picture of, as
        // in a file cut or put together from parts, or a damaged one, had no picture of the layer either, so every
        // picture of the layer after it stood one picture early: the Dolby Vision data of the rest of the film
        // belonged to the picture after. Such a slot gets a picture of the layer with the data of the slot before.
        int64_t missing = first->first - m_nextSlot;
        if (missing > MAX_FILLED)
        {
            // no hole but a jump (a damaged count, or another part of a film): counted, and said at the end
            ++m_gaps;
            missing = 0;
        }
        if (missing > 0)
        {
            ++m_holes;
            m_fillers += missing;
            for (int64_t i = 0; i < missing; ++i) handedOn += writeLayerPicture(demuxedData, m_lastRpu);
        }
        // a picture whose RPU is missing or could not be converted takes that of the picture before
        if (first->second.empty() && !m_lastRpu.empty())
        {
            first->second = m_lastRpu;
            ++m_withoutRpu;
        }
        handedOn += writeLayerPicture(demuxedData, first->second);
        if (!first->second.empty())
            m_lastRpu = std::move(first->second);
        m_nextSlot = first->first + 1;
        m_slots.erase(first);
    }
    return handedOn;
}

int HevcDolbyVisionFilter::demuxMade(DemuxedData& demuxedData, AVPacket& avPacket)
{
    uint8_t* dataEnd = avPacket.data + avPacket.size;
    int64_t discardSize = 0;
    int64_t handedOn = 0;
    // ** AN RPU WITHOUT A PICTURE GOES NOWHERE. ** An RPU follows the slices of its picture. One in front of the
    // first picture of a packet (the end of a picture whose beginning a recording does not have) made a picture of
    // the layer of its own: one picture too many, and every RPU after it one picture late.
    // m_currentQueued: a picture began in this packet, is in the queue, and nothing of a next one has come
    m_currentQueued = false;
    for (uint8_t* curNal = NALUnit::findNextNAL(avPacket.data, dataEnd); curNal < dataEnd;)
    {
        uint8_t* nextNal = NALUnit::findNALWithStartCode(curNal, dataEnd, true);
        uint8_t* nalEnd = nextNal;
        if (nextNal < dataEnd)
            while (nalEnd > curNal && nalEnd[-1] == 0) nalEnd--;  // the zeroes of the next start code
        if (nalEnd - curNal >= 2)
        {
            const int nalType = (curNal[0] >> 1) & 0x3F;
            notePicture(curNal, nalEnd, nalType);
            if (nalType == static_cast<int>(HevcUnit::NalType::DVRPU))
            {
                // The last NAL of a packet can have zero bytes behind it (a transport stream may fill with them);
                // they are no part of it.
                const uint8_t* rpuEnd = nalEnd;
                while (rpuEnd - curNal > 2 && rpuEnd[-1] == 0) rpuEnd--;
                std::string why;
                std::vector<uint8_t> rpu;
                if (!DoviLib::instance().convertRpuToMel(curNal, static_cast<size_t>(rpuEnd - curNal), rpu, why))
                {
                    // The first one: the library cannot do it, and nothing is written yet. A later one: one RPU
                    // of a long film, which takes that of the picture before (handOnSlots) and is counted.
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
                if (m_currentQueued && !m_queue.empty())
                    m_queue.back().rpu = std::move(rpu);
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
    slotQueued(false);
    handedOn += handOnSlots(demuxedData, HELD_SLOTS);
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
    slotQueued(true);
    m_reframed -= handOnSlots(demuxedData, 0);
    LTRACE(LT_INFO, 2,
           "Dolby Vision: the second layer was made with " << m_made << " pictures for the " << m_basePictures
                                                           << " pictures of the track.");
    if (m_leading > 0)
        LTRACE(LT_INFO, 2,
               "Dolby Vision: " << m_leading << (m_leading == 1 ? " picture" : " pictures") << " of the track "
                                << (m_leading == 1 ? "is" : "are")
                                << " shown before its start (the pictures that lead a first key picture); the layer "
                                   "begins at the start.");
    if (m_sameSlot > 0)
        LTRACE(LT_WARN, 2,
               "Warning: " << m_sameSlot << (m_sameSlot == 1 ? " picture" : " pictures") << " of the track "
                           << (m_sameSlot == 1 ? "stands" : "stand")
                           << " at the time of another picture (a damaged picture order); the second layer has one "
                              "picture for such a time.");
    if (m_orderGuessed > 0)
        LTRACE(LT_WARN, 2,
               "Warning: the place of " << m_orderGuessed << (m_orderGuessed == 1 ? " picture" : " pictures")
                                        << " of the track among the others could not be read from the video; "
                                        << (m_orderGuessed == 1 ? "it is" : "they are")
                                        << " placed as the muxer places " << (m_orderGuessed == 1 ? "it" : "them")
                                        << ".");
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
                   << ". The second layer has no pictures for such a jump, so its Dolby Vision data is out of "
                      "step with the picture from there on.");
}
