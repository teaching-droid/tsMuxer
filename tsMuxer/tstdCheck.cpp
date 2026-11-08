// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "tstdCheck.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "vod_common.h"

namespace
{
constexpr double CLOCK = 27000000.0;                    // 27 MHz ticks a second
constexpr int64_t PCR_WRAP = (int64_t{1} << 33) * 300;  // the PCR and PTS/DTS wrap, in 27 MHz ticks

// H.262 Tables 8-12 and 8-13: Rmax (bit/s) and the largest vbv_buffer_size (bits) of a profile and level, from the
// profile_and_level_indication of the sequence extension
bool limits(const int pl, double& rmax, double& vbvMax, bool& lowOrMainLevel)
{
    if (pl & 0x80)  // escape: the 4:2:2 profile
    {
        lowOrMainLevel = pl == 0x85;
        if (pl == 0x85)
            rmax = 50e6, vbvMax = 9437184;
        else if (pl == 0x82)
            rmax = 300e6, vbvMax = 47185920;
        else
            return false;
        return true;
    }
    const int profile = pl >> 4 & 7, level = pl & 15;
    lowOrMainLevel = level == 10 || level == 8;
    struct Limit
    {
        int profile, level;
        double rmax, vbv;
    };
    // profile 5 Simple, 4 Main, 3 SNR, 2 Spatial, 1 High; level 10 Low, 8 Main, 6 High-1440, 4 High
    static constexpr Limit table[] = {{5, 8, 15e6, 1835008}, {4, 10, 4e6, 475136},   {4, 8, 15e6, 1835008},
                                      {4, 6, 60e6, 7340032}, {4, 4, 80e6, 9781248},  {3, 10, 4e6, 475136},
                                      {3, 8, 15e6, 1835008}, {2, 6, 60e6, 7340032},  {1, 8, 20e6, 2441216},
                                      {1, 6, 80e6, 9781248}, {1, 4, 100e6, 12222464}};
    for (const Limit& l : table)
        if (l.profile == profile && l.level == level)
        {
            rmax = l.rmax;
            vbvMax = l.vbv;
            return true;
        }
    return false;
}

// Bytes appended through resize and copy: vector::insert from a pointer range gives a false -Wstringop-overflow in GCC
// 13
void append(std::vector<uint8_t>& to, const uint8_t* from, const size_t count)
{
    if (count == 0)
        return;
    const size_t at = to.size();
    to.resize(at + count);
    std::copy_n(from, count, to.data() + at);
}
}  // namespace

void TStdCheck::feed(const uint8_t* data, const size_t len)
{
    if (m_finished)
        return;
    size_t at = 0;
    if (!m_carry.empty())
    {
        const size_t need = m_packetSize - m_carry.size();
        const size_t take = (std::min)(need, len);
        append(m_carry, data, take);
        at = take;
        if (m_carry.size() < static_cast<size_t>(m_packetSize))
            return;
        packet(m_carry.data() + (m_packetSize - 188));
        m_carry.clear();
    }
    for (; at + m_packetSize <= len; at += m_packetSize) packet(data + at + (m_packetSize - 188));
    if (at < len)
        m_carry.clear();
    append(m_carry, data + at, len - at);
}

