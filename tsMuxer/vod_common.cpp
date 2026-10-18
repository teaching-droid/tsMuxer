#include "vod_common.h"

#include <atomic>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <fs/directory.h>

using namespace std;

bool sLastMsg = false;

std::string toNativeSeparators(const std::string& dirName)
{
    std::string rez = dirName;
    for (char& c : rez)
    {
        if (c == '\\' || c == '/')
            c = getDirSeparator();
    }
    return rez;
}

bool isFillerNullPacket(uint8_t* curBuf)
{
    const auto endPos = UnalignedPtr<uint32_t>(curBuf + TS_FRAME_SIZE);
    for (auto curBuf32 = UnalignedPtr<uint32_t>(curBuf + 4); curBuf32 < endPos; curBuf32++)
        if (*curBuf32 != UINT_MAX && *curBuf32 != 0)
            return false;
    return true;
}

std::string unquoteStr(const std::string& val)
{
    std::string tmp = val;
    if (!val.empty() && val[0] == '\"')
        tmp = tmp.substr(1, tmp.size());
    if (!val.empty() && val[val.size() - 1] == '\"')
        tmp = tmp.substr(0, tmp.size() - 1);
    return tmp;
}

std::string quoteStr(const std::string& val) { return "\"" + val + "\""; }

std::vector<std::string> extractFileList(const std::string& val)
{
    std::vector<std::string> rez;
    bool quoted = false;
    size_t lastStartPos = 0;
    for (size_t i = 0; i < val.size(); i++)
    {
        if (val[i] == '"')
            quoted = !quoted;
        else if (val[i] == '+' && !quoted)
        {
            if (i > lastStartPos)
            {
                std::string tmp = val.substr(lastStartPos, i - lastStartPos);
                tmp = trimStr(tmp);
                tmp = unquoteStr(tmp);
                if (!tmp.empty())
                    rez.push_back(tmp);
            }
            lastStartPos = i + 1;
        }
    }
    if (rez.empty())
        rez.push_back(unquoteStr(trimStr(val)));
    else
    {
        std::string tmp = val.substr(lastStartPos, val.size());
        tmp = trimStr(tmp);
        tmp = unquoteStr(tmp);
        if (!tmp.empty())
            rez.push_back(tmp);
    }
    return rez;
}

uint16_t AV_RB16(const uint8_t* buffer) { return static_cast<uint16_t>(*buffer << 8 | buffer[1]); }
uint32_t AV_RB32(uint8_t* buffer) { return my_ntohl(loadUnaligned<uint32_t>(buffer)); }
uint32_t AV_RB24(const uint8_t* buffer) { return buffer[0] << 16 | buffer[1] << 8 | buffer[2]; }

void AV_WB16(uint8_t* buffer, const uint16_t value) { storeUnaligned<uint16_t>(buffer, my_htons(value)); }

void AV_WB24(uint8_t* buffer, const uint32_t value)
{
    buffer[0] = static_cast<uint8_t>(value >> 16);
    buffer[1] = static_cast<uint8_t>(value >> 8);
    buffer[2] = static_cast<uint8_t>(value);
}
void AV_WB32(uint8_t* buffer, const uint32_t value) { storeUnaligned<uint32_t>(buffer, my_htonl(value)); }
void AV_WB64(uint8_t* buffer, const uint64_t value) { storeUnaligned<uint64_t>(buffer, my_htonll(value)); }

std::string floatToTime(const double time, const char msSeparator)
{
    // To the nearest millisecond. The fraction used to be cut off, and a time is rarely exact in a
    // double: 1.273 s is held as 1.27299999 and was written as 1.272, a millisecond early. Where a
    // subtitle is re-written as text that was enough to move it to the frame before its own, and a
    // chapter written out as XML lost the millisecond too. Counting whole milliseconds also carries
    // 59.9996 s over to the next minute instead of writing 59.999.
    int64_t msTotal = llround(time * 1000.0);
    const auto hour = static_cast<int32_t>(msTotal / 3600000);
    msTotal -= static_cast<int64_t>(hour) * 3600000;
    const auto min = static_cast<int32_t>(msTotal / 60000);
    msTotal -= static_cast<int64_t>(min) * 60000;
    const auto sec = static_cast<int32_t>(msTotal / 1000);
    const auto msec = static_cast<int32_t>(msTotal - static_cast<int64_t>(sec) * 1000);
    std::ostringstream str;
    str << strPadLeft(int32ToStr(hour), 2, '0') << ':';
    str << strPadLeft(int32ToStr(min), 2, '0') << ':';
    str << strPadLeft(int32ToStr(sec), 2, '0') << msSeparator << strPadLeft(int32ToStr(msec), 3, '0');
    return str.str();
}

