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

int HevcDolbyVisionFilter::demuxPacket(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, AVPacket& avPacket)
{
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
        }
        else if (rpu && !m_layerPicture.empty())
            m_kind = Kind::made;
        // A picture in front of the first RPU: the layer has a picture for it as well, or every later one would
        // stand one place early. Kept, by its place, until the track is known for single layer.
        else if (!m_layerPicture.empty())
        {
            int64_t firstKey = AV_NOPTS_VALUE_INTERNAL;
            for (const int64_t place : placesIn(avPacket.data, dataEnd, firstKey))
                if (m_before.size() < 64)
                    m_before.push_back(place);
            if (m_firstKeyTime == AV_NOPTS_VALUE_INTERNAL)
                m_firstKeyTime = firstKey;
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
// down where those bits wrap, measured from the picture before, as the HEVC reader does it. A slice header that
// cannot be read (no parameter sets yet, or damaged) puts the picture one place after the picture before: counted,
// and said at the end.
int64_t HevcDolbyVisionFilter::placeOf(const uint8_t* nal, const uint8_t* nalEnd, const int nalType)
{
    const bool idr = nalType == static_cast<int>(HevcUnit::NalType::IDR_W_RADL) ||
                     nalType == static_cast<int>(HevcUnit::NalType::IDR_N_LP);
    const bool bla = nalType >= static_cast<int>(HevcUnit::NalType::BLA_W_LP) &&
                     nalType <= static_cast<int>(HevcUnit::NalType::BLA_N_LP);
    HevcSliceHeader slice;
    slice.decodeBuffer(nal, (std::min)(nal + ORDER_SLICE_HEADER, nalEnd));
    const bool read =
        m_orderSps != nullptr && m_orderPps != nullptr && slice.deserialize(m_orderSps.get(), m_orderPps.get()) == 0;
    const int lsb = read ? static_cast<int>(slice.pic_order_cnt_lsb) : 0;
    if (idr || bla || !m_orderStarted)
    {
        if (m_orderStarted)
            ++m_period;
        m_orderStarted = true;
        m_orderMsb = 0;
        m_prevLsb = lsb;
        m_lastCount = lsb;
        if (!read && !idr)
            ++m_orderGuessed;
    }
    else if (!read)
    {
        ++m_orderGuessed;
        ++m_lastCount;
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

std::vector<int64_t> HevcDolbyVisionFilter::placesIn(uint8_t* data, uint8_t* dataEnd, int64_t& firstKey)
{
    std::vector<int64_t> places;
    firstKey = AV_NOPTS_VALUE_INTERNAL;
    for (uint8_t* curNal = NALUnit::findNextNAL(data, dataEnd); curNal < dataEnd;)
    {
        uint8_t* nextNal = NALUnit::findNALWithStartCode(curNal, dataEnd, true);
        const uint8_t* nalEnd = nextNal;
        if (nextNal < dataEnd)
            while (nalEnd > curNal && nalEnd[-1] == 0) nalEnd--;  // the zeroes of the next start code
        if (nalEnd - curNal >= 3)
        {
            const int nalType = (curNal[0] >> 1) & 0x3F;
            if (nalType == static_cast<int>(HevcUnit::NalType::SPS))
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
            else if (nalType < 32 && (curNal[2] & 0x80))  // the first slice of a picture
            {
                places.push_back(placeOf(curNal, nalEnd, nalType));
                if (nalType >= 16 && nalType <= 23 && firstKey == AV_NOPTS_VALUE_INTERNAL)
                    firstKey = places.back();
            }
        }
        curNal = NALUnit::findNextNAL(nextNal, dataEnd);
    }
    return places;
}

// ** THE LAYER STAYS IN STEP WHERE THE TRACK LEAVES A PICTURE OUT. ** The layer is timed by counting its pictures,
// the track by the places of its pictures in the order shown (their picture order counts). Where those leave a
// picture out, as in a file cut or put together from parts, or a damaged one, the layer had no picture for the hole
// either, so every picture of the layer after it stood one picture early: the Dolby Vision data of the rest of the
// film belonged to the picture after. The layer now gets a picture for each one the track leaves out, with the data
// of the picture before, and the two stay in step.
//
// How many pictures the track leaves out before the picture at the place `time`: the step from the picture before,
// in steps of one picture. Such a step is the mean of the last steps that were one picture (encoders differ in what
// they count a picture as), and a step of 1.75 of them or more is a hole. Not known before three steps are, and
// not from one period to the next. A hole of more than MAX_FILLED pictures is no missing picture but another part
// of a film: it is counted (m_gaps) and said.
int64_t HevcDolbyVisionFilter::stepsBefore(const int64_t time)
{
    static constexpr size_t STEPS_KEPT = 16;
    static constexpr int64_t MAX_FILLED = 600;
    if (m_lastTime == AV_NOPTS_VALUE_INTERNAL || time <= m_lastTime || m_unevenTimes)
        return 0;
    // the order count begins again with a period (placeOf): a step from one period to the next says nothing
    if ((time >> PERIOD_SHIFT) != (m_lastTime >> PERIOD_SHIFT))
        return 0;
    const int64_t step = time - m_lastTime;
    if (m_steps.size() >= 3)
    {
        std::vector<int64_t> sorted(m_steps.begin(), m_steps.end());
        std::sort(sorted.begin(), sorted.end());
        const int64_t usual = sorted[sorted.size() / 2];
        if (step * 2 < usual)
        {
            m_unevenTimes = true;
            return 0;
        }
        if (step * 4 >= usual * 7)
        {
            double mean = 0;
            for (const int64_t s : m_steps) mean += static_cast<double>(s);
            mean /= static_cast<double>(m_steps.size());
            const auto missing = static_cast<int64_t>(static_cast<double>(step) / mean + 0.5) - 1;
            if (missing > MAX_FILLED)
            {
                ++m_gaps;
                return 0;
            }
            return missing;
        }
    }
    m_steps.push_back(step);
    if (m_steps.size() > STEPS_KEPT)
        m_steps.pop_front();
    return 0;
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
    m_held.emplace(time, std::move(rpu));
}

int HevcDolbyVisionFilter::demuxMade(DemuxedData& demuxedData, AVPacket& avPacket)
{
    uint8_t* dataEnd = avPacket.data + avPacket.size;
    // the places of the pictures of this packet, in the order they are in it
    int64_t firstKey = AV_NOPTS_VALUE_INTERNAL;
    const std::vector<int64_t> places = placesIn(avPacket.data, dataEnd, firstKey);
    if (m_firstKeyTime == AV_NOPTS_VALUE_INTERNAL)
        m_firstKeyTime = firstKey;
    // the pictures in front of the first RPU, each with no RPU of its own
    for (const int64_t place : m_before) hold(place, {});
    m_before.clear();

    int64_t discardSize = 0;
    int64_t handedOn = 0;
    // the pictures of this packet (one, as a rule): each begins with its first slice, and its RPU follows its slices
    std::vector<std::vector<uint8_t>> rpus;
    for (uint8_t* curNal = NALUnit::findNextNAL(avPacket.data, dataEnd); curNal < dataEnd;)
    {
        uint8_t* nextNal = NALUnit::findNALWithStartCode(curNal, dataEnd, true);
        uint8_t* nalEnd = nextNal;
        if (nextNal < dataEnd)
            while (nalEnd > curNal && nalEnd[-1] == 0) nalEnd--;  // the zeroes of the next start code
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
                if (rpus.empty())
                    rpus.emplace_back();
                rpus.back() = std::move(rpu);
            }
            else if (nalType == static_cast<int>(HevcUnit::NalType::DVEL))
                THROW(ERR_SOURCE,
                      "Dolby Vision: this track began as single layer and now carries an enhancement "
                      "layer. It is not one stream, and no second layer is made for it.")
            else
            {
                handedOn += emit(m_blStreamIndex, curNal, nalEnd, demuxedData, discardSize, nalType);
                if (nalType < 32 && nalEnd - curNal >= 3 && (curNal[2] & 0x80))
                    rpus.emplace_back();
            }
        }
        curNal = NALUnit::findNextNAL(nextNal, dataEnd);
    }
    // one picture of the layer for each picture of the track, at the place of that picture. (An RPU in front of
    // the first picture of a packet makes one entry more than there are pictures: it has no place, and goes after
    // the last.)
    for (size_t i = 0; i < rpus.size(); ++i)
    {
        ++m_basePictures;
        hold(i < places.size() ? places[i] : (places.empty() ? m_lastTime : places.back()) + 1, std::move(rpus[i]));
    }
    handedOn += handOnHeld(demuxedData, HELD_PICTURES);
    ++m_packets;
    // the count of what was read stays whole, as in demuxPacket: the difference goes to the container
    const int counted = avPacket.size - static_cast<int>(discardSize);
    m_reframed += counted - handedOn;
    return counted;
}

void HevcDolbyVisionFilter::flush(DemuxedData& demuxedData, const PIDSet& /*acceptedPIDs*/)
{
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
                                        << " of the track among the others could not be read from the video; "
                                           "they are taken in the order they are stored in.");
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
