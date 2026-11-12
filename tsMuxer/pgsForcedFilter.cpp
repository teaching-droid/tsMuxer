// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "pgsForcedFilter.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace
{
constexpr uint8_t SEG_PALETTE = 0x14;
constexpr uint8_t SEG_OBJECT = 0x15;
constexpr uint8_t SEG_COMPOSITION = 0x16;
constexpr uint8_t SEG_WINDOWS = 0x17;
constexpr uint8_t SEG_END = 0x80;

constexpr uint8_t STATE_MASK = 0xC0;
constexpr uint8_t STATE_NORMAL = 0x00;
constexpr uint8_t STATE_EPOCH_START = 0x80;

// the rates of the PGS decoder model, in pixels a second: an object is decoded at one, a window drawn at the other
constexpr double DECODING_RATE = 128.0 / 8 * 1000000;
constexpr double COMPOSITION_RATE = 256.0 / 8 * 1000000;

int64_t decodeTicks(const double pixels) { return static_cast<int64_t>(ceil(90000.0 * pixels / DECODING_RATE)); }
int64_t drawTicks(const double pixels) { return static_cast<int64_t>(ceil(90000.0 * pixels / COMPOSITION_RATE)); }

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

// The payload of a segment. Of no segment at all, as the windows of an epoch that has not defined any, the size is 0:
// taken off an empty one it ran below zero, the size seemed endless, and the windows were read at address 3.
const uint8_t* body(const std::vector<uint8_t>& segment) { return segment.data() + 3; }
size_t bodySize(const std::vector<uint8_t>& segment) { return segment.size() >= 3 ? segment.size() - 3 : 0; }

struct Placed
{
    uint16_t id = 0;
    bool forced = false;
    std::vector<uint8_t> entry;  // its entry in the composition, 8 bytes or 16 with a crop
};

// the composition objects of a composition segment; false when the segment is too short for what it says
bool readComposition(const std::vector<uint8_t>& segment, std::vector<Placed>& objects)
{
    if (bodySize(segment) < 11)
        return false;
    const uint8_t* p = body(segment);
    const uint8_t* const end = p + bodySize(segment);
    const int count = p[10];
    p += 11;
    for (int i = 0; i < count; ++i)
    {
        if (end - p < 8)
            return false;
        const size_t size = p[3] & 0x80 ? 16 : 8;
        if (static_cast<size_t>(end - p) < size)
            return false;
        Placed o;
        o.id = be16(p);
        o.forced = (p[3] & 0x40) != 0;
        o.entry.assign(p, p + size);
        objects.push_back(o);
        p += size;
    }
    return true;
}

std::vector<uint8_t> segmentOf(const uint8_t type, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> s{type, static_cast<uint8_t>(payload.size() >> 8), static_cast<uint8_t>(payload.size())};
    s.insert(s.end(), payload.begin(), payload.end());
    return s;
}
}  // namespace

void PgsForcedFilter::push(const uint8_t* segment, const size_t length, const int64_t pts, const int64_t dts,
                           std::deque<Segment>& out)
{
    if (length < 3)
        return;
    const uint8_t type = segment[0];
    if (type == SEG_COMPOSITION)
    {
        if (!m_set.empty())
            finishSet(out);  // a set that never got its end
    }
    else if (m_set.empty())
        return;  // a segment outside a display set
    Segment s;
    s.pts = pts;
    s.dts = dts;
    s.startsSet = type == SEG_COMPOSITION;
    s.data.assign(segment, segment + length);
    m_set.push_back(std::move(s));
    if (type == SEG_END)
        finishSet(out);
}

void PgsForcedFilter::flush(std::deque<Segment>& out)
{
    if (!m_set.empty())
        finishSet(out);
}

void PgsForcedFilter::write(std::deque<Segment>& out, std::vector<Segment>& set)
{
    // in decoding order, and never before what was written last
    for (Segment& s : set)
    {
        if (s.dts <= m_lastEndDts)
            s.dts = m_lastEndDts + (s.startsSet ? 1 : 0);
        if (s.pts < s.dts)
            s.pts = s.dts;
        m_lastEndDts = s.dts;
        out.push_back(std::move(s));
    }
    set.clear();
}