double timeToFloat(const std::string& chapterStr)
{
    if (chapterStr.empty())
        return 0;
    const std::vector<std::string> timeParts = splitStr(chapterStr.c_str(), ':');
    double sec = 0;
    if (!timeParts.empty())
        sec = strToDouble(timeParts[timeParts.size() - 1].c_str());
    int min = 0;
    if (timeParts.size() > 1)
        min = strToInt32(timeParts[timeParts.size() - 2].c_str());
    int hour = 0;
    if (timeParts.size() > 2)
        hour = strToInt32(timeParts[timeParts.size() - 3].c_str());
    // Multiplied as doubles: in int, 600000 hours came out negative. Every time that fitted before gives
    // the same value.
    return hour * 3600.0 + min * 60.0 + sec;
}

double correctFps(const double fps)
{
    struct FPSCorrect
    {
        double from;
        double to;
    };

    FPSCorrect fpsCorrectList[] = {
        {5.994, 5.99400599400599},   {11.988, 11.98801198801198}, {23.976, 23.97602397602397},
        {47.952, 47.95204795204795}, {7.4925, 7.49250749250749},  {14.985, 14.98501498501498},
        {29.97, 29.97002997002997},  {59.94, 59.94005994005994},
    };

    for (const auto& i : fpsCorrectList)
    {
        if (fabs(fps - i.from) < 1e-4)
        {
            return i.to;
        }
    }
    return fps;
}

// ---------------------------------------------------------------------------------------
// Audio delay carried in a file name ("<name> DELAY -17ms.<ext>", the eac3to convention).
//
// Only "ms" is emitted, because that is what eac3to writes and what other tools expect, but
// the parser accepts the same suffixes the meta timeshift parameter does so that a value
// copied from a meta file behaves the same way in a name.
// ---------------------------------------------------------------------------------------

static const char DELAY_TOKEN[] = " DELAY ";

int64_t parseTimeshiftToMs(const std::string& value)
{
    // The same reading as the meta timeshift parameter: a number without a unit is in ms.
    int64_t ticks = 0;
    if (!parseTimeToTicks(value, "ms", &ticks))
        return 0;
    return ticks >= 0 ? (ticks + INTERNAL_PTS_PER_MS / 2) / INTERNAL_PTS_PER_MS
                      : -((-ticks + INTERNAL_PTS_PER_MS / 2) / INTERNAL_PTS_PER_MS);
}

