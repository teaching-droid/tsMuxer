#include "assFile.h"

#include <fs/discImage.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "memoryMatroska.h"
#include "vodCoreException.h"
#include "vod_common.h"

namespace
{
using namespace memoryMatroska;

struct Event
{
    int readOrder = 0;
    int64_t startMs = 0;
    int64_t endMs = 0;
    std::string fields;  // Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text, as a Matroska block holds them
};

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// H:MM:SS.cc; -1 when it is no time
int64_t parseTime(const std::string& s)
{
    int h = 0, m = 0, sec = 0;
    char c1 = 0, c2 = 0, dot = 0;
    std::istringstream in(trim(s));
    if (!(in >> h >> c1 >> m >> c2 >> sec >> dot) || c1 != ':' || c2 != ':' || (dot != '.' && dot != ','))
        return -1;
    std::string frac;
    in >> frac;
    int64_t ms = 0, scale = 100;
    for (const char c : frac)
    {
        if (c < '0' || c > '9')
            return -1;
        ms += (c - '0') * scale;
        scale /= 10;
    }
    return ((static_cast<int64_t>(h) * 60 + m) * 60 + sec) * 1000 + ms;
}

std::mutex preparedLock;
std::set<std::string>& prepared()
{
    static std::set<std::string> names;
    return names;
}
}  // namespace

bool isAssFileExt(const std::string& upperExt) { return upperExt == "ASS" || upperExt == "SSA"; }