void TStdCheck::packet(const uint8_t* p)
{
    const uint64_t index = m_index++;
    if (p[0] != 0x47)
        return;
    const int pid = (p[1] & 0x1F) << 8 | p[2];
    const bool start = p[1] & 0x40;
    const int afc = p[3] >> 4 & 3;
    int offset = 4;
    if (afc & 2)
    {
        const int afLen = p[4];
        if (afLen > 0 && pid == m_pcrPid && (p[5] & 0x10) && afLen >= 7)
        {
            const int64_t base = static_cast<int64_t>(p[6]) << 25 | p[7] << 17 | p[8] << 9 | p[9] << 1 | p[10] >> 7;
            const int64_t ext = (p[10] & 1) << 8 | p[11];
            const int64_t raw = base * 300 + ext;
            if (!m_havePcr)
            {
                m_havePcr = true;
                m_lastPcrLine = static_cast<double>(raw);
                m_firstPcrLine = m_lastPcrLine;
                m_waiting.clear();  // nothing before the first PCR is timed
            }
            else
            {
                int64_t d = (raw - m_lastPcrRaw) % PCR_WRAP;
                if (d < 0)
                    d += PCR_WRAP;
                const double lineNow = m_lastPcrLine + static_cast<double>(d);
                if (lineNow > m_lastPcrLine && index > m_lastPcrIndex)
                    m_lastRate = static_cast<double>(index - m_lastPcrIndex) * 188 / (lineNow - m_lastPcrLine);
                assignTimes(false);
                m_lastPcrLine = lineNow;
            }
            m_lastPcrRaw = raw;
            m_lastPcrIndex = index;
        }
        offset = 5 + afLen;
    }
    if (!(afc & 1) || offset >= 188)
        return;
    const uint8_t* payload = p + offset;
    const int len = 188 - offset;
    if (pid == 0 || pid == m_pmtPid)
        readPsi(p, pid, start, payload, len);
    else if (const auto v = m_video.find(pid); v != m_video.end() && m_havePcr)
        videoPacket(v->second, pid, start, payload, len);
}

// The PAT names the PMT; the PMT names the PCR PID and the MPEG-2 video PIDs (stream types 0x01 and 0x02)
void TStdCheck::readPsi(const uint8_t*, const int pid, const bool start, const uint8_t* payload, const int len)
{
    std::vector<uint8_t> section;
    if (pid == 0)
    {
        if (!start || len < 1 + payload[0] + 12)
            return;
        const uint8_t* s = payload + 1 + payload[0];
        const int sectionLen = (s[1] & 0x0F) << 8 | s[2];
        for (int k = 8; k + 4 <= 3 + sectionLen - 4 && k + 4 <= len - 1 - payload[0]; k += 4)
        {
            const int program = s[k] << 8 | s[k + 1];
            if (program != 0)
            {
                m_pmtPid = (s[k + 2] & 0x1F) << 8 | s[k + 3];
                break;
            }
        }
        return;
    }
    if (start)
    {
        if (len < 1 + payload[0])
            return;
        m_pmtSection.clear();
        append(m_pmtSection, payload + 1 + payload[0], static_cast<size_t>(len - 1 - payload[0]));
    }
    else if (!m_pmtSection.empty())
        append(m_pmtSection, payload, len);
    if (m_pmtSection.size() < 12)
        return;
    const uint8_t* s = m_pmtSection.data();
    const size_t sectionLen = (s[1] & 0x0F) << 8 | s[2];
    if (m_pmtSection.size() < 3 + sectionLen)
        return;
    m_pcrPid = (s[8] & 0x1F) << 8 | s[9];
    const size_t infoLen = (s[10] & 0x0F) << 8 | s[11];
    for (size_t k = 12 + infoLen; k + 5 <= 3 + sectionLen - 4;)
    {
        const int type = s[k];
        const int esPid = (s[k + 1] & 0x1F) << 8 | s[k + 2];
        const size_t esInfo = (s[k + 3] & 0x0F) << 8 | s[k + 4];
        if (type == 0x01 || type == 0x02)
            m_video.try_emplace(esPid);
        k += 5 + esInfo;
    }
    m_pmtSection.clear();
}

