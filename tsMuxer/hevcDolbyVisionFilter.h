// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef HEVC_DOLBY_VISION_FILTER_H_
#define HEVC_DOLBY_VISION_FILTER_H_

#include <cstdint>
#include <deque>
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
    // while the track comes in the order they are coded in: the RPUs are held by the slot of their picture on the
    // timeline and handed on slot by slot, as many held as HEVC can put pictures out of order, and the rest at the
    // end (flush).
    // ** THE SLOT OF A PICTURE IS READ FROM THE VIDEO. ** It was the time its file gives the picture, which only a
    // Matroska file gives for each picture: an MP4 file hands over its pictures a chunk at a time, and a transport
    // stream a packet at a time. It is now worked out from the picture order count in the slice header of the
    // picture, as the HEVC reader works out the time of the picture (slotQueued). So the layer is made the same way
    // from every container, and it stands where the reader puts the pictures.
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

    // ** THE LAYER HAS ONE PICTURE FOR EACH SLOT OF THE TRACK'S TIMELINE. ** The made layer is a stream of its own,
    // and its reader times it by counting: its first picture at the start, each next one a picture later. The track
    // is timed by its own HEVC reader, which gives each picture a slot on that timeline from its picture order
    // count (hevcStreamReader.cpp: toFullPicOrder, incTimings, guessPicOrder). So the layer is right exactly when
    // its picture number k carries the RPU of the track's picture at slot k, for every slot from 0 to the last.
    //
    // The filter therefore works out the slot the reader gives each picture, by the reader's own rules, and not a
    // place of its own making (it had one: the time of the file first, then the order count in its period, each of
    // which agreed with the reader for an undamaged stream that begins with a key picture, and not for others):
    //  - A picture begins at a slice that says it is the first of its picture, or at the first slice after anything
    //    that is no slice and no suffix; the first slice of the stream begins one whatever it says.
    //  - A period (it begins at an IDR or BLA picture, and with the first picture) starts one past the highest slot
    //    given, after the places of its own leading pictures.
    //  - A first picture that is no key picture stands after the pictures that follow it and are shown before it.
    //  - A picture whose slice header cannot be read (or that begins without saying so) takes the slot of the
    //    picture at the same place of the group before, or the count of the pictures so far.
    // The reader knows the parameter sets of the track before the mux begins (it has looked at the file) and counts
    // the pictures in front of the first parameter sets of the stream too. The filter sees them only where they
    // come, so the pictures wait in a queue, with the beginning of their first slice, until they can be read. They
    // also wait where a slot depends on the pictures that follow (the leading pictures of a period).
    //
    // The layer then gets, slot by slot: the RPU of the track's picture of that slot; where the track has none (a
    // picture it leaves out), the RPU of the slot before; a picture of a slot below 0 (shown before the start, as the
    // leading pictures of a first key picture are) gets none, and neither does a second picture of one slot.
    struct Picture
    {
        std::vector<uint8_t> head;  // the beginning of its first slice, as far as a slice header is read
        int nalType = 0;
        bool firstSlice = false;  // the slice says it is the first of its picture
        bool parsed = false;      // readable and lsb are set (the parameter sets were there)
        bool readable = false;    // its slice header was read
        bool refused = false;     // its first slice is one the reader does not take: the picture begins at the next
        bool moreSlices = false;  // it has more slices than its first
        int lsb = 0;              // the part of the picture order count the slice header carries
        int range = 0;            // the room of that part, by the SPS the header was read with
        std::vector<uint8_t> rpu;
    };
    std::deque<Picture> m_queue;  // the pictures not yet given a slot, in the order they came
    // Notes a NAL of the track: a parameter set, and whether a picture begins with it (then it is put at the end of
    // the queue, and true comes back)
    bool notePicture(const uint8_t* nal, const uint8_t* nalEnd, int nalType);
    void readHeader(Picture& picture) const;
    // Gives the pictures of the queue their slots, as far as that can be done; all of them when `last` is set
    void slotQueued(bool last);
    // The layer's pictures of the slots that are certain: all but the latest `keep`; the bytes handed on
    int64_t handOnSlots(DemuxedData& demuxedData, size_t keep);
    int64_t writeLayerPicture(DemuxedData& demuxedData, const std::vector<uint8_t>& rpu);

    std::unique_ptr<HevcSpsUnit> m_orderSps;  // the parameter sets the slice headers are read with: the last of each
    std::unique_ptr<HevcPpsUnit> m_orderPps;
    bool m_orderVps = false;  // a VPS was met
    // The kinds of parameter set (VPS 1, SPS 2, PPS 4) of which a damaged one came before any whole one: the reader
    // has none of that kind then, and counts no picture until a whole one comes (keepLastGoodUnit)
    unsigned m_lostSets = 0;
    bool m_auOpen = true;          // the next slice begins a picture whatever it says (see above)
    bool m_currentQueued = false;  // the picture whose units are coming is the last of the queue
    // the reader's state, as toFullPicOrder and incTimings keep it
    bool m_started = false;
    int64_t m_frameNum = 0;                 // the pictures so far, with the slots a period was moved on by
    int64_t m_maxSlot = -1;                 // the highest slot given
    int64_t m_base = 0;                     // what the order count of a picture of this period is added to
    int64_t m_msb = 0;                      // the part of the order count above what a slice header carries
    int m_prevLsb = 0;                      // the part a slice header carries, of the picture before
    std::vector<int64_t> m_gopOffsets;      // slot less picture count, of each picture of this group
    std::vector<int64_t> m_prevGopOffsets;  // and of the group before

    std::map<int64_t, std::vector<uint8_t>> m_slots;  // the RPU of each slot not yet handed on
    int64_t m_nextSlot = 0;                           // the slot the next picture of the layer is of
    std::vector<uint8_t> m_lastRpu;                   // the RPU handed on last
    int64_t m_made = 0;                               // pictures of the made layer handed on
    int64_t m_basePictures = 0;                       // pictures of the track
    int64_t m_leading = 0;     // of them, at a slot below 0 (shown before the start): no picture of the layer
    int64_t m_sameSlot = 0;    // of them, at a slot another picture has, or one that is past: no picture of its own
    int64_t m_withoutRpu = 0;  // pictures of the layer written with the RPU of the picture before
    int64_t m_fillers = 0;     // pictures of the layer for slots the track has no picture of, at m_holes places
    int64_t m_holes = 0;
    int64_t m_gaps = 0;               // places where the slots jump too far to be filled
    int64_t m_orderGuessed = 0;       // pictures whose slice header could not be read
    int64_t m_rpuWithoutPicture = 0;  // RPUs that belong to no picture: left out
};

#endif  // HEVC_DOLBY_VISION_FILTER_H_
