#include "discTitles.h"

#include <fs/directory.h>
#include <fs/discImage.h>
#include <fs/systemlog.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "dvdTitles.h"
#include "tsPacket.h"
#include "vod_common.h"

namespace
{
// A play item as a title plays it: a part of a clip
struct Item
{
    std::string clip;
    uint32_t in = 0;
    uint32_t out = 0;

    bool operator<(const Item& o) const
    {
        return clip != o.clip ? clip < o.clip : in != o.in ? in < o.in : out < o.out;
    }
    bool operator==(const Item& o) const { return clip == o.clip && in == o.in && out == o.out; }
};

struct Playlist
{
    DiscTitle title;
    std::vector<Item> items;
    std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>> parts;  // the footage of each clip, merged
    size_t unbrokenJoins = 0;  // play items joined to the one before without a pause (connection 5 or 6)
    std::string streams;       // the streams it offers: type, PID and language of each
    std::string marks;         // its chapter marks: play item and time of each
};

bool isFolder(const std::string& path)
{
    if (discImage::split(path, nullptr, nullptr))
        return discImage::isDir(path);
    std::error_code ec;
    return std::filesystem::is_directory(std::u8string(path.begin(), path.end()), ec);
}

std::string withoutEndSeparators(std::string s)
{
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

std::string hms(const double seconds)
{
    const auto s = static_cast<long long>(seconds + 0.5);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    return buf;
}

std::string minutesText(const double seconds)
{
    const auto s = static_cast<long long>(seconds + 0.5);
    if (s % 60 == 0)
        return std::to_string(s / 60) + (s == 60 ? " minute" : " minutes");
    return std::to_string(s) + " seconds";
}

// The footage two playlists share, each part of a clip counted once, in 45 kHz ticks
double sharedFootage(const Playlist& a, const Playlist& b)
{
    double shared = 0;
    for (const auto& [clip, partsA] : a.parts)
    {
        const auto it = b.parts.find(clip);
        if (it == b.parts.end())
            continue;
        for (const auto& [inA, outA] : partsA)
            for (const auto& [inB, outB] : it->second)
            {
                const uint32_t from = (std::max)(inA, inB), to = (std::min)(outA, outB);
                if (to > from)
                    shared += to - from;
            }
    }
    return shared / 45000.0;
}

// Runs of one play item, three or more in a row, making up 90 percent of the playlist: a menu's background or a
// short clip queued again and again. Empty when the playlist is no such loop; else the reason.
std::string repeatedReason(const std::vector<Item>& items)
{
    if (items.size() < 3)
        return {};
    std::set<Item> repeated;
    size_t queued = 0;
    for (size_t i = 0; i < items.size();)
    {
        size_t j = i + 1;
        while (j < items.size() && items[j] == items[i]) ++j;
        if (j - i >= 3)
        {
            repeated.insert(items[i]);
            queued += j - i;
        }
        i = j;
    }
    if (queued * 10 < items.size() * 9)
        return {};
    if (repeated.size() == 1)
        return "one clip repeated " + std::to_string(queued) + " times";
    return std::to_string(repeated.size()) + " clips repeated over and over, " + std::to_string(queued) +
           " play items in all";
}

Playlist readPlaylist(const std::string& path, const std::string& name)
{
    Playlist p;
    p.title.playlist = path;
    p.title.name = name;
    MPLSParser parser;
    parser.m_quietSkips = true;
    if (!parser.parse(path.c_str()))
    {
        p.title.note = "it cannot be read";
        return p;
    }
    for (size_t i = 0; i < parser.m_playItems.size(); ++i)
    {
        const MPLSPlayItem& pi = parser.m_playItems[i];
        const uint32_t out = (std::max)(pi.IN_time, pi.OUT_time);
        p.items.push_back({strToUpperCase(pi.fileName), pi.IN_time, out});
        p.title.seconds += (out - pi.IN_time) / 45000.0;
        p.title.angles = (std::max)(p.title.angles, pi.angles);
        if (i > 0 && (pi.connection_condition == 5 || pi.connection_condition == 6))
            ++p.unbrokenJoins;
    }
    // The footage of each clip, its parts merged where they overlap
    for (const Item& it : p.items) p.parts[it.clip].emplace_back(it.in, it.out);
    for (auto& [clip, parts] : p.parts)
    {
        std::sort(parts.begin(), parts.end());
        std::vector<std::pair<uint32_t, uint32_t>> merged;
        for (const auto& part : parts)
            if (!merged.empty() && part.first <= merged.back().second)
                merged.back().second = (std::max)(merged.back().second, part.second);
            else
                merged.push_back(part);
        parts = merged;
        for (const auto& [in, out] : parts) p.title.footage += (out - in) / 45000.0;
    }
    p.title.clips = p.items.size();
    p.title.chapters = parser.m_marks.size();
    for (const auto& m : parser.m_marks)
        p.marks += std::to_string(m.m_playItemID) + ':' + std::to_string(m.m_markTime) + ' ';
    for (const auto& s : parser.m_streamInfo)
        p.streams += std::to_string(static_cast<int>(s.stream_coding_type)) + ':' + std::to_string(s.streamPID) + ':' +
                     std::string(s.language_code, strnlen(s.language_code, 3)) + ' ';
    p.title.is3d = parser.isDependStreamExist;
    for (const auto& s : parser.m_streamInfo)
    {
        const std::string lang(s.language_code, strnlen(s.language_code, 3));
        if (isAudioStreamType(s.stream_coding_type) && !lang.empty() &&
            std::find(p.title.audio.begin(), p.title.audio.end(), lang) == p.title.audio.end())
            p.title.audio.push_back(lang);
    }
    return p;
}

// Playlist names with their numbers, runs shortened: 00300-00380 for 00300.mpls to 00380.mpls, every one there
std::string namesText(const std::vector<std::string>& names)
{
    const auto numberOf = [](const std::string& n)
    {
        const std::string stem = n.substr(0, n.find('.'));
        return stem.size() == 5 && stem.find_first_not_of("0123456789") == std::string::npos ? std::stoi(stem) : -1;
    };
    std::string out;
    for (size_t i = 0; i < names.size();)
    {
        size_t j = i;
        while (j + 1 < names.size() && numberOf(names[j]) >= 0 && numberOf(names[j + 1]) == numberOf(names[j]) + 1) ++j;
        out += out.empty() ? "" : " ";
        if (numberOf(names[i]) < 0)
            out += names[i];
        else
            out += j > i ? names[i].substr(0, 5) + "-" + names[j].substr(0, 5) : names[i].substr(0, 5);
        i = j + 1;
    }
    return out;
}
}  // namespace

std::string blurayDiscRoot(const std::string& path)
{
    const std::string p = withoutEndSeparators(path);
    if (p.empty())
        return {};
    const size_t sep = p.find_last_of("/\\");
    const std::string last = strToUpperCase(sep == std::string::npos ? p : p.substr(sep + 1));
    if (last == "BDMV" && isFolder(p))
        return sep == std::string::npos ? std::string(".") : sep == 0 ? p.substr(0, 1) : p.substr(0, sep);
    if (isFolder(p + "/BDMV"))
        return p;
    return {};
}

std::vector<std::pair<std::string, std::string>> discsInFolder(const std::string& folder)
{
    std::vector<std::pair<std::string, std::string>> discs;
    const std::string dir = withoutEndSeparators(folder);
    if (dir.empty() || discImage::split(dir, nullptr, nullptr))
        return discs;
    const char sep = dir.find('\\') != std::string::npos && dir.find('/') == std::string::npos ? '\\' : '/';
    std::error_code ec;
    std::filesystem::directory_iterator it(std::u8string(dir.begin(), dir.end()),
                                           std::filesystem::directory_options::skip_permission_denied, ec);
    for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        const std::u8string u = it->path().filename().u8string();
        const std::string path = dir + sep + std::string(u.begin(), u.end());
        std::error_code typeEc;
        const bool image = !it->is_directory(typeEc) && strToLowerCase(extractFileExt(path)) == "iso";
        if (!image && !it->is_directory(typeEc))
            continue;
        if (!blurayDiscRoot(path).empty())
            discs.emplace_back(path, image ? "Blu-ray image" : "Blu-ray folder");
        else if (!dvdVideoTs(path).empty())
            discs.emplace_back(path, image ? "DVD image" : "DVD folder");
    }
    std::sort(discs.begin(), discs.end(),
              [](const auto& a, const auto& b) { return strToUpperCase(a.first) < strToUpperCase(b.first); });
    return discs;
}