// The sequence header and its extension (H.262 6.2.2.1, 6.2.2.3): bit rate, vbv_buffer_size, profile and level,
// low_delay; and the buffer sizes and rates of 2.4.2.7 from them
bool TStdCheck::readSequence(Video& v, const std::vector<uint8_t>& es)
{
    for (size_t i = 0; i + 12 <= es.size(); ++i)
    {
        if (es[i] != 0 || es[i + 1] != 0 || es[i + 2] != 1 || es[i + 3] != 0xB3)
            continue;
        const uint8_t* d = es.data() + i + 4;
        int64_t bitRate = static_cast<int64_t>(d[4]) << 10 | d[5] << 2 | d[6] >> 6;
        int64_t vbv = (d[6] & 0x1F) << 5 | d[7] >> 3;
        for (size_t e = i + 12; e + 10 <= es.size() && e < i + 400; ++e)
        {
            if (es[e] != 0 || es[e + 1] != 0 || es[e + 2] != 1 || es[e + 3] != 0xB5 || es[e + 4] >> 4 != 1)
                continue;
            const uint8_t* x = es.data() + e + 4;
            const int pl = (x[0] & 0x0F) << 4 | x[1] >> 4;
            bitRate |= static_cast<int64_t>((x[2] & 0x1F) << 7 | x[3] >> 1) << 18;
            vbv |= static_cast<int64_t>(x[4]) << 10;
            v.lowDelay = x[5] >> 7;
            double rmax = 0, vbvMax = 0;
            bool lowOrMain = false;
            if (!limits(pl, rmax, vbvMax, lowOrMain))
                return false;
            const double rate = static_cast<double>(bitRate) * 400;
            v.ebs = static_cast<double>(vbv) * 16384 / 8;
            v.rxBytes = 1.2 * rmax / 8 / CLOCK;
            v.rbxBytes = (lowOrMain ? rmax : (std::min)(1.05 * rate, rmax)) / 8 / CLOCK;
            v.params = v.ebs > 0 && v.rbxBytes > 0;
            return v.params;
        }
        return false;  // an MPEG-1 sequence, or the extension is not in yet
    }
    return false;
}

void TStdCheck::videoPacket(Video& v, const int pid, const bool start, const uint8_t* payload, const int len)
{
    v.pid = pid;
    Packet pk{pid, m_index - 1, 0, len, false, 0, m_lastPcrRaw, m_lastPcrLine};
    if (start)
    {
        // the PES header: 00 00 01, stream id, length, flags, header length, PTS and DTS
        if (len < 9 || payload[0] != 0 || payload[1] != 0 || payload[2] != 1 || len < 9 + payload[8])
            return;
        const int flags = payload[7] >> 6;
        const auto ts = [payload](const int at)
        {
            return static_cast<int64_t>(payload[at] >> 1 & 7) << 30 | payload[at + 1] << 22 |
                   (payload[at + 2] >> 1) << 15 | payload[at + 3] << 7 | payload[at + 4] >> 1;
        };
        pk.header = 9 + payload[8];
        pk.payload = len - pk.header;
        if (flags & 2)
        {
            pk.startsAu = true;
            pk.dts90 = flags == 3 ? ts(14) : ts(9);
        }
    }
    if (!v.params)
    {
        // the sequence header comes at the start of a PES; it is looked for there and in the packets after it
        const uint8_t* es = payload + pk.header;
        if (start)
            v.seqProbe.clear();
        if (start || !v.seqProbe.empty())
            append(v.seqProbe, es, pk.payload);
        if (v.seqProbe.size() > 4096 || !readSequence(v, v.seqProbe))
        {
            if (v.seqProbe.size() > 4096)
                v.seqProbe.clear();
            return;
        }
        v.seqProbe.clear();
        // the model starts at the next access unit: this one began before the sizes were known
        return;
    }
    if (!v.started)
    {
        if (!pk.startsAu)
            return;
        v.started = true;
    }
    m_waiting.push_back(pk);
}

// The PCR before the packets and the rate up to this PCR give each waiting packet its time (2.4.2.2, eq. 2-4)
void TStdCheck::assignTimes(const bool)
{
    if (m_lastRate <= 0)
    {
        m_waiting.clear();
        return;
    }
    for (const Packet& pk : m_waiting)
    {
        const double arrival = m_lastPcrLine + (static_cast<double>(pk.index - m_lastPcrIndex) * 188 - 10) / m_lastRate;
        timed(m_video[pk.pid], pk, arrival, m_lastRate);
    }
    m_waiting.clear();
}

// A time stamp on the PCR time line: against the PCR before its packet, so that a wrap is crossed
double TStdCheck::line(const int64_t raw90, const int64_t pcrRaw, const double pcrLine) const
{
    int64_t dv = (raw90 * 300 - pcrRaw) % PCR_WRAP;
    if (dv < 0)
        dv += PCR_WRAP;
    if (dv >= PCR_WRAP / 2)
        dv -= PCR_WRAP;
    return pcrLine + static_cast<double>(dv);
}

