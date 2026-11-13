// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "jsonListing.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <streambuf>

namespace jsonListing
{
bool active = false;

namespace
{
// A text as a JSON string. What a file gives as a name is not always UTF-8: a byte that is no part of a UTF-8
// character is written as U+FFFD, so the document stays one a reader takes.
void writeText(std::string& out, const std::string& s)
{
    out += '"';
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    for (size_t i = 0; i < n;)
    {
        const unsigned char c = p[i];
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += static_cast<char>(c);
            ++i;
        }
        else if (c < 0x20 || c == 0x7f)
        {
            if (c == '\n')
                out += "\\n";
            else if (c == '\t')
                out += "\\t";
            else if (c == '\r')
                out += "\\r";
            else
            {
                char code[8];
                snprintf(code, sizeof(code), "\\u%04x", c);
                out += code;
            }
            ++i;
        }
        else if (c < 0x80)
        {
            out += static_cast<char>(c);
            ++i;
        }
        else
        {
            // the bytes of one character: 2 for C2..DF, 3 for E0..EF, 4 for F0..F4, each followed by 80..BF; the
            // forms that say a character in more bytes than it needs, the halves of UTF-16 and what is past U+10FFFF
            // are no characters
            size_t len = 0;
            if (c >= 0xC2 && c <= 0xDF)
                len = 2;
            else if (c >= 0xE0 && c <= 0xEF)
                len = 3;
            else if (c >= 0xF0 && c <= 0xF4)
                len = 4;
            bool good = len != 0 && i + len <= n;
            for (size_t k = 1; good && k < len; ++k) good = (p[i + k] & 0xC0) == 0x80;
            if (good && len == 3)
                good = !(c == 0xE0 && p[i + 1] < 0xA0) && !(c == 0xED && p[i + 1] > 0x9F);
            if (good && len == 4)
                good = !(c == 0xF0 && p[i + 1] < 0x90) && !(c == 0xF4 && p[i + 1] > 0x8F);
            if (good)
            {
                out.append(s, i, len);
                i += len;
            }
            else
            {
                out += "\xEF\xBF\xBD";
                ++i;
            }
        }
    }
    out += '"';
}

// Seconds as a number: to the billionth, without the zeros at its end
std::string secondsText(const double value)
{
    if (!std::isfinite(value))
        return "0";
    char text[64];
    snprintf(text, sizeof(text), "%.9f", value);
    std::string s = text;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.')
        s.pop_back();
    return s == "-0" ? "0" : s;
}

// The lines written to the standard output and the error output while a listing is collected
struct Line
{
    bool fromErrorOutput;
    std::string text;
};
std::mutex g_lock;
std::vector<Line> g_lines;

class Capture final : public std::streambuf
{
   public:
    explicit Capture(const bool errorOutput) : m_errorOutput(errorOutput) {}

    // a line without its end, when the output stops there
    void close()
    {
        const std::lock_guard<std::mutex> guard(g_lock);
        if (!m_line.empty())
            g_lines.push_back({m_errorOutput, m_line});
        m_line.clear();
    }

   protected:
    int_type overflow(const int_type c) override
    {
        if (!traits_type::eq_int_type(c, traits_type::eof()))
        {
            const std::lock_guard<std::mutex> guard(g_lock);
            put(traits_type::to_char_type(c));
        }
        return traits_type::not_eof(c);
    }

    std::streamsize xsputn(const char* s, const std::streamsize n) override
    {
        const std::lock_guard<std::mutex> guard(g_lock);
        for (std::streamsize i = 0; i < n; ++i) put(s[i]);
        return n;
    }

   private:
    bool m_errorOutput;
    std::string m_line;