bool prepareAssFile(const std::string& fileName)
{
    const std::string name = unquoteStr(trimStr(fileName));
    if (!isAssFileExt(strToUpperCase(extractFileExt(name))))
        return false;
    {
        std::lock_guard<std::mutex> guard(preparedLock);
        if (prepared().count(name))
            return true;
    }
    std::ifstream file(sourcePath(name), std::ios::binary);
    if (!file)
        return false;  // said by whatever opens it, as for any file that cannot be opened
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.size() >= 2 && ((static_cast<uint8_t>(text[0]) == 0xFF && static_cast<uint8_t>(text[1]) == 0xFE) ||
                             (static_cast<uint8_t>(text[0]) == 0xFE && static_cast<uint8_t>(text[1]) == 0xFF)))
        THROW(ERR_COMMON, "The subtitle script " << name
                                                 << " is saved in UTF-16. tsMuxeR reads it in UTF-8: save it as "
                                                    "UTF-8.")
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        text.erase(0, 3);

    // The header is every line but those of the events, with the [Events] section's Format line written in the order
    // a Matroska block holds the fields; the events by the Format line of the file, which can order them otherwise.
    const bool ssa = strToUpperCase(extractFileExt(name)) == "SSA";
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";  // the script's own line end
    std::string header;
    std::vector<std::string> format;
    std::vector<Event> events;
    bool inEvents = false, scriptInfo = false;
    int readOrder = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string t = trim(line);
        if (!t.empty() && t.front() == '[')
        {
            const std::string section = lower(t);
            inEvents = section == "[events]";
            scriptInfo = scriptInfo || section == "[script info]";
            header += t + eol;
            if (inEvents)
                header += std::string(ssa ? "Format: Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, "
                                            "Effect, Text"
                                          : "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
                                            "Effect, Text") +
                          eol;
            continue;
        }
        if (!inEvents)
        {
            header += line + eol;
            continue;
        }
        const size_t colon = t.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string key = lower(trim(t.substr(0, colon)));
        const std::string rest = trim(t.substr(colon + 1));
        if (key == "format")
        {
            format.clear();
            std::istringstream f(rest);
            std::string field;
            while (std::getline(f, field, ',')) format.push_back(lower(trim(field)));
            continue;
        }
        if (key != "dialogue")
        {
            // Comment, Picture, Sound, Movie, Command: nothing to show, kept in the header as they are
            header += line + eol;
            continue;
        }
        if (format.empty() || format.back() != "text")
            THROW(ERR_COMMON, "The subtitle script " << name
                                                     << " has a Dialogue line before a Format line that ends with "
                                                        "Text, so its fields cannot be read.")
        // the fields of the Format line, the last one, Text, with every comma it holds
        std::map<std::string, std::string> value;
        size_t from = 0;
        for (size_t i = 0; i < format.size(); ++i)
        {
            if (i + 1 == format.size())
            {
                value[format[i]] = rest.substr((std::min)(from, rest.size()));
                break;
            }
            const size_t comma = rest.find(',', from);
            if (comma == std::string::npos)
            {
                value[format[i]] = trim(rest.substr((std::min)(from, rest.size())));
                break;
            }
            value[format[i]] = trim(rest.substr(from, comma - from));
            from = comma + 1;
        }
        Event e;
        e.startMs = parseTime(value["start"]);
        e.endMs = parseTime(value["end"]);
        if (e.startMs < 0 || e.endMs < 0)
            THROW(ERR_COMMON, "The subtitle script " << name << " has a Dialogue line with a time that is no time: \""
                                                     << line << "\"")
        if (e.endMs <= e.startMs)
            continue;  // shown for no time
        std::string layer = ssa ? value["marked"] : value["layer"];
        if (strStartWith(lower(layer), "marked="))
            layer = layer.substr(7);
        if (layer.empty())
            layer = "0";
        const std::string style = value.count("style") && !value["style"].empty() ? value["style"] : "Default";
        const auto field = [&](const char* k) { return value.count(k) ? value[k] : std::string(); };
        const auto margin = [&](const char* k) { return value.count(k) && !value[k].empty() ? value[k] : "0"; };
        e.readOrder = readOrder++;
        e.fields = layer + "," + style + "," + field("name") + "," + margin("marginl") + "," + margin("marginr") + "," +
                   margin("marginv") + "," + field("effect") + "," + field("text");
        events.push_back(std::move(e));
    }
    if (!scriptInfo || format.empty())
        THROW(ERR_COMMON, "The file " << name
                                      << " is no ASS or SSA subtitle script: it has no [Script Info] section or no "
                                         "[Events] section with a Format line.")
    if (events.empty())
        THROW(ERR_COMMON, "The subtitle script " << name << " has no Dialogue line to show.")
    std::stable_sort(events.begin(), events.end(),
                     [](const Event& a, const Event& b) { return a.startMs < b.startMs; });

    // the Matroska file
    std::vector<uint8_t> mkv;
    ebmlHeader(mkv);
    std::vector<uint8_t> segment;
    {
        std::vector<uint8_t> info;
        uintElement(info, 0x2AD7B1, 1000000);  // TimestampScale: milliseconds
        // no application wrote it: the listing names none
        stringElement(info, 0x4D80, "");
        stringElement(info, 0x5741, "");
        int64_t lastEnd = 0;
        for (const Event& e : events) lastEnd = (std::max)(lastEnd, e.endMs);
        floatElement(info, 0x4489, static_cast<double>(lastEnd));
        element(segment, 0x1549A966, info);
    }
    {
        std::vector<uint8_t> entry;
        uintElement(entry, 0xD7, 1);
        uintElement(entry, 0x73C5, 1);
        uintElement(entry, 0x83, 0x11);
        uintElement(entry, 0x9C, 0);
        stringElement(entry, 0x86, ssa ? "S_TEXT/SSA" : "S_TEXT/ASS");
        stringElement(entry, 0x63A2, header);
        stringElement(entry, 0x22B59C, "und");
        std::vector<uint8_t> tracks;
        element(tracks, 0xAE, entry);
        element(segment, 0x1654AE6B, tracks);
    }
    // a cluster each 30 seconds of start times, so that a block's time is within the 16 bits it is given
    for (size_t i = 0; i < events.size();)
    {
        const int64_t clusterMs = events[i].startMs;
        std::vector<uint8_t> cluster;
        uintElement(cluster, 0xE7, static_cast<uint64_t>(clusterMs));
        for (; i < events.size() && events[i].startMs - clusterMs <= 30000; ++i)
        {
            const Event& e = events[i];
            const std::string payload = std::to_string(e.readOrder) + "," + e.fields;
            std::vector<uint8_t> block{0x81};
            const auto rel = static_cast<int16_t>(e.startMs - clusterMs);
            block.push_back(static_cast<uint8_t>(static_cast<uint16_t>(rel) >> 8));
            block.push_back(static_cast<uint8_t>(rel & 0xFF));
            block.push_back(0x00);
            block.insert(block.end(), payload.begin(), payload.end());
            std::vector<uint8_t> group;
            element(group, 0xA1, block);
            uintElement(group, 0x9B, static_cast<uint64_t>(e.endMs - e.startMs));
            element(cluster, 0xA0, group);
        }
        element(segment, 0x1F43B675, cluster);
    }
    element(mkv, 0x18538067, segment);
    discImage::addMemoryFile(name, std::move(mkv));
    std::lock_guard<std::mutex> guard(preparedLock);
    prepared().insert(name);
    return true;
}
