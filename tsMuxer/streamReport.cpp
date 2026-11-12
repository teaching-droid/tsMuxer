// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "streamReport.h"

#include <algorithm>
#include <mutex>
#include <sstream>

#include "vod_common.h"

bool StreamReport::wanted = false;

namespace
{
std::mutex g_lock;
std::vector<std::string> g_blocks;

// 1234567 as 1,234,567
std::string grouped(const int64_t value)
{
    std::string digits = std::to_string(value < 0 ? -value : value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<size_t>(at), ",");
    return value < 0 ? "-" + digits : digits;
}

std::string fixed(const double value, const int decimals)
{
    std::ostringstream text;
    text.setf(std::ios::fixed);
    text.precision(decimals);
    text << value;
    return text.str();
}

std::string rate(const double bitsPerSecond)
{
    return bitsPerSecond >= 1e6 ? fixed(bitsPerSecond / 1e6, 1) + " Mbit/s" : fixed(bitsPerSecond / 1e3, 0) + " kbit/s";
}

std::string clock(const int64_t seconds)
{
    std::ostringstream text;
    const auto two = [&text](const int64_t n) { text << (n < 10 ? "0" : "") << n; };
    two(seconds / 3600);
    text << ':';
    two(seconds / 60 % 60);
    text << ':';
    two(seconds % 60);
    return text.str();
}

std::string line(const char* label, const std::string& text)
{
    std::string out = std::string("  ") + label + ":";
    out.resize((std::max)(out.size() + 1, size_t{23}), ' ');
    return out + text;
}
}  // namespace

void StreamReport::picture(const Picture type, const Group group, const bool usedAsReference)
{
    ++m_count[static_cast<int>(type)];
    if (group != Group::None)
    {
        if (m_groupLen >= 0)
        {
            m_groupMin = m_groupsEnded == 0 ? m_groupLen : (std::min)(m_groupMin, m_groupLen);
            m_groupMax = (std::max)(m_groupMax, m_groupLen);
            ++m_groupsEnded;
        }
        m_groupLen = 0;
        ++(group == Group::Closed ? m_closed : m_open);
    }
    if (m_groupLen >= 0)
        ++m_groupLen;
    if (type == Picture::B)
    {
        m_bRunMax = (std::max)(m_bRunMax, ++m_bRun);
        if (usedAsReference)
            ++m_referenceB;
    }
    else
        m_bRun = 0;
}

void StreamReport::slices(const int count)
{
    if (count <= 0)
        return;
    m_slicesMin = m_slicesMin == 0 ? count : (std::min)(m_slicesMin, count);
    m_slicesMax = (std::max)(m_slicesMax, count);
}

void StreamReport::coded(const bool asFields, const bool interlacedFrame)
{
    m_codedKnown = true;
    if (asFields)
        ++m_fieldPictures;
    else if (interlacedFrame)
        ++m_interlacedFrames;
}

void StreamReport::shownFor(const int fields)
{
    if (fields >= 3)
    {
        ++m_heldPictures;
        m_heldFields += fields - 2;
    }
}

void StreamReport::referenceFrames(const int limit) { m_referenceFrames = (std::max)(m_referenceFrames, limit); }

void StreamReport::bytes(const int64_t dts, const int64_t count)
{
    if (count <= 0)
        return;
    if (m_firstDts == -1)
        m_firstDts = dts;
    m_lastDts = (std::max)(m_lastDts, dts);
    m_bytes += count;
    // A decoding time before the first one counts with the first second. A time a week on is damage: it does not
    // make the list that long.
    static constexpr int64_t LONGEST = 7 * 24 * 3600;
    const int64_t second = dts <= m_firstDts ? 0 : (dts - m_firstDts) / INTERNAL_PTS_FREQ;
    if (second >= LONGEST)
        return;
    if (static_cast<size_t>(second) >= m_bytesOfSecond.size())
        m_bytesOfSecond.resize(static_cast<size_t>(second) + 1, 0);
    m_bytesOfSecond[static_cast<size_t>(second)] += count;
}