void PgsForcedFilter::finishSet(std::deque<Segment>& out)
{
    std::vector<Segment> set;
    set.swap(m_set);
    ++m_sourceSets;
    const Segment& pcs = set.front();
    std::vector<Placed> placed;
    if (!readComposition(pcs.data, placed))
        return;  // a damaged composition: nothing of the set is used
    const uint8_t* const comp = body(pcs.data);
    const uint8_t state = comp[7] & STATE_MASK;
    const uint8_t paletteId = comp[9];

    // the epoch as a decoder keeps it: an epoch start forgets what came before
    if (state == STATE_EPOCH_START)
    {
        m_objects.clear();
        m_palettes.clear();
        m_windows.clear();
    }
    bool hasData = false;  // the set brings a palette or object data
    for (const Segment& s : set)
    {
        const uint8_t type = s.data[0];
        if (type == SEG_WINDOWS)
            m_windows = s.data;
        else if (type == SEG_PALETTE && bodySize(s.data) >= 2)
        {
            m_palettes[body(s.data)[0]] = s.data;
            hasData = true;
        }
        else if (type == SEG_OBJECT && bodySize(s.data) >= 4)
        {
            const uint8_t* p = body(s.data);
            const uint16_t id = be16(p);
            const bool first = (p[3] & 0x80) != 0;
            if (first)
            {
                Object o;
                if (bodySize(s.data) >= 11)
                {
                    o.width = be16(p + 7);
                    o.height = be16(p + 9);
                }
                o.segments.push_back(s.data);
                m_objects[id] = std::move(o);
            }
            else if (auto it = m_objects.find(id); it != m_objects.end())
                it->second.segments.push_back(s.data);
            hasData = true;
        }
    }

    std::vector<Placed> forced;
    for (const Placed& o : placed)
        if (o.forced)
            forced.push_back(o);
    const bool allForced = forced.size() == placed.size();
    if (!allForced)
        ++m_plainPictures;

    // ---- nothing forced in this set: the forced picture, if one is shown, goes away
    if (forced.empty())
    {
        if (!m_shown)
            return;
        m_shown = false;
        m_shownObjects.clear();
        m_shownPalette.clear();
        if (placed.empty())
        {
            write(out, set);  // the source clears the screen itself: its set as it is
            return;
        }
        // the source goes on with a picture that is not forced: a set without objects, at its time
        double windowPixels = 0;
        if (bodySize(m_windows) >= 1)
            for (size_t i = 0, at = 1; i < body(m_windows)[0] && at + 9 <= bodySize(m_windows); ++i, at += 9)
                windowPixels += static_cast<double>(be16(body(m_windows) + at + 5)) * be16(body(m_windows) + at + 7);
        const int64_t draw = drawTicks(windowPixels);
        std::vector<uint8_t> payload(comp, comp + 11);
        payload[7] = STATE_NORMAL;
        payload[8] = 0;   // no palette update
        payload[10] = 0;  // no object
        std::vector<Segment> clear;
        const int64_t dts = pcs.pts - draw - 1;
        clear.push_back({pcs.pts, dts, true, segmentOf(SEG_COMPOSITION, payload)});
        if (!m_windows.empty())
            clear.push_back({pcs.pts - draw, dts, false, m_windows});
        clear.push_back({dts, dts, false, {SEG_END, 0, 0}});
        write(out, clear);
        return;
    }

    // ---- what the forced picture is: its objects with their places and data, and its colours
    std::string objects;
    for (const Placed& o : forced)
    {
        objects.append(o.entry.begin(), o.entry.end());
        if (const auto it = m_objects.find(o.id); it != m_objects.end())
            for (const auto& seg : it->second.segments) objects.append(seg.begin(), seg.end());
    }
    objects.append(m_windows.begin(), m_windows.end());
    std::string palette;
    if (const auto it = m_palettes.find(paletteId); it != m_palettes.end())
        palette.assign(it->second.begin(), it->second.end());

    if (m_shown && objects == m_shownObjects && palette == m_shownPalette)
        return;  // the same forced picture stays: only what is not forced changed
    const bool onlyColours = m_shown && objects == m_shownObjects;
    if (!m_shown || !onlyColours)
        ++m_forcedPictures;
    m_shown = true;
    m_shownObjects = objects;
    m_shownPalette = palette;

    // A set that is complete on its own and shows forced objects only is written as it is: a track of forced
    // pictures alone comes out as it came in
    if (allForced && state == STATE_EPOCH_START)
    {
        write(out, set);
        return;
    }

    // ---- only the colours change (a fade): a palette update, as the source may have had it for the whole picture
    std::vector<uint8_t> payload(comp, comp + 10);
    payload.push_back(static_cast<uint8_t>(forced.size()));
    for (const Placed& o : forced) payload.insert(payload.end(), o.entry.begin(), o.entry.end());
    std::vector<Segment> made;
    if (onlyColours && !palette.empty())
    {
        payload[7] = STATE_NORMAL;
        payload[8] = 0x80;  // palette update
        made.push_back({pcs.pts, pcs.dts, true, segmentOf(SEG_COMPOSITION, payload)});
        made.push_back({pcs.dts, pcs.dts, false, m_palettes[paletteId]});
        made.push_back({pcs.dts, pcs.dts, false, {SEG_END, 0, 0}});
        // not before the picture it recolours was decoded
        write(out, made);
        return;
    }

    // ---- the forced picture as an epoch start of its own, by the decoder model: the objects are decoded, then the
    // windows drawn, before the picture is shown
    double objectPixels = 0;
    std::set<uint16_t> ids;
    for (const Placed& o : forced)
        if (ids.insert(o.id).second)
            if (const auto it = m_objects.find(o.id); it != m_objects.end())
                objectPixels += static_cast<double>(it->second.width) * it->second.height;
    double windowPixels = 0;
    if (bodySize(m_windows) >= 1)
        for (size_t i = 0, at = 1; i < body(m_windows)[0] && at + 9 <= bodySize(m_windows); ++i, at += 9)
            windowPixels += static_cast<double>(be16(body(m_windows) + at + 5)) * be16(body(m_windows) + at + 7);
    const double picturePixels = static_cast<double>(be16(comp)) * be16(comp + 2);
    const int64_t decode = decodeTicks(objectPixels);
    const int64_t draw = drawTicks(windowPixels);
    const int64_t lead = (std::max)(drawTicks(picturePixels), decode) + draw;
    const int64_t dts = pcs.pts - lead;
    payload[7] = STATE_EPOCH_START;
    payload[8] = 0;
    made.push_back({pcs.pts, dts, true, segmentOf(SEG_COMPOSITION, payload)});
    if (!m_windows.empty())
        made.push_back({pcs.pts - draw, dts, false, m_windows});
    if (!palette.empty())
        made.push_back({dts, dts, false, m_palettes[paletteId]});
    for (const uint16_t id : ids)
        if (const auto it = m_objects.find(id); it != m_objects.end())
            for (const auto& seg : it->second.segments) made.push_back({dts + decode, dts, false, seg});
    made.push_back({dts + decode, dts + decode, false, {SEG_END, 0, 0}});
    (void)hasData;
    write(out, made);
}

