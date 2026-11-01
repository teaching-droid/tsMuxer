#ifndef TSTD_CHECK_H_
#define TSTD_CHECK_H_

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

// The decoder's buffers of H.222.0 (the T-STD, 2.4.2) for the MPEG-2 video of a transport stream tsMuxeR writes,
// simulated from the bytes as they leave the muxer: a picture that is not whole in the elementary stream buffer at its
// decoding time comes late at a player, which pauses or shows a damaged picture there, and data that waits longer
// than one second breaks the rule of 2.4.2.6. Neither was checked: tsMuxeR sends each picture a fixed time (--vbv-len)
// before it is due, and with --maxbitrate, or a stream whose rate is near the cap, its large pictures can still come
// late once the buffer's leak (Rbx, 2.4.2.7) is counted, which the warning of pictures that arrive after they are due
// does not count.
//
// The model is the one of 2.4.2.7 for MPEG-2 video: a transport buffer TB of 512 bytes emptied at Rx = 1.2 x Rmax, a
// multiplexing buffer MB emptied into the elementary stream buffer EB at Rbx by the leak method, and EB of the
// stream's vbv_buffer_size, from which each picture is taken whole at its DTS. Rmax and the sizes come from the
// profile and level of the sequence header (H.262 Tables 8-12 and 8-13). The time of each byte is taken from the PCRs,
// between two of them in a straight line (2.4.2.2), for 188 and 192-byte packets alike; bytes before the first and
// after the last PCR are not judged. The output is not changed: this only reads it.
class TStdCheck
{
   public:
    explicit TStdCheck(int packetSize) : m_packetSize(packetSize) {}
    // The bytes of the output in the order they are written
    void feed(const uint8_t* data, size_t len);
    // Says what was found, once
    void finish();

   private:
    struct Packet  // a packet of a video PID, waiting for its time (the next PCR)
    {
        int pid;
        uint64_t index;  // its place in the stream, in packets
        int header;      // PES header bytes it carries
        int payload;     // elementary stream bytes it carries
        bool startsAu;   // its PES has a time stamp: a new access unit begins here
        int64_t dts90;   // that time stamp
        int64_t pcrRaw;  // the PCR the time stamp is read against, and its place on the time line
        double pcrLine;
    };
    struct Video
    {
        bool params = false;
        bool lowDelay = false;
        double rxBytes = 0;   // TB to MB, bytes per 27 MHz tick
        double rbxBytes = 0;  // MB to EB (leak)
        double ebs = 0;       // EB size, bytes
        // TB
        double tbLastDeparture = -1e30;
        // MB: header and payload bytes in order
        std::deque<std::pair<bool, double>> mb;
        // EB
        double ebIn = 0;      // payload bytes that reached EB
        double ebOut = 0;     // offset of the payload removed
        double payTotal = 0;  // payload bytes seen
        double now = 0;
        bool started = false;
        // access units: end offset, decoding time, arrival of the first byte
        struct Au
        {
            double end = 0;
            double td = 0;
            double firstArrival = 0;
            bool late = false;  // its bytes were still coming at its decoding time
        };
        std::deque<Au> pending;
        bool auOpen = false;
        Au open;
        // late access units: end offset and decoding time, until whole
        std::deque<std::pair<double, double>> late;
        // findings
        uint64_t aus = 0;
        double largest = 0;  // the largest picture, bytes
        uint64_t underflows = 0;
        double worstLate = 0;
        double firstLateAt = -1;
        uint64_t slow = 0;  // waited longer than 1 s
        double worstWait = 0;
        double firstSlowAt = -1;
        std::vector<uint8_t> seqProbe;  // the start of a PES with a sequence header, until its extension is in
    };

    void packet(const uint8_t* p);
    void readPsi(const uint8_t* p, int pid, bool start, const uint8_t* payload, int len);
    void videoPacket(Video& v, int pid, bool start, const uint8_t* payload, int len);
    void timed(Video& v, const Packet& pk, double arrival, double rate);
    void advance(Video& v, double to);
    void removeAu(Video& v, const Video::Au& au);
    void assignTimes(bool final);
    static bool readSequence(Video& v, const std::vector<uint8_t>& es);
    double line(int64_t raw90, int64_t pcrRaw, double pcrLine) const;

    int m_packetSize;
    std::vector<uint8_t> m_carry;  // the start of a packet cut by the end of a write
    uint64_t m_index = 0;
    int m_pmtPid = -1;
    int m_pcrPid = -1;
    std::map<int, Video> m_video;
    std::vector<uint8_t> m_pmtSection;
    std::vector<Packet> m_waiting;  // since the last PCR
    // the PCR time line, unwrapped, in 27 MHz ticks
    bool m_havePcr = false;
    int64_t m_lastPcrRaw = 0;
    double m_lastPcrLine = 0;
    double m_firstPcrLine = 0;
    uint64_t m_lastPcrIndex = 0;
    double m_lastRate = 0;  // bytes per tick between the last two PCRs
    bool m_finished = false;
    std::map<int, std::vector<uint8_t>> m_pesStart;  // the first bytes of a video PES cut over packets
};

#endif  // TSTD_CHECK_H_