    void put(const char c)
    {
        if (c == '\n')
        {
            if (!m_line.empty())
                g_lines.push_back({m_errorOutput, m_line});
            m_line.clear();
        }
        else if (c != '\r')
            m_line += c;
    }
};

Capture g_outCapture(false);
Capture g_errCapture(true);
std::streambuf* g_realOut = nullptr;
std::streambuf* g_realErr = nullptr;
Value g_doc;
std::string g_failure;
bool g_failed = false;
bool g_written = false;

// Should the program end on a way that does not come by finish(), the two outputs get their own buffers back before
// the buffers above are gone
struct Restore
{
    ~Restore()
    {
        if (g_realOut)
            std::cout.rdbuf(g_realOut);
        if (g_realErr)
            std::cerr.rdbuf(g_realErr);
    }
} g_restore;
}  // namespace

Value& Value::member(const std::string& key)
{
    for (auto& [name, value] : m_members)
        if (name == key)
            return value;
    m_members.emplace_back(key, Value());
    return m_members.back().second;
}

bool Value::has(const std::string& key) const
{
    for (const auto& [name, value] : m_members)
        if (name == key)
            return true;
    return false;
}

Value& Value::text(const std::string& key, const std::string& value)
{
    Value& v = member(key);
    v = Value();
    v.m_kind = Kind::Text;
    v.m_text = value;
    return *this;
}

Value& Value::number(const std::string& key, const int64_t value)
{
    Value& v = member(key);
    v = Value();
    v.m_kind = Kind::Literal;
    v.m_text = std::to_string(value);
    return *this;
}

Value& Value::seconds(const std::string& key, const double value)
{
    Value& v = member(key);
    v = Value();
    v.m_kind = Kind::Literal;
    v.m_text = secondsText(value);
    return *this;
}

Value& Value::flag(const std::string& key, const bool value)
{
    Value& v = member(key);
    v = Value();
    v.m_kind = Kind::Literal;
    v.m_text = value ? "true" : "false";
    return *this;
}

Value& Value::list(const std::string& key)
{
    const bool there = has(key);
    Value& v = member(key);
    if (!there || v.m_kind != Kind::List)
    {
        v = Value();
        v.m_kind = Kind::List;
    }
    return v;
}

Value& Value::object(const std::string& key)
{
    const bool there = has(key);
    Value& v = member(key);
    if (!there || v.m_kind != Kind::Object)
        v = Value();
    return v;
}

Value& Value::addText(const std::string& value)
{
    m_items.emplace_back();
    m_items.back().m_kind = Kind::Text;
    m_items.back().m_text = value;
    return *this;
}

Value& Value::addNumber(const int64_t value)
{
    m_items.emplace_back();
    m_items.back().m_kind = Kind::Literal;
    m_items.back().m_text = std::to_string(value);
    return *this;
}

Value& Value::addSeconds(const double value)
{
    m_items.emplace_back();
    m_items.back().m_kind = Kind::Literal;
    m_items.back().m_text = secondsText(value);
    return *this;
}

Value& Value::addObject()
{
    m_items.emplace_back();
    return m_items.back();
}

Value& Value::addList()
{
    m_items.emplace_back();
    m_items.back().m_kind = Kind::List;
    return m_items.back();
}

void Value::write(std::string& out, const int depth) const
{
    const std::string indent(static_cast<size_t>(depth) * 2, ' ');
    switch (m_kind)
    {
    case Kind::Text:
        writeText(out, m_text);
        break;
    case Kind::Literal:
        out += m_text;
        break;
    case Kind::List:
    {
        if (m_items.empty())
        {
            out += "[]";
            break;
        }
        // a list of texts or numbers stands on one line, a list of objects or lists with each item on its own
        bool plain = true;
        for (const Value& item : m_items) plain = plain && (item.m_kind == Kind::Text || item.m_kind == Kind::Literal);
        out += '[';
        for (size_t i = 0; i < m_items.size(); ++i)
        {
            if (plain)
                out += i ? ", " : "";
            else
                out += (i ? ",\n" : "\n") + indent + "  ";
            m_items[i].write(out, depth + 1);
        }
        if (!plain)
            out += "\n" + indent;
        out += ']';
        break;
    }
    case Kind::Object:
    {
        if (m_members.empty())
        {
            out += "{}";
            break;
        }
        out += '{';
        for (size_t i = 0; i < m_members.size(); ++i)
        {
            out += (i ? ",\n" : "\n") + indent + "  ";
            writeText(out, m_members[i].first);
            out += ": ";
            m_members[i].second.write(out, depth + 1);
        }
        out += "\n" + indent + '}';
        break;
    }
    }
}