void TStdCheck::timed(Video& v, const Packet& pk, const double arrival, const double rate)
{
    // TB: whole packets in, out at Rx (2.4.2.4)
    const double end = arrival + 187 / rate;
    const double begin = (std::max)(arrival, v.tbLastDeparture);
    const double departure = (std::max)(end, begin + 188 / v.rxBytes);
    v.tbLastDeparture = departure;
    if (v.now == 0)
        v.now = departure;
    // the pictures due before this packet reaches MB are taken from EB first
    while (!v.pending.empty() && v.pending.front().td <= departure)
    {
        const Video::Au au = v.pending.front();
        v.pending.pop_front();
        advance(v, au.td);
        removeAu(v, au);
    }
    // the picture still coming in at its decoding time is late; it is taken out when its end is known
    if (v.auOpen && !v.open.late && v.open.td <= departure)
    {
        advance(v, v.open.td);
        v.open.late = true;
    }
    advance(v, departure);
    if (pk.startsAu)
    {
        if (v.auOpen && v.payTotal > v.open.end)
        {
            Video::Au closed = v.open;
            closed.end = v.payTotal;
            if (closed.late)
                removeAu(v, closed);
            else
                v.pending.push_back(closed);
        }
        v.open = {v.payTotal, line(pk.dts90, pk.pcrRaw, pk.pcrLine), arrival};
        v.auOpen = true;
    }
    if (pk.header > 0)
        v.mb.emplace_back(true, pk.header);
    if (pk.payload > 0)
        v.mb.emplace_back(false, pk.payload);
    v.payTotal += pk.payload;
}

// MB to EB at Rbx while there is payload in MB and room in EB (the leak method, 2.4.2.7); a late picture is whole when
// the last of its bytes is in
void TStdCheck::advance(Video& v, const double to)
{
    if (to <= v.now)
        return;
    double budget = (to - v.now) * v.rbxBytes;
    const double budget0 = budget;
    double room = v.ebs - (v.ebIn - v.ebOut);
    while (budget > 1e-9 && !v.mb.empty())
    {
        auto& [header, bytes] = v.mb.front();
        if (header)
        {
            if (v.mb.size() > 1)  // a header is dropped as the payload after it goes on
            {
                v.mb.pop_front();
                continue;
            }
            break;
        }
        if (room <= 0)
            break;
        const double take = (std::min)({budget, bytes, room});
        while (!v.late.empty() && v.late.front().first <= v.ebIn + take)
        {
            const double whole = v.now + (budget0 - budget + (v.late.front().first - v.ebIn)) / v.rbxBytes;
            const double lateBy = whole - v.late.front().second;
            if (lateBy > v.worstLate)
                v.worstLate = lateBy;
            v.late.pop_front();
        }
        bytes -= take;
        v.ebIn += take;
        budget -= take;
        room -= take;
        if (bytes <= 1e-9)
            v.mb.pop_front();
    }
    v.now = to;
}

// A picture taken from EB at its decoding time: whole, or late; and how long its first byte waited (2.4.2.6)
void TStdCheck::removeAu(Video& v, const Video::Au& au)
{
    ++v.aus;
    if (diagReport::framesActive)
    {
        // the picture as the model takes it out: the buffer then, and how long its first byte waited
        const auto dts = static_cast<int64_t>(std::llround(au.td / 300));
        const double time = diagReport::frameTime(dts);
        if (diagReport::framesWanted(time))
        {
            const bool late = (au.late || v.ebIn + 0.5 < au.end) && !v.lowDelay;
            std::ostringstream row;
            row << "buffer			" << v.pid << "	MPEG-2			" << dts << '	'
                << doubleToStr(time, 6) << '	' << std::llround(au.end - v.ebOut) << "		"
                << doubleToStr((au.td - au.firstArrival) / 27000, 3) << '	' << std::llround(v.ebIn - v.ebOut)
                << '	' << std::llround(v.ebs) << '	' << (late ? "late" : "");
            diagReport::frameRow(row.str());
        }
    }
    // where the picture began: the end of the one before
    v.largest = (std::max)(v.largest, au.end - v.ebOut);
    const double wait = au.td - au.firstArrival;
    if (wait > CLOCK)
    {
        if (v.slow++ == 0)
            v.firstSlowAt = au.td;
        v.worstWait = (std::max)(v.worstWait, wait);
    }
    if ((au.late || v.ebIn + 0.5 < au.end) && !v.lowDelay)
    {
        if (v.underflows++ == 0)
            v.firstLateAt = au.td;
        if (v.ebIn + 0.5 < au.end)
            v.late.emplace_back(au.end, au.td);  // whole when the last of its bytes is in (advance)
        else
            v.worstLate = (std::max)(v.worstLate, v.now - au.td);
    }
    v.ebOut = au.end;
}