DiscTitles listDiscTitles(const std::string& root, const double minSeconds, const bool all)
{
    DiscTitles result;
    result.minSeconds = minSeconds;
    const std::string dir = root + getDirSeparator() + "BDMV" + getDirSeparator() + "PLAYLIST" + getDirSeparator();
    std::vector<std::string> names;
    findFiles(dir, "*", &names, false);
    names.erase(std::remove_if(names.begin(), names.end(),
                               [](const std::string& n) { return strToLowerCase(extractFileExt(n)) != "mpls"; }),
                names.end());
    std::sort(names.begin(), names.end(),
              [](const std::string& a, const std::string& b) { return strToUpperCase(a) < strToUpperCase(b); });

    std::vector<Playlist> lists;
    for (const auto& name : names) lists.push_back(readPlaylist(dir + name, name));

    // 1. Playlists that are no title, each with the reason: unreadable, one clip repeated, many short pieces
    std::vector<Playlist*> left;
    for (auto& p : lists)
    {
        if (p.title.note.empty())
            p.title.note = repeatedReason(p.items);
        if (p.title.note.empty() && p.items.size() >= 100 && p.title.seconds / p.items.size() < 60)
            p.title.note = std::to_string(p.items.size()) + " short pieces";
        if (p.title.note.empty())
            left.push_back(&p);
    }
    // 2. The same play items as another playlist, or the same ones in another order, with the same streams: the same
    // clips with other audio, as a version of the film in another language, are titles of their own. Of each group the
    // one kept is the one whose play items join without a pause most often: clips put in another order cannot join so,
    // and on some discs the film has a higher number than its reordered copies. Then the lowest number. A copy with
    // other chapter marks is a copy all the same, and is said so.
    std::map<std::pair<std::vector<Item>, std::string>, std::vector<Playlist*>> bySet;
    for (Playlist* p : left)
    {
        std::vector<Item> key = p->items;
        std::sort(key.begin(), key.end());
        bySet[{key, p->streams}].push_back(p);
    }
    for (auto& [key, group] : bySet)
    {
        if (group.size() < 2)
            continue;
        Playlist* kept = group.front();
        for (Playlist* p : group)
            if (p->unbrokenJoins > kept->unbrokenJoins)
                kept = p;
        for (Playlist* p : group)
        {
            if (p == kept)
                continue;
            if (p->items != kept->items)
                p->title.note = "the same clips as " + kept->title.name + " in another order";
            else if (p->marks != kept->marks)
                p->title.note = "the same as " + kept->title.name + " with other chapter marks";
            else
                p->title.note = "the same as " + kept->title.name;
        }
    }
    // 3. The minimum length
    std::vector<Playlist*> titles;
    for (Playlist* p : left)
    {
        if (!p->title.note.empty())
            continue;
        if (p->title.seconds < minSeconds)
            result.shortOnes.push_back(p->title.name);
        else
            titles.push_back(p);
    }
    // A disc of short pieces only, as a trailer or a demonstration disc, has its titles all the same
    if (titles.empty() && !result.shortOnes.empty())
    {
        result.minApplied = false;
        result.shortOnes.clear();
        for (Playlist* p : left)
            if (p->title.note.empty())
                titles.push_back(p);
    }
    // 4. The main film plays the most footage, then has the most chapters, then the lowest number. A title that shares
    // most of its footage with the main film and is about as long is a version of it (seamless branching: another
    // cut, or the film with other title cards).
    std::stable_sort(titles.begin(), titles.end(),
                     [](const Playlist* a, const Playlist* b)
                     {
                         if (a->title.footage != b->title.footage)
                             return a->title.footage > b->title.footage;
                         return a->title.chapters > b->title.chapters;
                     });
    // A disc can hold more than one main title: the episodes of a series, or several films. Another title is one too
    // when it plays footage of its own (it is no version of a main title) for at least 75 % of the footage of the main
    // film and at least 15 minutes. Each is listed with its versions after it. The main film comes first and title=main
    // takes it, the other main titles follow in the order of the disc: a documentary or another cut as long as that
    // must not take the film's place. Where the main titles are all of one length (within 3 minutes), as the episodes
    // of a series disc, they are all in the order of the disc, and title=main takes the first: one episode was the
    // main film for being a second longer than the others.
    if (!titles.empty())
    {
        std::vector<Playlist*> mains{titles.front()};
        std::map<const Playlist*, std::vector<Playlist*>> versionsOf;
        std::vector<Playlist*> others;
        for (size_t i = 1; i < titles.size(); ++i)
        {
            Playlist* p = titles[i];
            const auto of = std::find_if(mains.begin(), mains.end(),
                                         [p](const Playlist* m) {
                                             return sharedFootage(*p, *m) >= 0.5 * p->title.footage &&
                                                    p->title.footage >= 0.75 * m->title.footage;
                                         });
            if (of != mains.end())
                versionsOf[*of].push_back(p);
            else if (p->title.footage >= 0.75 * mains.front()->title.footage && p->title.footage >= 900)
                mains.push_back(p);
            else
                others.push_back(p);
        }
        // mains is by footage, the longest first
        const bool episodes = mains.front()->title.footage - mains.back()->title.footage <= 180;
        std::stable_sort(mains.begin() + (episodes ? 0 : 1), mains.end(),
                         [](const Playlist* a, const Playlist* b) { return a->title.name < b->title.name; });
        // The others by what they play, the longest first, counted in whole seconds as listed, and those of one length
        // in the order of the disc: the episodes of a series disc came in an order the parts of a second gave
        std::stable_sort(others.begin(), others.end(),
                         [](const Playlist* a, const Playlist* b)
                         {
                             const auto sa = llround(a->title.seconds), sb = llround(b->title.seconds);
                             return sa != sb ? sa > sb : a->title.name < b->title.name;
                         });
        // Playlists of the same play items (other audio or subtitles) stay together, the one with the most chapters
        // first: the others are then named as the same clips as it
        {
            std::vector<std::vector<Playlist*>> groups;
            for (Playlist* p : others)
            {
                auto g = std::find_if(groups.begin(), groups.end(), [p](const std::vector<Playlist*>& group)
                                      { return group.front()->items == p->items; });
                if (g == groups.end())
                    groups.push_back({p});
                else
                    g->push_back(p);
            }
            others.clear();
            for (auto& group : groups)
            {
                std::stable_sort(group.begin(), group.end(), [](const Playlist* a, const Playlist* b)
                                 { return a->title.chapters > b->title.chapters; });
                others.insert(others.end(), group.begin(), group.end());
            }
        }
        titles.clear();
        for (Playlist* m : mains)
        {
            m->title.note = "main film";
            m->title.isMain = true;
            titles.push_back(m);
            const std::string number = std::to_string(titles.size());
            for (Playlist* p : versionsOf[m])
            {
                std::set<std::string> a, b, differ;
                for (const auto& it : p->items) a.insert(it.clip);
                for (const auto& it : m->items) b.insert(it.clip);
                std::set_symmetric_difference(a.begin(), a.end(), b.begin(), b.end(),
                                              std::inserter(differ, differ.begin()));
                // The same play items with other streams (a copy with the same streams is left out above), the same
                // clips played in other parts, or another cut
                if (p->items == m->items)
                    p->title.note = "version of title " + number + ": the same clips, other audio or subtitles";
                else if (differ.empty())
                    p->title.note = "version of title " + number + ": the same clips, other parts of them";
                else
                    p->title.note = "version of title " + number + ", " + std::to_string(differ.size()) +
                                    (differ.size() == 1 ? " clip differs" : " clips differ");
                titles.push_back(p);
            }
        }
        const size_t firstOther = titles.size();
        titles.insert(titles.end(), others.begin(), others.end());
        result.mainTitles = static_cast<int>(mains.size());
        // A title with the play items of one before it differs only in its streams
        for (size_t i = firstOther; i < titles.size(); ++i)
            for (size_t k = 0; k < i; ++k)
                if (titles[k]->items == titles[i]->items)
                {
                    titles[i]->title.note =
                        "the same clips as title " + std::to_string(k + 1) + ", other audio or subtitles";
                    break;
                }
    }
    // A title whose clips are not all there cannot be muxed whole: said in its note
    const std::string streamDir = root + getDirSeparator() + "BDMV" + getDirSeparator() + "STREAM" + getDirSeparator();
    for (Playlist* p : titles)
    {
        std::set<std::string> missing;
        for (const Item& it : p->items)
            if (!missing.count(it.clip) && !fileExists(streamDir + it.clip + ".m2ts") &&
                !fileExists(streamDir + strToLowerCase(it.clip) + ".m2ts") &&
                !fileExists(streamDir + it.clip + ".M2TS"))
                missing.insert(it.clip);
        if (!missing.empty())
        {
            std::string list;
            for (const auto& c : missing) list += (list.empty() ? "" : " ") + c + ".m2ts";
            p->title.note += (p->title.note.empty() ? "" : "; ") + std::string("missing ") + list;
        }
        result.titles.push_back(p->title);
    }
    // Every playlist with a reason that is no title is left out
    const std::set<const Playlist*> kept(titles.begin(), titles.end());
    for (const auto& p : lists)
        if (!kept.count(&p) && !p.title.note.empty())
            result.leftOut.push_back(p.title);
    if (all)
    {
        // --all-playlists: every playlist as a title, the left out and the short ones after the others
        for (const auto& t : result.leftOut) result.titles.push_back(t);
        for (const auto& p : lists)
            if (std::find(result.shortOnes.begin(), result.shortOnes.end(), p.title.name) != result.shortOnes.end())
            {
                DiscTitle t = p.title;
                t.note = "under " + minutesText(minSeconds);
                result.titles.push_back(t);
            }
        result.leftOut.clear();
        result.shortOnes.clear();
    }
    for (size_t i = 0; i < result.titles.size(); ++i) result.titles[i].number = static_cast<int>(i + 1);
    return result;
}