std::vector<std::string> StreamReport::lines(const double fps, const bool interlaced) const
{
    std::vector<std::string> out;
    const int64_t pictures = m_count[0] + m_count[1] + m_count[2];
    // The time the pictures are shown for, at the frame rate: a picture is two fields, and one that is held (pulldown)
    // the fields more it is held for. With pulldown the pictures come fewer to the second than the frame rate says.
    const double length = fps > 0
                              ? static_cast<double>(2 * pictures + m_heldFields) / (2 * fps)
                              : static_cast<double>(m_lastDts - m_firstDts) / static_cast<double>(INTERNAL_PTS_FREQ);
    const double pictureRate = fps > 0 && length > 0 ? static_cast<double>(pictures) / length : 0.0;
    out.push_back(line("Pictures", grouped(pictures) + " (I " + grouped(m_count[0]) + ", P " + grouped(m_count[1]) +
                                       ", B " + grouped(m_count[2]) + ")"));

    // The group still being read when the stream ends is cut short by the end: it counts for the longest, and for
    // the shortest only where no group ended before it.
    if (m_groupLen < 0)
        out.push_back(line("Group of pictures", "no picture that begins a group was found"));
    else
    {
        const int64_t shortest = m_groupsEnded == 0 ? m_groupLen : m_groupMin;
        const int64_t longest = (std::max)(m_groupMax, m_groupLen);
        const auto seconds = [pictureRate](const int64_t n) { return fixed(static_cast<double>(n) / pictureRate, 2); };
        std::string text;
        if (shortest == longest)
            text = grouped(longest) + (longest == 1 ? " picture" : " pictures") +
                   (pictureRate > 0 ? " (" + seconds(longest) + " s)" : "") + " throughout";
        else
            text = grouped(shortest) + " to " + grouped(longest) + " pictures" +
                   (pictureRate > 0 ? " (" + seconds(shortest) + " to " + seconds(longest) + " s)" : "");
        out.push_back(line("Group of pictures", text));
        out.push_back(line("Groups", "closed " + grouped(m_closed) + ", open " + grouped(m_open)));
    }

    if (m_count[2] == 0)
        out.push_back(line("B pictures in a row", "none"));
    else
        out.push_back(line("B pictures in a row",
                           grouped(m_bRunMax) + " at most; " +
                               (m_referenceB == 0 ? std::string("none is used as reference")
                                                  : grouped(m_referenceB) + (m_referenceB == 1 ? " is" : " are") +
                                                        " used as reference")));
    if (m_referenceFrames > 0)
        out.push_back(line("Reference frames", grouped(m_referenceFrames) + " at most"));
    if (m_slicesMax > 0)
        out.push_back(line("Slices per picture", m_slicesMin == m_slicesMax
                                                     ? grouped(m_slicesMax)
                                                     : grouped(m_slicesMin) + " to " + grouped(m_slicesMax)));

    std::string cadence;
    if (!m_codedKnown)
        cadence = interlaced ? "interlaced" : "progressive";
    else if (m_fieldPictures == 0 && m_interlacedFrames == 0)
        cadence = "progressive";
    else if (m_interlacedFrames == 0 && m_fieldPictures == pictures)
        cadence = "interlaced, each picture coded as two fields";
    else if (m_fieldPictures == 0)
        cadence = "interlaced, the pictures coded as frames";
    else
        cadence = "interlaced, " + grouped(m_fieldPictures) + " pictures coded as two fields, the others as frames";
    if (m_codedKnown)
        cadence += m_heldPictures == 0
                       ? ", no pulldown"
                       : ", " + grouped(m_heldPictures) + (m_heldPictures == 1 ? " picture is" : " pictures are") +
                             " held for a third field (pulldown)";
    out.push_back(line("Cadence", cadence));

    if (m_bytes > 0 && length > 0)
    {
        std::string text = rate(static_cast<double>(m_bytes) * 8 / length) + " on average";
        const auto highest = std::max_element(m_bytesOfSecond.begin(), m_bytesOfSecond.end());
        if (m_bytesOfSecond.size() > 1 && highest != m_bytesOfSecond.end())
            text += ", " + rate(static_cast<double>(*highest) * 8) + " at most over 1 s (at " +
                    clock(highest - m_bytesOfSecond.begin()) + ")";
        out.push_back(line("Bitrate", text));
    }
    return out;
}

void StreamReport::give(const std::string& heading, const std::vector<std::string>& lines)
{
    const std::lock_guard<std::mutex> guard(g_lock);
    g_blocks.push_back(heading);
    g_blocks.insert(g_blocks.end(), lines.begin(), lines.end());
}

std::vector<std::string> StreamReport::all()
{
    const std::lock_guard<std::mutex> guard(g_lock);
    return g_blocks;
}