// Each value used to be read by its own few lines, and they disagreed: "500us" was 500 s (the "s" was found), "2S"
// was 2 ms (no "s" in it), a fraction of a timeshift was cut off, a cut value without a unit was taken in the
// internal clock's ticks (5,292,000,000 a second), "00:01:30" as 0, and a timeshift over 1742.9 s ran over 64 bits.
bool parseTimeToTicks(const std::string& text, const char* defaultUnit, int64_t* ticks)
{
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    size_t e = text.size();
    while (e > i && (text[e - 1] == ' ' || text[e - 1] == '\t')) --e;
    std::string t = text.substr(i, e - i);
    bool negative = false;
    if (!t.empty() && (t[0] == '-' || t[0] == '+'))
    {
        negative = t[0] == '-';
        t.erase(0, 1);
    }
    // digits, then an optional fraction; at least one digit
    auto readNumber = [](const std::string& s, size_t& p, long double& value)
    {
        const size_t start = p;
        long double v = 0;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') v = v * 10 + (s[p++] - '0');
        bool digits = p > start;
        if (p < s.size() && s[p] == '.')
        {
            ++p;
            long double scale = 0.1L;
            const size_t fracStart = p;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9')
            {
                v += (s[p++] - '0') * scale;
                scale /= 10;
            }
            digits = digits || p > fracStart;
        }
        value = v;
        return digits;
    };
    long double seconds = 0;
    size_t p = 0;
    if (t.find(':') != std::string::npos)
    {
        // h:mm:ss or m:ss, each part after the first below 60, a fraction on the seconds only
        std::vector<long double> parts;
        while (true)
        {
            const size_t start = p;
            while (p < t.size() && t[p] >= '0' && t[p] <= '9') ++p;
            if (p == start)
                return false;
            if (p < t.size() && t[p] == ':')
            {
                parts.push_back(static_cast<long double>(strToInt64(t.substr(start, p - start).c_str())));
                ++p;
                continue;
            }
            p = start;
            long double last = 0;
            if (!readNumber(t, p, last) || p != t.size())
                return false;
            parts.push_back(last);
            break;
        }
        if (parts.size() > 3)
            return false;
        for (size_t k = 1; k < parts.size(); ++k)
            if (parts[k] >= 60)
                return false;
        for (const long double part : parts) seconds = seconds * 60 + part;
    }
    else
    {
        long double value = 0;
        if (!readNumber(t, p, value))
            return false;
        std::string unit = strToLowerCase(t.substr(p));
        if (unit.empty())
        {
            if (defaultUnit == nullptr)
                return false;
            unit = defaultUnit;
        }
        long double perUnit;
        if (unit == "ns")
            perUnit = 1e-9L;
        else if (unit == "us")
            perUnit = 1e-6L;
        else if (unit == "ms")
            perUnit = 1e-3L;
        else if (unit == "s")
            perUnit = 1;
        else if (unit == "min")
            perUnit = 60;
        else if (unit == "h")
            perUnit = 3600;
        else
            return false;
        seconds = value * perUnit;
    }
    if (seconds > 100000000)
        return false;
    const auto value = static_cast<int64_t>(llroundl(seconds * INTERNAL_PTS_FREQ));
    *ticks = negative ? -value : value;
    return true;
}

// Locate the last " DELAY <n>ms" that sits immediately before the extension. Returns the
// offset of the token, or npos. strict form only: optional sign, digits, then "ms".
static size_t findDelayToken(const std::string& name, int64_t* valueOut)
{
    size_t best = std::string::npos;
    size_t from = 0;
    while (true)
    {
        const size_t at = name.find(DELAY_TOKEN, from);
        if (at == std::string::npos)
            break;
        size_t p = at + sizeof(DELAY_TOKEN) - 1;
        const size_t numStart = p;
        if (p < name.size() && (name[p] == '-' || name[p] == '+'))
            p++;
        const size_t digitsStart = p;
        while (p < name.size() && name[p] >= '0' && name[p] <= '9') p++;
        if (p > digitsStart && p + 1 < name.size() + 1 && name.compare(p, 2, "ms") == 0)
        {
            // Accept only when "ms" ends the name, or is followed by an extension. This
            // keeps a directory called "DELAY 5ms" in the middle of a path from matching.
            const size_t after = p + 2;
            if (after == name.size() || name[after] == '.')
            {
                best = at;
                if (valueOut)
                    *valueOut = strToInt64(name.substr(numStart, p - numStart).c_str());
            }
        }
        from = at + 1;
    }
    return best;
}

