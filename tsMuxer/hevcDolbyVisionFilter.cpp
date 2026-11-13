// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "hevcDolbyVisionFilter.h"

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
        int pictures = 0;
        for (uint8_t* nal = NALUnit::findNextNAL(avPacket.data, dataEnd); nal < dataEnd;)
        {
            if (dataEnd - nal >= 3)
            {
                const int type = (nal[0] >> 1) & 0x3F;
                wrapped |= type == static_cast<int>(HevcUnit::NalType::DVEL);
                rpu |= type == static_cast<int>(HevcUnit::NalType::DVRPU);
                pictures += type < 32 && (nal[2] & 0x80) ? 1 : 0;  // the first slice of a picture
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
        // stand one place early. Kept until the track is known for single layer.
        else if (!m_layerPicture.empty() && avPacket.pts != AV_NOPTS_VALUE_INTERNAL && m_before.size() < 64)
            for (int i = 0; i < pictures; ++i) m_before.push_back(avPacket.pts);
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

// The held RPUs beyond `keep`, earliest first, each behind the given picture: the bytes handed on
int64_t HevcDolbyVisionFilter::handOnHeld(DemuxedData& demuxedData, const size_t keep)
{
    int64_t handedOn = 0;
    while (m_held.size() > keep)
    {
        const auto first = m_held.begin();
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
        // The layer is timed by counting its pictures, the track by what its pictures say: where the times of the
        // track leave pictures out (a damaged file, a file put together from parts), the two are out of step from
        // there on. Counted, and said at the end.
        if (m_lastTime != AV_NOPTS_VALUE_INTERNAL && first->first > m_lastTime)
        {
            const int64_t step = first->first - m_lastTime;
            if (m_step == 0 || step < m_step)
                m_step = step;
            else if (step > m_step * 5 / 2)
                ++m_gaps;
        }
        m_lastTime = first->first;
        if (!first->second.empty())
            m_lastRpu = std::move(first->second);
        ++m_made;
        m_held.erase(first);
    }
    return handedOn;
}

// A picture of the track with its RPU, to be handed on in the order of the times
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
    // The order the pictures are shown in comes from their times: without them the layer cannot be put in order
    if (avPacket.pts == AV_NOPTS_VALUE_INTERNAL)
        THROW(ERR_SOURCE,
              "Dolby Vision: a picture of this track has no time in its file, so the second layer "
              "cannot be put in the order its pictures are shown in. --no-dv-layer on the MUXOPT line "
              "writes the track as the one stream it is.")
    // the pictures in front of the first RPU, each with no RPU of its own
    for (const int64_t time : m_before) hold(time, {});
    m_before.clear();

    uint8_t* dataEnd = avPacket.data + avPacket.size;
    int64_t discardSize = 0;
    int64_t handedOn = 0;
    // the pictures of this packet (one, as a rule): each begins with its first slice, and its RPU follows its slices
    std::vector<std::vector<uint8_t>> rpus;
    bool key = false;
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
                {
                    rpus.emplace_back();
                    key |= nalType >= 16 && nalType <= 23;
                }
            }
        }
        curNal = NALUnit::findNextNAL(nextNal, dataEnd);
    }
    if (key && m_firstKeyTime == AV_NOPTS_VALUE_INTERNAL)
        m_firstKeyTime = avPacket.pts;
    // one picture of the layer for each picture of the track; a second picture of the packet stands a tick later
    for (size_t i = 0; i < rpus.size(); ++i)
    {
        ++m_basePictures;
        hold(avPacket.pts + static_cast<int64_t>(i), std::move(rpus[i]));
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
    if (m_withoutRpu > 0)
        LTRACE(LT_WARN, 2,
               "Warning: " << m_withoutRpu
                           << " pictures of the track have no Dolby Vision data that could be used; "
                              "in the second layer they have the data of the picture before.");
    if (m_gaps > 0)
        LTRACE(LT_WARN, 2,
               "Warning: the times of the track's pictures leave pictures out at "
                   << m_gaps
                   << " places. If pictures are missing there, the Dolby Vision data of the second layer is "
                      "out of step with the picture after such a place.");
}