void PgsForcedTrack::hand(std::deque<PgsForcedFilter::Segment>& made, std::deque<PgsForcedFilter::Segment>& out)
{
    if (!m_written && m_filter.plainPictures() > 0 && (!m_held.empty() || !made.empty()))
        m_written = true;
    std::deque<PgsForcedFilter::Segment>& to = m_written ? out : m_held;
    if (m_written)
    {
        for (auto& s : m_held) out.push_back(std::move(s));
        m_held.clear();
    }
    for (auto& s : made) to.push_back(std::move(s));
    made.clear();
}

void PgsForcedTrack::push(const uint8_t* data, const size_t size, const int64_t pts, const int64_t dts,
                          std::deque<PgsForcedFilter::Segment>& out)
{
    if (size == 0)
        return;
    if (m_segment.empty())
    {
        m_segmentPts = pts;
        m_segmentDts = dts;
    }
    m_segment.insert(m_segment.end(), data, data + size);
    if (m_segment.size() < 3)
        return;
    const size_t whole = 3 + (static_cast<size_t>(m_segment[1]) << 8 | m_segment[2]);
    if (m_segment.size() < whole)
        return;
    std::deque<PgsForcedFilter::Segment> made;
    m_filter.push(m_segment.data(), whole, m_segmentPts, m_segmentDts, made);
    m_segment.clear();
    hand(made, out);
}

void PgsForcedTrack::finish(std::deque<PgsForcedFilter::Segment>& out)
{
    std::deque<PgsForcedFilter::Segment> made;
    m_filter.flush(made);
    hand(made, out);
    m_held.clear();
}