void TStdCheck::finish()
{
    if (m_finished)
        return;
    m_finished = true;
    // the packets after the last PCR have no time (2.4.2.2) and are not judged, nor the picture they belong to
    std::map<int, bool> cut;
    for (const Packet& pk : m_waiting) cut[pk.pid] = true;
    m_waiting.clear();
    for (auto& [pid, v] : m_video)
    {
        if (!v.started)
            continue;
        if (v.auOpen && !cut[pid] && v.payTotal > v.open.end)
        {
            Video::Au last = v.open;
            last.end = v.payTotal;
            v.pending.push_back(last);
        }
        for (const Video::Au& au : v.pending)
        {
            advance(v, au.td);
            removeAu(v, au);
        }
        v.pending.clear();
        // a late picture still not whole: the rest of MB goes on at Rbx
        if (!v.late.empty())
            advance(v, v.now + 600 * CLOCK);
        // A picture larger than the buffer the stream declares cannot be whole in it at any rate: the stream was
        // encoded against a buffer size it does not fit, which the mux cannot change. That is said, not the cap: a test
        // stream declaring 6144 bytes for pictures of up to 13 kB had pictures late with no cap at all.
        if (v.underflows > 0 && v.largest > v.ebs)
            LTRACE(LT_WARN, 2,
                   "Warning: the MPEG-2 video on PID 0x"
                       << std::hex << pid << std::dec << " declares a decoder buffer (vbv_buffer_size) of "
                       << llround(v.ebs) << " bytes, smaller than its largest picture of " << llround(v.largest)
                       << " bytes, so " << v.underflows << " of its " << v.aus
                       << " pictures cannot be whole in it at their decoding time (the T-STD of H.222.0). The video "
                          "was encoded with a wrong buffer size; a player that goes by it may pause there. Re-encoding "
                          "the video with a correct buffer size removes it; the mux cannot.");
        else if (v.underflows > 0)
            LTRACE(LT_WARN, 2,
                   "Warning: the MPEG-2 video on PID 0x"
                       << std::hex << pid << std::dec << " comes late in the decoder's buffers (the T-STD of H.222.0): "
                       << v.underflows << " of its " << v.aus
                       << (v.underflows == 1 ? " pictures is not whole at its decoding time"
                                             : " pictures are not whole at their decoding time")
                       << ", up to " << doubleToStr(v.worstLate / 27000, 1) << " ms late, the first "
                       << floatToTime((v.firstLateAt - m_firstPcrLine) / CLOCK)
                       << " into the output. A player may pause or show damaged pictures there. The video's peaks are "
                          "more than the output carries: give a higher --maxbitrate, or encode the video at a lower "
                          "peak rate.");
        if (v.slow > 0)
            LTRACE(LT_WARN, 2,
                   "Warning: "
                       << v.slow << (v.slow == 1 ? " picture" : " pictures") << " of the MPEG-2 video on PID 0x"
                       << std::hex << pid << std::dec << (v.slow == 1 ? " waits" : " wait")
                       << " longer than one second in the decoder's buffers, up to "
                       << doubleToStr(v.worstWait / CLOCK, 2) << " s, the first "
                       << floatToTime((v.firstSlowAt - m_firstPcrLine) / CLOCK)
                       << " into the output, where H.222.0 allows one second (2.4.2.6). Some players stall on such a "
                          "stream: give a shorter --vbv-len.");
    }
}
