#ifndef HEVC_DOLBY_VISION_FILTER_H_
#define HEVC_DOLBY_VISION_FILTER_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "abstractDemuxer.h"
#include "subTrackFilter.h"

// Split one merged Dolby Vision video track back into its two layers.
//
// A Matroska file carries a dual layer title as ONE track: the base layer NALs, then the
// enhancement layer's NALs each wrapped in an unspecified NAL of type 63, then the RPU. A disc
// wants them apart again, base layer on one PID and enhancement layer on another. This is the
// exact inverse of what MatroskaMuxer does on the way in, and it is a pure byte transform: strip
// the two wrapper bytes and route.
//
// It is used exactly like the MVC filter, through subTrack= on the meta line, so one file and one
// track can feed two tsMuxeR streams:
//
//     V_MPEGH/ISO/HEVC, "film.mkv", track=1, subTrack=1     the base layer
//     V_MPEGH/ISO/HEVC, "film.mkv", track=1, subTrack=2     the enhancement layer
//
// THE CLASSIFIER MUST BE EXACT. If a single type 62 or 63 NAL leaks onto the base layer side, that
// reader reports itself as carrying Dolby Vision, the muxer sends BOTH tracks to the enhancement
// layer PID, and the disc is wrong. So the rule is stated positively: types 62 and 63 go to the
// enhancement layer, everything else goes to the base layer, and nothing is ever sent to both.
// A file written as profile 8.1 carries CONVERTED RPUs inline and the disc's ORIGINALS as an
// attachment, because that conversion cannot be undone: it is many to one, so nothing can recover
// a profile 7 RPU from an 8.1 one. Rebuilding the disc therefore means putting the originals back,
// and this is what carries them from the container to the point of substitution.
//
// The entries are in presentation order and the pictures are stored in decode order, so a
// timestamp index travels with them. Substitution is a lookup, never a guess: a picture whose
// timestamp is not in the index is refused rather than given a neighbour's metadata.
struct DvOriginalRpus
{
    std::vector<uint8_t> data;      // exactly as attached: start code then payload, per entry
    std::vector<uint32_t> offsets;  // where each entry's payload starts within data
    std::vector<uint32_t> lengths;  // and how long it is
    std::vector<int64_t> pts;       // presentation time of each entry, milliseconds, sorted
};

class HevcDolbyVisionFilter final : public SubTrackFilter
{
   public:
    explicit HevcDolbyVisionFilter(int demuxedPID);
    ~HevcDolbyVisionFilter() override = default;

    int demuxPacket(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, AVPacket& avPacket) override;

    // Hand over the preserved originals. Moved, never copied: a feature has upwards of 160,000 of
    // them. Anything wrong with them throws HERE, before a single picture has been written, rather
    // than part way through authoring a disc.
    void setOriginalRpus(DvOriginalRpus&& rpus);

    // How the source framed each NAL type, as recorded in the manifest, for example
    // "4:35,32,33,34 3:1,39". Anything not named keeps the four byte form this muxer writes by
    // default, so an unparsable or empty rule simply changes nothing.
    void setStartCodeRule(const std::string& rule);

    // Sub-track numbers as they appear in subTrack= on the meta line.
    static constexpr int BL_SUB_TRACK = 1;
    static constexpr int EL_SUB_TRACK = 2;

   private:
    void fillPids(const PIDSet& acceptedPIDs, int pid);
    // nalType decides the start code length when the source's framing was recorded; pass -1 to
    // keep the four byte default.
    void emit(int streamIndex, const uint8_t* data, const uint8_t* dataEnd, DemuxedData& demuxedData,
              int64_t& discardSize, int nalType) const;
    // Which preserved original belongs to the picture at this timestamp, or -1 if none does.
    [[nodiscard]] int64_t entryForPts(int64_t pts) const;

    bool m_firstDemuxCall;
    int m_blStreamIndex;
    int m_elStreamIndex;
    int64_t m_unwrapped;  // enhancement NALs whose type 63 wrapper was removed
    int64_t m_rpu;        // RPU NALs passed through unchanged
    bool m_warnedNoWrappers;

    DvOriginalRpus m_originals;
    bool m_haveOriginals;
    int64_t m_restored;  // RPUs swapped back for the disc's own

    std::map<int, int> m_startCodeByType;  // NAL type to 3 or 4, empty means four throughout
};

#endif  // HEVC_DOLBY_VISION_FILTER_H_