// Every ISO 639-2 code, bibliographic and terminological, space delimited and with a space at each
// end so a search for " xxx " can only match a whole one. Generated from the list published by the
// Library of Congress, who are the registration authority for the standard.
static const char* const ISO_639_2_CODES =
    " aar abk ace ach ada ady afa afh afr ain aka akk alb ale alg alt amh ang anp apa ara arc arg arm "
    "arn arp art arw asm ast ath aus ava ave awa aym aze bad bai bak bal bam ban baq bas bat bej bel "
    "bem ben ber bho bih bik bin bis bla bnt bod bos bra bre btk bua bug bul bur byn cad cai car cat "
    "cau ceb cel ces cha chb che chg chi chk chm chn cho chp chr chu chv chy cmc cnr cop cor cos cpe "
    "cpf cpp cre crh crp csb cus cym cze dak dan dar day del den deu dgr din div doi dra dsb dua dum "
    "dut dyu dzo efi egy eka ell elx eng enm epo est eus ewe ewo fan fao fas fat fij fil fin fiu fon "
    "fra fre frm fro frr frs fry ful fur gaa gay gba gem geo ger gez gil gla gle glg glv gmh goh gon "
    "gor got grb grc gre grn gsw guj gwi hai hat hau haw heb her hil him hin hit hmn hmo hrv hsb hun "
    "hup hye iba ibo ice ido iii ijo iku ile ilo ina inc ind ine inh ipk ira iro isl ita jav jbo jpn "
    "jpr jrb kaa kab kac kal kam kan kar kas kat kau kaw kaz kbd kha khi khm kho kik kin kir kmb kok "
    "kom kon kor kos kpe krc krl kro kru kua kum kur kut lad lah lam lao lat lav lez lim lin lit lol "
    "loz ltz lua lub lug lui lun luo lus mac mad mag mah mai mak mal man mao map mar mas may mdf mdr "
    "men mga mic min mis mkd mkh mlg mlt mnc mni mno moh mon mos mri msa mul mun mus mwl mwr mya myn "
    "myv nah nai nap nau nav nbl nde ndo nds nep new nia nic niu nld nno nob nog non nor nqo nso nub "
    "nwc nya nym nyn nyo nzi oci oji ori orm osa oss ota oto paa pag pal pam pan pap pau peo per phi "
    "phn pli pol pon por pra pro pus que raj rap rar roa roh rom ron rum run rup rus sad sag sah sai "
    "sal sam san sas sat scn sco sel sem sga sgn shn sid sin sio sit sla slk slo slv sma sme smi smj "
    "smn smo sms sna snd snk sog som son sot spa sqi srd srn srp srr ssa ssw suk sun sus sux swa swe "
    "syc syr tah tai tam tat tel tem ter tet tgk tgl tha tib tig tir tiv tkl tlh tli tmh tog ton tpi "
    "tsi tsn tso tuk tum tup tur tut tvl twi tyv udm uga uig ukr umb und urd uzb vai ven vie vol vot "
    "wak wal war was wel wen wln wol xal xho yao yap yid yor ypk zap zbl zen zgh zha zho znd zul zun "
    "zxx zza ";

bool isIso639_2(const std::string& code)
{
    if (code.size() != 3)
        return false;
    for (const char c : code)
        if (c < 'a' || c > 'z')
            return false;
    return strstr(ISO_639_2_CODES, (" " + code + " ").c_str()) != nullptr;
}

// A language carried in a file name, which is where a raw elementary stream has to keep it. Our own
// demux writes it there, "<name>.track_4352_eng.ac3", and eac3to and others put a code at the end
// of the name too.
//
// ** ONLY THE LAST TOKEN, AND ONLY IF IT IS REALLY A LANGUAGE CODE. ** Guessing a language wrongly
// is worse than leaving a track untagged, so the token has to be three letters that appear in ISO
// 639-2, and it has to be the last one before the extension. "Space Odyssey.ac3" holds "spa" but
// not as a token of its own, and nothing here will find it.
//
// A BRACKETED code is the exception, and is looked for anywhere in the name. That is what
// eac3to writes now, and it keeps describing the track afterwards, so the code is nowhere
// near the end: "bigjoin - 2 - AC3, [dan], 2.0 channels, 192kbps, 48kHz.ac3".
// The last word of a name and the character in front of it (0 when the word is the whole name), after any trailing
// spaces, dots and closing brackets.
static std::string lastNameToken(std::string& name, char& separator)
{
    while (!name.empty() && (name.back() == ' ' || name.back() == '.' || name.back() == ')' || name.back() == ']'))
        name.pop_back();
    const size_t at = name.find_last_of("_-. ()[]");
    separator = at == std::string::npos ? 0 : name[at];
    std::string token = at == std::string::npos ? name : name.substr(at + 1);
    name = at == std::string::npos ? std::string() : name.substr(0, at);
    return token;
}

static std::string lowerCase(const std::string& s)
{
    std::string lower;
    for (const char c : s) lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return lower;
}

