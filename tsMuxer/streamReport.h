// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef STREAM_REPORT_H_
#define STREAM_REPORT_H_

#include <cstdint>
#include <string>
#include <vector>

// ** WHAT A VIDEO STREAM IS MADE OF (the stream report of --scan). ** Whether a stream suits a disc or a player is a
// matter of how it is coded: how long its groups of pictures are, whether they are closed, how many B pictures stand
// in a row, how high the bitrate goes. That was to be found out with another program. A scan reads every picture of
// the source anyway, so its video readers count as they go, and the scan ends with a block of lines for each video
// track. A mux does not count and prints nothing.
//
// A reader gives each coded picture once, in decoding order, as it begins (the two fields of a frame are one
// picture), and what it knows of it. What a reader does not know is left out of the block.
class StreamReport
{
   public:
    enum class Picture
    {
        I,
        P,
        B
    };
    // A group of pictures begins with this picture: Closed where it can be decoded without the group before it (an
    // IDR picture, a group header marked closed), Open where it begins with another I picture.
    enum class Group
    {
        None,
        Closed,
        Open
    };

    static bool wanted;  // the run is a scan: the readers count

    void picture(Picture type, Group group, bool usedAsReference);
    // of the picture given last
    void slices(int count);
    // of the picture given last: coded as two fields, or as a frame of a stream that is not progressive
    void coded(bool asFields, bool interlacedFrame);
    // the picture given last is shown for this many fields: 3 or more is a picture held (pulldown)
    void shownFor(int fields);
    void referenceFrames(int limit);
    // bytes of the stream, with the decoding time they belong to (internal clock)
    void bytes(int64_t dts, int64_t count);

    [[nodiscard]] bool empty() const { return m_count[0] + m_count[1] + m_count[2] == 0; }
    // The lines of the block, without its heading. fps: the frame rate of the track, 0 where it is not known.
    // interlaced: what the reader says of the stream, for a reader that gave no coded().
    [[nodiscard]] std::vector<std::string> lines(double fps, bool interlaced) const;

    // the blocks of the run, each with its heading, for the end of the scan
    static void give(const std::string& heading, const std::vector<std::string>& lines);
    [[nodiscard]] static std::vector<std::string> all();

   private:
    int64_t m_count[3] = {0, 0, 0};
    int64_t m_referenceB = 0;
    int64_t m_closed = 0;
    int64_t m_open = 0;
    int64_t m_groupLen = -1;  // pictures of the group being read; -1 before the first group begins
    int64_t m_groupMin = 0;   // of the groups that ended
    int64_t m_groupMax = 0;
    int64_t m_groupsEnded = 0;
    int64_t m_bRun = 0;
    int64_t m_bRunMax = 0;
    int m_slicesMin = 0;  // 0: not known
    int m_slicesMax = 0;
    int m_referenceFrames = 0;
    bool m_codedKnown = false;
    int64_t m_fieldPictures = 0;
    int64_t m_interlacedFrames = 0;
    int64_t m_heldPictures = 0;
    int64_t m_heldFields = 0;  // the fields those pictures are shown for beyond the two of a frame
    int64_t m_firstDts = -1;
    int64_t m_lastDts = -1;
    int64_t m_bytes = 0;
    std::vector<int64_t> m_bytesOfSecond;  // by whole seconds from the first decoding time
};

#endif  // STREAM_REPORT_H_
