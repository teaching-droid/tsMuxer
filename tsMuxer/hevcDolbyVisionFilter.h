// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef HEVC_DOLBY_VISION_FILTER_H_
#define HEVC_DOLBY_VISION_FILTER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "abstractDemuxer.h"
#include "hevc.h"
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
    void flush(DemuxedData& demuxedData, const PIDSet& acceptedPIDs) override;

    // ** THE SECOND LAYER OF A SINGLE LAYER TRACK IS MADE. ** A track with an RPU and no enhancement layer (profile
    // 8) had nothing to split, and was refused. With a picture to write (dvLayerPicture.h) its second layer is made
    // here: the base side gets the track without its RPUs, and the second side gets, for each picture of the track,
    // the given picture and behind it the RPU of the track's picture, converted to the form a second layer carries.
    // The given picture is a key picture each time, so the made layer is in the order the pictures are shown in,
    // while the track comes in the order they are coded in: the RPUs are held by the place of their picture in the
    // order shown and handed on in that order, as many held as HEVC can put pictures out of order, and the rest at
    // the end (flush).
    // ** THE PLACE OF A PICTURE IS READ FROM THE VIDEO. ** The place was the time its file gives the picture, which
    // only a Matroska file gives for each picture: an MP4 file hands over its pictures a chunk at a time, and a
    // transport stream a packet at a time. It is now the picture order count of the picture, read from its slice
    // header (placeOf), which is what the HEVC reader times the picture of the track by. So the layer is made the
    // same way from every container, and it stands where the reader puts the pictures.
    void setLayerPicture(const std::vector<uint8_t>& picture) { m_layerPicture = picture; }
    // ** WHOLE PICTURES FROM THE PIECES OF A TRANSPORT STREAM. ** A Matroska or MP4 reader hands the filter whole
    // pictures. A transport stream reader hands on what one packet of 188 bytes carries, wherever in a picture
    // that is, so the filter could not be used with it, and a single layer Dolby Vision track of a transport stream
    // got no second layer. Told so here, the filter gathers the pieces and works on whole pictures: a picture is
    // whole when the next one begins (an access unit delimiter, a parameter set, a prefix SEI or the first slice of
    // a picture, after a slice), and the last one when the file has ended (flush).
    void setPieces() { m_pieces = true; }
    [[nodiscard]] bool makesLayer() const override { return m_kind == Kind::made; }
    [[nodiscard]] int64_t picturesMade() const { return m_made; }

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
    // keep the four byte default. The bytes handed on to the track, start code included.
    int64_t emit(int streamIndex, const uint8_t* data, const uint8_t* dataEnd, DemuxedData& demuxedData,
                 int64_t& discardSize, int nalType) const;
    // Which preserved original belongs to the picture at this timestamp, or -1 if none does.
    [[nodiscard]] int64_t entryForPts(int64_t pts) const;

    bool m_firstDemuxCall;
    int m_blStreamIndex;
    int m_elStreamIndex;
    int64_t m_unwrapped;    // enhancement NALs whose type 63 wrapper was removed
    int64_t m_rpu;          // RPU NALs passed through unchanged
    int64_t m_packets = 0;  // packets seen, to tell a track without any Dolby Vision

    DvOriginalRpus m_originals;
    bool m_haveOriginals;
    int64_t m_restored;  // RPUs swapped back for the disc's own

    std::map<int, int> m_startCodeByType;  // NAL type to 3 or 4, empty means four throughout

    // a made second layer (setLayerPicture)
    enum class Kind
    {
        undecided,  // no Dolby Vision NAL seen yet
        dual,       // a merged dual layer track: split
        made        // a single layer track: its second layer is made
    };
    Kind m_kind = Kind::undecided;
    std::vector<uint8_t> m_layerPicture;
    std::multimap<int64_t, std::vector<uint8_t>> m_held;  // the RPUs not yet handed on, by the time of their picture
    std::vector<int64_t> m_before;                        // the times of the pictures before the first RPU
    std::vector<uint8_t> m_lastRpu;                       // the RPU handed on last
    int64_t m_made = 0;                                   // pictures of the made layer handed on
    int64_t m_basePictures = 0;                           // pictures of the track
    int64_t m_leading = 0;     // of them, shown before the first key picture: they get no picture of the layer
    int64_t m_withoutRpu = 0;  // pictures of the layer written with the RPU of the picture before
    int64_t m_firstKeyTime = AV_NOPTS_VALUE_INTERNAL;  // the time of the first key picture
    int64_t m_lastTime = AV_NOPTS_VALUE_INTERNAL;      // the time of the picture handed on last
    // Where the track leaves pictures out (stepsBefore): the step from one picture to the next in the counts of
    // this track (the least one met; 1 for every encoder known), how many pictures of the layer were written for
    // pictures the track does not have and at how many places, and the places where that could not be done.
    int64_t m_unit = 0;
    int64_t m_fillers = 0;
    int64_t m_holes = 0;
    int64_t m_gaps = 0;
    int64_t stepsBefore(int64_t time);
    int demuxMade(DemuxedData& demuxedData, AVPacket& avPacket);
    // pieces (setPieces): the bytes not yet handed on, how far they were searched for start codes, and whether a
    // slice was met since the last picture began
    bool m_pieces = false;
    bool m_wholeNow = false;  // demuxPacket is running on whole pictures put together from pieces
    std::vector<uint8_t> m_pending;
    size_t m_scanPos = 0;
    bool m_seenSlice = false;
    // How many of the first bytes of m_pending are whole pictures: up to where the last picture found begins
    size_t wholePictures();
    // Runs demuxPacket on the first `size` bytes of m_pending and takes them out of it; what demuxPacket returns
    int handOnWhole(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, size_t size);
    // ** THE PICTURES ARE COUNTED AS THE HEVC READER COUNTS THEM. ** The layer has one picture for each picture the
    // reader of the track gives a time, so the two have to agree on what a picture is, and where it stands.
    // A picture begins at a slice that says it is the first of its picture, or at the first slice after anything
    // that is no slice and no suffix (the reader's rule: isSlice, isSuffix, auOpen).
    // The reader knows the parameter sets of the track before the mux begins (it has looked at the file), so it
    // counts the pictures in front of the first parameter sets of the stream as well, as a recording that begins in
    // the middle of a group has them. The filter sees the parameter sets only where they come in the stream: such
    // pictures are kept back (m_deferred, with the beginning of their first slice and their RPU) and are placed when
    // the parameter sets have come, before the picture that follows them.
    //
    // A picture of a packet: where it begins and ends, and its place (placeOf); DEFERRED while the parameter sets
    // are not known; NO_PLACE for a picture that cannot be placed (its slice header cannot be read): such a picture
    // gets no picture of the layer of its own, and its RPU goes nowhere.
    struct PictureAt
    {
        const uint8_t* nal;
        const uint8_t* nalEnd;
        int nalType;
        int64_t place;
    };
    static constexpr int64_t NO_PLACE = INT64_MIN + 1;
    static constexpr int64_t DEFERRED = INT64_MIN + 2;
    struct Deferred
    {
        std::vector<uint8_t> head;  // the beginning of the first slice, as far as a slice header is read
        int nalType;
        std::vector<uint8_t> rpu;
    };
    std::vector<Deferred> m_deferred;  // the pictures in front of the parameter sets, in the order they came
    // of them, those placed since: place and RPU, to be held with the pictures of the packet that is read
    std::vector<std::pair<int64_t, std::vector<uint8_t>>> m_placedLate;
    std::vector<PictureAt> picturesIn(uint8_t* data, uint8_t* dataEnd);
    void keepBack(const PictureAt& picture);
    // The place of a picture that is counted, and what follows from the first of them (the first key picture)
    int64_t placePicture(const uint8_t* nal, const uint8_t* nalEnd, int nalType, bool firstSlice);
    int64_t placeOf(const uint8_t* nal, const uint8_t* nalEnd, int nalType);
    bool m_orderVps = false;          // a VPS was met
    bool m_auOpen = false;            // something that is no slice and no suffix was met since the last picture began
    bool m_firstCounted = false;      // the first picture the reader counts was met
    int64_t m_rpuWithoutPicture = 0;  // RPUs that belong to no picture: left out
    std::unique_ptr<HevcSpsUnit> m_orderSps;  // the parameter sets the slice headers are read with: the last of each
    std::unique_ptr<HevcPpsUnit> m_orderPps;
    bool m_orderStarted = false;
    int64_t m_period = 0;        // a new one begins at each IDR or BLA picture, where the order count begins again
    int64_t m_orderMsb = 0;      // the part of the order count above what a slice header carries
    int m_prevLsb = 0;           // the part a slice header carries, of the picture before
    int64_t m_lastCount = 0;     // the order count of the picture before
    int64_t m_orderGuessed = 0;  // pictures whose slice header could not be read: they get no picture of the layer
    int64_t handOnHeld(DemuxedData& demuxedData, size_t keep);
    void hold(int64_t time, std::vector<uint8_t>&& rpu);
};

#endif  // HEVC_DOLBY_VISION_FILTER_H_