// ** AN UNBRACKETED CODE COMES AFTER "_" OR ".", AND IS NOT A WORD OF A TITLE. ** Ordinary English words are ISO
// 639-2 codes: "Pebble Man.ac3" was Mandingo, "Teapot War" Waray and "Clockwork Men" Mende. The tools that write a code
// into a name join it with an underscore or a dot (Movie_spa.ac3, movie.eng.srt). Joined that way, a word of a title
// looks the same as a code ("PEBBLE.MAN", "pebble_man", "clockwork.men", "teapot_war"), and a capital does not tell
// them apart either: "Movie.Eng" and "Movie_Ger" are codes. So the codes that are English words are read only in
// brackets ([man]), and the codes of languages that tracks are made in keep their place even where they are words too
// (cat, may, nor, per).
static bool isTitleWord(const std::string& lower)
{
    static const char* const WORDS =
        " ace art bad ban bat bin car day den din fan fat gay got hat her him hit kin lad "
        "mad man map mar men min mis nap new pal pap pro rap run sad sag sam sat son sun "
        "ton war was wen yap zen ";
    return lower.size() == 3 && strstr(WORDS, (" " + lower + " ").c_str()) != nullptr;
}

static bool unbracketedCode(const std::string& token, const char separator)
{
    if (separator != 0 && separator != '_' && separator != '.')
        return false;
    const std::string lower = lowerCase(token);
    return isIso639_2(lower) && !isTitleWord(lower);
}

// A code in brackets at the end of a name, as "Movie (forced) [eng]"
static bool bracketedCode(const std::string& token, const char separator)
{
    return (separator == '[' || separator == '(') && isIso639_2(lowerCase(token));
}

std::string langFromFileName(const std::string& fileName)
{
    // extractFileName already drops the extension. It returned nothing for a bare name with no
    // directory in front of it; it gives the name now, and the fallback below stays for safety.
    std::string name = extractFileName(fileName);
    if (name.empty())
    {
        name = fileName;
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);
    }
    // the delay token sits after the language when both are present, so it goes first
    name = stripDelayToken(name);
    // A BRACKETED CODE IS LOOKED FOR ANYWHERE, AND THE LAST ONE WINS. eac3to writes the code in
    // brackets and then keeps describing the track, so the code is not the last token at all:
    //   "bigjoin - 2 - AC3, [dan], 2.0 channels, 192kbps, 48kHz.ac3"
    // That was read off a demux run here rather than taken from the report. Brackets are the
    // whole reason this is safe to do away from the end of the name: nobody brackets three
    // letters by accident, whereas an unbracketed "spa" is a syllable of an ordinary word.
    {
        std::string found;
        for (size_t i = 0; i < name.size(); ++i)
        {
            if (name[i] != '[' && name[i] != '(')
                continue;
            const char close = name[i] == '[' ? ']' : ')';
            const size_t end = name.find(close, i + 1);
            if (end == std::string::npos)
                continue;
            std::string inner;
            for (size_t j = i + 1; j < end; ++j)
                inner += static_cast<char>(tolower(static_cast<unsigned char>(name[j])));
            if (isIso639_2(inner))
                found = inner;
            i = end;
        }
        if (!found.empty())
            return found;
    }
    // Unbracketed, the code still has to be the last token, where a wrong guess is far too easy; or the one before a
    // last "forced", as a forced subtitle file is named (movie_eng_forced.srt), which read as no language at all.
    char separator = 0;
    std::string token = lastNameToken(name, separator);
    if (lowerCase(token) == "forced" && !name.empty())
        token = lastNameToken(name, separator);
    return unbracketedCode(token, separator) ? lowerCase(token) : std::string();
}

bool isForcedSubtitleName(const std::string& fileName)
{
    std::string name = extractFileName(fileName);
    if (name.empty())
    {
        name = fileName;
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);
    }

    // The LAST word, exactly as the language is the last token. Anything looser marks a film
    // whose title merely begins with the word as the disc's forced subtitle track, which is
    // a worse mistake than not recognising a name at all. The tools that split these files
    // out put it at the end, bracketed or plain: "... fra (forced).sup", "movie_forced.sup".
    // Or the word before a language code at the end (movie_forced_eng.srt), which was read as the language only, and
    // before a code in brackets ("Movie (forced) [eng]"), which was not forced.
    char separator = 0;
    std::string token = lastNameToken(name, separator);
    if (lowerCase(token) != "forced" && !name.empty() &&
        (unbracketedCode(token, separator) || bracketedCode(token, separator)))
        token = lastNameToken(name, separator);
    return lowerCase(token) == "forced";
}