// "The disc has 4 main titles: 1, 2, 3, 4. title=main takes title 1."
static std::string mainTitlesText(const DiscTitles& t)
{
    std::string numbers;
    for (const auto& title : t.titles)
        if (title.isMain)
            numbers += (numbers.empty() ? "" : ", ") + std::to_string(title.number);
    return "The disc has " + std::to_string(t.mainTitles) + " main titles: " + numbers + ". title=main takes title " +
           numbers.substr(0, numbers.find(',')) + ".";
}

std::string describeDiscTitles(const DiscTitles& t)
{
    std::ostringstream s;
    if (t.titles.empty())
        s << "No playlist of this disc is a title.\n";
    if (!t.minApplied)
        s << "No playlist of this disc is " << minutesText(t.minSeconds) << " long, so the shorter ones are listed.\n";
    const size_t width = t.titles.size() >= 10 ? 2 : 1;
    for (const auto& title : t.titles)
    {
        std::string langs;
        for (size_t i = 0; i < title.audio.size() && i < 8; ++i) langs += (i ? " " : "") + title.audio[i];
        if (title.audio.size() > 8)
            langs += " ...";
        std::string number = std::to_string(title.number);
        number.insert(0, width - (std::min)(width, number.size()), ' ');
        char line[160];
        snprintf(line, sizeof(line), "Title %s  %-11s %9s  %3zu %-8s  %4zu %-5s", number.c_str(), title.name.c_str(),
                 hms(title.seconds).c_str(), title.chapters, title.chapters == 1 ? "chapter" : "chapters", title.clips,
                 title.clips == 1 ? "clip" : "clips");
        s << line;
        if (!langs.empty())
            s << "  " << langs;
        if (title.is3d)
            s << "  3D";
        if (title.angles > 1)
            s << "  " << title.angles << " angles";
        if (!title.note.empty())
            s << "  (" << title.note << ")";
        s << '\n';
    }
    if (t.mainTitles > 1)
        s << '\n' << mainTitlesText(t) << '\n';
    if (!t.leftOut.empty() || !t.shortOnes.empty())
    {
        s << "\nLeft out:\n";
        for (const auto& title : t.leftOut) s << "  " << title.name << ": " << title.note << '\n';
        if (!t.shortOnes.empty())
        {
            s << "  " << t.shortOnes.size() << (t.shortOnes.size() == 1 ? " playlist" : " playlists") << " under "
              << minutesText(t.minSeconds) << ": " << namesText(t.shortOnes) << '\n';
        }
        s << "--all-playlists lists them as titles, and --min-title-length=<seconds> sets the length a title needs.\n";
    }
    if (!t.titles.empty())
        s << "\nGive the playlist of a title as the source to list its tracks, or name the disc with title=<number> "
             "or title=main in a meta file.\n";
    return s.str();
}