Value& doc() { return g_doc; }

void fail(const std::string& message)
{
    g_failed = true;
    g_failure = message;
}

bool takeOption(int& argc, char** argv, const char* version)
{
    bool given = false;
    int kept = 1;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--json") == 0)
            given = true;
        else
            argv[kept++] = argv[i];
    }
    if (!given)
        return true;
    argc = kept;
    active = true;
    std::cout.flush();
    std::cerr.flush();
    g_realOut = std::cout.rdbuf(&g_outCapture);
    g_realErr = std::cerr.rdbuf(&g_errCapture);
    g_doc.number("format", 1);
    g_doc.text("program", "tsMuxeR");
    g_doc.text("version", version);

    // What is left has to be a listing: a source, and before it only the options a listing takes. The source is the
    // last word, and it is none of the options that are a run of their own: --batch=<list> stood there and its jobs
    // were run, writing what they write.
    static const char* const listingOptions[] = {
        "--all-playlists", "--min-title-length=", "--show-title=", "--lang-codes=", "--playlist-clips="};
    static const char* const otherRuns[] = {"--batch",       "--make-sample", "--report",
                                            "--bdmv-to-iso", "--scan",        "--languages="};
    const auto beginsWith = [](const std::string& text, const char* const start) { return text.rfind(start, 0) == 0; };
    bool listing = argc >= 2;
    for (int i = 1; listing && i < argc - 1; ++i)
    {
        bool taken = false;
        for (const char* const option : listingOptions) taken = taken || beginsWith(argv[i], option);
        listing = taken;
    }
    if (listing)
    {
        const std::string source = argv[argc - 1];
        // the help is no listing; any other word that begins as an option is said to be unknown where a listing
        // without --json says it, unless a file has that name
        std::string lower = source;
        for (char& c : lower)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        listing = !(lower == "--help" || lower == "-h" || lower == "-?" || lower == "/?" ||
                    source.find_first_not_of(" \t") == std::string::npos);
        for (const char* const option : listingOptions) listing = listing && !beginsWith(source, option);
        for (const char* const option : otherRuns) listing = listing && !beginsWith(source, option);
        if (listing)
            g_doc.text("source", source);
    }
    if (!listing)
        fail(
            "--json gives the listing of a source as a JSON document: tsMuxeR --json <source>, where the source is a "
            "file, a playlist, or a disc folder or image. The options of a listing may stand before the source: "
            "--all-playlists, --min-title-length=, --show-title=, --lang-codes= and --playlist-clips=. It does not "
            "go with a mux or with any other option.");
    return listing;
}

void finish(const int exitCode)
{
    if (!active || g_written)
        return;
    g_written = true;
    std::cout.flush();
    std::cerr.flush();
    g_outCapture.close();
    g_errCapture.close();
    std::cout.rdbuf(g_realOut);
    std::cerr.rdbuf(g_realErr);

    std::vector<Line> lines;
    {
        const std::lock_guard<std::mutex> guard(g_lock);
        lines.swap(g_lines);
    }
    if (exitCode != 0)
    {
        // the reason: the one given, or the last line of the error output, which then is no message as well
        std::string message = g_failure;
        if (!g_failed)
            for (size_t i = lines.size(); i-- > 0;)
                if (lines[i].fromErrorOutput)
                {
                    message = lines[i].text;
                    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
        if (message.rfind("Error: ", 0) == 0)
            message.erase(0, 7);
        Value& error = g_doc.object("error");
        error.number("exitCode", exitCode);
        error.text("message", message);
    }
    if (!lines.empty())
    {
        Value& messages = g_doc.list("messages");
        for (const Line& line : lines)
            messages.addObject().text("level", line.fromErrorOutput ? "warning" : "info").text("text", line.text);
    }
    std::string text;
    g_doc.write(text, 0);
    text += '\n';
    std::cout << text;
    std::cout.flush();
}

}  // namespace jsonListing