int defaultParamKind(const std::string& value)
{
    const std::string lower = lowerCase(trimStr(value));
    if (defaultParamMeansNo(lower))
        return 0;
    if (lower == "forced")
        return 2;
    if (lower == "all" || lower == "yes" || lower == "1" || lower == "true" || lower == "on")
        return 1;
    return -1;
}

bool defaultParamMeansNo(const std::string& value)
{
    std::string lower;
    for (const char c : value) lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return lower == "none" || lower == "no" || lower == "0" || lower == "false" || lower == "off";
}

std::string checkLayerBreakLbn(const int lbn, std::string& warning)
{
    warning.clear();
    if (lbn <= 0)
        return "a layer break must be a sector number above zero, and this one is " + std::to_string(lbn) +
               ". A comma separates VALUES here and is not a thousands separator, so write 12219392 and not "
               "12,219,392.";
    // 2 GiB in 2048-byte sectors. The smallest layer on any Blu-ray is a 25 GB one, about 12219392
    // sectors, and a defect-managed disc still reports millions. So anything under this is not a
    // layer boundary that exists, while the 12, 219 and 392 a grouped number turns into are five
    // orders of magnitude below it.
    const int smallestPlausibleLayer = 1048576;
    if (lbn < smallestPlausibleLayer)
        warning = "a layer break at sector " + std::to_string(lbn) +
                  " is far too small to be a layer boundary on any Blu-ray, so the guard band will land inside "
                  "your data rather than at the layer change. A comma separates VALUES in this option and is not "
                  "a thousands separator: write the sector number with no grouping, such as 12219392.";
    return std::string();
}

// The meta route read each sector with strtol, so 12.219.392 was sector 12 with only a warning and 4294967296 became
// 2147483647 on Windows in silence; --bdmv-to-iso read it with std::stoi, which stopped at the dots in the same way
// and refused the large one. One reading for both, which refuses what is not a sector number.
std::string parseLayerBreakList(const std::string& value, std::vector<int>& lbns, std::vector<std::string>& warnings)
{
    lbns.clear();
    warnings.clear();
    // Groups of three digits after the first part, which together make a real layer boundary: a number written with
    // thousands separators. It became three breaks of 12, 219 and 392 sectors with only a warning, while the same
    // number written with points was refused.
    {
        const std::vector<std::string> parts = splitStr(value.c_str(), ',');
        bool grouped = parts.size() > 1;
        std::string joined;
        for (size_t k = 0; k < parts.size() && grouped; ++k)
        {
            const std::string p = trimStr(parts[k]);
            grouped = !p.empty() && p.find_first_not_of("0123456789") == std::string::npos &&
                      (k == 0 ? p.size() <= 3 : p.size() == 3);
            joined += p;
        }
        if (grouped && joined.size() <= 10 && strToInt64(joined.c_str()) >= 1048576)
            return "\"" + trimStr(value) +
                   "\" is a number written with thousands separators, and a comma separates "
                   "two breaks here. Write " +
                   joined + ".";
    }
    for (const auto& raw : splitStr(value.c_str(), ','))
    {
        const std::string tok = trimStr(raw);
        if (tok.empty() || tok.find_first_not_of("0123456789") != std::string::npos)
            return "\"" + tok +
                   "\" is not a sector number. A layer break is written in digits alone, without dots, spaces or "
                   "other grouping, such as 12219392; a comma separates two breaks.";
        if (tok.size() > 10 || strToInt64(tok.c_str()) > INT_MAX)
            return "sector " + tok + " is past the end of any Blu-ray.";
        const int lbn = static_cast<int>(strToInt64(tok.c_str()));
        std::string warning;
        const std::string err = checkLayerBreakLbn(lbn, warning);
        if (!err.empty())
            return err;
        if (!warning.empty())
            warnings.push_back(warning);
        lbns.push_back(lbn);
    }
    return std::string();
}

