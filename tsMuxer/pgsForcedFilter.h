// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef PGS_FORCED_FILTER_H_
#define PGS_FORCED_FILTER_H_

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

// ** THE FORCED PICTURES OF A PGS TRACK, AS A TRACK OF THEIR OWN. ** A Blu-ray marks the pictures a player shows also
// with subtitles off, the translation of a sign or of a line in another language, object by object (forced_on_flag in
// the composition). Only a disc player reads that mark: in a Matroska file or a .sup file the whole track is shown or
// none of it, and the forced pictures had to be pulled out with a second tool.
//
// The forced pictures cannot simply be copied out. A display set often shows objects that an earlier set of the epoch
// defined, with a new palette or beside an object that is not forced, so a stream of the forced sets alone would name
// objects it never defines. This filter follows the epoch as a decoder does (its windows, palettes and objects) and
// writes each forced picture as a display set that is complete on its own: an epoch start with the forced objects
// alone, their palette, their object data and the windows; a palette update where only the colours change (a fade);
// and a set without objects where the forced picture goes away. Sets without a forced object give nothing.
class PgsForcedFilter
{
   public:
    struct Segment
    {
        int64_t pts = 0;  // 90 kHz
        int64_t dts = 0;
        bool startsSet = false;     // the composition segment of a display set
        std::vector<uint8_t> data;  // the segment: its type, its length in two bytes, its payload
    };

    // One whole segment of the source, with its times in 90 kHz. The segments to write are added to `out`.
    void push(const uint8_t* segment, size_t length, int64_t pts, int64_t dts, std::deque<Segment>& out);
    // The end of the source: a display set still open is taken as complete.
    void flush(std::deque<Segment>& out);

    [[nodiscard]] int64_t sourceSets() const { return m_sourceSets; }
    [[nodiscard]] int64_t forcedPictures() const { return m_forcedPictures; }
    // The compositions of the source that show an object that is not forced
    [[nodiscard]] int64_t plainPictures() const { return m_plainPictures; }

   private:
    struct Object
    {
        std::vector<std::vector<uint8_t>> segments;  // its object definition segments, in order
        uint16_t width = 0;
        uint16_t height = 0;
    };
    std::vector<Segment> m_set;                          // the display set being read, from its composition on
    std::map<uint16_t, Object> m_objects;                // the objects of the epoch, by id
    std::map<uint8_t, std::vector<uint8_t>> m_palettes;  // its palettes, by id: the palette definition segment
    std::vector<uint8_t> m_windows;                      // its window definition segment
    bool m_shown = false;                                // the stream written shows a forced picture
    std::string m_shownObjects;                          // what it shows: the composition objects and their data
    std::string m_shownPalette;                          // and in which colours
    uint16_t m_compositionNumber = 0;
    int64_t m_lastEndDts = -1;  // the decoding time of the last segment written
    int64_t m_sourceSets = 0;
    int64_t m_forcedPictures = 0;
    int64_t m_plainPictures = 0;

    void finishSet(std::deque<Segment>& out);
    void write(std::deque<Segment>& out, std::vector<Segment>& set);
};

// ** THE FORCED PICTURES OF A TRACK, WRITTEN BESIDE IT. ** A demux and a Matroska output write the forced pictures of
// a PGS track as a file or a track of their own, beside the whole track, which stays as it is. Only for a track that
// has both kinds: a track of forced pictures alone would only be written twice. So the forced pictures are held back
// until the track shows a picture that is not forced, and a track that never does gives none.
//
// The packets of the track go in as the reader hands them out (a segment to a packet, a long one in pieces), and the
// segments to write come out.
class PgsForcedTrack
{
   public:
    // A packet of the track, with its times in 90 kHz. The segments to write now are added to `out`.
    void push(const uint8_t* data, size_t size, int64_t pts, int64_t dts, std::deque<PgsForcedFilter::Segment>& out);
    // The end of the track
    void finish(std::deque<PgsForcedFilter::Segment>& out);
    // Whether the forced pictures are written: the track has both kinds
    [[nodiscard]] bool written() const { return m_written; }
    [[nodiscard]] int64_t forcedPictures() const { return m_filter.forcedPictures(); }
    [[nodiscard]] int64_t plainPictures() const { return m_filter.plainPictures(); }

   private:
    PgsForcedFilter m_filter;
    std::vector<uint8_t> m_segment;  // the segment being gathered
    int64_t m_segmentPts = 0;
    int64_t m_segmentDts = 0;
    std::deque<PgsForcedFilter::Segment> m_held;  // forced pictures of a track not yet known to have the other kind
    bool m_written = false;
    void hand(std::deque<PgsForcedFilter::Segment>& made, std::deque<PgsForcedFilter::Segment>& out);
};

#endif
