// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef SUB_TRACK_FILTER_H_
#define SUB_TRACK_FILTER_H_

class SubTrackFilter
{
   public:
    SubTrackFilter(const int pid) : m_srcPID(pid) {}
    virtual ~SubTrackFilter() = default;

    static int pidToSubPid(const int pid, const int subPid) { return (pid << 16) + subPid; }
    static bool isSubTrack(const int pid) { return pid >= 0x10000; }

    [[nodiscard]] bool isSupportedTrack(const int pid) const { return m_srcPID == pid; }
    virtual int demuxPacket(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, AVPacket& avPacket) = 0;
    // The bytes of the packets demuxPacket counted as handed on since the last call, less those it handed on: a filter
    // that frames the data anew hands on more or fewer. The container counts them as discarded, so that the source is
    // still read to its end by the count that checks it.
    int64_t takeReframedBytes()
    {
        const int64_t bytes = m_reframed;
        m_reframed = 0;
        return bytes;
    }

   protected:
    int m_srcPID;
    int64_t m_reframed = 0;
};

#endif  // SUB_TRACK_FILTER_H_