std::string parseGuardMB(const std::string& option, const std::string& value, int& mb)
{
    const std::string t = trimStr(value);
    // Leading zeros are not counted as digits: 09999 is 9999 MB, and was refused for its five digits
    const size_t first = t.find_first_not_of('0');
    const size_t digits = first == std::string::npos ? 1 : t.size() - first;
    if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos || digits > 4)
        return "Invalid " + option + " value '" + value + "'. Expected 0..9999 (megabytes).";
    mb = strToInt32(t.c_str());
    return std::string();
}

// The number of layers was guessed from the size alone, so a size that is no disc's got breaks no disc has: 66 GB
// (the first two layers of a 100 GB disc) three layers, at 10861909 and 21723818 for 16292864, and 50g and 100g
// even splits of their round numbers, 12207031 for 12219392.
std::vector<int> defaultLayerBreaks(const int64_t discBytes, std::string& warning, std::string& error)
{
    warning.clear();
    error.clear();
    struct KnownDisc
    {
        int64_t sectors;
        int layers;
        const char* name;
    };
    static constexpr KnownDisc discs[] = {
        {24438784, 2, "BD-R DL 50 GB"},
        {23652352, 2, "BD-RE DL 50 GB"},
        {32585728, 2, "BD-R XL 100 GB, first 2 layers only (66 GB)"},
        {48878592, 3, "BD-R XL 100 GB"},
        {47305728, 3, "BD-R XL 100 GB with defect management"},
        {62500864, 4, "BD-R XL 128 GB"},
        {60403712, 4, "BD-R XL 128 GB with defect management"},
    };
    const int64_t bytes = discBytes > 0 ? discBytes : BD50_CAPACITY;
    if (bytes <= BD25_CAPACITY)
        return {};  // a single layer disc
    const int64_t sectors = bytes / 2048;
    const KnownDisc* disc = nullptr;
    int64_t off = 0;
    for (const KnownDisc& d : discs)
    {
        const int64_t diff = std::llabs(sectors - d.sectors);
        if (diff * 50 <= d.sectors && (disc == nullptr || diff < off))
        {
            disc = &d;
            off = diff;
        }
    }
    if (disc == nullptr)
    {
        error = "A disc of " + std::to_string(bytes) +
                " bytes is not one whose layer breaks are known (50, 66, 100 or 128 GB), so the layer break guard "
                "cannot be placed. Give the breaks with --layer-break-lbn, or the size of the disc.";
        return {};
    }
    if (off != 0)
        warning = "Warning: " + std::to_string(bytes) + " bytes is not the capacity of a known disc; the layer " +
                  (disc->layers > 2 ? "breaks" : "break") + " of a " + disc->name + " (" +
                  std::to_string(disc->sectors) + " sectors) " + (disc->layers > 2 ? "are" : "is") +
                  " used. Give --layer-break-lbn for another disc.";
    std::vector<int> breaks;
    for (int k = 1; k < disc->layers; ++k) breaks.push_back(static_cast<int>(disc->sectors * k / disc->layers));
    return breaks;
}

int64_t delayFromFileName(const std::string& fileName)
{
    std::string name = extractFileName(fileName);
    int64_t value = 0;
    if (findDelayToken(name, &value) == std::string::npos)
        return 0;
    return value;
}

std::string stripDelayToken(const std::string& fileName)
{
    const size_t at = findDelayToken(fileName, nullptr);
    if (at == std::string::npos)
        return fileName;
    return fileName.substr(0, at);
}

namespace
{
std::atomic<bool> saveSrtSucceeded{false};
std::filesystem::path saveSrtPath(const std::string& name) { return {std::u8string(name.begin(), name.end())}; }
}  // namespace

std::string saveSrtTempName(const std::string& name)
{
#ifdef _WIN32
    const auto pid = static_cast<unsigned long>(_getpid());
#else
    const auto pid = static_cast<unsigned long>(getpid());
#endif
    return name + ".tsmuxer-new-" + std::to_string(pid);
}

void setSaveSrtJobSucceeded() { saveSrtSucceeded = true; }

int finishSaveSrt(const std::string& tempName, const std::string& name, std::string& why)
{
    std::error_code ec;
    if (!saveSrtSucceeded)
    {
        std::filesystem::remove(saveSrtPath(tempName), ec);
        return 1;
    }
    std::filesystem::rename(saveSrtPath(tempName), saveSrtPath(name), ec);
    if (!ec)
        return 0;
    why = ec.message();
    return 2;
}