std::string discTitlePlaylist(const std::string& discPath, const std::string& title, const double minSeconds,
                              const bool all)
{
    const std::string root = blurayDiscRoot(discPath);
    if (root.empty())
        throw std::runtime_error(discPath + " is no Blu-ray disc folder or image, so title=" + title +
                                 " names nothing in it");
    // Each line of a meta file that names the disc asks again: the titles are worked out once
    static std::map<std::string, DiscTitles> known;
    const std::string key = root + '|' + std::to_string(minSeconds) + (all ? "|all" : "");
    auto it = known.find(key);
    if (it == known.end())
        it = known.emplace(key, listDiscTitles(root, minSeconds, all)).first;
    const DiscTitles& t = it->second;
    if (t.titles.empty())
        throw std::runtime_error(discPath +
                                 " has no title: every playlist is left out. List the disc to see why, and "
                                 "give a playlist instead");
    if (strToLowerCase(title) == "main")
    {
        if (t.mainTitles > 1)
            LTRACE(LT_INFO, 2, discPath << ": " << mainTitlesText(t));
        return t.titles.front().playlist;
    }
    const std::string digits = title.find_first_not_of("0123456789") == std::string::npos ? title : "";
    const int n = digits.empty() || digits.size() > 4 ? 0 : std::stoi(digits);
    if (n < 1 || n > static_cast<int>(t.titles.size()))
        throw std::runtime_error("title=" + title + " is not a title of " + discPath + ", which has " +
                                 std::to_string(t.titles.size()) + (t.titles.size() == 1 ? " title" : " titles") +
                                 ": give a number from 1 to " + std::to_string(t.titles.size()) + ", or main");
    return t.titles[n - 1].playlist;
}
