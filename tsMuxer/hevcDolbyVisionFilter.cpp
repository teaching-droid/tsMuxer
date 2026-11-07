// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "hevcDolbyVisionFilter.h"

#include <fs/systemlog.h>

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
        THROW(ERR_COMMON, "Dolby Vision: this file preserves "
                              << m_originals.offsets.size() << " original RPUs but timestamps for "
                              << m_originals.pts.size()
                              << ". They cannot be matched to pictures, so the disc is not rebuilt from it.")

    // Presentation order is what sorts them, so the timestamps must already be in order. If they
    // are not, the file has been altered and a lookup would silently find the wrong entry.
    for (size_t i = 1; i < m_originals.pts.size(); ++i)
    {
        if (m_originals.pts[i] <= m_originals.pts[i - 1])
            THROW(ERR_COMMON,
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
                        THROW(ERR_COMMON,
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
        THROW(ERR_COMMON, (singleLayer ? "Dolby Vision: this track carries an RPU but no enhancement layer, so it is "
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
