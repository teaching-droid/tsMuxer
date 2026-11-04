// Largely rewritten by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
// _WIN32 and not Q_OS_WIN: the Qt macro comes from a Qt header, and none has been included yet
// at this point in the file.
#ifdef _WIN32
#include <windows.h>

#include <mmsystem.h>
#endif

#include "tsmuxerwindow.h"

#include "ac3CoreDialog.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCursor>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QScopeGuard>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSharedPointer>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QTime>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <unordered_set>

#include "checkboxedheaderview.h"
#include "codecinfo.h"
#include "discTitleDialog.h"
#include "fontsettingstablemodel.h"
#include "lang_codes.h"
#include "muxForm.h"
#include "sampleDialog.h"

namespace QtCompat
{
inline QString strLeft(const QString& str, qsizetype n) { return str.first(n); }

inline QString strRight(const QString& str, qsizetype n) { return str.last(n); }

inline QString strMid(const QString& str, qsizetype pos, qsizetype len = -1)
{
    if (len < 0)
        return str.sliced(pos);
    return str.sliced(pos, len);
}
}  // namespace QtCompat

#include "ui_tsmuxerwindow.h"

namespace
{
QString getExistingDialogDir(const QString& path)
{
    const QString normalizedPath = QDir::fromNativeSeparators(path.trimmed());
    if (normalizedPath.isEmpty())
        return QDir::homePath();

    QFileInfo fileInfo(normalizedPath);
    if (fileInfo.exists())
    {
        if (fileInfo.isDir())
            return fileInfo.absoluteFilePath();
        return fileInfo.absolutePath();
    }

    QDir dir(normalizedPath);
    if (dir.exists())
        return dir.absolutePath();

    // The path is usually a file that does not exist yet: the output about to be written. That
    // used to send the dialog to the home folder every time, however long the output name had been
    // pointing somewhere else, and on Windows the dialog then also showed the home folder's view
    // settings rather than those of the folder the user works in. The nearest folder that does
    // exist is where the user meant to be. A relative name such as the default output name has no
    // folder of its own, so it still starts in the home folder.
    if (fileInfo.isRelative())
        return QDir::homePath();
    QDir parent(fileInfo.absolutePath());
    while (!parent.exists())
    {
        if (!parent.cdUp())
            return QDir::homePath();
    }
    return parent.absolutePath();
}

// ISO 639-2 writes twenty languages two ways, a bibliographic code and a terminological one: German
// is "ger" and "deu". The language list of this window holds the terminological codes only, while
// a track can carry either, depending on its source and on the language code form chosen on the
// General tab. This is the same table the muxer uses (metaDemuxer.cpp); the two must change
// together. The four pairs marked false are withdrawn: their bibliographic codes are still
// understood when they arrive, but never produced.
struct LangCodePair
{
    const char* bibliographic;
    const char* terminological;
    bool current;
};

constexpr LangCodePair LANG_CODE_PAIRS[] = {
    {"alb", "sqi", true},  {"arm", "hye", true},  {"baq", "eus", true},  {"bur", "mya", true}, {"cze", "ces", true},
    {"chi", "zho", true},  {"dut", "nld", true},  {"ger", "deu", true},  {"gre", "ell", true}, {"fre", "fra", true},
    {"geo", "kat", true},  {"ice", "isl", true},  {"jaw", "jav", false}, {"mac", "mkd", true}, {"mao", "mri", true},
    {"may", "msa", true},  {"mol", "ron", false}, {"per", "fas", true},  {"rum", "ron", true}, {"scc", "srp", false},
    {"scr", "hrv", false}, {"slo", "slk", true},  {"tib", "bod", true},  {"wel", "cym", true},
};

QString toTerminologicalLang(const QString& code)
{
    for (const auto& pair : LANG_CODE_PAIRS)
        if (code == QLatin1String(pair.bibliographic))
            return QLatin1String(pair.terminological);
    return code;
}

QString toBibliographicLang(const QString& code)
{
    for (const auto& pair : LANG_CODE_PAIRS)
        if (pair.current && code == QLatin1String(pair.terminological))
            return QLatin1String(pair.bibliographic);
    return code;
}

using FileFilterVec = std::vector<std::pair<QString, std::vector<const char*>>>;

QString makeFileFilter(const FileFilterVec& filters)
{
    QString rv;
    QTextStream s(&rv);
    for (auto& f : filters)
    {
        s << f.first << " (";
        for (auto& e : f.second)
        {
            s << "*." << e;
            if (&e != &f.second.back())
            {
                s << ' ';
            }
        }
        s << ");;";
    }
    rv.append(QString("%1 (*)").arg(TsMuxerWindow::tr("All files")));
    return rv;
}

QString fileDialogFilter()
{
    // The elementary files the command line reads, VC-1, VVC, MP2, MP3 and FLAC among them, as the window lists them:
    // they were missing, so the open dialog did not show them.
    // Consolidated filters to keep the dialog manageable on all platforms
#if defined(Q_OS_MACOS)
    // Native macOS dialogs are stricter about wildcard/type conversion.
    // Keep filters simple and extension-only to avoid silent failures.
    FileFilterVec filters = {
        {TsMuxerWindow::tr("All supported media files"),
         {"ac3", "ddp", "ec3", "eac3", "aac",  "avc",  "mvc",   "264",   "h264", "hevc",   "265",  "h265", "obu",
          "vc1", "vvc", "266", "h266", "ivf",  "dts",  "dtshd", "dtsma", "thd",  "truehd", "mpv",  "m1v",  "m2v",
          "mpa", "mp2", "mp3", "flac", "ts",   "m2ts", "mts",   "m2t",   "ssif", "mpg",    "mpeg", "vob",  "evo",
          "mkv", "mka", "mks", "mk3d", "webm", "mp4",  "m4a",   "m4v",   "mov",  "mpls",   "mpl",  "iso",  "sup",
          "srt", "sub", "ass", "ssa", "wav",  "w64",  "pcm",  "ogg",   "oga",   "opus", "wv"}},
        {TsMuxerWindow::tr("Container files (TS, MKV, MP4, MOV, etc.)"),
         {"ts", "m2ts", "mts", "m2t", "ssif", "mpg", "mpeg", "vob", "evo", "mkv", "mka", "mks", "mk3d", "webm", "mp4",
          "m4a", "m4v", "mov"}},
        {TsMuxerWindow::tr("Video elementary streams"),
         {"avc", "mvc", "264", "h264", "hevc", "265", "h265", "obu", "vc1", "vvc", "266", "h266", "ivf", "mpv", "m1v",
          "m2v"}},
        {TsMuxerWindow::tr("Audio files"),
         {"ac3", "ddp", "ec3",  "eac3", "aac", "dts", "dtshd", "dtsma", "thd",  "truehd", "mpa",
          "mp2", "mp3", "flac", "wav",  "w64", "pcm", "ogg",   "oga",   "opus", "wv"}},
        {TsMuxerWindow::tr("Subtitle files"), {"sup", "srt", "sub", "ass", "ssa"}},
        {TsMuxerWindow::tr("Blu-ray playlist"), {"mpls", "mpl"}},
        {TsMuxerWindow::tr("Disc image"), {"iso"}}};
#else
    FileFilterVec filters = {
        {TsMuxerWindow::tr("All supported media files"),
         {"ac3", "ddp", "ec3", "eac3", "aac", "avc",  "mvc",   "264",   "h264", "hevc",   "265",     "h265",    "obu",
          "vc1", "vvc", "266", "h266", "ivf", "dts",  "dtshd", "dtsma", "thd",  "truehd", "ac3+thd", "thd+ac3", "mpv",
          "m1v", "m2v", "mpa", "mp2",  "mp3", "flac", "ts",    "m2ts",  "mts",  "m2t",    "ssif",    "mpg",     "mpeg",
          "vob", "evo", "mkv", "mka",  "mks", "mk3d", "webm",  "mp4",   "m4a",  "m4v",    "mov",     "mpls",    "mpl",
          "iso", "sup", "srt", "sub", "ass",  "ssa", "wav",  "w64",   "pcm",   "ogg",  "oga",    "opus",    "wv"}},
        {TsMuxerWindow::tr("Container files (TS, MKV, MP4, MOV, etc.)"),
         {"ts", "m2ts", "mts", "m2t", "ssif", "mpg", "mpeg", "vob", "evo", "mkv", "mka", "mks", "mk3d", "webm", "mp4",
          "m4a", "m4v", "mov"}},
        {TsMuxerWindow::tr("Video elementary streams"),
         {"avc", "mvc", "264", "h264", "hevc", "265", "h265", "obu", "vc1", "vvc", "266", "h266", "ivf", "mpv", "m1v",
          "m2v"}},
        {TsMuxerWindow::tr("Audio files"),
         {"ac3", "ddp", "ec3", "eac3", "aac", "dts", "dtshd", "dtsma", "thd", "truehd", "ac3+thd", "thd+ac3",
          "mpa", "mp2", "mp3", "flac", "wav", "w64", "pcm",   "ogg",   "oga", "opus",   "wv"}},
        {TsMuxerWindow::tr("Subtitle files"), {"sup", "srt", "sub", "ass", "ssa"}},
        {TsMuxerWindow::tr("Blu-ray playlist"), {"mpls", "mpl"}},
        {TsMuxerWindow::tr("Disc image"), {"iso"}}};
#endif
    return makeFileFilter(filters);
}

QString TI_DEFAULT_TAB_NAME() { return TsMuxerWindow::tr("General track options"); }

QString TI_MKV_TAB_NAME() { return TsMuxerWindow::tr("Matroska options"); }

// The meta keys of the six Matroska track flags, in the order of QtvCodecInfo::mkvFlags.
const char* const MKV_FLAG_KEYS[6] = {"forced",   "hearing-impaired", "visual-impaired", "text-descriptions",
                                      "original", "commentary"};

QString TS_SAVE_DIALOG_FILTER() { return makeFileFilter({{TsMuxerWindow::tr("Transport Stream"), {"ts"}}}); }

QString M2TS_SAVE_DIALOG_FILTER()
{
    // .m2ts only: a .mts or .ssif chosen here became name.mts.m2ts, and a typed one was refused at the start
    return makeFileFilter({{TsMuxerWindow::tr("BDAV Transport Stream"), {"m2ts"}}});
}

QString ISO_SAVE_DIALOG_FILTER() { return makeFileFilter({{TsMuxerWindow::tr("Disk image"), {"iso"}}}); }

// A .webm name writes WebM, the Matroska subset of the web; the dialog offered no such type (#44)
QString MKV_SAVE_DIALOG_FILTER()
{
    return makeFileFilter(
        {{TsMuxerWindow::tr("Matroska"), {"mkv", "mka", "mks", "mk3d"}}, {TsMuxerWindow::tr("WebM"), {"webm"}}});
}

// A name for a Matroska output that tsMuxeR writes as it is: .mkv, .mka, .mks, .mk3d, and .webm, which it writes as
// WebM. A click on "MKV muxing", even when it was chosen already, turned .webm and .mka into .mkv, and "Start muxing"
// refused .webm.
bool isMatroskaOutputName(const QString& name)
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    return suffix == QLatin1String("mkv") || suffix == QLatin1String("mka") || suffix == QLatin1String("mks") ||
           suffix == QLatin1String("mk3d") || suffix == QLatin1String("webm");
}

// Whether two names are one file. QFileInfo compares them as the system does, without regard to case on Windows, so
// "00000.M2TS" and "00000.m2ts", or a name with each kind of slash, are the same file.
// One file, also when the two names are hard links to it: compared by name, an output that is a hard link to a source
// got the question whether to overwrite it, and the muxer then refused it as a source
bool isSameFile(const QString& a, const QString& b)
{
    const QFileInfo fa(a);
    const QFileInfo fb(b);
    // A name that is not on the disk, as a playlist inside a disc image or an output not written yet, has no canonical
    // path, and QFileInfo took two such names for one file: a title of an image got "<playlist>_new.mpls" as its output
    // name, and Start muxing refused every output as "one of the source files". Such names are compared as paths.
    if (!fa.exists() || !fb.exists())
    {
#ifdef Q_OS_WIN
        const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
        const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
        return QDir::cleanPath(fa.absoluteFilePath()).compare(QDir::cleanPath(fb.absoluteFilePath()), cs) == 0;
    }
    if (fa == fb)
        return true;
    std::error_code ec;
    const bool same = std::filesystem::equivalent(std::filesystem::path(a.toStdU16String()),
                                                  std::filesystem::path(b.toStdU16String()), ec);
    return same && !ec;
}

QSettings* settings = nullptr;

enum FileCustomData
{
    MplsItemRole = Qt::UserRole,
    FileNameRole,
    ChaptersRole,
    FileDurationRole,
    VideoStartRole,  // the listing's "Video start" and "Video end", set only when it printed them
    VideoEndRole,
    DvdTitleRole,  // a DVD title: its disc number (QtvCodecInfo::dvdTitle); FileNameRole holds the disc
    AngleRole      // the angle of a disc title or playlist (QtvCodecInfo::angle), 1 for the first
};

static const QString FILE_JOIN_PREFIX(" ++ ");

bool doubleCompare(double a, double b) { return qAbs(a - b) < 1e-6; }

// True when a MUXOPT line of the meta text asks for a split. The line is read as tsMuxeR reads it: a tab outside quotes
// is a space, the line is split at spaces outside quotes, and an option is the text before "=". A word inside a quoted
// value, such as --label="no --split-size here", is no option.
bool metaAsksForSplit(const QString& meta)
{
    for (const QString& rawLine : meta.split('\n'))
    {
        QString line = rawLine;
        while (!line.isEmpty() && (line[0] == ' ' || line[0] == '\t')) line.remove(0, 1);
        if (!line.startsWith(QLatin1String("MUXOPT")))
            continue;
        QStringList tokens;
        QString token;
        bool quoted = false;
        for (const QChar c : line)
        {
            if (c == '"')
                quoted = !quoted;
            if ((c == ' ' || c == '\t' || c == '\r') && !quoted)
            {
                tokens << token;
                token.clear();
            }
            else
                token += c;
        }
        tokens << token;
        for (const QString& t : tokens)
        {
            const QString name = t.left(t.indexOf('='));
            if (name == QLatin1String("--split-duration") || name == QLatin1String("--split-size") ||
                name == QLatin1String("--split-chapters"))
                return true;
        }
    }
    return false;
}

QString closeDirPath(const QString& src)
{
    if (src.isEmpty())
        return src;
    if (src[src.length() - 1] == '/' || src[src.length() - 1] == '\\')
        return src;
    return src + QDir::separator();
}

QString unquoteStr(QString val)
{
    val = val.trimmed();
    if (val.isEmpty())
        return val;
    if (val.at(0) == '\"')
    {
        if (QtCompat::strRight(val, 1) == "\"")
            return QtCompat::strMid(val, 1, val.length() - 2);
        else
            return QtCompat::strMid(val, 1, val.length() - 1);
    }
    else
    {
        if (QtCompat::strRight(val, 1) == "\"")
            return QtCompat::strMid(val, 0, val.length() - 1);
        else
            return val;
    }
}

bool isVideoCodec(const QString& displayName)
{
    return displayName == "H.264" || displayName == "MVC" || displayName == "VC-1" || displayName == "MPEG-2" ||
           displayName == "HEVC" || displayName == "VVC" || displayName == "AV1" || displayName == "VP8" ||
           displayName == "VP9" || displayName == "MPEG-4 ASP" || displayName == "ProRes";
}

QString floatToTime(double time, char msSeparator = '.')
{
    // To the nearest millisecond, as the command line writes times. The fraction used to be cut off, and a time is
    // rarely exact in a double: a chapter at 3:54.080 is held as 234.0799999 and was listed as 3:54.079, so a disc
    // made from the chapter box got the mark a millisecond early. Counting whole milliseconds also carries 59.9996 s
    // over to the next minute.
    qint64 msTotal = qRound64(time * 1000.0);
    const int hour = static_cast<int>(msTotal / 3600000);
    msTotal -= static_cast<qint64>(hour) * 3600000;
    const int min = static_cast<int>(msTotal / 60000);
    msTotal -= static_cast<qint64>(min) * 60000;
    const int sec = static_cast<int>(msTotal / 1000);
    const int msec = static_cast<int>(msTotal - static_cast<qint64>(sec) * 1000);
    QString str;
    str += QString::number(hour).rightJustified(2, '0');
    str += ':';
    str += QString::number(min).rightJustified(2, '0');
    str += ':';
    str += QString::number(sec).rightJustified(2, '0');
    str += msSeparator;
    str += QString::number(msec).rightJustified(3, '0');
    return str;
}

QTime qTimeFromFloat(double secondsF)
{
    // To the nearest millisecond, as floatToTime above: cut off, a start time of 1.080 s showed as 1.079.
    const qint64 msTotal = qRound64(secondsF * 1000.0);
    const int hours = static_cast<int>(msTotal / 3600000);
    const int minute = static_cast<int>(msTotal / 60000 % 60);
    const int seconds = static_cast<int>(msTotal / 1000 % 60);
    return QTime(hours, minute, seconds, static_cast<int>(msTotal % 1000));
}

int qTimeToMsec(const QTime& time)
{
    return (time.hour() * 3600 + time.minute() * 60 + time.second()) * 1000 + time.msec();
}

double timeToFloat(const QString& chapterStr)
{
    if (chapterStr.size() == 0)
        return 0;
    QStringList timeParts = chapterStr.split(':');
    double sec = 0;
    if (timeParts.size() > 0)
        sec = timeParts[timeParts.size() - 1].toDouble();
    int min = 0;
    if (timeParts.size() > 1)
        min = timeParts[timeParts.size() - 2].toInt();
    int hour = 0;
    if (timeParts.size() > 2)
        hour = timeParts[timeParts.size() - 3].toInt();
    return hour * 3600 + min * 60 + sec;
}

QString changeFileExt(const QString& value, const QString& newExt)
{
    QFileInfo fi(unquoteStr(value));
    if (fi.suffix().length() > 0 || (!value.isEmpty() && QtCompat::strRight(value, 1) == "."))
        return unquoteStr(QtCompat::strLeft(value, value.length() - fi.suffix().length()) + newExt);
    else
        return unquoteStr(value) + "." + newExt;
}

QString fpsTextToFpsStr(const QString& fpsText)
{
    int p = fpsText.indexOf('/');
    if (p >= 0)
    {
        auto left = QtCompat::strMid(fpsText, 0, p).toFloat();
        auto right = QtCompat::strMid(fpsText, p + 1).toFloat();
        return QString::number(left / right, 'f', 3);
    }
    else
        return fpsText;
}

float extractFloatFromDescr(const QString& str, const QString& pattern)
{
    try
    {
        int p = 0;
        if (!pattern.isEmpty())
            p = str.indexOf(pattern);
        if (p >= 0)
        {
            p += pattern.length();
            int p2 = p;
            while (p2 < str.length() &&
                   ((str.at(p2) >= '0' && str.at(p2) <= '9') || str.at(p2) == '.' || str.at(p2) == '-'))
                p2++;
            return QtCompat::strMid(str, p, p2 - p).toFloat();
        }
    }
    catch (...)
    {
        return 0;
    }
    return 0;
}

QString quoteStr(const QString& val)
{
    QString rez;
    if (val.isEmpty())
        return "";
    if (val.at(0) != '\"')
        rez = '\"' + val;
    else
        rez = val;
    if (val.at(val.length() - 1) != '\"')
        rez += '\"';
    return rez;
}

QString myUnquoteStr(const QString& val) { return unquoteStr(val); }

// The window takes its colours from the system, so a label is painted on a light ground on one
// machine and a dark one on another. Reaching the contrast that normal text needs on BOTH is not
// possible with a single value, so the ground is asked and the colour chosen for it.
//
// The ground is read from the widget's own palette rather than from any platform theme call, so
// it follows whatever the style actually paints and needs no Qt version test.
QString themedInk(const QWidget* w, const QLatin1String& onLight, const QLatin1String& onDark)
{
    const QColor ground = w->palette().color(w->backgroundRole());
    return QString(ground.lightness() < 128 ? onDark : onLight);
}

QString getComboBoxTrackText(int idx, const QtvCodecInfo& codecInfo)
{
    auto text = QString("[%1] %2").arg(idx + 1).arg(codecInfo.displayName);
    if (!codecInfo.lang.isEmpty())
    {
        text.append(", lang : ");
        text.append(codecInfo.lang);
    }
    text.append(", ");
    text.append(codecInfo.descr);
    return text;
}

void initLanguageComboBox(QComboBox* comboBox)
{
    comboBox->addItem("English", "en");  // 0th index is also used as default if the language isn't set in the settings.
    comboBox->addItem(QString::fromUtf8("Русский"), "ru");
    comboBox->addItem(QString::fromUtf8("Français"), "fr");
    comboBox->addItem(QString::fromUtf8("简体中文"), "zh");
    comboBox->addItem(QString::fromUtf8("Deutsch"), "de");
    comboBox->addItem(QString::fromUtf8("עִברִית"), "he");
    comboBox->addItem(QString::fromUtf8("Español"), "es");
    comboBox->addItem(QString::fromUtf8("日本語"), "ja");
    comboBox->setCurrentIndex(-1);  // makes sure currentIndexChanged() is emitted when reading settings.
}

}  // namespace

// ----------------------- TsMuxerWindow -------------------------------------

QString TsMuxerWindow::getOutputDir() const
{
    QString result = ui->radioButtonOutoutInInput->isChecked() ? lastSourceDir : lastOutputDir;
    if (!result.isEmpty())
        result = QDir::toNativeSeparators(closeDirPath(result));
    return result;
}

QString TsMuxerWindow::getDefaultOutputFileName() const
{
    QString prefix = getOutputDir();

    if (ui->radioButtonTS->isChecked())
        return prefix + QString("default.ts");
    else if (ui->radioButtonM2TS->isChecked())
        return prefix + QString("default.m2ts");
    else if (ui->radioButtonMKV->isChecked())
        return prefix + QString("default.mkv");
    else if (ui->radioButtonBluRayISO->isChecked())
        return prefix + QString("default.iso");
    else
        return prefix;
}

// Long tooltips must be rich text, else Qt renders them as a single endless line; a <p>
// wrapper turns on word wrapping without touching the translated strings themselves.
static QString wrapTip(const QString& text) { return QStringLiteral("<p>") + text + QStringLiteral("</p>"); }

// A simple text chapter file can be pasted in as it is. eac3to and MKVToolNix both write CHAPTER01=00:00:00.000
// followed by CHAPTER01NAME=, and every one of those lines used to be rejected because this box wants a bare time. The
// time is taken from a CHAPTERnn= line and a CHAPTERnnNAME= line is skipped (empty); the names are dropped because a
// disc has nowhere to put them and the meta line carries times only.
static QString chapterLineTime(const QString& line)
{
    QString tmpStr = line.trimmed();
    if (tmpStr.startsWith(QLatin1String("CHAPTER"), Qt::CaseInsensitive))
    {
        const int eq = tmpStr.indexOf(QLatin1Char('='));
        if (eq > 0)
        {
            const QString key = tmpStr.left(eq).trimmed();
            if (key.endsWith(QLatin1String("NAME"), Qt::CaseInsensitive))
                return QString();
            tmpStr = QtCompat::strMid(tmpStr, eq + 1).trimmed();
        }
    }
    return tmpStr;
}

// The rule tsMuxeR applies to each time of --custom-chapters (chapterTimeToSeconds): up to three parts between colons,
// each starting with a digit, only digits and one decimal point in the seconds, minutes and seconds below 60, and
// from 0 to 100000 hours. A space ends the value on the meta line, so a time with one in it is refused too.
static bool isChapterTime(const QString& t)
{
    const QStringList parts = t.split(QLatin1Char(':'));
    if (parts.size() > 3)
        return false;
    double seconds = 0.0;
    for (int i = 0; i < parts.size(); ++i)
    {
        const QString& part = parts[i];
        if (part.isEmpty() || part[0] < QLatin1Char('0') || part[0] > QLatin1Char('9') ||
            part.count(QLatin1Char('.')) > (i == parts.size() - 1 ? 1 : 0))
            return false;
        for (const QChar c : part)
            if ((c < QLatin1Char('0') || c > QLatin1Char('9')) && c != QLatin1Char('.'))
                return false;
        const double v = part.toDouble();
        if (i > 0 && v >= 60.0)
            return false;
        seconds = seconds * 60.0 + v;
    }
    return seconds <= 100000.0 * 3600.0;
}

// A red frame and the reason as the tooltip, or the field as it was
static void markWrongValue(QWidget* field, const QString& problem)
{
    if (!field->property("plainToolTip").isValid())
        field->setProperty("plainToolTip", field->toolTip());
    if (problem.isEmpty())
    {
        field->setStyleSheet(QString());
        field->setToolTip(field->property("plainToolTip").toString());
    }
    else
    {
        field->setStyleSheet(QStringLiteral("%1 { border: 2px solid #c0392b; }").arg(field->metaObject()->className()));
        field->setToolTip(wrapTip(problem));
    }
}

// The 3D titles of a BDMV folder, read from its playlists as the command line reads them: a playlist with a
// stereoscopic sub path (extension data 2/2, sub path type 8) plays its clips in 3D, and each such clip has its .ssif
// in BDMV/STREAM/SSIF. Only the playlist files are read. clips counts those clips, missing the ones without a .ssif.
struct Bdmv3dScan
{
    int clips = 0;
    int missing = 0;
};

// A name inside a folder whatever its case. Discs and copies of them write BDMV, PLAYLIST, STREAM and the clip names
// in either case, and on a drive that tells upper and lower case apart, as is usual on Linux, a scan for the exact
// case found nothing in a copy named the other way: the 3D box was greyed and forced off, and every .ssif was reported
// missing. --bdmv-to-iso on the command line compares without case already.
static QString pathNoCase(const QString& dir, const QString& name)
{
    const QDir d(dir);
    if (d.exists(name))
        return d.filePath(name);
    for (const QString& entry : d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot))
        if (entry.compare(name, Qt::CaseInsensitive) == 0)
            return d.filePath(entry);
    return d.filePath(name);
}

static Bdmv3dScan scanBdmv3d(const QString& root)
{
    Bdmv3dScan scan;
    const QString bdmv = pathNoCase(root, QStringLiteral("BDMV"));
    const QDir playlistDir(pathNoCase(bdmv, QStringLiteral("PLAYLIST")));
    const QString streamPath = pathNoCase(bdmv, QStringLiteral("STREAM"));
    const QString ssifPath = pathNoCase(streamPath, QStringLiteral("SSIF"));
    QStringList clips;
    for (const QString& name : playlistDir.entryList(QDir::Files))
    {
        if (!name.endsWith(QStringLiteral(".mpls"), Qt::CaseInsensitive))
            continue;
        QFile file(playlistDir.filePath(name));
        if (file.size() > (1 << 20) || !file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray b = file.readAll();
        const auto at = [&b](const qint64 pos, const int len) -> qint64
        {
            if (pos < 0 || pos + len > b.size())
                return -1;
            qint64 v = 0;
            for (int i = 0; i < len; ++i) v = (v << 8) | static_cast<uchar>(b[static_cast<int>(pos) + i]);
            return v;
        };
        const qint64 ext = at(16, 4);
        if (!b.startsWith("MPLS") || ext <= 0 || at(ext, 4) <= 0)
            continue;
        bool stereo = false;
        const qint64 entries = at(ext + 11, 1);
        for (qint64 k = 0; k < entries && !stereo; ++k)
        {
            const qint64 e = ext + 12 + 12 * k;
            if (at(e, 2) != 2 || at(e + 2, 2) != 2)
                continue;
            const qint64 start = ext + at(e + 4, 4);
            const qint64 subPaths = at(start + 4, 2);
            qint64 pos = start + 6;
            for (qint64 s = 0; s < subPaths && pos >= 0 && !stereo; ++s)
            {
                stereo = at(pos + 5, 1) == 8;
                const qint64 len = at(pos, 4);
                pos = len < 0 ? -1 : pos + 4 + len;
            }
        }
        if (!stereo)
            continue;
        const qint64 list = at(8, 4);
        const qint64 items = at(list + 6, 2);
        qint64 pos = list + 10;
        for (qint64 i = 0; i < items && pos >= 0 && pos + 7 <= b.size(); ++i)
        {
            const QString clip = QString::fromLatin1(b.mid(static_cast<int>(pos) + 2, 5));
            if (!clips.contains(clip))
                clips << clip;
            const qint64 len = at(pos, 2);
            pos = len < 0 ? -1 : pos + 2 + len;
        }
    }
    // Only clips that are in the folder count: one whose .m2ts is missing too cannot be rebuilt either.
    for (const QString& clip : clips)
    {
        if (!QFileInfo::exists(pathNoCase(streamPath, clip + QStringLiteral(".m2ts"))))
            continue;
        ++scan.clips;
        if (!QFileInfo::exists(pathNoCase(ssifPath, clip + QStringLiteral(".ssif"))))
            ++scan.missing;
    }
    return scan;
}

// ── The disc table, in ONE place ─────────────────────────────────────────────────────────────
// Two parts of the interface need it: the "BDMV folder -> ISO" tab, which works out the layer
// break from it, and the dual-layer group beside the normal Blu-ray output, which used to carry
// its own much cruder list (BD25/BD50/BD100/BD128 with no capacities at all) and therefore put
// the guard at the BD-R DL break on every disc, including 100 and 128 GB ones where that sector
// is not a layer boundary at all. Sharing the table is what keeps the two honest.
//
// The Free Sectors are what ImgBurn reports and what the layer break is computed from, so they
// appear in the label as well: the same disc size exists at two capacities, a defect-managed
// disc holding 1 GiB per layer back as ISA/OSA spare and reporting 47,305,728 (100 GB),
// 60,403,712 (128 GB) or 23,652,352 (BD-RE DL) instead. Both kinds have been measured on real
// discs, so neither can be assumed, and picking the wrong one moves the break a whole GiB, far
// outside any guard band. These full capacities are also exactly what the engine's own BD25/BD50/
// BD100/BD128 constants have always used (vod_common.h), so the two now agree.
struct DiscEntry
{
    const char* label;   // shown in both combos, capacity included so it can be matched to ImgBurn
    qint64 freeSectors;  // 2048-byte sectors
    int layers;          // how many layers the data is laid across (breaks = layers - 1)
    bool isXL;           // BD-R XL media: drives the player-compatibility warning
    const char* band;    // capacity band name, for spotting a Free Sectors value that disagrees
};

static const DiscEntry* discTable(int& count)
{
    static const DiscEntry entries[] = {
        {QT_TRANSLATE_NOOP("TsMuxerWindow", "BD-R DL 50 GB, 2 layers (24,438,784 sectors)"), 24438784, 2, false,
         "50 GB"},
        {QT_TRANSLATE_NOOP("TsMuxerWindow", "BD-RE DL 50 GB, 2 layers (23,652,352 sectors)"), 23652352, 2, false,
         "50 GB"},
        {QT_TRANSLATE_NOOP("TsMuxerWindow", "BD-R XL 100 GB, 3 layers (48,878,592 sectors)"), 48878592, 3, true,
         "100 GB"},
        // 66 GB on a 100 GB disc: there is no 66 GB BD-R and many players cope with two layers
        // better than three. Two thirds of the disc, so its one break falls on the disc's real
        // first layer boundary and the third layer is never written.
        {QT_TRANSLATE_NOOP("TsMuxerWindow", "BD-R XL 100 GB, first 2 layers only (32,585,728 sectors)"), 32585728, 2,
         true, "66 GB"},
        {QT_TRANSLATE_NOOP("TsMuxerWindow", "BD-R XL 128 GB, 4 layers (62,500,864 sectors)"), 62500864, 4, true,
         "128 GB"},
    };
    count = static_cast<int>(sizeof(entries) / sizeof(entries[0]));
    return entries;
}

// Single layer discs, for the "Fit to disc" list beside the normal Blu-ray output ONLY. That list
// is older than the disc table above and always offered BD25, so dropping it would take away a
// size people use. They are not offered in the layer-break calculator because a single layer disc
// has no layer break to calculate: capacity is all they contribute, which is exactly what fitting
// to a disc needs.
//
// Only the FULL capacity is listed, and deliberately so. A disc formatted with defect management
// reports less, but "less" is not one number: asked what it could be formatted to, a single blank
// 25 GB disc here offered 11,826,176, 12,088,320 and 5,796,864 blocks, three different spare area
// sizes. Listing one of them would present an arbitrary choice as the answer, which is the mistake
// this whole area already made once. A defect-managed disc is served by the manual field, and in
// future by reading the capacity off the drive.
static const DiscEntry* singleLayerDiscs(int& count)
{
    static const DiscEntry entries[] = {
        {QT_TRANSLATE_NOOP("TsMuxerWindow", "BD 25 GB, 1 layer (12,219,392 sectors)"), 12219392, 1, false, "25 GB"},
    };
    count = static_cast<int>(sizeof(entries) / sizeof(entries[0]));
    return entries;
}

// The break sector(s) for a disc: every layer boundary except the last, which has no data after it.
static QList<qint64> discBreaks(qint64 freeSectors, int layers)
{
    QList<qint64> breaks;
    if (freeSectors > 0)
        for (int k = 1; k < layers; ++k) breaks << freeSectors * k / layers;
    return breaks;
}

namespace
{
// The content of a tab, in the scroll area of its page: as high as its layout needs, which scrolls when the window is
// lower, and as wide as the window, as the tab was before. Scrolled sideways, the buttons beside the track list went
// out of sight.
class TabContent final : public QWidget
{
   public:
    [[nodiscard]] QSize minimumSizeHint() const override { return {0, QWidget::minimumSizeHint().height()}; }
};
}  // namespace

TsMuxerWindow::TsMuxerWindow()
    : ui(new Ui::TsMuxerWindow),
      disableUpdatesCnt(0),
      outFileNameModified(false),
      outFileNameDisableChange(false),
      muxForm(new MuxForm(this)),
      m_updateMeta(true),
      m_3dMode(false),
      m_fileDialogOpen(false),
      m_customChaptersUserOverride(false),
      m_metaUserOverride(false)
{
    ui->setupUi(this);
    setUiMetaItemsData();
    qApp->installTranslator(&qtCoreTranslator);
    qApp->installTranslator(&tsMuxerTranslator);
    initLanguageComboBox(ui->languageSelectComboBox);
    setWindowTitle("tsMuxeR GUI " TSMUXER_VERSION);
    lastInputDir = QDir::homePath();
    lastOutputDir = QDir::homePath();

    void (QComboBox::*comboBoxIndexChanged)(int) = &QComboBox::currentIndexChanged;
    connect(ui->languageSelectComboBox, comboBoxIndexChanged, this, &TsMuxerWindow::onLanguageComboBoxIndexChanged);

    // BEFORE readSettings, not after. The stored choice is restored by looking the value up in
    // this box, so an empty box means the lookup fails and the default wins. That is exactly the
    // shape of the fault the interface language appears to have, and it was found here only
    // because the restore was tested rather than assumed to work.
    // Three positions, not a tick: wanting the source's own codes untouched is as reasonable as
    // wanting the ones eac3to shows. The data is what reaches the muxer, so it stays English while
    // the text beside it is translated.
    auto fillLangCodes = [this]()
    {
        const QString keep = ui->langCodesComboBox->currentData().toString();
        const QSignalBlocker block(ui->langCodesComboBox);
        ui->langCodesComboBox->clear();
        ui->langCodesComboBox->addItem(tr("As the source says"), "source");
        ui->langCodesComboBox->addItem(tr("Terminological (ell, deu, nld)"), "terminological");
        ui->langCodesComboBox->addItem(tr("Bibliographic (gre, ger, dut)"), "bibliographic");
        const int idx = ui->langCodesComboBox->findData(keep.isEmpty() ? "terminological" : keep);
        ui->langCodesComboBox->setCurrentIndex(idx < 0 ? 1 : idx);
    };
    fillLangCodes();
    m_retranslateHooks.push_back(fillLangCodes);

    // Before the settings are read: readSettings() fills it, and the font is a setting like any other.
    fontSettingsModel = new FontSettingsTableModel(this);

    // With a tsMuxerGUI.ini beside the program, the ini is the ONLY place settings are kept. The registry was read
    // first and the ini laid over it, so whatever the ini lacked came from the registry of the machine it ran on, and
    // several things were written to the registry on every start. An ini that is still empty takes over the registry's
    // settings once, read only, so making a copy portable keeps the choices made before.
    if (QFile::exists(tsMuxerGuiIniName()))
    {
        settings = new QSettings(tsMuxerGuiIniName(), QSettings::IniFormat);
        if (!readSettings())
        {
            QSettings* ini = settings;
            settings = m_seedSettings = new QSettings();
            // Settings left by a version before the name change are only under the old company name, and the copy
            // to the new name (main.cpp) is not made while an ini exists: the empty ini took over nothing.
            if (m_seedSettings->allKeys().isEmpty())
            {
                delete m_seedSettings;
                settings = m_seedSettings = new QSettings(QStringLiteral("Network Optix"), QStringLiteral("tsMuxeR"));
            }
            readSettings();
            settings = ini;
        }
    }
    else
    {
        settings = new QSettings();
        readSettings();
    }

    ui->outFileName->setText(getDefaultOutputFileName());

    m_header = new QnCheckBoxedHeaderView(this);
    ui->trackLV->setHorizontalHeader(m_header);
    ui->trackLV->horizontalHeader()->setStretchLastSection(true);
    m_header->setVisible(true);
    m_header->setSectionsClickable(true);

    connect(m_header, &QnCheckBoxedHeaderView::checkStateChanged, this, &TsMuxerWindow::at_sectionCheckstateChanged);

    /////////////////////////////////////////////////////////////
    for (int i = 0; i <= 3600; i += 5 * 60) ui->memoChapters->insertPlainText(floatToTime(i, '.') + '\n');

    const static int colWidths[] = {31, 200, 62, 62, 52, 40, 10};
    for (unsigned i = 0u; i < sizeof(colWidths) / sizeof(int); ++i)
        ui->trackLV->horizontalHeader()->resizeSection(i, colWidths[i]);
    // The order columns (#52) say where a playlist puts a track among its kind and where its clip has it; they show
    // only while a track of a playlist is in the list
    const auto orderTips = [this]()
    {
        if (auto* h = ui->trackLV->horizontalHeaderItem(4))
            h->setToolTip(
                wrapTip(tr("The track's number among the tracks of its kind (audio, subtitles) in the "
                           "playlist: the number a player gives it. The tracks are muxed in this order.")));
        if (auto* h = ui->trackLV->horizontalHeaderItem(5))
            h->setToolTip(
                wrapTip(tr("The track's number among the tracks of its kind in the clip (the .m2ts file), "
                           "by PID. It can differ from the playlist's.")));
    };
    orderTips();
    m_retranslateHooks.push_back(orderTips);
    updateOrderColumns();
    ui->trackLV->setWordWrap(false);

    ui->fontSettingsTableView->setModel(fontSettingsModel);
    ui->fontSettingsTableView->horizontalHeader()->resizeSection(0, 65);
    ui->fontSettingsTableView->horizontalHeader()->resizeSection(1, 185);
    for (int i = 0; i < fontSettingsModel->rowCount(QModelIndex()); ++i)
    {
        ui->fontSettingsTableView->setRowHeight(i, 20);
    }

    // The four rows are Name, Size, Color and Options, and the box was given a fixed maximum of
    // 84 pixels for them. That is exact only if a row really is 20 pixels and the frame really is
    // one pixel each side, and with this style and this display it is a little short. The fourth
    // row was therefore cut off and a scroll bar appeared to reach it, which is what someone asked
    // about. Ask the view how tall its own rows came out instead of assuming.
    const int fontTableHeight =
        ui->fontSettingsTableView->verticalHeader()->length() + 2 * ui->fontSettingsTableView->frameWidth();
    ui->fontSettingsTableView->setMinimumHeight(fontTableHeight);
    ui->fontSettingsTableView->setMaximumHeight(fontTableHeight);

    langCodesModel = new LangCodesModel(this);
    ui->langComboBox->setModel(langCodesModel);
    ui->videoLangComboBox->setModel(langCodesModel);

    void (QSpinBox::*spinBoxValueChanged)(int) = &QSpinBox::valueChanged;
    void (QDoubleSpinBox::*doubleSpinBoxValueChanged)(double) = &QDoubleSpinBox::valueChanged;
    connect(&opacityTimer, &QTimer::timeout, this, &TsMuxerWindow::onOpacityTimer);
    connect(ui->trackLV, &QTableWidget::itemSelectionChanged, this, &TsMuxerWindow::trackLVItemSelectionChanged);
    connect(ui->trackLV, &QTableWidget::itemChanged, this, &TsMuxerWindow::trackLVItemChanged);
    connect(ui->inputFilesLV, &QListWidget::currentRowChanged, this, &TsMuxerWindow::inputFilesLVChanged);
    connect(ui->addBtn, &QPushButton::clicked, this, &TsMuxerWindow::onAddBtnClick);
    connect(ui->btnAppend, &QPushButton::clicked, this, &TsMuxerWindow::onAppendButtonClick);
    connect(ui->removeFile, &QPushButton::clicked, this, &TsMuxerWindow::onRemoveBtnClick);
    connect(ui->sampleBtn, &QPushButton::clicked, this, &TsMuxerWindow::onMakeSample);
    connect(ui->removeTrackBtn, &QPushButton::clicked, this, &TsMuxerWindow::onRemoveTrackButtonClick);
    connect(ui->removeAllTracksBtn, &QPushButton::clicked, this, &TsMuxerWindow::onRemoveAllTracksButtonClick);
    connect(ui->moveupBtn, &QPushButton::clicked, this, &TsMuxerWindow::onMoveUpButtonCLick);
    connect(ui->movedownBtn, &QPushButton::clicked, this, &TsMuxerWindow::onMoveDownButtonCLick);
    connect(ui->checkFPS, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onVideoCheckBoxChanged);
    connect(ui->checkBoxLevel, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onVideoCheckBoxChanged);
    // The Dolby Vision profile selector had no connection at all, while every other combo here has
    // one. The meta file handed to the muxer IS the text in the preview, so a choice that never
    // refreshed the preview never reached the mux either: picking profile 8.1 changed the combo
    // and nothing else, and the disc came out as profile 7 with nothing said about it.
    connect(ui->comboBoxDvProfile, comboBoxIndexChanged, this, &TsMuxerWindow::updateMetaLines);
    connect(ui->comboBoxSEI, comboBoxIndexChanged, this, &TsMuxerWindow::onVideoCheckBoxChanged);
    connect(ui->checkBoxSecondaryVideo, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onVideoCheckBoxChanged);
    connect(ui->checkBoxSPS, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onVideoCheckBoxChanged);
    connect(ui->checkBoxHdr10Override, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onVideoCheckBoxChanged);
    connect(ui->checkBoxRemovePulldown, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onPulldownCheckBoxChanged);
    connect(ui->comboBoxFPS, comboBoxIndexChanged, this, &TsMuxerWindow::onVideoComboBoxChanged);
    connect(ui->comboBoxLevel, comboBoxIndexChanged, this, &TsMuxerWindow::onVideoComboBoxChanged);
    connect(ui->comboBoxAR, comboBoxIndexChanged, this, &TsMuxerWindow::onVideoComboBoxChanged);
    connect(ui->videoLangComboBox, comboBoxIndexChanged, this, &TsMuxerWindow::onVideoComboBoxChanged);
    connect(ui->checkBoxKeepFps, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onAudioSubtitlesParamsChanged);
    connect(ui->dtsDwnConvert, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onAudioSubtitlesParamsChanged);
    connect(ui->dropAc3CoreCheckBox, &QCheckBox::checkStateChanged, this,
            &TsMuxerWindow::onAudioSubtitlesParamsChanged);
    connect(ui->splitAc3CoreCheckBox, &QCheckBox::checkStateChanged, this,
            &TsMuxerWindow::onAudioSubtitlesParamsChanged);
    connect(ui->w64CheckBox, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onAudioSubtitlesParamsChanged);
    connect(ui->secondaryCheckBox, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onAudioSubtitlesParamsChanged);
    // the option on the MUXOPT line follows the question being on or off (getMuxOpts)
    connect(ui->checkBoxAskExtraTracks, &QCheckBox::checkStateChanged, this, [this] { updateMetaLines(); });
    connect(ui->mergeAc3TrackSpinBox, spinBoxValueChanged, this, &TsMuxerWindow::onAudioSubtitlesParamsChanged);
    connect(ui->mergeAc3FileLineEdit, &QLineEdit::textChanged, this, &TsMuxerWindow::onAudioSubtitlesParamsChanged);
    connect(ui->mergeAc3FileBrowseButton, &QPushButton::clicked, this, &TsMuxerWindow::onMergeAc3FileBrowseClicked);
    connect(ui->langComboBox, comboBoxIndexChanged, this, &TsMuxerWindow::onAudioSubtitlesParamsChanged);
    // A double quote would end the quoted name in the meta, so it cannot be typed.
    ui->mkvTrackNameEdit->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[^\"]*")), ui->mkvTrackNameEdit));
    connect(ui->mkvTrackNameEdit, &QLineEdit::textChanged, this, &TsMuxerWindow::onMkvTrackOptionsChanged);
    for (QCheckBox* box : {ui->mkvFlagForced, ui->mkvFlagHearing, ui->mkvFlagVisual, ui->mkvFlagTextDesc,
                           ui->mkvFlagOriginal, ui->mkvFlagCommentary})
        connect(box, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onMkvTrackOptionsChanged);
    connect(ui->offsetsComboBox, comboBoxIndexChanged, this, &TsMuxerWindow::onAudioSubtitlesParamsChanged);
    connect(ui->langCodesComboBox, comboBoxIndexChanged, this, &TsMuxerWindow::onSavedParamChanged);
    // The language box listed every code in its terminological form, so with Bibliographic chosen a track tagged
    // "cze" showed "ces (Czech)", although "cze" is what the track keeps and what is written. The box now shows the
    // codes in the chosen form. "As the source says" has no single form, so it shows the list as it is.
    auto showLangCodeForm = [this]()
    {
        if (ui->langCodesComboBox->currentData().toString() == QLatin1String("bibliographic"))
            langCodesModel->setShownCode([](const QString& code) { return toBibliographicLang(code); });
        else
            langCodesModel->setShownCode(nullptr);
    };
    showLangCodeForm();
    connect(ui->langCodesComboBox, comboBoxIndexChanged, this, showLangCodeForm);
    connect(ui->comboBoxPipCorner, comboBoxIndexChanged, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->comboBoxPipSize, comboBoxIndexChanged, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->spinBoxPipOffsetH, spinBoxValueChanged, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->spinBoxPipOffsetV, spinBoxValueChanged, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->checkBoxSound, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->editDelay, spinBoxValueChanged, this, &TsMuxerWindow::onEditDelayChanged);
    connect(ui->muxTimeEdit, &QTimeEdit::timeChanged, this, &TsMuxerWindow::updateMuxTime1);
    connect(ui->muxTimeClock, spinBoxValueChanged, this, &TsMuxerWindow::updateMuxTime2);
    connect(ui->muxTimeEdit, &QTimeEdit::editingFinished, this, &TsMuxerWindow::snapMuxTimeToBdMinimum);
    connect(ui->muxTimeClock, &QSpinBox::editingFinished, this, &TsMuxerWindow::snapMuxTimeToBdMinimum);
    connect(ui->fontButton, &QPushButton::clicked, this, &TsMuxerWindow::onFontBtnClicked);
    connect(ui->colorButton, &QPushButton::clicked, this, &TsMuxerWindow::onColorBtnClicked);
    connect(ui->checkBoxVBR, &QPushButton::clicked, this, &TsMuxerWindow::onGeneralCheckboxClicked);
    connect(ui->spinBoxMplsNum, spinBoxValueChanged, this, &TsMuxerWindow::onGeneralCheckboxClicked);
    connect(ui->spinBoxM2tsNum, spinBoxValueChanged, this, &TsMuxerWindow::onGeneralCheckboxClicked);
    connect(ui->checkBoxBlankPL, &QPushButton::clicked, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->checkBoxV3, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::updateMetaLines);
    connect(ui->checkBoxRateLimit, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::updateMetaLines);
    // Which of the two controls applies depends on the other two boxes, so both have to settle the
    // enabled state as well as rewrite the meta.
    connect(ui->checkBoxV3, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::updateBluRayTabEnabled);
    connect(ui->checkBoxRateLimit, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::updateBluRayTabEnabled);
    // ** THE UHD LIMIT IS A TABLE, NOT A NUMBER. ** BD-ROM Version 3 gives a maximum multiplex rate
    // per disc capacity and per zone, so which figure applies depends on the disc being made:
    //
    //   50 GB                                 64 Mbit/s and 81.7
    //   66 GB and 100 GB                      81.7 and 109
    //   the high transfer rate zone of those  127.9
    //
    // 109 is the default because it is what a UHD Blu-ray is mastered to: two conformant UHD
    // sources measured here sit exactly on it. HD has one figure, 48, so the choice is offered only for V3.
    // The value is kbit/s, which is what --maxbitrate takes.
    ui->comboRateLimit->addItem(tr("109 Mbit/s (66 or 100 GB)"), 109000);
    ui->comboRateLimit->addItem(tr("81.7 Mbit/s (50 GB, or the inner zone of a larger disc)"), 81700);
    ui->comboRateLimit->addItem(tr("64 Mbit/s (the inner zone of a 50 GB disc)"), 64000);
    ui->comboRateLimit->addItem(tr("127.9 Mbit/s (high transfer rate zone)"), 127900);
    connect(ui->comboRateLimit, comboBoxIndexChanged, this, &TsMuxerWindow::updateMetaLines);
    connect(ui->BlackplaylistCombo, spinBoxValueChanged, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->checkBoxNewAudioPes, &QAbstractButton::clicked, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->checkBoxCrop, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->checkBoxPlaylistParts, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onPlaylistPartsChanged);
    connect(ui->checkBoxRVBR, &QAbstractButton::clicked, this, &TsMuxerWindow::onGeneralCheckboxClicked);
    connect(ui->checkBoxCBR, &QAbstractButton::clicked, this, &TsMuxerWindow::onGeneralCheckboxClicked);
    connect(ui->radioButtonStoreOutput, &QAbstractButton::clicked, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->radioButtonOutoutInInput, &QAbstractButton::clicked, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->editVBVLen, spinBoxValueChanged, this, &TsMuxerWindow::onGeneralSpinboxValueChanged);
    connect(ui->editMaxBitrate, doubleSpinBoxValueChanged, this, &TsMuxerWindow::onGeneralSpinboxValueChanged);
    connect(ui->editMinBitrate, doubleSpinBoxValueChanged, this, &TsMuxerWindow::onGeneralSpinboxValueChanged);
    connect(ui->editCBRBitrate, doubleSpinBoxValueChanged, this, &TsMuxerWindow::onGeneralSpinboxValueChanged);
    connect(ui->rightEyeCheckBox, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::updateMetaLines);
    connect(ui->radioButtonAutoChapter, &QAbstractButton::clicked, this, &TsMuxerWindow::onChapterParamsChanged);
    connect(ui->radioButtonNoChapters, &QAbstractButton::clicked, this, &TsMuxerWindow::onChapterParamsChanged);
    connect(ui->radioButtonCustomChapters, &QAbstractButton::clicked, this, &TsMuxerWindow::onChapterParamsChanged);
    connect(ui->spinEditChapterLen, spinBoxValueChanged, this, &TsMuxerWindow::onChapterParamsChanged);
    connect(ui->memoChapters, &QPlainTextEdit::textChanged, this, &TsMuxerWindow::onChapterParamsChanged);
    connect(ui->noSplit, &QAbstractButton::clicked, this, &TsMuxerWindow::onSplitCutParamsChanged);
    connect(ui->splitByDuration, &QAbstractButton::clicked, this, &TsMuxerWindow::onSplitCutParamsChanged);
    connect(ui->splitBySize, &QAbstractButton::clicked, this, &TsMuxerWindow::onSplitCutParamsChanged);
    connect(ui->splitByChapters, &QAbstractButton::clicked, this, &TsMuxerWindow::onSplitCutParamsChanged);
    connect(ui->spinEditSplitDuration, spinBoxValueChanged, this, &TsMuxerWindow::onSplitCutParamsChanged);
    connect(ui->editSplitSize, doubleSpinBoxValueChanged, this, &TsMuxerWindow::onSplitCutParamsChanged);
    connect(ui->comboBoxMeasure, comboBoxIndexChanged, this, &TsMuxerWindow::onSplitCutParamsChanged);
    connect(ui->checkBoxCut, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::onSplitCutParamsChanged);
    connect(ui->cutStartTimeEdit, &QTimeEdit::timeChanged, this, &TsMuxerWindow::onSplitCutParamsChanged);
    connect(ui->cutEndTimeEdit, &QTimeEdit::timeChanged, this, &TsMuxerWindow::onSplitCutParamsChanged);
    connect(ui->checkBoxCutInside, &QAbstractButton::clicked, this, &TsMuxerWindow::onSplitCutParamsChanged);
    connect(ui->cutStartTimeEdit, &QTimeEdit::editingFinished, this, &TsMuxerWindow::checkCut);
    connect(ui->cutEndTimeEdit, &QTimeEdit::editingFinished, this, &TsMuxerWindow::checkCut);
    connect(ui->checkBoxCut, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::checkCut);
    connect(ui->radioButtonAutoChapter, &QAbstractButton::clicked, this, &TsMuxerWindow::checkCustomChapters);
    connect(ui->radioButtonNoChapters, &QAbstractButton::clicked, this, &TsMuxerWindow::checkCustomChapters);
    connect(ui->radioButtonCustomChapters, &QAbstractButton::clicked, this, &TsMuxerWindow::checkCustomChapters);
    connect(ui->spinEditBorder, spinBoxValueChanged, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->comboBoxAnimation, comboBoxIndexChanged, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->lineSpacing, doubleSpinBoxValueChanged, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->spinEditOffset, spinBoxValueChanged, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->keepTextCheckBox, &QCheckBox::toggled, this,
            [this]
            {
                updateKeepTextControls();
                onSavedParamChanged();
            });
    connect(ui->saveSrtCheckBox, &QCheckBox::toggled, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->textStAsSrtCheckBox, &QCheckBox::toggled, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->assAsSrtCheckBox, &QCheckBox::toggled, this,
            [this]
            {
                updateKeepTextControls();
                onSavedParamChanged();
            });
    connect(ui->fontsDirLineEdit, &QLineEdit::textChanged, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->fontsDirBrowseButton, &QAbstractButton::clicked, this,
            [this]
            {
                const QString current = ui->fontsDirLineEdit->text().trimmed();
                const QString dir = QFileDialog::getExistingDirectory(
                    this, tr("Fonts folder for ASS and SSA subtitles"), current.isEmpty() ? lastInputDir : current);
                if (!dir.isEmpty())
                    ui->fontsDirLineEdit->setText(QDir::toNativeSeparators(dir));
            });
    connect(ui->radioButtonTS, &QAbstractButton::clicked, this, &TsMuxerWindow::RadioButtonMuxClick);
    connect(ui->radioButtonM2TS, &QAbstractButton::clicked, this, &TsMuxerWindow::RadioButtonMuxClick);
    connect(ui->radioButtonMKV, &QAbstractButton::clicked, this, &TsMuxerWindow::RadioButtonMuxClick);
    connect(ui->radioButtonBluRay, &QAbstractButton::clicked, this, &TsMuxerWindow::RadioButtonMuxClick);
    connect(ui->radioButtonBluRayISO, &QAbstractButton::clicked, this, &TsMuxerWindow::RadioButtonMuxClick);
    connect(ui->radioButtonAVCHD, &QAbstractButton::clicked, this, &TsMuxerWindow::RadioButtonMuxClick);
    connect(ui->radioButtonDemux, &QAbstractButton::clicked, this, &TsMuxerWindow::RadioButtonMuxClick);
    connect(ui->outFileName, &QLineEdit::textChanged, this, &TsMuxerWindow::outFileNameChanged);
    connect(ui->DiskLabelEdit, &QLineEdit::textChanged, this, &TsMuxerWindow::onGeneralCheckboxClicked);
    connect(ui->btnBrowse, &QAbstractButton::clicked, this, &TsMuxerWindow::saveFileDialog);
    connect(ui->buttonMux, &QAbstractButton::clicked, this, &TsMuxerWindow::startMuxing);
    connect(muxForm, &MuxForm::stopped, this, &TsMuxerWindow::onMuxStopped);
    connect(muxForm, &MuxForm::makeSampleRequested, this, &TsMuxerWindow::onMakeSample);
    connect(ui->trackLV->model(), &QAbstractItemModel::rowsInserted, this, &TsMuxerWindow::updateMuxButton);
    connect(ui->trackLV->model(), &QAbstractItemModel::rowsRemoved, this, &TsMuxerWindow::updateMuxButton);
    connect(ui->trackLV->model(), &QAbstractItemModel::modelReset, this, &TsMuxerWindow::updateMuxButton);
    updateMuxButton();
    connect(ui->buttonSaveMeta, &QAbstractButton::clicked, this, &TsMuxerWindow::saveMetaFileBtnClick);
    connect(ui->buttonOpenMeta, &QAbstractButton::clicked, this, &TsMuxerWindow::openMetaFileBtnClick);
    connect(ui->metaAutoCheckBox, &QAbstractButton::clicked, this,
            [this](const bool on)
            {
                if (on)
                    onResetMetaBtnClick();
                else
                {
                    m_metaUserOverride = true;
                    updateMetaState();
                }
            });
    connect(ui->memoMeta, &QPlainTextEdit::textChanged, this, &TsMuxerWindow::onMetaTextChanged);
    connect(ui->radioButtonOutoutInInput, &QAbstractButton::clicked, this, &TsMuxerWindow::onSavedParamChanged);
    connect(ui->defaultAudioTrackComboBox, comboBoxIndexChanged, this, &TsMuxerWindow::updateMetaLines);
    connect(ui->defaultSubTrackComboBox, comboBoxIndexChanged, this, &TsMuxerWindow::updateMetaLines);
    connect(ui->defaultAudioTrackCheckBox, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::updateMetaLines);
    connect(ui->defaultSubTrackCheckBox, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::updateMetaLines);
    connect(ui->defaultSubTrackForcedOnlyCheckBox, &QCheckBox::checkStateChanged, this,
            &TsMuxerWindow::updateMetaLines);
    connect(ui->pipTransparencySpinBox, spinBoxValueChanged, this, &TsMuxerWindow::updateMetaLines);

    connect(&proc, &QProcess::readyReadStandardOutput, this, &TsMuxerWindow::readFromStdout);
    connect(&proc, &QProcess::readyReadStandardError, this, &TsMuxerWindow::readFromStderr);
    void (QProcess::*processFinished)(int, QProcess::ExitStatus) = &QProcess::finished;
    connect(&proc, processFinished, this, &TsMuxerWindow::onProcessFinished);
    void (QProcess::*processError)(QProcess::ProcessError) = &QProcess::errorOccurred;
    connect(&proc, processError, this, &TsMuxerWindow::onProcessError);

    // The input file panel explained nothing about itself. Every control on it, the two buttons
    // and the list, had no tooltip at all, so someone holding a release split across two discs saw
    // a button called "join" sitting between "add" and "remove" with nothing to say what it joins
    // or why they would want it. The only description of the feature was one line in a sixty line
    // list on the About tab. That is what sends people to other tools for something this already
    // does, so the answer is here rather than in the documentation.
    ui->addBtn->setToolTip(
        wrapTip(tr("Add a file as a NEW, separate input. To continue a title with its next "
                   "part instead, select that part's first file and use join.")));
    // A disc folder or a drive cannot be chosen in the file dialog of "add", which takes files: an image (.iso) can,
    // and any of them can be dropped on the window. This button chooses a folder or a drive; a disc then shows its
    // titles to choose from (onDiscTitlesReceived).
    auto* addDiscBtn = m_addDiscBtn = new QPushButton(this);
    addDiscBtn->setObjectName(QStringLiteral("addDiscBtn"));
    ui->verticalLayout->insertWidget(ui->verticalLayout->indexOf(ui->addBtn) + 1, addDiscBtn);
    const auto addDiscTexts = [this, addDiscBtn]()
    {
        addDiscBtn->setText(tr("add disc"));
        addDiscBtn->setToolTip(
            wrapTip(tr("Add a title of a Blu-ray or DVD disc: choose the disc's folder, the "
                       "folder holding BDMV or VIDEO_TS, or a drive. Its titles are listed, the "
                       "main film first, and the one chosen is added. A disc image (.iso) is "
                       "added with \"add\", and a disc can also be dropped on the window.")));
    };
    addDiscTexts();
    m_retranslateHooks.push_back(addDiscTexts);
    connect(addDiscBtn, &QPushButton::clicked, this,
            [this]()
            {
                if (m_fileDialogOpen)
                    return;
                const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose a disc folder or drive"),
                                                                      getExistingDialogDir(lastInputDir));
                if (dir.isEmpty())
                    return;
                lastInputDir = dir;
                writeSettings();
                addFileList = {QUrl::fromLocalFile(QDir::toNativeSeparators(dir))};
                addFile();
            });
    ui->mergeAc3TrackSpinBox->setToolTip(
        wrapTip(tr("A Blu-ray carries a TrueHD track with its AC-3 core braided into the same stream, and a "
                   "Matroska file has to keep them apart. Give the ROW NUMBER of the AC-3 track in the list "
                   "above and the two are put back together, which is what a disc needs. 0 leaves them "
                   "separate. That row keeps its tick and is still shown, it simply travels inside the "
                   "TrueHD track instead of being written on its own.")));
    ui->mergeAc3TrackLabel->setToolTip(ui->mergeAc3TrackSpinBox->toolTip());
    ui->btnAppend->setToolTip(
        wrapTip(tr("Append another file to the END of the selected one, so the two are muxed as a single "
                   "continuous title. This is what you want for a film split across two discs or two parts: "
                   "select part 1, press join, and choose part 2. The appended file is shown indented with "
                   "++ beneath the one it continues. Every part must have the same file extension.")));
    ui->inputFilesLV->setToolTip(
        wrapTip(tr("The files this mux reads. A line indented with ++ is joined onto the "
                   "file above it and continues it, rather than being a separate input.")));
    // The two "remove" buttons were missed when the rest of this panel was explained, and between
    // them they hold the sharpest traps left here. The one under "join" does NOT remove a joined
    // part along with the file it continues: onRemoveBtnClick strips the ++ from the next line
    // instead, which quietly turns part 2 into a title of its own. The one beside the track list
    // sits a few pixels from the tick box that does the reversible version of the same job.
    ui->removeFile->setToolTip(
        wrapTip(tr("Remove the selected file, and its tracks with it. Take care with a joined "
                   "title: a part joined beneath this one with ++ is NOT removed too. It loses "
                   "the ++ and stays behind as a separate input in its own right, so removing "
                   "part 1 leaves part 2 muxing on its own.")));
    ui->removeTrackBtn->setToolTip(
        wrapTip(tr("Remove the selected track from the mux altogether. To leave a track out "
                   "without losing it, untick its box in the list instead: that can be "
                   "undone, this cannot, and the file has to be added again to get the "
                   "track back.")));
    ui->w64CheckBox->setToolTip(
        wrapTip(tr("Write this track as Sony Wave64 rather than as a WAV. A WAV states its sizes "
                   "in 32 bits, so it cannot describe a file past 4 GiB and a long track has to "
                   "come out as two, each one playable and with nothing lost. Wave64 is the same "
                   "audio with 64 bit sizes and stays a single file. tsMuxeR reads both, and so "
                   "does most software that handles long recordings.")));
    ui->langCodesComboBox->setToolTip(
        wrapTip(tr("Which form a language read from a disc or a container is shown in. Twenty "
                   "languages have two codes in ISO 639-2: Greek is ell or gre, German deu or "
                   "ger. Discs mostly carry the first kind and eac3to reports the second, so this "
                   "chooses which you see. It also decides what a mux writes, because the meta "
                   "file is built from what is shown. A language you type into a file name is "
                   "left exactly as you typed it. The setting is remembered.")));
    ui->splitByChapters->setToolTip(
        wrapTip(tr("Start a new file at every chapter. The marks come from the source: a Blu-ray "
                   "playlist's own chapters, or a Matroska's chapter list. A list typed on the "
                   "Blu-ray tab overrides them. A file always begins at a key frame, so a split "
                   "lands on the first one at or after the mark; on a disc, whose marks already "
                   "sit on key frames, that is the mark itself.")));
    ui->buttonOpenMeta->setToolTip(
        wrapTip(tr("Load a meta file and mux from it as it stands. The text appears in the Meta "
                   "file box below, and that box is what is handed to the muxer, so anything in "
                   "the file is used exactly as written. The track list is not filled in from it "
                   "and will not match: tick Build the meta file from the settings to go back to "
                   "building the meta from the list.")));
    ui->removeAllTracksBtn->setToolTip(
        wrapTip(tr("Empty the list: every input file and every track goes, and the output name "
                   "and the track settings go back to what they are for a fresh start. Removing "
                   "them one at a time does the same thing, only slower, because the meta file "
                   "is rebuilt after each one.")));

    ui->DiskLabel->setVisible(false);
    ui->DiskLabelEdit->setVisible(false);
    // The ISO label is written quoted in the meta as well: a double quote ended it there, and the options after it on
    // the MUXOPT line were lost. It cannot be typed, as in the Matroska names.
    ui->DiskLabelEdit->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[^\"]*")), ui->DiskLabelEdit));
    // The Matroska output's title and --keep-editions, shown for MKV output only. A double quote would end the quoted
    // title in the meta, so it cannot be typed.
    ui->mkvTitleEdit->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[^\"]*")), ui->mkvTitleEdit));
    connect(ui->mkvTitleEdit, &QLineEdit::textChanged, this, &TsMuxerWindow::updateMetaLines);
    connect(ui->mkvKeepEditionsCheck, &QCheckBox::checkStateChanged, this, &TsMuxerWindow::updateMetaLines);
    // The disc's title and pictures (BDMV/META/DL, #30), shown for a Blu-ray folder or image only. Not remembered: like
    // the disc label, the folder belongs to one job. A double quote would end the quoted folder in the meta.
    ui->comboDiscMeta->addItem(tr("From the source disc"));
    ui->comboDiscMeta->addItem(tr("From a folder"));
    ui->comboDiscMeta->addItem(tr("No title and no pictures"));
    ui->discMetaDirEdit->setPlaceholderText(tr("Folder with bdmt_eng.xml and its pictures"));
    ui->discMetaDirEdit->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[^\"]*")), ui->discMetaDirEdit));
    connect(ui->comboDiscMeta, &QComboBox::currentIndexChanged, this,
            [this]
            {
                updateDiscMetaOptions();
                updateMetaLines();
            });
    connect(ui->discMetaDirEdit, &QLineEdit::textChanged, this, &TsMuxerWindow::updateMetaLines);
    connect(ui->btnDiscMetaBrowse, &QAbstractButton::clicked, this,
            [this]
            {
                const QString current = ui->discMetaDirEdit->text().trimmed();
                const QString dir = QFileDialog::getExistingDirectory(
                    this, tr("Folder with the disc's title and pictures"), current.isEmpty() ? lastInputDir : current);
                if (!dir.isEmpty())
                    ui->discMetaDirEdit->setText(QDir::toNativeSeparators(dir));
            });
    updateMkvOutputOptions();
    updateDiscMetaOptions();
    // Nothing has been loaded yet, so nothing about Dolby Vision applies. The designer file hides
    // this row too, so it is never seen even for a frame; the call is here so the state comes from
    // the same rule that maintains it afterwards rather than from a designer default.
    updateDvProfileVisible();

    ui->label_Donate->installEventFilter(this);
    ui->trackLV->installEventFilter(this);
    ui->memoChapters->installEventFilter(this);  // checked when it is left

    ui->trackLV->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->trackLV, &QWidget::customContextMenuRequested, this, &TsMuxerWindow::onTrackContextMenu);

    trackLVItemSelectionChanged();

    ui->trackSplitter->setStretchFactor(0, 10);
    ui->trackSplitter->setStretchFactor(1, 100);

    // ---- "BDMV folder -> ISO" tab (menu-preserving full-disc copy + dual-layer guard band) ----
    {
        auto* bdmvTab = new QWidget();
        auto* g = new QGridLayout(bdmvTab);
        auto* info = new QLabel(
            tr("Wrap an existing BDMV disc folder into a burnable BD-ROM ISO, byte-for-byte, so BD-J menus and "
               "every stream are kept intact (no re-mux). Works with any unprotected BDMV, whether you "
               "authored it yourself or it is an already-readable disc copy. On a multi-layer disc (dual-layer "
               "BD-R DL, or triple and quad-layer BD-R XL) the layer-break guard fills the defect-prone sectors "
               "at each layer transition with zeros, so the movie plays across the break without a pause. Point it at "
               "the folder that contains BDMV/ (and CERTIFICATE/ if present)."),
            bdmvTab);
        info->setWordWrap(true);
        auto* folderEdit = new QLineEdit(bdmvTab);
        folderEdit->setObjectName("isoSourceFolder");  // a name for UI Automation, not remembered
        auto* folderBtn = new QPushButton(tr("Browse..."), bdmvTab);
        // The source may also be a disc image, which tsMuxeR reads in place, without mounting it: an image had to be
        // mounted first, which Windows 7 cannot do without extra software
        auto* imageBtn = new QPushButton(bdmvTab);
        imageBtn->setObjectName("isoSourceImage");
        const auto imageBtnTexts = [this, imageBtn]()
        {
            imageBtn->setText(tr("Image..."));
            imageBtn->setToolTip(
                wrapTip(tr("Choose a Blu-ray disc image (.iso) as the source instead of a folder. "
                           "It is read in place, without mounting it.")));
        };
        imageBtnTexts();
        m_retranslateHooks.push_back(imageBtnTexts);
        const auto isImage = [](const QString& path)
        { return path.endsWith(QLatin1String(".iso"), Qt::CaseInsensitive) && QFileInfo(path).isFile(); };
        // Icon-only refresh: re-reads the selected folder (size, status, fit) after its contents
        // changed on disk, since re-picking the same path fires no change signal.
        auto* folderRefreshBtn = new QPushButton(bdmvTab);
        folderRefreshBtn->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
        folderRefreshBtn->setFixedWidth(folderRefreshBtn->sizeHint().height() + 8);
        auto* isoEdit = new QLineEdit(bdmvTab);
        isoEdit->setObjectName("isoOutputFile");  // a name for UI Automation, not remembered
        auto* isoBtn = new QPushButton(tr("Browse..."), bdmvTab);
        auto* folderStatusLabel = new QLabel(bdmvTab);
        folderStatusLabel->setWordWrap(true);
        auto* guardSpin = new QSpinBox(bdmvTab);
        guardSpin->setObjectName("isoGuard");  // a remembered choice
        guardSpin->setRange(0, 9999);
        // 288 MB default: reported real-world defect zones cluster around 35/64/258 MB, and on
        // defect-managed discs with asymmetric spare areas the real transition sits up to 128 MB
        // after the calculated break; 288 covers all of these with margin. The rare 1.5 GB class defects that
        // have also been reported need a manually raised value (the field goes to 9999).
        guardSpin->setValue(288);
        guardSpin->setSuffix(tr(" MB"));
        guardSpin->setToolTip(
            wrapTip(tr("The zeros fill whole 2048-byte sectors and snap to the movie's file boundaries, so "
                       "the burned zone matches this value within about 1 MB (never meaningfully short).")));
        auto* guardHintLabel = new QLabel(bdmvTab);
        guardHintLabel->setWordWrap(true);
        // Two-zone guard (advanced): also pad BEFORE the break, for media defective on both sides of the
        // transition rather than only the start of the next layer.
        auto* beforeCheck = new QCheckBox(tr("Also fill before the break (advanced)"), bdmvTab);
        beforeCheck->setObjectName("isoGuardBeforeCheck");  // a remembered choice
        beforeCheck->setToolTip(wrapTip(
            tr("Most discs only fail at the start of the next layer, so the default puts the fill there. Some "
               "media are also weak just before the break, so you can set the before and after zones independently.")));
        auto* guardBeforeSpin = new QSpinBox(bdmvTab);
        guardBeforeSpin->setObjectName("isoGuardBefore");  // a remembered choice
        guardBeforeSpin->setRange(0, 9999);
        // Starts at, and follows, the engine's proportional default (after/16, floor 4 MB) until the
        // user edits it themselves; from then on their explicit value wins. One-way: the after field
        // never follows this one.
        guardBeforeSpin->setValue(16);
        guardBeforeSpin->setSuffix(tr(" MB"));
        QSharedPointer<bool> beforeOverridden = QSharedPointer<bool>::create(false);
        QSharedPointer<bool> beforeSyncing = QSharedPointer<bool>::create(false);
        auto* guardBeforeLabel = new QLabel(tr("Guard before break:"), bdmvTab);
        auto* guardBeforeHint = new QLabel(
            tr("The default is asymmetric (most defects sit at the start of the next layer). Turn this on only for "
               "media that also fail just before the break."),
            bdmvTab);
        guardBeforeHint->setWordWrap(true);
        guardBeforeHint->setStyleSheet("color:#7f8c8d;");
        guardBeforeLabel->setVisible(false);
        guardBeforeSpin->setVisible(false);
        guardBeforeHint->setVisible(false);
        // Fit estimate label (folder size + guard band + UDF overhead vs the disc's Free Sectors capacity).
        auto* fitLabel = new QLabel(bdmvTab);
        fitLabel->setWordWrap(true);
        // --- Layer-break calculator: disc type + ImgBurn "Free Sectors" -> break(s), auto-filled ---
        auto* discTypeCombo = new QComboBox(bdmvTab);
        discTypeCombo->setObjectName("isoDiscType");  // a remembered choice
        // Item data: UserRole = how many layers to lay the data across (drives the break calculation);
        // UserRole+1 = the Free Sectors pre-filled into the (locked) field below; UserRole+2 = 1 for BD-R XL
        // media, which decides the player-compatibility warning and cannot be read off the layer count once
        // a two-layer XL entry exists; UserRole+3 = the capacity band the value belongs to, used to spot a
        // Free Sectors figure that does not match the disc picked.
        //
        // The number is written into the label so it can be matched against ImgBurn at a glance, because the
        // same disc size exists at two different capacities and picking the wrong one puts the guard in the
        // wrong place. A BD-R XL reports its full capacity, while a defect-managed disc keeps 1 GiB per layer
        // back as ISA/OSA spare and reports 47,305,728 (100 GB) or 60,403,712 (128 GB) instead. Both have been
        // measured here on physical discs, so neither can be assumed: the field is unlockable for exactly that.
        // BD-RE reserves spare area as well, which is why BD-R DL and BD-RE DL are separate entries.
        //
        // The two-layer 100 GB entry is for a 66 GB burn on a 100 GB disc, which exists because there is no
        // 66 GB BD-R and because many players handle two layers better than three. It is two thirds of the
        // disc, so the one break it needs falls on the disc's real first layer boundary, and the third layer
        // is never written.
        {
            int discCount = 0;
            const DiscEntry* discs = discTable(discCount);
            for (int i = 0; i < discCount; ++i)
            {
                discTypeCombo->addItem(tr(discs[i].label), discs[i].layers);
                discTypeCombo->setItemData(i, discs[i].freeSectors, Qt::UserRole + 1);
                discTypeCombo->setItemData(i, discs[i].isXL ? 1 : 0, Qt::UserRole + 2);
                discTypeCombo->setItemData(i, QString::fromLatin1(discs[i].band), Qt::UserRole + 3);
            }
        }
        discTypeCombo->setToolTip(
            wrapTip(tr("Picking a disc pre-fills the FULL Free Sectors for that size. ALWAYS check it against what "
                       "ImgBurn shows for your own disc. A disc formatted with defect management reports less, and "
                       "how much less depends on how it was formatted: one blank 25 GB disc offered three different "
                       "spare area sizes, so no single reduced figure can be listed here. If yours differs, tick "
                       "\"Enter Free Sectors manually\" and type what ImgBurn shows: the layer break is worked out "
                       "from that number, so a wrong one puts the guard in the wrong place.")));
        auto* freeSectorsEdit = new QLineEdit(bdmvTab);
        freeSectorsEdit->setObjectName("isoFreeSectors");  // a remembered choice
        freeSectorsEdit->setPlaceholderText(tr("ImgBurn -> Free Sectors (e.g. 47,305,728)"));
        // Accept the number exactly as ImgBurn prints it: grouped with commas, dots or spaces (locale-agnostic).
        freeSectorsEdit->setValidator(
            new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9 .,]*")), freeSectorsEdit));
        // The value is pre-filled from the disc type and LOCKED, so the correct number cannot be changed by
        // accident. The checkbox below makes it editable for a non-standard disc (a reformatted BD-RE, or a
        // no-defect-management BDXL burn); un-ticking re-locks and restores the safe default.
        freeSectorsEdit->setReadOnly(true);
        auto* manualCheck = new QCheckBox(tr("Enter Free Sectors manually (advanced)"), bdmvTab);
        manualCheck->setObjectName("isoFreeSectorsManual");  // a remembered choice
        manualCheck->setToolTip(
            wrapTip(tr("The Free Sectors above are pre-filled for the selected disc and locked to "
                       "prevent accidental changes. Tick this only if ImgBurn shows a different Free "
                       "Sectors for your exact disc, then type that number. Un-tick to restore the "
                       "standard value.")));
        auto* orderCheck = new QCheckBox(tr("Keep original file order (seamless branching)"), bdmvTab);
        orderCheck->setObjectName("isoKeepOrder");  // a remembered choice
        orderCheck->setToolTip(
            wrapTip(tr("Files are normally written largest first, so the main movie sits on the layer break and gets "
                       "the guard. Discs that use seamless branching play many separate segments in sequence; keeping "
                       "the original order stores them close to their playback order, which reads more smoothly.")));
        auto* keepExtrasCheck = new QCheckBox(tr("Include all files from the folder (not just BDMV)"), bdmvTab);
        keepExtrasCheck->setObjectName("isoKeepExtras");  // a remembered choice
        keepExtrasCheck->setToolTip(
            wrapTip(tr("By default the BDMV, CERTIFICATE and AACS disc folders are written, and the full standard "
                       "Blu-ray folder structure is completed automatically (the empty AUXDATA, BDJO, JAR and META "
                       "folders, CERTIFICATE, and a populated BACKUP) so the image matches a real BD/UHD disc; "
                       "anything else next to BDMV (readme files, cover art, extra folders) is skipped. Tick this to "
                       "add those extras to the image too.")));
        auto* innerOnlyCheck = new QCheckBox(tr("Keep data on the inner disc area (pad the outer edge)"), bdmvTab);
        innerOnlyCheck->setObjectName("isoInnerOnly");  // a remembered choice
        innerOnlyCheck->setToolTip(wrapTip(
            tr("The outer edge of a disc is the most error-prone part to burn. With this ticked, the movie is "
               "packed toward the inner tracks of every layer and the outer/rim region is filled with zeros, so "
               "the data stays off the weak outer edge. On a two- or four-layer disc the image is padded to the full "
               "disc, so the burn writes the whole disc; a three-layer image ends inside its last layer, and a "
               "single-layer disc is not padded. The layer-break guard is sized automatically; needs the disc Free "
               "Sectors set.")));
        auto* noGuardCheck = new QCheckBox(tr("Leave out the layer-break guard"), bdmvTab);
        noGuardCheck->setObjectName("isoNoGuard");  // a remembered choice
        noGuardCheck->setToolTip(wrapTip(
            tr("An image whose data crosses a layer break gets the layer-break guard set above, as a burnt dual-layer "
               "disc often reads badly at the layer change and a player then stalls there. Tick this to leave it out: "
               "the image is then written without zeros at the break.")));
        auto* singleCopy3dCheck = new QCheckBox(tr("3D disc: store the video once, not twice"), bdmvTab);
        singleCopy3dCheck->setToolTip(wrapTip(
            tr("A 3D disc stores its video once and gives it three names: the .ssif and the two .m2ts are the "
               "same sectors seen three ways. Copying all three by name writes the video twice and "
               "roughly doubles the image, which is why a 3D disc taken from a BD50 will not fit back "
               "onto one. It is ticked by itself when the folder holds a 3D disc: the video is then stored once, the "
               "way the source disc holds it, and a missing .ssif is rebuilt from the two views. Unticked, the "
               "folder is copied as it is, both views and any .ssif files, and the disc stays 3D.")));
        singleCopy3dCheck->setEnabled(false);  // until a 3D folder is chosen (see updateFolderStatus)
        auto* discLabelLabel = new QLabel(tr("Disc label (optional):"), bdmvTab);
        auto* labelEdit = new QLineEdit(bdmvTab);
        labelEdit->setToolTip(wrapTip(tr("Volume label written into the ISO. Leave empty to keep the default.")));
        // The disc's title and pictures (BDMV/META/DL, #30): the folder's own are kept, or a folder of one's own or
        // none takes their place. Not remembered: like the label, it belongs to one image.
        auto* metaLabel = new QLabel(tr("Disc title and pictures:"), bdmvTab);
        auto* metaCombo = new QComboBox(bdmvTab);
        metaCombo->setObjectName("isoMetaChoice");  // a name for UI Automation, not remembered
        metaCombo->addItems({tr("Keep the folder's own"), tr("From a folder"), tr("No title and no pictures")});
        metaCombo->setToolTip(wrapTip(
            tr("The disc's title and the pictures a player such as the PS3 shows (BDMV/META/DL): kept as the folder "
               "has them, or taken from a folder of your own, with bdmt_eng.xml and the like and the pictures "
               "they name. Or no title and no pictures.")));
        auto* metaEdit = new QLineEdit(bdmvTab);
        metaEdit->setObjectName("isoMetaFolder");
        metaEdit->setPlaceholderText(tr("Folder with bdmt_eng.xml and its pictures"));
        auto* metaBtn = new QPushButton(tr("Browse..."), bdmvTab);
        const auto updateMetaRow = [metaCombo, metaEdit, metaBtn]
        {
            metaEdit->setVisible(metaCombo->currentIndex() == 1);
            metaBtn->setVisible(metaCombo->currentIndex() == 1);
        };
        connect(metaCombo, &QComboBox::currentIndexChanged, this, updateMetaRow);
        updateMetaRow();
        connect(metaBtn, &QPushButton::clicked, this,
                [this, metaEdit]
                {
                    const QString current = metaEdit->text().trimmed();
                    const QString dir =
                        QFileDialog::getExistingDirectory(this, tr("Folder with the disc's title and pictures"),
                                                          current.isEmpty() ? lastInputDir : current);
                    if (!dir.isEmpty())
                        metaEdit->setText(QDir::toNativeSeparators(dir));
                });
        auto* helpBtn = new QPushButton(tr("Where do I find this?"), bdmvTab);
        auto* breaksLabel = new QLabel(bdmvTab);
        breaksLabel->setWordWrap(true);
        auto* compatLabel = new QLabel(bdmvTab);
        compatLabel->setWordWrap(true);
        compatLabel->setStyleSheet("color:#c0392b; font-weight:bold;");  // red: BD-R XL at-your-own-risk caveat
        auto* divisLabel = new QLabel(bdmvTab);
        divisLabel->setWordWrap(true);
        divisLabel->setStyleSheet("color:#c0392b; font-weight:bold;");  // red: Free Sectors input looks wrong
        auto* buildBtn = new QPushButton(tr("Build ISO"), bdmvTab);
        // Static row labels kept as pointers so they can be re-translated on a runtime language change.
        auto* folderLabel = new QLabel(tr("BDMV folder:"), bdmvTab);
        auto* outputLabel = new QLabel(tr("Output ISO:"), bdmvTab);
        auto* guardLabel = new QLabel(tr("Layer-break guard (after break):"), bdmvTab);
        auto* discTypeLabel = new QLabel(tr("Disc type:"), bdmvTab);
        auto* freeSectorsLabel = new QLabel(tr("Free Sectors (ImgBurn):"), bdmvTab);

        // Read Free Sectors as a plain number no matter how it was pasted (47,305,728 / 47.305.728 / 47 305 728).
        auto readFreeSectors = [freeSectorsEdit]() -> qint64
        {
            QString digits = freeSectorsEdit->text();
            digits.remove(QRegularExpression(QStringLiteral("[^0-9]")));
            return digits.toLongLong();
        };
        // Compute the break sector(s) from disc type + Free Sectors (empty = nothing entered yet).
        auto breaksList = [discTypeCombo, readFreeSectors]() -> QStringList
        {
            QStringList parts;
            for (const qint64 b : discBreaks(readFreeSectors(), discTypeCombo->currentData().toInt()))
                parts << QString::number(b);
            return parts;
        };
        auto refresh = [breaksList, readFreeSectors, discTypeCombo, breaksLabel, compatLabel, divisLabel]()
        {
            const int layers = discTypeCombo->currentData().toInt();
            const qint64 total = readFreeSectors();
            const QStringList parts = breaksList();
            if (parts.isEmpty())
                breaksLabel->setText(
                    tr("Enter the disc's \"Free Sectors\" (from ImgBurn) to calculate the layer break(s) for the "
                       "exact disc you are burning."));
            else
            {
                QStringList pretty;
                for (const QString& p : parts) pretty << QLocale().toString(p.toLongLong());
                breaksLabel->setText(tr("Calculated layer break(s): %1").arg(pretty.join("   |   ")));
            }
            // Sanity checks on the entered Free Sectors. Standard BD user-data capacities (in 2048-byte sectors)
            // sit in well-separated bands, so a value far from the selected disc type almost always means the
            // wrong disc type was picked or the wrong line was copied.
            QStringList warns;
            if (total > 0)
            {
                QString looksLike;
                if (total >= 10000000 && total < 20000000)
                    looksLike = QStringLiteral("25 GB");
                else if (total >= 20000000 && total <= 28000000)
                    looksLike = QStringLiteral("50 GB");
                else if (total >= 28000000 && total < 42000000)
                    looksLike = QStringLiteral("66 GB");
                else if (total >= 42000000 && total <= 52000000)
                    looksLike = QStringLiteral("100 GB");
                else if (total >= 56000000 && total <= 68000000)
                    looksLike = QStringLiteral("128 GB");
                // Taken from the entry itself rather than from the layer count, which no longer identifies a
                // disc now that a 100 GB disc can be used across two layers.
                const QString selected = discTypeCombo->currentData(Qt::UserRole + 3).toString();
                if (looksLike.isEmpty())
                    warns << tr("%1 sectors does not match any standard BD disc size. Double-check the Free "
                                "Sectors value against ImgBurn.")
                                 .arg(QLocale().toString(total));
                else if (looksLike != selected)
                    warns << tr("%1 sectors looks like a %2 disc, but %3 is selected. Check the disc type above, "
                                "or the Free Sectors value.")
                                 .arg(QLocale().toString(total), looksLike, selected);
                // A correctly formatted N-layer disc's Free Sectors divides evenly by N; if it does not, the
                // value was most likely mistyped or the wrong ImgBurn line was copied (for example the "EL:" line).
                if (total % layers != 0)
                    warns << tr("%1 does not divide evenly across %2 layers. A correctly formatted %2-layer disc "
                                "normally does, so check for a mistyped or wrong Free Sectors line. The break is "
                                "still placed safely just under the boundary and the guard band absorbs the rounding.")
                                 .arg(QLocale().toString(total))
                                 .arg(layers);
            }
            divisLabel->setText(warns.join("\n\n"));
            // The media decides this, not the layer count: a 100 GB disc used across two layers is still a
            // BD-R XL and still needs a player that can read one.
            compatLabel->setText(
                discTypeCombo->currentData(Qt::UserRole + 2).toInt() == 1
                    ? (layers >= 3
                           ? tr("At your own risk: many Blu-ray players cannot read 100/128 GB BD-R XL discs at all, "
                                "and there is no guarantee yours will. Keeping the image around 66 GB (the first two "
                                "layers, selectable above) and finalizing the disc improves the odds on some players, "
                                "but even 66 GB is not guaranteed to play. The full 100/128 GB needs a recent player "
                                "that explicitly supports high-capacity BD-R XL media. Always test on your own "
                                "device.")
                           : tr("At your own risk: this is still BD-R XL media, which many Blu-ray players cannot "
                                "read at all. Using only the first two layers and finalizing the disc improves the "
                                "odds on some players, but nothing guarantees it. Always test on your own device."))
                    : QString());
        };
        // Pre-fill the standard Free Sectors for the selected disc so the user does not have to run ImgBurn for a
        // disc whose capacity never changes. Still editable (a reformatted BD-RE, or an odd disc, can differ).
        auto autoFillFreeSectors = [discTypeCombo, freeSectorsEdit]()
        {
            const qint64 fs = discTypeCombo->currentData(Qt::UserRole + 1).toLongLong();
            if (fs > 0)
                freeSectorsEdit->setText(QLocale().toString(fs));
        };
        // Colour-coded guidance for the guard size. The layer defect measured on real hardware was about 35 MB,
        // but larger zones and the defect-managed break shift make 256 MB the safe recommendation. Lower is
        // allowed (0 = align only) but the risk is made visible.
        auto updateGuard = [guardSpin, guardHintLabel]()
        {
            const int mb = guardSpin->value();
            QString colour, text;
            if (mb == 0)
            {
                colour = QStringLiteral("#7f8c8d");
                text = tr("Alignment only, no defect protection.");
            }
            else if (mb < 35)
            {
                colour = QStringLiteral("#c0392b");
                text =
                    tr("Below the ~35 MB layer defect measured on real hardware. Video may land on bad "
                       "sectors. 288 MB recommended.");
            }
            else if (mb < 288)
            {
                colour = QStringLiteral("#b26a00");
                text =
                    tr("Covers the typical ~35 MB defect, but larger bad zones are common on real media, and "
                       "on a defect-managed disc the true layer switch can sit up to 128 MB after the "
                       "calculated break. 288 MB recommended.");
            }
            else
            {
                colour = QStringLiteral("#2e7d32");
                text =
                    tr("Recommended. Covers all commonly reported defect zones (35 to 258 MB) and the shifted "
                       "layer switch of defect-managed discs. For the rare defects over 1 GB, raise it; the "
                       "field goes up to 9999.");
            }
            guardHintLabel->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;").arg(colour));
            guardHintLabel->setText(text);
        };
        // Fit calculator: cache the folder byte size (recomputed only when the folder changes) and combine it
        // with the guard band and the Free Sectors capacity into a live fits / does-not-fit estimate.
        QSharedPointer<qint64> folderBytes = QSharedPointer<qint64>::create(-1);
        auto recomputeFolderSize = [folderEdit, folderBytes, isImage]()
        {
            const QString folder = folderEdit->text().trimmed();
            if (isImage(folder))  // an image: what it holds is about its own size
            {
                *folderBytes = QFileInfo(folder).size();
                return;
            }
            // Only measure a folder that actually holds a BDMV. This also prevents walking huge
            // unrelated trees (a parent folder full of movies, or a bare drive root while a path is
            // being typed), which froze the GUI and left a misleading size on screen.
            if (folder.isEmpty() || !QDir(folder).exists() ||
                !QFileInfo::exists(pathNoCase(folder, QStringLiteral("BDMV"))))
            {
                *folderBytes = -1;
                return;
            }
            // On read-only media (a mounted ISO or optical disc) the folder is the whole disc, so use the
            // volume's used space instead of walking every file, which is slow on an optical drive.
            const QStorageInfo si(folder);
            if (si.isValid() && si.isReadOnly())
            {
                *folderBytes = si.bytesTotal() - si.bytesAvailable();
                return;
            }
            // Match the CLI: only the disc-structure folders count. Anything else at the root (a
            // previously built ISO, helper folders, stray files) is not written to the image, so it
            // must not count toward the estimate either; a leftover output ISO inside the source
            // folder used to inflate the estimate into a false "does not fit".
            qint64 sum = 0;
            for (const auto& sub : {QStringLiteral("BDMV"), QStringLiteral("CERTIFICATE"), QStringLiteral("AACS")})
            {
                const QString subPath = folder + QLatin1Char('/') + sub;
                if (!QDir(subPath).exists())
                    continue;
                QDirIterator it(subPath, QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext())
                {
                    it.next();
                    sum += it.fileInfo().size();
                }
            }
            *folderBytes = sum;
        };
        // Immediate feedback on the selected folder: whether it holds a BDMV, and whether it is a read-only disc
        // (in which case the output ISO must go to a writable location, not the disc).
        //
        // A 3D folder also brings the 3D box to life and ticks it, once for each folder chosen: the video is then
        // stored once, as on the disc, and a .ssif missing from the folder is rebuilt from the two views. A folder
        // copied with its SSIF folder would otherwise put the 3D video into the image twice, and one copied
        // without it would give an image that plays in 2D. Taken off, the image is built as found. For any other
        // folder the box is off and greyed.
        auto lastTickedFolder = QSharedPointer<QString>::create();
        auto updateFolderStatus = [folderEdit, folderStatusLabel, singleCopy3dCheck, lastTickedFolder, isImage]()
        {
            const QString folder = folderEdit->text().trimmed();
            // An image is not looked into here: its 3D clips are not counted, and it is never on a disc being written
            if (isImage(folder))
            {
                singleCopy3dCheck->setChecked(false);
                singleCopy3dCheck->setEnabled(false);
                lastTickedFolder->clear();
                folderStatusLabel->setStyleSheet(QStringLiteral("color:#2e7d32; font-weight:bold;"));
                folderStatusLabel->setText(tr("Disc image found. It is read in place, without mounting it."));
                return;
            }
            const Bdmv3dScan scan =
                !folder.isEmpty() && QFileInfo::exists(pathNoCase(folder, "BDMV")) ? scanBdmv3d(folder) : Bdmv3dScan();
            singleCopy3dCheck->setEnabled(scan.clips > 0);
            if (scan.clips == 0)
            {
                singleCopy3dCheck->setChecked(false);
                lastTickedFolder->clear();
            }
            else if (*lastTickedFolder != folder)
            {
                singleCopy3dCheck->setChecked(true);
                *lastTickedFolder = folder;
            }
            QString info3d;
            if (scan.clips > 0 && scan.missing == 0)
                info3d = QStringLiteral(" ") + tr("3D disc detected: with the 3D box ticked, its 3D video is stored "
                                                  "once, as on the disc.");
            else if (scan.clips > 0)
                info3d = QStringLiteral(" ") + tr("3D disc detected: %1 of %2 .ssif files are missing. With the 3D "
                                                  "box ticked they are rebuilt from the two views; without it those "
                                                  "titles play in 2D.")
                                                   .arg(scan.missing)
                                                   .arg(scan.clips);
            if (folder.isEmpty())
            {
                folderStatusLabel->setText(QString());
                return;
            }
            if (!QFileInfo::exists(pathNoCase(folder, "BDMV")))
            {
                folderStatusLabel->setStyleSheet(QStringLiteral("color:#b26a00; font-weight:bold;"));
                folderStatusLabel->setText(
                    tr("No BDMV subfolder here. Select the folder that contains BDMV (a disc's root)."));
            }
            else if (QStorageInfo(folder).isReadOnly())
            {
                // One colour cannot do this. Reaching 4.5 to 1 on both grounds needs a luminance
                // of at most 0.154 against the light window and at least 0.240 against the dark
                // one, and those do not overlap, so a single value can only be a compromise that
                // fails on both. The first attempt here was exactly that: #1978e4 reads 3.80 on
                // the light window and 3.27 on the dark one, and it cost the light window the
                // 4.5 the original #1565c0 already had.
                //
                // So the ground decides. There is no single "the light ground": the window paints
                // about #fbfbfb, the Qt palette reports #f3f3f3, and the first attempt assumed
                // #f0f0f0; the dark side likewise ranges over #1e1e1e to #2d2d2d. Rather than
                // quote one of them, here is the WORST case for each colour across all six, so
                // the figures hold whichever ground is really underneath:
                //
                //   #1565c0 on a light ground   5.04 at worst   (5.55 on what is painted)
                //   #4da3ff on a dark ground    5.25 at worst   (6.35 on the palette value)
                //
                // Both clear 4.5 on every one of those grounds. The colour NOT chosen for a
                // ground would read between 2.30 and 2.90 there, which is why one value cannot
                // serve both.
                folderStatusLabel->setStyleSheet(
                    QStringLiteral("color:%1; font-weight:bold;")
                        .arg(themedInk(folderStatusLabel, QLatin1String("#1565c0"), QLatin1String("#4da3ff"))));
                folderStatusLabel->setText(tr("BDMV disc detected (read-only). The output ISO cannot be written to the "
                                              "disc, so choose a writable output location.") +
                                           info3d);
            }
            else
            {
                folderStatusLabel->setStyleSheet(QStringLiteral("color:#2e7d32; font-weight:bold;"));
                folderStatusLabel->setText(tr("BDMV folder found.") + info3d);
            }
        };
        auto updateFit = [folderBytes, readFreeSectors, discTypeCombo, guardSpin, beforeCheck, guardBeforeSpin,
                          fitLabel, innerOnlyCheck, noGuardCheck]()
        {
            const qint64 fb = *folderBytes;
            const qint64 freeSec = readFreeSectors();
            if (fb < 0 || freeSec <= 0)
            {
                fitLabel->setStyleSheet(QStringLiteral("color:#7f8c8d;"));
                fitLabel->setText(tr("Select a BDMV folder and enter Free Sectors to estimate whether it fits."));
                return;
            }
            const int layers = discTypeCombo->currentData().toInt();
            const int breaks = layers - 1;
            const int afterMB = guardSpin->value();
            // mirrors the engine default: before = after/16 (the 64:4 design ratio), at least 4 MB
            const int beforeMB =
                beforeCheck->isChecked() ? guardBeforeSpin->value() : qMin(afterMB, qMax(4, afterMB / 16));
            // Inner-only sizes its zeros from what is left on the disc, so they never make the image too big. The
            // guard set above is not used then, and counting it said "Does NOT fit" of a folder that fits.
            // Nor is it with the guard left out.
            const qint64 guardBytes = innerOnlyCheck->isChecked() || noGuardCheck->isChecked()
                                          ? 0
                                          : static_cast<qint64>(afterMB + beforeMB) * breaks * 1024 * 1024;
            const qint64 overhead = 16LL * 1024 * 1024;  // approximate UDF metadata
            const qint64 occupied = fb + guardBytes + overhead;
            const qint64 capacity = freeSec * 2048LL;
            const double usedGb = occupied / 1000000000.0;
            const double capGb = capacity / 1000000000.0;
            const double pct = capacity > 0 ? occupied * 100.0 / capacity : 0.0;
            if (occupied <= capacity)
            {
                fitLabel->setStyleSheet(QStringLiteral("color:#2e7d32; font-weight:bold;"));
                fitLabel->setText(tr("Estimated image %1 GB of %2 GB (%3%). Fits, %4 GB free.")
                                      .arg(usedGb, 0, 'f', 2)
                                      .arg(capGb, 0, 'f', 2)
                                      .arg(pct, 0, 'f', 1)
                                      .arg((capacity - occupied) / 1000000000.0, 0, 'f', 2));
            }
            else
            {
                fitLabel->setStyleSheet(QStringLiteral("color:#c0392b; font-weight:bold;"));
                fitLabel->setText(tr("Estimated image %1 GB of %2 GB (%3%). Does NOT fit, over by %4 GB.")
                                      .arg(usedGb, 0, 'f', 2)
                                      .arg(capGb, 0, 'f', 2)
                                      .arg(pct, 0, 'f', 1)
                                      .arg((occupied - capacity) / 1000000000.0, 0, 'f', 2));
            }
        };
        int r = 0;
        g->addWidget(info, r++, 0, 1, 3);
        g->addWidget(folderLabel, r, 0);
        g->addWidget(folderEdit, r, 1);
        auto* folderBtns = new QHBoxLayout();
        folderBtns->setContentsMargins(0, 0, 0, 0);
        folderBtns->addWidget(folderBtn);
        folderBtns->addWidget(imageBtn);
        g->addLayout(folderBtns, r, 2);
        g->addWidget(folderRefreshBtn, r++, 3);
        g->addWidget(folderStatusLabel, r++, 0, 1, 4);
        g->addWidget(outputLabel, r, 0);
        g->addWidget(isoEdit, r, 1);
        g->addWidget(isoBtn, r++, 2);
        g->addWidget(guardLabel, r, 0);
        g->addWidget(guardSpin, r++, 1);
        g->addWidget(guardHintLabel, r++, 0, 1, 3);
        g->addWidget(beforeCheck, r++, 0, 1, 3);
        g->addWidget(guardBeforeLabel, r, 0);
        g->addWidget(guardBeforeSpin, r++, 1);
        g->addWidget(guardBeforeHint, r++, 0, 1, 3);
        g->addWidget(discTypeLabel, r, 0);
        g->addWidget(discTypeCombo, r, 1);
        g->addWidget(helpBtn, r++, 2);
        g->addWidget(freeSectorsLabel, r, 0);
        g->addWidget(freeSectorsEdit, r++, 1);
        g->addWidget(manualCheck, r++, 1, 1, 2);
        g->addWidget(breaksLabel, r++, 0, 1, 3);
        g->addWidget(divisLabel, r++, 0, 1, 3);
        g->addWidget(compatLabel, r++, 0, 1, 3);
        g->addWidget(fitLabel, r++, 0, 1, 3);
        g->addWidget(discLabelLabel, r, 0);
        g->addWidget(labelEdit, r++, 1, 1, 2);
        g->addWidget(metaLabel, r, 0);
        g->addWidget(metaCombo, r++, 1, 1, 2);
        g->addWidget(metaEdit, r, 1);
        g->addWidget(metaBtn, r++, 2);
        g->addWidget(keepExtrasCheck, r++, 0, 1, 3);
        g->addWidget(innerOnlyCheck, r++, 0, 1, 3);
        g->addWidget(noGuardCheck, r++, 0, 1, 3);
        g->addWidget(singleCopy3dCheck, r++, 0, 1, 3);
        g->addWidget(orderCheck, r++, 0, 1, 3);
        g->addWidget(buildBtn, r++, 1);
        g->setRowStretch(r, 1);
        ui->tabWidget->addTab(bdmvTab, tr("BDMV folder -> ISO"));

        connect(discTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), bdmvTab,
                [autoFillFreeSectors, refresh, updateFit](int)
                {
                    autoFillFreeSectors();  // pre-fill the picked disc's standard Free Sectors (editable)
                    refresh();
                    updateFit();
                });
        connect(freeSectorsEdit, &QLineEdit::textChanged, bdmvTab,
                [refresh, updateFit](const QString&)
                {
                    refresh();
                    updateFit();
                });
        connect(manualCheck, &QCheckBox::toggled, bdmvTab,
                [freeSectorsEdit, autoFillFreeSectors](bool on)
                {
                    freeSectorsEdit->setReadOnly(!on);
                    if (!on)
                        autoFillFreeSectors();  // re-locking restores the safe pre-filled value
                });
        // The before field mirrors the proportional default (after/16, floor 4 MB) until the user
        // touches it; a programmatic update is marked with beforeSyncing so it does not count as a
        // user edit.
        auto syncBeforeSpin = [guardSpin, guardBeforeSpin, beforeOverridden, beforeSyncing]()
        {
            if (*beforeOverridden)
                return;
            const int after = guardSpin->value();
            *beforeSyncing = true;
            guardBeforeSpin->setValue(qMin(after, qMax(4, after / 16)));
            *beforeSyncing = false;
        };
        connect(guardSpin, QOverload<int>::of(&QSpinBox::valueChanged), bdmvTab,
                [syncBeforeSpin, updateGuard, updateFit](int)
                {
                    syncBeforeSpin();
                    updateGuard();
                    updateFit();
                });
        connect(guardBeforeSpin, QOverload<int>::of(&QSpinBox::valueChanged), bdmvTab,
                [beforeOverridden, beforeSyncing, updateFit](int)
                {
                    if (!*beforeSyncing)
                        *beforeOverridden = true;  // the user set an explicit value; stop following
                    updateFit();
                });
        connect(folderEdit, &QLineEdit::textChanged, bdmvTab,
                [updateFolderStatus, recomputeFolderSize, updateFit](const QString&)
                {
                    updateFolderStatus();
                    recomputeFolderSize();
                    updateFit();
                });
        connect(
            beforeCheck, &QCheckBox::toggled, bdmvTab,
            [guardBeforeLabel, guardBeforeSpin, guardBeforeHint, beforeOverridden, syncBeforeSpin, updateFit](bool on)
            {
                guardBeforeLabel->setVisible(on);
                guardBeforeSpin->setVisible(on);
                guardBeforeHint->setVisible(on);
                if (on)
                    syncBeforeSpin();  // opening the advanced field shows the current proportional value
                else
                    *beforeOverridden = false;  // closing it returns to the following default
                updateFit();
            });
        connect(innerOnlyCheck, &QCheckBox::toggled, bdmvTab,
                [guardSpin, guardLabel, guardHintLabel, beforeCheck, guardBeforeLabel, guardBeforeSpin, guardBeforeHint,
                 updateFit, noGuardCheck](bool on)
                {
                    on = on || noGuardCheck->isChecked();  // the guard left out greys them as well
                    // inner-only takes over the layer break: it sizes the guard automatically from the disc
                    // type and the content, so the WHOLE manual guard mechanism (after-guard + the advanced
                    // before-guard) is greyed out while it is active. Disc type / Free Sectors stay active
                    // because inner-only reads them.
                    guardSpin->setEnabled(!on);
                    guardLabel->setEnabled(!on);
                    guardHintLabel->setEnabled(!on);
                    beforeCheck->setEnabled(!on);
                    guardBeforeLabel->setEnabled(!on);
                    guardBeforeSpin->setEnabled(!on);
                    guardBeforeHint->setEnabled(!on);
                    updateFit();
                });
        // The guard left out greys the guard boxes as inner-only does; the size set stays for when it is unticked
        connect(noGuardCheck, &QCheckBox::toggled, bdmvTab,
                [guardSpin, guardLabel, guardHintLabel, beforeCheck, guardBeforeLabel, guardBeforeSpin, guardBeforeHint,
                 updateFit, innerOnlyCheck](bool on)
                {
                    on = on || innerOnlyCheck->isChecked();
                    guardSpin->setEnabled(!on);
                    guardLabel->setEnabled(!on);
                    guardHintLabel->setEnabled(!on);
                    beforeCheck->setEnabled(!on);
                    guardBeforeLabel->setEnabled(!on);
                    guardBeforeSpin->setEnabled(!on);
                    guardBeforeHint->setEnabled(!on);
                    updateFit();
                });
        connect(helpBtn, &QPushButton::clicked, this,
                [this]
                {
                    QMessageBox::information(
                        this, tr("Finding the disc's \"Free Sectors\""),
                        tr("<b>You only need ONE number from the blank disc: its total capacity in sectors.</b>"
                           "<br><br><b>In ImgBurn:</b>"
                           "<ol>"
                           "<li>Insert the blank BD-R / BD-RE disc.</li>"
                           "<li>Choose <i>Write image file to disc</i>.</li>"
                           "<li>Under <i>Destination</i>, select your burner.</li>"
                           "<li>In the right-hand <i>Device / Disc Information</i> panel, read the "
                           "<b>Free Sectors</b> line (for example 47,305,728).</li>"
                           "<li>The <i>Number of Layers</i> line just below tells you 50 / 100 / 128 GB.</li>"
                           "</ol>"
                           "Pick the matching disc type here and paste <b>Free Sectors</b> into the box - the "
                           "layer break(s) are calculated for you:"
                           "<ul>"
                           "<li>BD50 (2 layers): Free / 2</li>"
                           "<li>BD100 (3 layers): Free / 3 and Free x 2 / 3</li>"
                           "<li>BD128 (4 layers): Free / 4, Free x 2 / 4, Free x 3 / 4</li>"
                           "</ul>"
                           "<b>Important:</b> use ImgBurn's <i>Free Sectors</i>, not the Windows capacity - "
                           "Windows can report a smaller number and put the break in the wrong place."));
                });
        autoFillFreeSectors();  // start with the default disc's standard Free Sectors already filled in
        refresh();
        updateGuard();
        recomputeFolderSize();
        updateFit();
        updateFolderStatus();

        // Re-translate everything created here on a runtime language change. Designer widgets are handled by
        // ui->retranslateUi(); these hand-built widgets are not, so register a hook the changeEvent will call.
        m_retranslateHooks.push_back(
            [this, bdmvTab, info, folderLabel, outputLabel, guardLabel, discTypeLabel, freeSectorsLabel, folderBtn,
             isoBtn, helpBtn, buildBtn, discTypeCombo, freeSectorsEdit, manualCheck, orderCheck, keepExtrasCheck,
             innerOnlyCheck, singleCopy3dCheck, discLabelLabel, labelEdit, guardSpin, refresh, updateGuard, beforeCheck,
             guardBeforeLabel, guardBeforeSpin, guardBeforeHint, fitLabel, updateFit, updateFolderStatus, noGuardCheck,
             metaLabel, metaCombo, metaEdit, metaBtn]()
            {
                ui->tabWidget->setTabText(ui->tabWidget->indexOf(bdmvTab), tr("BDMV folder -> ISO"));
                info->setText(
                    tr("Wrap an existing BDMV disc folder into a burnable BD-ROM ISO, byte-for-byte, so "
                       "BD-J menus and every stream are kept intact (no re-mux). Works with any "
                       "unprotected BDMV, whether you authored it yourself or it is an already-readable "
                       "disc copy. On a multi-layer disc (dual-layer BD-R DL, or triple and quad-layer "
                       "BD-R XL) the layer-break guard fills the defect-prone sectors at each layer "
                       "transition with zeros, so the movie plays across the break without a pause. Point it "
                       "at the folder that contains BDMV/ (and CERTIFICATE/ if present)."));
                folderLabel->setText(tr("BDMV folder:"));
                outputLabel->setText(tr("Output ISO:"));
                guardLabel->setText(tr("Layer-break guard (after break):"));
                discTypeLabel->setText(tr("Disc type:"));
                freeSectorsLabel->setText(tr("Free Sectors (ImgBurn):"));
                folderBtn->setText(tr("Browse..."));
                isoBtn->setText(tr("Browse..."));
                helpBtn->setText(tr("Where do I find this?"));
                buildBtn->setText(tr("Build ISO"));
                int discCount = 0;
                const DiscEntry* discs = discTable(discCount);
                for (int i = 0; i < discCount; ++i) discTypeCombo->setItemText(i, tr(discs[i].label));
                discTypeCombo->setToolTip(wrapTip(
                    tr("Picking a disc pre-fills the FULL Free Sectors for that size. ALWAYS check it against what "
                       "ImgBurn shows for your own disc. A disc formatted with defect management reports less, and "
                       "how much less depends on how it was formatted: one blank 25 GB disc offered three different "
                       "spare area sizes, so no single reduced figure can be listed here. If yours differs, tick "
                       "\"Enter Free Sectors manually\" and type what ImgBurn shows: the layer break is worked out "
                       "from that number, so a wrong one puts the guard in the wrong place.")));
                freeSectorsEdit->setPlaceholderText(tr("ImgBurn -> Free Sectors (e.g. 48,878,592)"));
                orderCheck->setText(tr("Keep original file order (seamless branching)"));
                orderCheck->setToolTip(wrapTip(
                    tr("Files are normally written largest first, so the main movie sits on the layer break and "
                       "gets the guard. Discs that use seamless branching play many separate segments in "
                       "sequence; keeping the original order stores them close to their playback order, which "
                       "reads more smoothly.")));
                keepExtrasCheck->setText(tr("Include all files from the folder (not just BDMV)"));
                keepExtrasCheck->setToolTip(wrapTip(
                    tr("By default the BDMV, CERTIFICATE and AACS disc folders are written, and the full standard "
                       "Blu-ray folder structure is completed automatically (the empty AUXDATA, BDJO, JAR and META "
                       "folders, CERTIFICATE, and a populated BACKUP) so the image matches a real BD/UHD disc; "
                       "anything else next to BDMV (readme files, cover art, extra folders) is skipped. Tick this to "
                       "add those extras to the image too.")));
                innerOnlyCheck->setText(tr("Keep data on the inner disc area (pad the outer edge)"));
                innerOnlyCheck->setToolTip(wrapTip(tr(
                    "The outer edge of a disc is the most error-prone part to burn. With this ticked, the movie is "
                    "packed toward the inner tracks of every layer and the outer/rim region is filled with zeros, so "
                    "the data stays off the weak outer edge. On a two- or four-layer disc the image is padded to the "
                    "full "
                    "disc, so the burn writes the whole disc; a three-layer image ends inside its last layer, and a "
                    "single-layer disc is not padded. The layer-break guard is sized automatically; needs the disc "
                    "Free Sectors set.")));
                noGuardCheck->setText(tr("Leave out the layer-break guard"));
                noGuardCheck->setToolTip(wrapTip(
                    tr("An image whose data crosses a layer break gets the layer-break guard set above, as a burnt "
                       "dual-layer disc often reads badly at the layer change and a player then stalls there. Tick "
                       "this to leave it out: the image is then written without zeros at the break.")));
                singleCopy3dCheck->setText(tr("3D disc: store the video once, not twice"));
                singleCopy3dCheck->setToolTip(wrapTip(
                    tr("A 3D disc stores its video once and gives it three names: the .ssif and the two .m2ts are the "
                       "same sectors seen three ways. Copying all three by name writes the video twice and "
                       "roughly doubles the image, which is why a 3D disc taken from a BD50 will not fit back "
                       "onto one. It is ticked by itself when the folder holds a 3D disc: the video is then stored "
                       "once, the way the source disc holds it, and a missing .ssif is rebuilt from the two views. "
                       "Unticked, the folder is copied as it is, both views and any .ssif files, and the disc "
                       "stays 3D.")));
                discLabelLabel->setText(tr("Disc label (optional):"));
                metaLabel->setText(tr("Disc title and pictures:"));
                metaCombo->setItemText(0, tr("Keep the folder's own"));
                metaCombo->setItemText(1, tr("From a folder"));
                metaCombo->setItemText(2, tr("No title and no pictures"));
                metaCombo->setToolTip(wrapTip(
                    tr("The disc's title and the pictures a player such as the PS3 shows (BDMV/META/DL): kept as the "
                       "folder has them, or taken from a folder of your own, with bdmt_eng.xml and the like and the "
                       "pictures they name. Or no title and no pictures.")));
                metaEdit->setPlaceholderText(tr("Folder with bdmt_eng.xml and its pictures"));
                metaBtn->setText(tr("Browse..."));
                labelEdit->setToolTip(
                    wrapTip(tr("Volume label written into the ISO. Leave empty to keep the default.")));
                manualCheck->setText(tr("Enter Free Sectors manually (advanced)"));
                manualCheck->setToolTip(
                    wrapTip(tr("The Free Sectors above are pre-filled for the selected disc and locked to "
                               "prevent accidental changes. Tick this only if ImgBurn shows a different "
                               "Free Sectors for your exact disc, then type that number. Un-tick to "
                               "restore the standard value.")));
                guardSpin->setSuffix(tr(" MB"));
                guardSpin->setToolTip(
                    wrapTip(tr("The zeros fill whole 2048-byte sectors and snap to the movie's file "
                               "boundaries, so the burned zone matches this value within about 1 MB (never "
                               "meaningfully short).")));
                beforeCheck->setText(tr("Also fill before the break (advanced)"));
                beforeCheck->setToolTip(wrapTip(
                    tr("Most discs only fail at the start of the next layer, so the default puts the fill there. Some "
                       "media are also weak just before the break, so you can set the before and after zones "
                       "independently.")));
                guardBeforeLabel->setText(tr("Guard before break:"));
                guardBeforeSpin->setSuffix(tr(" MB"));
                guardBeforeHint->setText(
                    tr("The default is asymmetric (most defects sit at the start of the next layer). Turn this on only "
                       "for media that also fail just before the break."));
                refresh();             // re-render the dynamic break / warning labels in the new language
                updateGuard();         // re-render the guard hint in the new language
                updateFit();           // re-render the fit estimate in the new language
                updateFolderStatus();  // re-render the folder status in the new language
            });

        connect(
            folderBtn, &QPushButton::clicked, this,
            [this, folderEdit, isoEdit, updateFolderStatus, recomputeFolderSize, updateFit]
            {
                QString d = QFileDialog::getExistingDirectory(this, tr("Select the BDMV disc folder"), lastInputDir);
                if (d.isEmpty())
                    return;
                // Common slip: picking the BDMV folder itself instead of the disc root above it.
                // Step up to the parent so the built ISO gets the proper BDMV/ root.
                if (QFileInfo(d).fileName().compare(QStringLiteral("BDMV"), Qt::CaseInsensitive) == 0)
                    d = QFileInfo(d).absolutePath();
                folderEdit->setText(QDir::toNativeSeparators(d));
                // Re-picking the same folder fires no textChanged, but its contents may have
                // changed (files removed to make the image fit), so refresh explicitly.
                updateFolderStatus();
                recomputeFolderSize();
                updateFit();
                lastInputDir = d;
                writeSettings();
                if (isoEdit->text().isEmpty())
                {
                    // A drive root (a mounted ISO) has no file name; fall back to the volume label, then "disc".
                    QString name = QFileInfo(d).fileName();
                    if (name.isEmpty())
                        name = QStorageInfo(d).displayName();
                    if (name.isEmpty())
                        name = QStringLiteral("disc");
                    // Never default the output ISO into the source folder itself: a leftover ISO
                    // there would count into the next fit estimate and even into a rebuild. Use
                    // the PARENT folder; for a drive root or read-only media fall back to a
                    // writable location (the user can still change it).
                    QString outDir = QFileInfo(d).absolutePath();
                    if (outDir == d || QStorageInfo(d).isReadOnly() || !QFileInfo(outDir).isWritable())
                    {
                        outDir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
                        if (outDir.isEmpty())
                            outDir = QDir::homePath();
                    }
                    // QDir::filePath joins correctly even when outDir is a drive root ("I:/"),
                    // where naive string concatenation produced "I:\\name.iso"
                    isoEdit->setText(QDir::toNativeSeparators(QDir(outDir).filePath(name + QStringLiteral(".iso"))));
                }
            });
        connect(imageBtn, &QPushButton::clicked, this,
                [this, folderEdit, isoEdit, updateFolderStatus, recomputeFolderSize, updateFit]
                {
                    const QString f = QFileDialog::getOpenFileName(this, tr("Select the Blu-ray disc image"),
                                                                   lastInputDir, tr("Disk image (*.iso)"));
                    if (f.isEmpty())
                        return;
                    folderEdit->setText(QDir::toNativeSeparators(f));
                    updateFolderStatus();
                    recomputeFolderSize();
                    updateFit();
                    lastInputDir = QFileInfo(f).absolutePath();
                    writeSettings();
                    // Beside the image, with another name: the output cannot be the image it is read from
                    if (isoEdit->text().isEmpty())
                    {
                        QString outDir = QFileInfo(f).absolutePath();
                        if (QStorageInfo(outDir).isReadOnly() || !QFileInfo(outDir).isWritable())
                        {
                            outDir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
                            if (outDir.isEmpty())
                                outDir = QDir::homePath();
                        }
                        isoEdit->setText(QDir::toNativeSeparators(
                            QDir(outDir).filePath(QFileInfo(f).completeBaseName() + QStringLiteral("_new.iso"))));
                    }
                });
        connect(isoBtn, &QPushButton::clicked, this,
                [this, isoEdit]
                {
                    const QString f =
                        QFileDialog::getSaveFileName(this, tr("Output ISO"), isoEdit->text(), tr("Disk image (*.iso)"));
                    if (!f.isEmpty())
                        isoEdit->setText(QDir::toNativeSeparators(f));
                });
        connect(folderRefreshBtn, &QPushButton::clicked, bdmvTab,
                [folderEdit, updateFolderStatus, recomputeFolderSize, updateFit]
                {
                    // also fix a hand-typed path that points at the BDMV folder itself
                    const QString cur = folderEdit->text().trimmed();
                    if (QFileInfo(cur).fileName().compare(QStringLiteral("BDMV"), Qt::CaseInsensitive) == 0)
                        folderEdit->setText(QDir::toNativeSeparators(QFileInfo(cur).absolutePath()));
                    updateFolderStatus();
                    recomputeFolderSize();
                    updateFit();
                });
        connect(
            buildBtn, &QPushButton::clicked, this,
            [this, folderEdit, isoEdit, guardSpin, discTypeCombo, freeSectorsEdit, breaksList, buildBtn, beforeCheck,
             guardBeforeSpin, orderCheck, keepExtrasCheck, innerOnlyCheck, singleCopy3dCheck, labelEdit,
             readFreeSectors, noGuardCheck, metaCombo, metaEdit, isImage]
            {
                const QString folder = folderEdit->text().trimmed();
                const QString iso = isoEdit->text().trimmed();
                if (folder.isEmpty() || iso.isEmpty())
                {
                    QMessageBox::warning(this, tr("tsMuxeR"),
                                         tr("Please select a BDMV folder and an output ISO file."));
                    return;
                }
                if (QStorageInfo(QFileInfo(iso).absolutePath()).isReadOnly())
                {
                    QMessageBox::warning(this, tr("tsMuxeR"),
                                         tr("The output ISO is on a read-only drive, so it cannot be written there. "
                                            "Choose a writable output location (not the mounted disc)."));
                    return;
                }
                const QStringList breaks = breaksList();
                if (breaks.isEmpty())
                {
                    QMessageBox::warning(this, tr("tsMuxeR"),
                                         tr("Enter the disc's \"Free Sectors\" (from ImgBurn) so the layer "
                                            "break can be calculated for the exact disc you are burning."));
                    return;
                }
                if (discTypeCombo->currentData(Qt::UserRole + 2).toInt() == 1 &&
                    QMessageBox::warning(
                        this, tr("BD-R XL - read at your own risk"),
                        tr("Many Blu-ray players cannot read 100/128 GB BD-R XL discs at all, and there is no "
                           "guarantee yours will. You are proceeding at your own risk.\n\n"
                           "Keeping the image around 66 GB (the first two layers, selectable as its own disc "
                           "entry) improves the odds on some players, but even 66 GB is not guaranteed to play. "
                           "Test on your own device.\n\n"
                           "Build the ISO anyway?"),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                    return;
                if (isImage(folder) && isSameFile(iso, folder))
                {
                    QMessageBox::warning(this, tr("tsMuxeR"),
                                         tr("The output ISO is the disc image it is built from. Choose another "
                                            "name for the output."));
                    return;
                }
                if (!isImage(folder) && !QFileInfo::exists(pathNoCase(folder, "BDMV")) &&
                    QMessageBox::question(this, tr("tsMuxeR"),
                                          tr("The selected folder has no BDMV subfolder. Continue anyway?")) !=
                        QMessageBox::Yes)
                    return;
                if (QFile::exists(iso) &&
                    QMessageBox::question(this, tr("tsMuxeR"), tr("The output ISO already exists. Overwrite it?")) !=
                        QMessageBox::Yes)
                    return;
                muxForm->prepare(tr("Building BD-ROM ISO from BDMV folder"));
                buildBtn->setEnabled(false);
                muxForm->show();
                runInMuxMode = true;
                QStringList args;
                args << "--bdmv-to-iso";
                // The guard is the engine's default too; ticked off, the tab says so instead of sending a size
                if (noGuardCheck->isChecked())
                    args << "--no-layer-break-guard";
                else
                {
                    args << ("--layer-break-guard=" + QString::number(guardSpin->value()));
                    if (beforeCheck->isChecked())
                        args << ("--layer-break-guard-before=" + QString::number(guardBeforeSpin->value()));
                }
                if (!breaks.isEmpty())
                    args << ("--layer-break-lbn=" + breaks.join(","));
                if (orderCheck->isChecked())
                    args << "--original-order";
                // Lets the engine's layer-fit placement know the true disc end, so a file is only
                // moved wholly past the break when it genuinely still fits on the disc.
                if (const qint64 fsSectors = readFreeSectors(); fsSectors > 0)
                    args << ("--disc-capacity=" + QString::number(fsSectors));
                if (keepExtrasCheck->isChecked())
                    args << "--keep-extra-files";
                if (innerOnlyCheck->isChecked())
                    args << "--inner-only";
                if (singleCopy3dCheck->isChecked())
                    args << "--3d-single-copy";
                if (const QString lbl = labelEdit->text().trimmed(); !lbl.isEmpty())
                    args << ("--label=" + lbl);
                if (const QString metaDir = metaEdit->text().trimmed();
                    metaCombo->currentIndex() == 1 && !metaDir.isEmpty())
                    args << ("--meta-dir=" + metaDir);
                else if (metaCombo->currentIndex() == 2)
                    args << "--meta-dir=none";
                if (ui->checkBoxReport->isChecked())
                {
                    const QString report = QDir::toNativeSeparators(QDir::tempPath()) + QDir::separator() + "tsMuxeR_" +
                                           QFileInfo(iso).completeBaseName() + "_report.txt";
                    QFile::remove(report);
                    args << (QLatin1String("--report=") + report);
                    muxForm->setReportFile(report);
                }
                args << folder << iso;
                noteRunOutput(iso, false);
                tsMuxerExecute(args);
            });
        // re-enable the Build button whenever the tsMuxer process finishes
        void (QProcess::*finishedSig)(int, QProcess::ExitStatus) = &QProcess::finished;
        connect(&proc, finishedSig, this, [buildBtn](int, QProcess::ExitStatus) { buildBtn->setEnabled(true); });

        // The BDMV to ISO tab is self-contained (it has its own Build ISO button), so hide the main window's
        // mux controls (Output type, File name, Meta file, and the mux/meta buttons) while it is the active
        // tab. They apply only to the normal mux flow on the other tabs and are confusing here.
        auto updateBottomForTab = [this, bdmvTab]()
        {
            const bool onBdmv = ui->tabWidget->currentWidget() == bdmvTab;
            ui->groupBox_4->setVisible(!onBdmv);      // Output (type radios + File name)
            ui->groupBox_2->setVisible(!onBdmv);      // Meta file
            ui->buttonMux->setVisible(!onBdmv);       // Start muxing
            ui->buttonSaveMeta->setVisible(!onBdmv);  // Save meta file
            ui->buttonOpenMeta->setVisible(!onBdmv);  // Open meta file
        };
        connect(ui->tabWidget, &QTabWidget::currentChanged, this, [updateBottomForTab](int) { updateBottomForTab(); });
        updateBottomForTab();
    }

    // ---- dual-layer (BD-R/RE DL) guard controls for the normal Blu-ray / Blu-ray ISO output ----
    {
        auto* dlBox = new QGroupBox(tr("Dual-layer (BD-R/RE DL)"), this);
        auto* dl = new QGridLayout(dlBox);
        auto* discSizeCombo = new QComboBox(dlBox);
        discSizeCombo->setObjectName("dlDiscSize");
        // The same disc table the BDMV -> ISO tab uses. It used to be a bare BD25/BD50/BD100/BD128
        // list with no capacities behind it, so the guard was placed at the BD-R DL break whatever
        // was selected: on a 100 or 128 GB disc that sector is not a layer boundary at all and the
        // guard protected nothing. Item data mirrors the other combo: UserRole = layers,
        // UserRole+1 = Free Sectors, UserRole+2 = is BD-R XL, UserRole+3 = capacity band.
        discSizeCombo->addItem(tr("Off"), 0);
        discSizeCombo->setItemData(0, 0, Qt::UserRole + 1);
        {
            // Single layer sizes first, then the multi-layer table, which keeps the list in
            // ascending capacity order and keeps BD25, which this list has always offered.
            int slCount = 0;
            const DiscEntry* sl = singleLayerDiscs(slCount);
            int discCount = 0;
            const DiscEntry* discs = discTable(discCount);
            int row = 1;
            for (int pass = 0; pass < 2; ++pass)
            {
                const DiscEntry* set = pass == 0 ? sl : discs;
                const int n = pass == 0 ? slCount : discCount;
                for (int i = 0; i < n; ++i, ++row)
                {
                    discSizeCombo->addItem(tr(set[i].label), set[i].layers);
                    discSizeCombo->setItemData(row, set[i].freeSectors, Qt::UserRole + 1);
                    discSizeCombo->setItemData(row, set[i].isXL ? 1 : 0, Qt::UserRole + 2);
                    discSizeCombo->setItemData(row, QString::fromLatin1(set[i].band), Qt::UserRole + 3);
                }
            }
        }
        auto* guardCheck = new QCheckBox(tr("Layer-break guard"), dlBox);
        guardCheck->setObjectName("dlGuardCheck");
        auto* guardSpin = new QSpinBox(dlBox);
        guardSpin->setObjectName("dlGuardSpin");
        guardSpin->setRange(0, 9999);
        guardSpin->setValue(288);
        guardSpin->setSuffix(tr(" MB"));
        guardSpin->setEnabled(false);
        // Guard the far side of the break as well, the same control the BDMV -> ISO tab has. Most
        // media only fail at the start of the next layer, which is why the default puts the whole
        // budget there, but some are weak just before it too.
        auto* beforeCheck = new QCheckBox(tr("Also fill before the break"), dlBox);
        beforeCheck->setObjectName("dlGuardBeforeCheck");
        auto* beforeSpin = new QSpinBox(dlBox);
        beforeSpin->setObjectName("dlGuardBeforeSpin");
        beforeSpin->setRange(0, 9999);
        beforeSpin->setValue(288);
        beforeSpin->setSuffix(tr(" MB"));
        beforeSpin->setEnabled(false);
        beforeCheck->setEnabled(false);
        // The capacity of a physical disc, and the field that lets a user correct it. The same disc size
        // exists at two capacities, and the layer break is computed from whatever is here, so a
        // figure that does not match the disc in the drive puts the guard in the wrong place. The
        // value is pre-filled from the disc above and locked, exactly as in the BDMV -> ISO tab, so
        // it cannot be changed by accident; the checkbox makes it editable for a disc that reports
        // something else.
        auto* dlFreeSectorsLabel = new QLabel(tr("Free Sectors (ImgBurn):"), dlBox);
        auto* dlFreeSectorsEdit = new QLineEdit(dlBox);
        dlFreeSectorsEdit->setObjectName("dlFreeSectors");
        dlFreeSectorsEdit->setReadOnly(true);
        dlFreeSectorsEdit->setValidator(
            new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9 .,]*")), dlFreeSectorsEdit));
        auto* dlManualCheck = new QCheckBox(tr("Enter Free Sectors manually (advanced)"), dlBox);
        dlManualCheck->setObjectName("dlFreeSectorsManual");
        auto* oversizeCheck = new QCheckBox(tr("Allow oversize"), dlBox);
        oversizeCheck->setObjectName("dlAllowOversize");
        // The BDMV -> ISO tab's inner-only option for a direct mux to an ISO image (#3). The muxer lays the image out
        // from a folder it muxes first, the same way that tab does.
        auto* innerCheck = new QCheckBox(tr("Keep data on the inner disc area (pad the outer edge)"), dlBox);
        innerCheck->setObjectName("dlInnerOnly");
        // The guard an image crossing a layer break gets when none is ticked above, and the way to leave it out
        auto* noGuardCheck = new QCheckBox(tr("Leave out the layer-break guard"), dlBox);
        noGuardCheck->setObjectName("dlNoGuard");
        noGuardCheck->setToolTip(wrapTip(
            tr("An ISO image whose data crosses a layer break gets the layer-break guard without being asked: 288 MB "
               "after the break and 18 MB before it are left empty, as a burnt dual-layer disc often reads "
               "badly there and a player then stalls. Tick this to leave it out. A guard ticked above is used "
               "instead.")));
        innerCheck->setToolTip(wrapTip(
            tr("The outer edge of a disc is the most error-prone part to burn. With this ticked, the movie is kept on "
               "the inner part of every layer and the outer edge is filled with zeros. The disc is muxed into a folder "
               "beside the image first and the image is then laid out from it, as on the BDMV folder -> ISO tab, so "
               "the drive needs room for both while it runs. The zeros at the layer breaks are sized automatically, so "
               "the layer-break guard is not used. ISO output on a disc of two or more layers.")));
        auto* fitLabel = new QLabel(tr("Fit to disc:"), dlBox);
        // The BDMV -> ISO tab explains every one of its controls; this group had nothing at all, so the
        // same three questions came up here. Text only, no behaviour change.
        const QString fitTip =
            wrapTip(tr("Target disc size for the muxed output. Leave it Off to mux without a size limit. Pick the disc "
                       "you intend to burn and tsMuxeR checks the finished size against that capacity and warns if it "
                       "does not fit."));
        fitLabel->setToolTip(fitTip);
        discSizeCombo->setToolTip(fitTip);
        guardCheck->setToolTip(wrapTip(
            tr("On a multi-layer disc the drive switches layers at a fixed point, and those sectors are the ones "
               "most likely to burn badly. Switch this on to fill them with zeros so no part of the movie sits "
               "there and playback runs smoothly across the break. Applies to ISO output.")));
        guardSpin->setToolTip(
            wrapTip(tr("How much to fill at the layer break. 288 MB covers the usual defect zone. Larger values are "
                       "safer but take that space away from the movie.")));
        oversizeCheck->setToolTip(
            wrapTip(tr("Mux even when the result does not fit the disc size chosen above. Useful when you burn to a "
                       "larger disc than selected or write the image to disk only. The image will not fit the selected "
                       "disc.")));
        beforeCheck->setToolTip(wrapTip(
            tr("The guard normally goes entirely after the break, where discs fail most often. Switch this on to "
               "fill before it as well, for media that are also weak just before the transition. Both zones are "
               "set independently; this one costs that much more space.")));
        beforeSpin->setToolTip(
            wrapTip(tr("How much to fill BEFORE the break. Only used when the box beside it is ticked.")));
        auto* breaksLabel = new QLabel(dlBox);
        breaksLabel->setWordWrap(true);
        breaksLabel->setStyleSheet("color:#7f8c8d;");
        dlFreeSectorsLabel->setToolTip(fitTip);
        dlFreeSectorsEdit->setToolTip(
            wrapTip(tr("The capacity of the disc you are burning, as ImgBurn reports it under Free Sectors. The layer "
                       "break is worked out from this number, so if your disc reports something else, tick the box "
                       "below and enter that value.")));
        dlManualCheck->setToolTip(
            wrapTip(tr("The Free Sectors above are pre-filled for the selected disc and locked to prevent accidental "
                       "changes. Tick this only if ImgBurn shows a different Free Sectors for your exact disc, then "
                       "type that number. Un-tick to restore the standard value.")));
        int rr = 0;
        dl->addWidget(fitLabel, rr, 0);
        dl->addWidget(discSizeCombo, rr++, 1);
        dl->addWidget(dlFreeSectorsLabel, rr, 0);
        dl->addWidget(dlFreeSectorsEdit, rr++, 1);
        dl->addWidget(dlManualCheck, rr++, 0, 1, 2);
        dl->addWidget(guardCheck, rr, 0);
        dl->addWidget(guardSpin, rr++, 1);
        dl->addWidget(beforeCheck, rr, 0);
        dl->addWidget(beforeSpin, rr++, 1);
        dl->addWidget(innerCheck, rr++, 0, 1, 2);
        dl->addWidget(noGuardCheck, rr++, 0, 1, 2);
        dl->addWidget(oversizeCheck, rr++, 0, 1, 2);
        dl->addWidget(breaksLabel, rr++, 0, 1, 2);

        // Whatever is in the field wins, so a corrected capacity reaches the break calculation and
        // the muxer rather than only being displayed.
        auto dlReadFreeSectors = [dlFreeSectorsEdit]() -> qint64
        {
            QString digits = dlFreeSectorsEdit->text();
            digits.remove(QRegularExpression(QStringLiteral("[^0-9]")));
            return digits.toLongLong();
        };
        auto dlFillFreeSectors = [discSizeCombo, dlFreeSectorsEdit]()
        {
            const qint64 fs = discSizeCombo->currentData(Qt::UserRole + 1).toLongLong();
            dlFreeSectorsEdit->setText(fs > 0 ? QLocale().toString(fs) : QString());
        };
        // Show where the guard will actually land, so a wrong disc choice is visible before the mux
        // rather than after the burn.
        //
        // Only while there is a break to name, and only for ISO output. With Off, a single layer disc
        // or an emptied Free Sectors field the line is blank, and a blank label still takes a whole
        // row of the group. The meta builder never reads this label, so hiding it changes nothing the
        // muxer is given. updateDlVisibility applies the same rule when the output changes.
        auto updateDlBreaks = [this, discSizeCombo, breaksLabel, dlReadFreeSectors]()
        {
            const QList<qint64> breaks = discBreaks(dlReadFreeSectors(), discSizeCombo->currentData().toInt());
            if (breaks.isEmpty())
            {
                breaksLabel->clear();
                breaksLabel->setVisible(false);
                return;
            }
            QStringList pretty;
            for (const qint64 b : breaks) pretty << QLocale().toString(b);
            breaksLabel->setText(TsMuxerWindow::tr("Layer break(s) at sector: %1").arg(pretty.join("   |   ")));
            breaksLabel->setVisible(ui->radioButtonBluRayISO->isChecked());
        };
        auto updateDlFreeSectorsRow = [discSizeCombo, dlFreeSectorsLabel, dlFreeSectorsEdit, dlManualCheck]()
        {
            // Nothing to correct when no disc is selected.
            const bool on = discSizeCombo->currentIndex() > 0;
            dlFreeSectorsLabel->setVisible(on);
            dlFreeSectorsEdit->setVisible(on);
            dlManualCheck->setVisible(on);
        };
        // The inner-only tick is shown for ISO output only, as the guard is, and can be ticked only for a disc with a
        // layer break: a single layer disc has no outer layer edge to pad. While it is on it sizes the zeros at the
        // breaks itself, so the guard boxes are greyed, as on the BDMV -> ISO tab.
        auto updateDlInner =
            [this, innerCheck, discSizeCombo, guardCheck, guardSpin, beforeCheck, beforeSpin, noGuardCheck]()
        {
            const bool iso = ui->radioButtonBluRayISO->isChecked();
            innerCheck->setVisible(iso);
            innerCheck->setEnabled(iso && discSizeCombo->currentData().toInt() >= 2);
            const bool inner = innerCheck->isEnabled() && innerCheck->isChecked();
            guardCheck->setEnabled(iso && !inner);
            guardSpin->setEnabled(iso && !inner && guardCheck->isChecked());
            beforeCheck->setEnabled(iso && !inner && guardCheck->isChecked());
            beforeSpin->setEnabled(iso && !inner && guardCheck->isChecked() && beforeCheck->isChecked());
            // Leaving out the guard is a choice of an image without a guard ticked: inner-only pads its own way
            noGuardCheck->setVisible(iso);
            noGuardCheck->setEnabled(iso && !inner && !guardCheck->isChecked());
        };
        dlFillFreeSectors();
        updateDlFreeSectorsRow();
        updateDlBreaks();
        connect(
            discSizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), dlBox,
            [dlFillFreeSectors, updateDlBreaks, updateDlFreeSectorsRow, dlManualCheck, guardCheck, discSizeCombo](int)
            {
                // A new disc means a new standard capacity: re-fill and re-lock, so a value typed
                // for the previous disc cannot silently carry over to this one.
                dlManualCheck->setChecked(false);
                dlFillFreeSectors();
                updateDlFreeSectorsRow();
                updateDlBreaks();
                // Choosing a disc that HAS a layer break turns the guard on with it. A file lying
                // across that break is the fault the guard exists to prevent, so fitting to a
                // dual layer disc without it checked the size and left the break unprotected,
                // which is half of what asking to fit a disc means. The BDMV to ISO tab has
                // always applied the guard on its own, and its disc list is multi-layer only.
                //
                // This is a default and not a lock. Unticking it afterwards sticks, and only
                // choosing another disc sets it again.
                //
                // Off and a single layer disc have NO break, so the guard is cleared for them
                // rather than left naming a boundary that does not exist. The combo's data is
                // the layer count, and breaks are layers - 1.
                guardCheck->setChecked(discSizeCombo->currentData().toInt() >= 2);
            });
        // after the handler above, which sets the guard for the new disc
        connect(discSizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), dlBox,
                [updateDlInner](int) { updateDlInner(); });
        connect(dlManualCheck, &QCheckBox::toggled, dlBox,
                [dlFreeSectorsEdit, dlFillFreeSectors, updateDlBreaks](const bool on)
                {
                    dlFreeSectorsEdit->setReadOnly(!on);
                    if (!on)
                        dlFillFreeSectors();  // un-ticking restores the standard value
                    updateDlBreaks();
                });
        connect(dlFreeSectorsEdit, &QLineEdit::textChanged, dlBox,
                [updateDlBreaks](const QString&) { updateDlBreaks(); });
        // guardSpin follows guardCheck, but only where the guard actually does something (see
        // updateDlVisibility below).
        connect(guardCheck, &QCheckBox::toggled, guardSpin,
                [this, guardSpin, beforeCheck, beforeSpin, updateDlInner](const bool on)
                {
                    const bool iso = ui->radioButtonBluRayISO->isChecked();
                    guardSpin->setEnabled(on && iso);
                    beforeCheck->setEnabled(on && iso);
                    beforeSpin->setEnabled(on && iso && beforeCheck->isChecked());
                    updateDlInner();
                });
        connect(beforeCheck, &QCheckBox::toggled, beforeSpin,
                [this, guardCheck, beforeSpin, updateDlInner](const bool on)
                {
                    beforeSpin->setEnabled(on && guardCheck->isChecked() && ui->radioButtonBluRayISO->isChecked());
                    updateDlInner();
                });
        connect(innerCheck, &QCheckBox::toggled, dlBox, [updateDlInner](bool) { updateDlInner(); });
        // The group sits beside the Meta file box, not in the Output group under the file name. There,
        // every row it showed made the bottom of the window taller by the same amount, and that height
        // could only come from the track list: the Output group has a fixed height, the Meta file box
        // stops shrinking at its minimum, and the splitter above them then takes the rest from the
        // Input tab, where only the track list can give way. With Blu-ray ISO and a multi-layer disc
        // the window at its default size showed three of six tracks, and the tracks scrolled out of
        // sight were taken for tracks the ISO had dropped. Beside the Meta file box the group only
        // costs the height by which it is taller than that box. It is aligned to the top, so a group
        // shorter than the Meta file box keeps its own row spacing instead of being stretched.
        //
        // ui->verticalLayout_13 and ui->groupBox_2 are used directly, never looked up by name with
        // findChild: the muxForm progress dialog is a child of this window and has a groupBox_2 and a
        // verticalLayout_2 of its own, which is how this group once ended up in the progress dialog.
        {
            const int metaIndex = ui->verticalLayout_13->indexOf(ui->groupBox_2);
            ui->verticalLayout_13->removeWidget(ui->groupBox_2);
            auto* metaRow = new QHBoxLayout();
            metaRow->addWidget(ui->groupBox_2, 1);
            metaRow->addWidget(dlBox, 0, Qt::AlignTop);
            ui->verticalLayout_13->insertLayout(metaIndex, metaRow);
            // Under it, a line for a 3D track on a Blu-ray output (see updateMux3dLabel).
            m_mux3dLabel = new QLabel(this);
            m_mux3dLabel->setWordWrap(true);
            m_mux3dLabel->setVisible(false);
            ui->verticalLayout_13->insertWidget(metaIndex + 1, m_mux3dLabel);
            m_retranslateHooks.push_back([this]() { updateMux3dLabel(); });
        }
        // These options only apply to Blu-ray ISO / folder output (see the meta builder), so show the group
        // only for those output modes and keep it hidden for TS / M2TS / MKV / AVCHD / Demux.
        auto updateDlVisibility =
            [this, dlBox, guardCheck, guardSpin, beforeCheck, beforeSpin, breaksLabel, updateDlInner]()
        {
            const bool iso = ui->radioButtonBluRayISO->isChecked();
            // The BDMV folder -> ISO tab hides the Output group while it is the current tab. This group
            // belongs to the output choice but no longer sits inside that group, so it is hidden with it here.
            dlBox->setVisible((iso || ui->radioButtonBluRay->isChecked()) && !ui->groupBox_4->isHidden());
            // A Blu-ray FOLDER has no image and therefore no layer break, so the guard controls are
            // HIDDEN there rather than merely greyed out. Greyed out still invites the question of
            // what they would do, and the answer is nothing. Fit to disc and Allow oversize stay,
            // because the capacity check applies to a folder just as much as to an image.
            guardCheck->setVisible(iso);
            guardSpin->setVisible(iso);
            beforeCheck->setVisible(iso);
            beforeSpin->setVisible(iso);
            // The break line also needs a break to name; see updateDlBreaks.
            breaksLabel->setVisible(iso && !breaksLabel->text().isEmpty());
            beforeCheck->setEnabled(iso && guardCheck->isChecked());
            beforeSpin->setEnabled(iso && guardCheck->isChecked() && beforeCheck->isChecked());
            // The layer-break guard writes zero padding into the disc IMAGE: BlurayHelper::open
            // only applies it when the output extension is .iso. A Blu-ray FOLDER has no image and
            // no layer break, so leaving the control usable there let you tick it and set a size
            // that was then silently ignored. Fit to disc and Allow oversize DO apply to folder
            // output, since the capacity check runs for any disc type, so those stay enabled.
            guardCheck->setEnabled(iso);
            guardSpin->setEnabled(iso && guardCheck->isChecked());
            updateDlInner();  // the inner-only tick and what it greys
            updateMux3dLabel();
        };
        updateDlVisibility();
        connect(ui->radioButtonBluRayISO, &QAbstractButton::toggled, dlBox,
                [updateDlVisibility](bool) { updateDlVisibility(); });
        connect(ui->radioButtonBluRay, &QAbstractButton::toggled, dlBox,
                [updateDlVisibility](bool) { updateDlVisibility(); });
        // A tab change has to reach this group too, now that it is outside the Output group.
        // updateBottomForTab is connected to the same signal earlier, so the Output group has already
        // been shown or hidden for the new tab when this runs.
        connect(ui->tabWidget, &QTabWidget::currentChanged, dlBox, [updateDlVisibility](int) { updateDlVisibility(); });

        // EVERY control in this group has to rebuild the meta, and until now not one of them did.
        // The meta is what the muxer is actually given, so ticking the guard, choosing a disc or
        // allowing oversize changed nothing: the mux ran with the text as it stood, and the option
        // only appeared once something else rebuilt it, such as switching the output between ISO
        // and folder. That is exactly how it was reported, twice: a guard that did nothing, and an
        // Allow oversize that still refused.
        //
        // The same fault was fixed once before for the Dolby Vision selector a few hundred lines up.
        // A control added to this window is not wired until it says so here.
        connect(discSizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                &TsMuxerWindow::updateMetaLines);
        connect(guardCheck, &QCheckBox::toggled, this, &TsMuxerWindow::updateMetaLines);
        connect(guardSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &TsMuxerWindow::updateMetaLines);
        connect(beforeCheck, &QCheckBox::toggled, this, &TsMuxerWindow::updateMetaLines);
        connect(beforeSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &TsMuxerWindow::updateMetaLines);
        connect(oversizeCheck, &QCheckBox::toggled, this, &TsMuxerWindow::updateMetaLines);
        connect(innerCheck, &QCheckBox::toggled, this, &TsMuxerWindow::updateMetaLines);
        connect(noGuardCheck, &QCheckBox::toggled, this, &TsMuxerWindow::updateMetaLines);
        connect(dlManualCheck, &QCheckBox::toggled, this, &TsMuxerWindow::updateMetaLines);
        connect(dlFreeSectorsEdit, &QLineEdit::textChanged, this, [this](const QString&) { updateMetaLines(); });

        // Re-translate this groupbox on a runtime language change (see the BDMV->ISO tab hook above).
        m_retranslateHooks.push_back(
            [dlBox, fitLabel, guardCheck, guardSpin, oversizeCheck, discSizeCombo, beforeCheck, beforeSpin,
             updateDlBreaks, dlFreeSectorsLabel, dlFreeSectorsEdit, dlManualCheck, innerCheck, noGuardCheck]()
            {
                dlBox->setTitle(tr("Dual-layer (BD-R/RE DL)"));
                fitLabel->setText(tr("Fit to disc:"));
                dlFreeSectorsLabel->setText(tr("Free Sectors (ImgBurn):"));
                dlManualCheck->setText(tr("Enter Free Sectors manually (advanced)"));
                dlFreeSectorsEdit->setToolTip(wrapTip(
                    tr("The capacity of the disc you are burning, as ImgBurn reports it under Free Sectors. The "
                       "layer break is worked out from this number, so if your disc reports something else, tick "
                       "the box below and enter that value.")));
                dlManualCheck->setToolTip(wrapTip(
                    tr("The Free Sectors above are pre-filled for the selected disc and locked to prevent "
                       "accidental changes. Tick this only if ImgBurn shows a different Free Sectors for your exact "
                       "disc, then type that number. Un-tick to restore the standard value.")));
                guardCheck->setText(tr("Layer-break guard"));
                beforeCheck->setText(tr("Also fill before the break"));
                oversizeCheck->setText(tr("Allow oversize"));
                noGuardCheck->setText(tr("Leave out the layer-break guard"));
                noGuardCheck->setToolTip(wrapTip(
                    tr("An ISO image whose data crosses a layer break gets the layer-break guard without being asked: "
                       "288 MB after the break and 18 MB before it are left empty, as a burnt dual-layer disc often "
                       "reads badly there and a player then stalls. Tick this to leave it out. A guard ticked above is "
                       "used instead.")));
                innerCheck->setText(tr("Keep data on the inner disc area (pad the outer edge)"));
                innerCheck->setToolTip(wrapTip(
                    tr("The outer edge of a disc is the most error-prone part to burn. With this ticked, the movie is "
                       "kept on the inner part of every layer and the outer edge is filled with zeros. The disc is "
                       "muxed into a folder beside the image first and the image is then laid out from it, as on the "
                       "BDMV folder -> ISO tab, so the drive needs room for both while it runs. The zeros at the layer "
                       "breaks are sized automatically, so the layer-break guard is not used. ISO output on a disc of "
                       "two or more layers.")));
                discSizeCombo->setItemText(0, tr("Off"));
                {
                    int slCount = 0;
                    const DiscEntry* sl = singleLayerDiscs(slCount);
                    int discCount = 0;
                    const DiscEntry* discs = discTable(discCount);
                    int row = 1;
                    for (int i = 0; i < slCount; ++i, ++row) discSizeCombo->setItemText(row, tr(sl[i].label));
                    for (int i = 0; i < discCount; ++i, ++row) discSizeCombo->setItemText(row, tr(discs[i].label));
                }
                guardSpin->setSuffix(tr(" MB"));
                beforeSpin->setSuffix(tr(" MB"));
                beforeCheck->setToolTip(wrapTip(
                    tr("The guard normally goes entirely after the break, where discs fail most often. Switch this "
                       "on to fill before it as well, for media that are also weak just before the transition. Both "
                       "zones are set independently; this one costs that much more space.")));
                beforeSpin->setToolTip(
                    wrapTip(tr("How much to fill BEFORE the break. Only used when the box beside it is ticked.")));
                updateDlBreaks();  // the "Layer break(s) at sector:" line carries a translated prefix
                // The tooltips need re-setting too, exactly as the BDMV -> ISO tab hook does.
                // Without this they keep the language the window was built in.
                const QString fitTip =
                    wrapTip(tr("Target disc size for the muxed output. Leave it Off to mux without a size limit. Pick "
                               "the disc you intend to burn and tsMuxeR checks the finished size against that capacity "
                               "and warns if it does not fit."));
                fitLabel->setToolTip(fitTip);
                discSizeCombo->setToolTip(fitTip);
                guardCheck->setToolTip(wrapTip(
                    tr("On a multi-layer disc the drive switches layers at a fixed point, and those sectors are "
                       "the ones most likely to burn badly. Switch this on to fill them with zeros so no part of "
                       "the movie sits there and playback runs smoothly across the break. Applies to ISO "
                       "output.")));
                guardSpin->setToolTip(wrapTip(
                    tr("How much to fill at the layer break. 288 MB covers the usual defect zone. Larger values "
                       "are safer but take that space away from the movie.")));
                oversizeCheck->setToolTip(
                    wrapTip(tr("Mux even when the result does not fit the disc size chosen above. Useful when you burn "
                               "to a larger disc than selected or write the image to disk only. The image will not fit "
                               "the selected disc.")));
            });
    }

    // ** A TAB SCROLLS WHEN THE WINDOW IS TOO SMALL FOR IT. ** The window may be made as small as 800 x 650, so that it
    // fits a small screen, and its tabs need more than that: at the smallest size the track list showed one row, the
    // buttons beside it were squashed, and rows of the Subtitles and Blu-ray tabs were drawn over each other. The
    // content of every tab now sits in a scroll area inside its page. The page itself stays, so its title and
    // everything that finds a tab by its page are as they were. The track list keeps at least four rows.
    ui->trackLV->setMinimumHeight(ui->trackLV->horizontalHeader()->sizeHint().height() +
                                  4 * ui->trackLV->verticalHeader()->defaultSectionSize() +
                                  2 * ui->trackLV->frameWidth());
    for (int i = 0; i < ui->tabWidget->count(); ++i)
    {
        QWidget* page = ui->tabWidget->widget(i);
        QLayout* content = page->layout();
        if (content == nullptr)
            continue;
        auto* inner = new TabContent;
        inner->setLayout(content);  // the layout, and the widgets in it, move to inner
        // the layout no longer fixes the smallest size of the content: TabContent says it, its height only
        content->setSizeConstraint(QLayout::SetNoConstraint);
        auto* area = new QScrollArea(page);
        area->setFrameShape(QFrame::NoFrame);
        area->setWidgetResizable(true);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        area->setWidget(inner);
        // the page's own background shows through, as before
        area->viewport()->setAutoFillBackground(false);
        inner->setAutoFillBackground(false);
        auto* pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        pageLayout->addWidget(area);
    }

    // ** WHETHER THE META FILE FOLLOWS THE SETTINGS IS SHOWN. ** Text typed into the Meta file box, even by accident,
    // or a meta file opened, stopped the window from rebuilding the meta, and nothing said so: a setting changed in a
    // tab went nowhere, and the mux ran without it, until "Reset meta to auto-generated" was found and clicked. The box
    // above the text says now whether the meta file is built from the settings, the box's title says when it is not,
    // a line says so when a change in a tab is not applied, and ticking the box again builds it once more.
    ui->metaHintLabel->setStyleSheet(QStringLiteral("color: #c0392b;"));
    updateMetaState();
    m_retranslateHooks.push_back([this] { updateMetaState(); });

    // Settle everything that follows from the output once at start up as well, as a click on it does.
    // RadioButtonMuxClick does it on every later change, but the remembered output is restored by
    // setOutputMode, which only sets its button: a remembered Demux opened with "Start muxing" and
    // "File name" until another output was clicked. The new audio PES tick was remembered with the
    // output and is kept.
    applyOutputMode(false);
    // retranslateUi puts back the texts of the .ui file, "Start muxing" and "File name", on every
    // language change, and the saved language itself arrives as one after this constructor: a
    // remembered Demux opened with them, and a language change while Demux was chosen did the same.
    m_retranslateHooks.push_back([this] { updateOutputModeTexts(); });

    restoreChoices(m_seedSettings ? *m_seedSettings : *settings);
    delete m_seedSettings;  // only read: nothing goes back to the registry
    m_seedSettings = nullptr;
    writeSettings();
}

// The program choices the window remembers besides the ones writeSettings() names: every control that sets HOW a job
// is done, not WHAT it is. Left out on purpose: the cut on/off and its times, the custom chapter list, the disc label,
// the output name, the BDMV folder, the ISO name and the meta box, which belong to one job; and the start mux time,
// the right-eye view and the 3D single copy, which are taken from the source each time.
//
// Restored in this order, which matters: a disc type fills its Free Sectors and sets the layer-break guard, so the
// manual Free Sectors and the guard come after the disc type they belong to.
static const char* const CHOICE_WIDGETS[] = {
    "tabWidget",
    // General tab
    "checkBoxVBR", "checkBoxRVBR", "checkBoxCBR", "editMaxBitrate", "editMinBitrate", "editCBRBitrate", "editVBVLen",
    "radioButtonNoChapters", "radioButtonAutoChapter", "radioButtonCustomChapters", "spinEditChapterLen",
    // Blu-ray tab
    "checkBoxV3", "checkBoxRateLimit", "comboRateLimit", "spinBoxMplsNum", "spinBoxM2tsNum", "pipTransparencySpinBox",
    // Split and cut tab
    "noSplit", "splitByDuration", "splitBySize", "splitByChapters", "spinEditSplitDuration", "editSplitSize",
    "comboBoxMeasure",
    // Output
    "comboBoxDvProfile",
    // the dual-layer group beside the Meta file box
    "dlDiscSize", "dlGuardCheck", "dlGuardSpin", "dlGuardBeforeCheck", "dlGuardBeforeSpin", "dlFreeSectorsManual",
    "dlFreeSectors", "dlAllowOversize", "dlInnerOnly", "dlNoGuard",
    // the BDMV folder -> ISO tab
    "isoDiscType", "isoFreeSectorsManual", "isoFreeSectors", "isoGuard", "isoGuardBeforeCheck", "isoGuardBefore",
    "isoInnerOnly", "isoKeepOrder", "isoKeepExtras", "isoNoGuard"};

void TsMuxerWindow::saveChoices()
{
    if (!m_choicesRestored)
        return;
    settings->beginGroup("choices");
    for (const char* name : CHOICE_WIDGETS)
    {
        const QString key = QLatin1String(name);
        QWidget* w = findChild<QWidget*>(key);
        if (auto* tab = qobject_cast<QTabWidget*>(w))
            settings->setValue(key, tab->currentIndex());
        else if (auto* button = qobject_cast<QAbstractButton*>(w))
            settings->setValue(key, button->isChecked());
        else if (auto* spin = qobject_cast<QSpinBox*>(w))
            settings->setValue(key, spin->value());
        else if (auto* dspin = qobject_cast<QDoubleSpinBox*>(w))
            settings->setValue(key, dspin->value());
        else if (auto* combo = qobject_cast<QComboBox*>(w))
            // With the number of entries, so that a list that changes in a later version is not restored by position
            settings->setValue(
                key, QString::number(combo->currentIndex()) + QLatin1Char('/') + QString::number(combo->count()));
        else if (auto* edit = qobject_cast<QLineEdit*>(w))
            settings->setValue(key, edit->text());
    }
    settings->endGroup();
}

void TsMuxerWindow::restoreChoices(QSettings& from)
{
    from.beginGroup("choices");
    for (const char* name : CHOICE_WIDGETS)
    {
        const QString key = QLatin1String(name);
        QWidget* w = findChild<QWidget*>(key);
        if (!w || !from.contains(key))
            continue;
        const QVariant v = from.value(key);
        // Set the way a user would, so the controls that follow this one (an enabled field, a filled-in Free Sectors,
        // the meta) follow it now as well.
        if (auto* tab = qobject_cast<QTabWidget*>(w))
        {
            const int idx = v.toInt();
            if (idx >= 0 && idx < tab->count())
                tab->setCurrentIndex(idx);
        }
        else if (auto* button = qobject_cast<QAbstractButton*>(w))
            button->setChecked(v.toBool());
        else if (auto* spin = qobject_cast<QSpinBox*>(w))
        {
            // The guard before the break follows the guard after it (1/16, at least 4 MB) until it is set by hand.
            // A stored value equal to that is the followed one, and setting it would count as set by hand.
            if (key == QLatin1String("isoGuardBefore"))
            {
                const auto* after = findChild<QSpinBox*>(QStringLiteral("isoGuard"));
                if (after && v.toInt() == qMin(after->value(), qMax(4, after->value() / 16)))
                    continue;
            }
            spin->setValue(v.toInt());
        }
        else if (auto* dspin = qobject_cast<QDoubleSpinBox*>(w))
            dspin->setValue(v.toDouble());
        else if (auto* combo = qobject_cast<QComboBox*>(w))
        {
            const QStringList parts = v.toString().split(QLatin1Char('/'));
            if (parts.size() == 2 && parts[1].toInt() == combo->count() && parts[0].toInt() >= 0 &&
                parts[0].toInt() < combo->count())
                combo->setCurrentIndex(parts[0].toInt());
        }
        else if (auto* edit = qobject_cast<QLineEdit*>(w))
        {
            // A Free Sectors figure is the disc type's own unless it was entered by hand
            QString manual;
            if (key == QLatin1String("isoFreeSectors"))
                manual = QStringLiteral("isoFreeSectorsManual");
            else if (key == QLatin1String("dlFreeSectors"))
                manual = QStringLiteral("dlFreeSectorsManual");
            const auto* manualCheck = manual.isEmpty() ? nullptr : findChild<QAbstractButton*>(manual);
            if (manualCheck && !manualCheck->isChecked())
                continue;
            edit->setText(v.toString());
        }
    }
    from.endGroup();
    // These follow clicks rather than changes, so a restored choice does not reach them by itself
    onGeneralCheckboxClicked();
    onChapterParamsChanged();
    onSplitCutParamsChanged();
    m_choicesRestored = true;

    // From now on every change of a choice is saved when it happens, not only when the window closes, so a choice made
    // just before the program ends some other way is kept as well.
    const auto save = [this] { writeSettings(); };
    for (const char* name : CHOICE_WIDGETS)
    {
        QWidget* w = findChild<QWidget*>(QLatin1String(name));
        if (auto* tab = qobject_cast<QTabWidget*>(w))
            connect(tab, &QTabWidget::currentChanged, this, save);
        else if (auto* button = qobject_cast<QAbstractButton*>(w))
            connect(button, &QAbstractButton::toggled, this, save);
        else if (auto* spin = qobject_cast<QSpinBox*>(w))
            connect(spin, &QSpinBox::valueChanged, this, save);
        else if (auto* dspin = qobject_cast<QDoubleSpinBox*>(w))
            connect(dspin, &QDoubleSpinBox::valueChanged, this, save);
        else if (auto* combo = qobject_cast<QComboBox*>(w))
            connect(combo, &QComboBox::currentIndexChanged, this, save);
        else if (auto* edit = qobject_cast<QLineEdit*>(w))
            connect(edit, &QLineEdit::textChanged, this, save);
    }
    // The output and the SEI choices used to be saved only with the next save of something else, or at closing
    const QAbstractButton* const savedButtons[] = {
        ui->radioButtonTS,     ui->radioButtonM2TS,  ui->radioButtonMKV,   ui->radioButtonBluRayISO,
        ui->radioButtonBluRay, ui->radioButtonAVCHD, ui->radioButtonDemux, ui->checkBoxSPS};
    for (const QAbstractButton* b : savedButtons) connect(b, &QAbstractButton::toggled, this, save);
    connect(ui->comboBoxSEI, &QComboBox::currentIndexChanged, this, save);
}

QString tsMuxerGuiIniName()
{
    // The program's own folder. argv[0] is the name the program was started by, which for a start through PATH or by a
    // relative name points at the current folder, where the ini is not.
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("tsMuxerGUI.ini"));
}

void TsMuxerWindow::showEvent(QShowEvent* event)
{
    m_wasShown = true;
    QWidget::showEvent(event);
}

TsMuxerWindow::~TsMuxerWindow()
{
    disableUpdatesCnt = 0;
    writeSettings();
    delete settings;
}

void TsMuxerWindow::onTsMuxerCodecInfoReceived()
{
    m_updateMeta = false;
    codecList.clear();
    int p;
    QtvCodecInfo* codecInfo = 0;
    int lastTrackID = 0;
    QString tmpStr;
    bool firstMark = true;
    // Tracks tsMuxeR recognised but cannot mux, e.g. the Interactive Graphics menu overlay.
    // They are deliberately reported without the "Stream type: " prefix so they are never
    // offered as selectable, which previously meant they vanished from the GUI with no
    // explanation at all. Collect them and say so once, naming the stream.
    QStringList unsupportedTracks;
    // Clips the playlist names that will not be muxed, because the streams they carry contradict
    // the rest of it. The CLI leaves them out and says which; without this the window simply
    // showed one fewer file in its playlist list and gave no reason for it.
    QStringList skippedClips;
    codecList.clear();
    m_listedDiscTitle.clear();
    mplsFileList.clear();
    chapters.clear();
    fileDuration = 0;
    listedWritingApp.clear();
    videoStart = videoEnd = 0;
    for (int i = 0; i < procStdOutput.size(); ++i)
    {
        // A Matroska track's name is the file's own text, so it can hold any of the prefixes looked for below. Its
        // line is read first, and nothing else is looked for in it.
        if (procStdOutput[i].startsWith("Stream name: "))
        {
            if (codecInfo)
                codecInfo->trackName = codecInfo->srcTrackName =
                    QtCompat::strMid(procStdOutput[i], QString("Stream name: ").length());
            continue;
        }
        // The file's own text as well, so read first and nothing else looked for in it
        if (procStdOutput[i].startsWith("Writing application: "))
        {
            listedWritingApp = QtCompat::strMid(procStdOutput[i], QString("Writing application: ").length());
            continue;
        }
        if (procStdOutput[i].startsWith("Stream flags: "))
        {
            if (codecInfo)
                for (const QString& flag :
                     QtCompat::strMid(procStdOutput[i], QString("Stream flags: ").length()).split(", "))
                    for (int k = 0; k < 6; ++k)
                        if (flag == QLatin1String(MKV_FLAG_KEYS[k]))
                            codecInfo->mkvFlags[k] = codecInfo->srcMkvFlags[k] = true;
            continue;
        }
        p = procStdOutput[i].indexOf("Track ID:    ");
        if (p >= 0)
        {
            lastTrackID = QtCompat::strMid(procStdOutput[i], QString("Track ID:    ").length()).toInt();
            codecList << QtvCodecInfo();
            codecInfo = &(codecList.back());
            codecInfo->trackID = lastTrackID;
        }

        p = procStdOutput[i].indexOf("Stream type: ");
        if (p >= 0)
        {
            if (lastTrackID == 0)
            {
                codecList << QtvCodecInfo();
                codecInfo = &(codecList.back());
            }
            codecInfo->descr = "Can't detect codec";
            codecInfo->displayName = QtCompat::strMid(procStdOutput[i], QString("Stream type: ").length());
            /* Add SEI and SPS only with AVC and MVC (currently disabled)
            if (codecInfo->displayName != "H.264" && codecInfo->displayName != "MVC")
            {
                codecInfo->addSEIMethod = 0;
                codecInfo->addSPS = false;
            }
            */
            if (codecInfo->displayName == "HEVC" || codecInfo->displayName == "VVC" || codecInfo->displayName == "AV1")
                ui->checkBoxV3->setChecked(true);
            else if (codecInfo->displayName == "H.264" || codecInfo->displayName == "MVC" ||
                     codecInfo->displayName == "MPEG-2" || codecInfo->displayName == "VC-1")
                ui->checkBoxV3->setChecked(false);
            lastTrackID = 0;
        }
        p = procStdOutput[i].indexOf("Stream ID:   ");
        if (p >= 0)
            codecInfo->programName = QtCompat::strMid(procStdOutput[i], QString("Stream ID:   ").length());
        p = procStdOutput[i].indexOf("Stream info: ");
        if (p >= 0)
            codecInfo->descr = QtCompat::strMid(procStdOutput[i], QString("Stream info: ").length());
        p = procStdOutput[i].indexOf("Stream lang: ");
        if (p >= 0)
            codecInfo->lang = codecInfo->srcLang =
                QtCompat::strMid(procStdOutput[i], QString("Stream lang: ").length());
        // "Stream order: playlist 2, clip 4": a playlist's track among its kind, in the playlist and in its clip (#52)
        if (codecInfo && procStdOutput[i].startsWith(QLatin1String("Stream order: playlist ")))
        {
            static const QRegularExpression order(QStringLiteral("^Stream order: playlist (\\d+), clip (\\d+)"));
            if (const auto m = order.match(procStdOutput[i]); m.hasMatch())
            {
                codecInfo->playlistPlace = m.captured(1).toInt();
                codecInfo->clipPlace = m.captured(2).toInt();
            }
        }
        if (procStdOutput[i].startsWith(QLatin1String("Disc title: ")))
            m_listedDiscTitle = QtCompat::strMid(procStdOutput[i], QString("Disc title: ").length()).trimmed();
        p = procStdOutput[i].indexOf("Stream file: ");
        if (p >= 0 && codecInfo != nullptr)
            codecInfo->ownFile = QtCompat::strMid(procStdOutput[i], QString("Stream file: ").length()).trimmed();
        p = procStdOutput[i].indexOf("Stream delay: ");
        if (p >= 0)
        {
            tmpStr = QtCompat::strMid(procStdOutput[i], QString("Stream delay: ").length());
            codecInfo->delay = codecInfo->listedDelay = tmpStr.toInt();
        }
        p = procStdOutput[i].indexOf("subTrack: ");
        if (p >= 0)
        {
            tmpStr = QtCompat::strMid(procStdOutput[i], QString("subTrack: ").length());
            codecInfo->subTrack = tmpStr.toInt();
        }
        p = procStdOutput[i].indexOf("Secondary: 1");
        if (p == 0)
        {
            codecInfo->isSecondary = true;
        }
        p = procStdOutput[i].indexOf("Unselected: 1");
        if (p == 0)
        {
            codecInfo->enabledByDefault = false;
        }

        if (procStdOutput[i].startsWith("Not supported: "))
        {
            // The CLI sentence is English only, so relaying it verbatim produced a
            // half-translated dialog: a localised "Track 5120:" followed by an English
            // description. Recover the stream type code from it instead and build the whole
            // line here, where it can be translated. Unknown types fall back to the original
            // text, which is still better than nothing.
            const QString what = QtCompat::strMid(procStdOutput[i], QString("Not supported: ").length());
            // Reads "(stream type 0xNN)" and, for the types that need it, "(stream type 0xNN,
            // descriptor 0xMM)". A number ends at the comma or at the bracket, whichever comes
            // first, so a line carrying no descriptor reads exactly as it always did.
            const auto codeAfter = [&what](const QString& marker)
            {
                const int at = what.indexOf(marker);
                if (at < 0)
                    return -1;
                const int from = at + marker.length();
                int to = from;
                while (to < what.length() && what[to] != ',' && what[to] != ')') ++to;
                return to > from ? QtCompat::strMid(what, from, to - from).toInt(nullptr, 16) : -1;
            };
            const int streamType = codeAfter(QStringLiteral("(stream type 0x"));
            const int dvbDescriptor = codeAfter(QStringLiteral("descriptor 0x"));
            QString description;
            switch (streamType)
            {
            case 0x91:
                description = tr("Interactive Graphics, the disc menu overlay");
                break;
            case 0x92:
                description = tr("Text subtitles (Text-ST)");
                break;
            case 0x06:
                // 0x06 alone means only "private data". What the data actually is comes from the
                // descriptor beside it, which is why this case needs the second number.
                if (dvbDescriptor == 0x59)
                    description = tr("DVB bitmap subtitles (EN 300 743)");
                else if (dvbDescriptor == 0x56)
                    description = tr("EBU teletext (EN 300 468)");
                else if (dvbDescriptor == 0x7F || dvbDescriptor == 0x05)
                    description = tr("AC-4 audio");
                break;
            default:
                break;
            }
            if (description.isEmpty())
                description = what;  // unknown type: keep whatever the CLI said
            else
                description = tr("%1 (stream type 0x%2). Muxing this stream type is not implemented.")
                                  .arg(description)
                                  .arg(streamType, 0, 16);

            if (lastTrackID > 0)
            {
                unsupportedTracks << tr("Track %1: %2").arg(lastTrackID).arg(description);
                // The preceding "Track ID:" line already pushed an entry, and no
                // "Stream type:" will ever arrive to fill it. Left in place it becomes a
                // blank row in the track list, and it also makes codecList non-empty, which
                // suppressed the "Can't detect stream type" warning for a file that really
                // does contain nothing muxable.
                if (!codecList.isEmpty() && codecList.back().trackID == lastTrackID &&
                    codecList.back().displayName.isEmpty())
                {
                    codecList.removeLast();
                    codecInfo = nullptr;
                }
                lastTrackID = 0;
            }
            else
            {
                unsupportedTracks << description;
            }
        }
        else if (procStdOutput[i].startsWith("Skipped clip: "))
        {
            skippedClips << QtCompat::strMid(procStdOutput[i], QString("Skipped clip: ").length()).trimmed();
        }
        else if (procStdOutput[i].startsWith("Error: "))
        {
            tmpStr = QtCompat::strMid(procStdOutput[i], QString("Error: ").length());
            QMessageBox msgBox(this);
            msgBox.setWindowTitle(tr("Not supported"));
            msgBox.setText(tmpStr);
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setStandardButtons(QMessageBox::Ok);
            msgBox.exec();
        }
        else if (procStdOutput[i].startsWith("File #"))
        {
            // Format: "File #XXXXX name=<filename>", offset 17 = len("File #") + 5 + len(" name=")
            tmpStr = QtCompat::strMid(procStdOutput[i], 17);
            // the listing reads a playlist in its first angle: another angle's clip is named in its place, the
            // parts of several angles in their order
            for (int k = 0; k < m_pendingAngleClips.size(); ++k)
                if (QFileInfo(tmpStr).fileName().compare(m_pendingAngleClips[k].first, Qt::CaseInsensitive) == 0)
                {
                    tmpStr = QFileInfo(tmpStr).dir().filePath(m_pendingAngleClips[k].second);
                    tmpStr = QDir::toNativeSeparators(tmpStr);
                    m_pendingAngleClips.removeAt(k);
                    break;
                }
            mplsFileList << MPLSFileInfo(tmpStr, 0.0);
        }
        else if (procStdOutput[i].startsWith("Duration:"))
        {
            // Format: "Duration: <time>", offset 10 = len("Duration: ")
            tmpStr = QtCompat::strMid(procStdOutput[i], 10);
            if (!mplsFileList.empty())
            {
                mplsFileList.last().duration = timeToFloat(tmpStr);
            }
            else
            {
                fileDuration = timeToFloat(tmpStr);
            }
        }
        else if (procStdOutput[i].startsWith("Video start: "))
        {
            // Format: "Video start: <time>", offset 13 = len("Video start: ")
            videoStart = timeToFloat(QtCompat::strMid(procStdOutput[i], 13));
        }
        else if (procStdOutput[i].startsWith("Video end: "))
        {
            // Format: "Video end: <time>", offset 11 = len("Video end: ")
            videoEnd = timeToFloat(QtCompat::strMid(procStdOutput[i], 11));
        }
        else if (procStdOutput[i].startsWith("Base view: "))
        {
            // Format: "Base view: <eye>", offset 11 = len("Base view: ")
            tmpStr = QtCompat::strMid(procStdOutput[i], 11);
            ui->rightEyeCheckBox->setChecked(tmpStr.trimmed() == "right-eye");
        }
        else if (procStdOutput[i].startsWith("start-time: "))
        {
            tmpStr = QtCompat::strMid(procStdOutput[i], 12);
            if (tmpStr.contains(':'))
            {
                double secondsF = timeToFloat(tmpStr);
                ui->muxTimeEdit->setTime(qTimeFromFloat(secondsF));
            }
            else
            {
                ui->muxTimeClock->setValue(tmpStr.toInt());
            }
        }

        // A DVD title listed with --show-title gives its chapters as the times of --custom-chapters, which a playlist
        // gives as its marks: they are taken the same way
        static const QString dvdChapters = QStringLiteral("Chapters of the title, for a disc: --custom-chapters=");
        const bool dvdMarks = m_pendingDvdTitle && procStdOutput[i].startsWith(dvdChapters);
        p = procStdOutput[i].indexOf("Marks: ");
        if (p == 0 || dvdMarks)
        {
            if (firstMark)
            {
                firstMark = false;
                ui->radioButtonCustomChapters->setChecked(true);
                // setChecked does not emit clicked(), so the slot that settles everything which
                // depends on this radio never ran: the chapter list box stayed greyed out while
                // the mux was using the list inside it. Calling the slot keeps the rule in one
                // place. sender() is null here, so it leaves m_customChaptersUserOverride to the
                // line below, which sets it deliberately.
                onChapterParamsChanged();
            }
            m_customChaptersUserOverride = false;
            QStringList stringList = dvdMarks ? QtCompat::strMid(procStdOutput[i], dvdChapters.length()).split(';')
                                              : QtCompat::strMid(procStdOutput[i], 7).split(' ');
            for (int k = 0; k < stringList.size(); ++k)
                if (!stringList[k].isEmpty())
                    chapters << timeToFloat(stringList[k]);
        }
    }
    if (fileDuration == 0 && !mplsFileList.isEmpty())
    {
        for (const MPLSFileInfo& mplsFile : mplsFileList) fileDuration += mplsFile.duration;
    }
    // The tracks of a DVD title are listed from its first part: the title's length is the one the disc listing gave,
    // and where a join would put a file after it does not apply, a DVD title being joined to nothing
    if (m_pendingDvdTitle)
    {
        fileDuration = m_pendingDvdSeconds;
        videoStart = videoEnd = 0;
    }

    m_updateMeta = true;
    if (codecList.isEmpty())
    {
        QMessageBox msgBox(this);
        if (unsupportedTracks.isEmpty())
        {
            msgBox.setWindowTitle(tr("Unsupported format"));
            msgBox.setText(tr("Can't detect stream type. File name: \"%1\"").arg(newFileName));
        }
        else
        {
            // Every track in the file is one we recognise but cannot mux, e.g. a clip that
            // holds nothing but the menu overlay. Saying "can't detect stream type" here
            // would be both wrong and unhelpful: the type was detected, it just cannot be
            // muxed. Name it instead.
            msgBox.setWindowTitle(tr("Nothing to mux"));
            msgBox.setText(tr("This file contains no tracks that can be muxed:"));
            msgBox.setInformativeText(unsupportedTracks.join('\n'));
        }
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.exec();
        // As after a refusal (onProcessFinished): the files chosen after this one are still examined.
        if (!addFileList.isEmpty() && m_addNextFile)
            (this->*m_addNextFile)();
        return;
    }
    if (!unsupportedTracks.isEmpty())
    {
        // One informational box for the whole file rather than one per track: a disc can
        // carry dozens of menu overlays, and this is information, not a failure. The usable
        // tracks were all added normally.
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Some tracks were skipped"));
        msgBox.setText(tr("These tracks cannot be muxed and were not added:"));
        msgBox.setInformativeText(unsupportedTracks.join('\n'));
        msgBox.setIcon(QMessageBox::Information);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.exec();
    }
    if (!skippedClips.isEmpty())
    {
        // Clips rather than tracks, so its own box and its own words. The output will be shorter
        // than the playlist, which is the part someone has to be told about.
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Part of the playlist was left out"));
        msgBox.setText(
            tr("These clips are not part of the playlist as it will be muxed, because the streams "
               "they carry are not the ones the rest of the playlist uses. A single output cannot "
               "change codec part way through:"));
        msgBox.setInformativeText(skippedClips.join('\n'));
        msgBox.setIcon(QMessageBox::Information);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.exec();
    }
    updateMetaLines();
    emit codecListReady();
}

int TsMuxerWindow::findLangByCode(const QString& code)
{
    // The list holds terminological codes, so a track tagged "ger" found no row and the box showed
    // "und" for it. The bibliographic code is looked up as the language it stands for. A code that
    // is in no row at all still shows as row 0, "und".
    const QString wanted[] = {code, toTerminologicalLang(code)};
    for (const QString& w : wanted)
    {
        for (int i = 0; i < ui->langComboBox->count(); ++i)
        {
            const QString addr = ui->langComboBox->itemData(i).toString();
            if (!addr.isEmpty() && w == addr)
                return i;
        }
    }
    return 0;
}

bool TsMuxerWindow::isLangCodeForMeta(const QString& code)
{
    // tsMuxeR stops a job whose lang= is not three letters. A track's code comes from what tsMuxeR read in its
    // source, and a source can carry anything there: three bytes of a transport stream or any three characters of a
    // Matroska file. Such a code is left out of the meta line, so tsMuxeR goes on taking the source's own language, as
    // before.
    if (code.size() != 3)
        return false;
    for (const QChar c : code)
        if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))))
            return false;
    return true;
}

QString TsMuxerWindow::langForTrack(const QComboBox* box, const QString& current)
{
    // Every change of any option on a track used to write the language box back into the track.
    // When the box could not show the track's code, which it could not for a bibliographic code
    // such as "ger" or for any code missing from the list, it showed "und", and ticking an unrelated
    // box such as Bind to video FPS then turned the track's language into "und" in the meta, and so
    // on the disc. The track keeps its own code as long as the box still shows the language that
    // code stands for; only a language chosen in the box replaces it.
    if (box->currentIndex() == findLangByCode(current))
        return current;
    const QString chosen = box->itemData(box->currentIndex()).toString();
    // A language picked by hand is written in the form chosen on the General tab.
    if (ui->langCodesComboBox->currentData().toString() == QLatin1String("bibliographic"))
        return toBibliographicLang(chosen);
    return chosen;
}

QtvCodecInfo* TsMuxerWindow::getCodecInfo(int idx)
{
    auto iCodec = ui->trackLV->item(idx, 0)->data(Qt::UserRole).toLongLong();
    return reinterpret_cast<QtvCodecInfo*>(iCodec);
}

QtvCodecInfo* TsMuxerWindow::getCurrentCodec()
{
    auto row = ui->trackLV->currentRow();
    if (row == -1)
        return nullptr;
    return getCodecInfo(row);
}

void TsMuxerWindow::onVideoComboBoxChanged(int)
{
    if (disableUpdatesCnt)
        return;
    QtvCodecInfo* codecInfo = getCurrentCodec();
    if (!codecInfo)
        return;
    codecInfo->fpsText = ui->comboBoxFPS->itemText(ui->comboBoxFPS->currentIndex());
    codecInfo->levelText = ui->comboBoxLevel->itemText(ui->comboBoxLevel->currentIndex());
    codecInfo->arText = ui->comboBoxAR->currentData().toString();
    codecInfo->lang = langForTrack(ui->videoLangComboBox, codecInfo->lang);
    ui->trackLV->item(ui->trackLV->currentRow(), 3)->setText(codecInfo->lang);
    updateMetaLines();
}

void TsMuxerWindow::onVideoCheckBoxChanged()
{
    if (disableUpdatesCnt)
        return;
    QtvCodecInfo* codecInfo = getCurrentCodec();
    if (codecInfo == nullptr)
        return;
    ui->comboBoxFPS->setEnabled(ui->checkFPS->isChecked());
    codecInfo->checkFPS = ui->checkFPS->isChecked();
    ui->comboBoxLevel->setEnabled(ui->checkBoxLevel->isChecked());
    codecInfo->checkLevel = ui->checkBoxLevel->isChecked();
    codecInfo->addSEIMethod = ui->comboBoxSEI->currentIndex();
    codecInfo->addSPS = ui->checkBoxSPS->isChecked();
    if (ui->checkBoxHdr10Override->isEnabled())
        codecInfo->hdr10Override = ui->checkBoxHdr10Override->isChecked();
    codecInfo->isSecondary = ui->checkBoxSecondaryVideo->isChecked();
    colorizeCurrentRow(codecInfo);
    updateMetaLines();
}

void TsMuxerWindow::updateCurrentColor(int dr, int dg, int db, int row)
{
    if (row == -1)
        row = ui->trackLV->currentRow();
    if (row == -1)
        return;
    QColor color = ui->trackLV->palette().color(QPalette::Base);
    color.setRed(qBound(0, color.red() + dr, 255));
    color.setGreen(qBound(0, color.green() + dg, 255));
    color.setBlue(qBound(0, color.blue() + db, 255));
    for (int i = 0; i < 5; ++i)
    {
        QModelIndex index = ui->trackLV->model()->index(row, i);
        ui->trackLV->model()->setData(index, QBrush(color), Qt::BackgroundRole);
    }
}

bool TsMuxerWindow::isCodecIncompatibleWithFormat(const QString& programName) const
{
    // FLAC: only supported for MKV and Demux output
    if (programName == "A_FLAC")
        return !(ui->radioButtonMKV->isChecked() || ui->radioButtonDemux->isChecked());

    // ALAC: MKV output only. VP8, VP9 and Vorbis: MKV, or a demux into an IVF or an Ogg file. None has a form in a
    // transport stream or on a disc.
    if (programName == "A_ALAC" || programName == "V_MPEG4/ISO/ASP" || programName == "V_PRORES" ||
        programName == "S_ARIBSUB")
        return !ui->radioButtonMKV->isChecked();
    if (programName == "V_VP8" || programName == "V_VP9" || programName == "A_VORBIS")
        return !(ui->radioButtonMKV->isChecked() || ui->radioButtonDemux->isChecked());

    // Opus: supported for MKV, TS, and Demux output (not M2TS / Blu-ray)
    if (programName == "A_OPUS")
        return !(ui->radioButtonMKV->isChecked() || ui->radioButtonDemux->isChecked() ||
                 ui->radioButtonTS->isChecked());

    return false;
}

void TsMuxerWindow::colorizeCurrentRow(const QtvCodecInfo* codecInfo, int rowIndex)
{
    if (rowIndex == -1)
        rowIndex = ui->trackLV->currentRow();
    if (rowIndex == -1)
        return;

    // Check if this audio codec is incompatible with the selected output format
    const bool codecIncompat = isCodecIncompatibleWithFormat(codecInfo->programName);

    if (codecIncompat)
    {
        updateCurrentColor(-40, 0, 0, rowIndex);  // red tint

        // Update the codec column to show the warning
        QTableWidgetItem* codecItem = ui->trackLV->item(rowIndex, 2);
        if (codecItem && !codecItem->text().contains(tr("(incompatible)")))
            codecItem->setText(codecInfo->displayName + " " + tr("(incompatible)"));

        // Set a tooltip on the whole row, codec-specific message
        QString tip;
        if (codecInfo->programName == "A_ALAC" || codecInfo->programName == "V_MPEG4/ISO/ASP" ||
            codecInfo->programName == "V_PRORES" || codecInfo->programName == "S_ARIBSUB")
            tip = tr("%1 is only supported for MKV output. "
                     "This track will be ignored for the selected output format.")
                      .arg(codecInfo->displayName);
        else if (codecInfo->programName == "A_FLAC" || codecInfo->programName == "A_VORBIS" ||
                 codecInfo->programName == "V_VP8" || codecInfo->programName == "V_VP9")
            tip = tr("%1 is only supported for MKV and Demux output. "
                     "This track will be ignored for the selected output format.")
                      .arg(codecInfo->displayName);
        else
            tip = tr("%1 is not supported for M2TS or Blu-ray output. "
                     "This track will be ignored for the selected output format.")
                      .arg(codecInfo->displayName);
        for (int col = 0; col < ui->trackLV->columnCount(); ++col)
        {
            QTableWidgetItem* item = ui->trackLV->item(rowIndex, col);
            if (item)
                item->setToolTip(tip);
        }
    }
    else
    {
        if (codecInfo->isSecondary)
            updateCurrentColor(-40, 0, 0, rowIndex);
        else
            updateCurrentColor(0, 0, 0, rowIndex);

        // Restore the codec column text if it had the warning
        QTableWidgetItem* codecItem = ui->trackLV->item(rowIndex, 2);
        if (codecItem && codecItem->text().contains(tr("(incompatible)")))
            codecItem->setText(codecInfo->displayName);

        // Clear tooltip
        for (int col = 0; col < ui->trackLV->columnCount(); ++col)
        {
            QTableWidgetItem* item = ui->trackLV->item(rowIndex, col);
            if (item)
                item->setToolTip(QString());
        }
    }
}

void TsMuxerWindow::colorizeAllRows()
{
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        auto codecInfo = getCodecInfo(i);
        if (codecInfo)
            colorizeCurrentRow(codecInfo, i);
    }
}

void TsMuxerWindow::addTrackToDefaultComboBox(int trackRowIdx)
{
    auto codecInfo = getCodecInfo(trackRowIdx);
    auto text = getComboBoxTrackText(trackRowIdx, *codecInfo);
    if (codecInfo->programName.startsWith('A'))
    {
        ui->defaultAudioTrackComboBox->addItem(text, trackRowIdx);
    }
    else if (codecInfo->programName.startsWith('S'))
    {
        ui->defaultSubTrackComboBox->addItem(text, trackRowIdx);
    }
}

void TsMuxerWindow::removeTrackFromDefaultComboBox(QComboBox* targetComboBox, QCheckBox* targetCheckBox,
                                                   int comboBoxIdx, int trackRowIdx)
{
    if (targetComboBox->currentData().toInt() == trackRowIdx)
    {
        targetCheckBox->setChecked(false);
    }
    targetComboBox->removeItem(comboBoxIdx);
}

static void fixupIndices(QComboBox* comboBox, int removedTrackIdx)
{
    for (int i = 0; i < comboBox->count(); ++i)
    {
        auto trackIdx = comboBox->itemData(i).toInt();
        if (trackIdx > removedTrackIdx)
        {
            comboBox->setItemData(i, trackIdx - 1);
        }
    }
}

void TsMuxerWindow::removeTrackFromDefaultComboBox(int trackRowIdx)
{
    auto comboBoxIdx = ui->defaultAudioTrackComboBox->findData(trackRowIdx);
    if (comboBoxIdx != -1)
    {
        removeTrackFromDefaultComboBox(ui->defaultAudioTrackComboBox, ui->defaultAudioTrackCheckBox, comboBoxIdx,
                                       trackRowIdx);
    }
    comboBoxIdx = ui->defaultSubTrackComboBox->findData(trackRowIdx);
    if (comboBoxIdx != -1)
    {
        removeTrackFromDefaultComboBox(ui->defaultSubTrackComboBox, ui->defaultSubTrackCheckBox, comboBoxIdx,
                                       trackRowIdx);
    }
    fixupIndices(ui->defaultAudioTrackComboBox, trackRowIdx);
    fixupIndices(ui->defaultSubTrackComboBox, trackRowIdx);
}

void TsMuxerWindow::updateTracksComboBox(QComboBox* comboBox)
{
    for (int i = 0; i < comboBox->count(); ++i)
    {
        auto trackRowIdx = comboBox->itemData(i).toInt();
        auto codecInfo = getCodecInfo(trackRowIdx);
        comboBox->setItemText(i, getComboBoxTrackText(trackRowIdx, *codecInfo));
    }
}

void TsMuxerWindow::moveTrackInDefaultComboBox(int oldTrackRowIdx, int newTrackRowIdx)
{
    auto currentSubTrack = ui->defaultSubTrackComboBox->currentData();
    auto currentAudioTrack = ui->defaultAudioTrackComboBox->currentData();
    ui->defaultSubTrackComboBox->clear();
    ui->defaultAudioTrackComboBox->clear();
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        addTrackToDefaultComboBox(i);
    }
    postMoveComboBoxUpdate(ui->defaultAudioTrackComboBox, currentAudioTrack, oldTrackRowIdx, newTrackRowIdx);
    postMoveComboBoxUpdate(ui->defaultSubTrackComboBox, currentSubTrack, oldTrackRowIdx, newTrackRowIdx);
}

void TsMuxerWindow::postMoveComboBoxUpdate(QComboBox* comboBox, const QVariant& preMoveIndex, int oldIndex,
                                           int newIndex)
{
    if (!preMoveIndex.isValid())
    {
        return;
    }
    auto curTrackIdx = preMoveIndex.toInt();
    if (curTrackIdx == oldIndex)
    {
        curTrackIdx = newIndex;
    }
    else if (curTrackIdx == newIndex)
    {
        curTrackIdx = oldIndex;
    }
    auto idx = comboBox->findData(curTrackIdx);
    Q_ASSERT(idx != -1);
    comboBox->setCurrentIndex(idx);
}

void TsMuxerWindow::setUiMetaItemsData()
{
    // unfortunately, the .ui files don't allow the user to specify "item data" for combo boxes, which is the most
    // convenient way to associate some extra data that's not displayed in the UI in a combo box item without having
    // to resort to some kind of external containers which need to be kept synchronised.
    // as some of the combo boxes are taken as input for the meta file, it would end up having translated strings in it
    // if a non-English translation is active, and thus being invalid. item data for these UI items should always
    // contain the valid metafile tokens, as they are the actual things incorporated into the metafile content.
    ui->comboBoxAR->setItemData(0, QString());
    ui->comboBoxAR->setItemData(1, "1:1");
    ui->comboBoxAR->setItemData(2, "4:3");
    ui->comboBoxAR->setItemData(3, "16:9");
    ui->comboBoxAR->setItemData(4, "2.21:1");
    ui->comboBoxMeasure->setItemData(0, "KB");
    ui->comboBoxMeasure->setItemData(1, "KiB");
    ui->comboBoxMeasure->setItemData(2, "MB");
    ui->comboBoxMeasure->setItemData(3, "MiB");
    ui->comboBoxMeasure->setItemData(4, "GB");
    ui->comboBoxMeasure->setItemData(5, "GiB");
}

void TsMuxerWindow::onAudioSubtitlesParamsChanged()
{
    if (disableUpdatesCnt)
        return;
    QtvCodecInfo* codecInfo = getCurrentCodec();
    if (!codecInfo)
        return;
    codecInfo->bindFps = ui->checkBoxKeepFps->isChecked();
    codecInfo->dtsDownconvert = ui->dtsDwnConvert->isChecked();
    codecInfo->dropAc3Core = ui->dropAc3CoreCheckBox->isChecked();
    codecInfo->splitAc3Core = ui->splitAc3CoreCheckBox->isChecked();
    codecInfo->w64 = ui->w64CheckBox->isChecked();
    codecInfo->isSecondary = ui->secondaryCheckBox->isChecked();
    // The two are opposite operations on the same track: one keeps the core and drops the
    // lossless part, the other drops the core and keeps it. Ticking either clears the other, and
    // the box has to be cleared as well as the flag or the window shows both of them ticked while
    // only one reaches the meta file.
    if (codecInfo->dtsDownconvert && codecInfo->dropAc3Core)
    {
        QCheckBox* const loser = sender() == ui->dropAc3CoreCheckBox ? ui->dtsDwnConvert : ui->dropAc3CoreCheckBox;
        if (sender() == ui->dropAc3CoreCheckBox)
            codecInfo->dtsDownconvert = false;
        else
            codecInfo->dropAc3Core = false;
        const bool blocked = loser->blockSignals(true);
        loser->setChecked(false);
        loser->blockSignals(blocked);
    }
    codecInfo->offsetId = ui->offsetsComboBox->currentIndex() - 1;
    codecInfo->lang = langForTrack(ui->langComboBox, codecInfo->lang);

    // TrueHD + AC-3 merge (CLI meta parameters on A_MLP only)
    if (codecInfo->programName == "A_MLP")
    {
        // A number the user puts in the box, 0 included, is theirs: the window no longer sets it.
        if (ui->mergeAc3TrackSpinBox->value() != codecInfo->mergeAc3Track)
            codecInfo->mergeAc3Auto = false;
        codecInfo->mergeAc3Track = ui->mergeAc3TrackSpinBox->value();
        codecInfo->mergeAc3File = ui->mergeAc3FileLineEdit->text().trimmed();
        updateMergeAc3Exclusion(codecInfo);
        updateMergeAc3Suffix();
    }
    else
    {
        codecInfo->mergeAc3Track = 0;
        codecInfo->mergeAc3File.clear();
    }

    ui->trackLV->item(ui->trackLV->currentRow(), 3)->setText(codecInfo->lang);
    colorizeCurrentRow(codecInfo);

    // The two default track entries are built once, when the track is added, and carry the
    // language as it stood then. A track read out of a container arrives with one and looks
    // right; a subtitle file carries none, so its entry was made without one and kept saying
    // nothing however often the language was changed afterwards. The same staleness could show
    // an out of date language rather than none at all. Rebuilding the texts costs two loops
    // over a handful of items, and setItemText leaves the current index alone, so nothing else
    // is disturbed.
    updateTracksComboBox(ui->defaultAudioTrackComboBox);
    updateTracksComboBox(ui->defaultSubTrackComboBox);

    updateMetaLines();
}

// merge-ac3-track and merge-ac3-file are mutually exclusive: metaDemuxer.cpp refuses a track line
// carrying both, in two separate places, so only one of them can ever reach a mux.
//
// This used to be enforced by WIPING the file field the moment a track was set. The field stayed
// enabled the whole time, so it took focus and accepted keystrokes and threw them away without a
// word: a path typed with a track already set vanished as it was entered, a path typed FIRST was
// deleted when a track was set afterwards, and clearing the track again did not bring it back.
//
// Grey out the one that does not apply instead. The exclusion becomes visible, nothing anyone types
// is ever discarded, and clearing whichever control is in use re-enables the other, so there is
// always a way back.
void TsMuxerWindow::updateMergeAc3Exclusion(QtvCodecInfo* codecInfo)
{
    const bool isTrueHd =
        codecInfo != nullptr && codecInfo->programName == "A_MLP" && codecInfo->displayName == QString("TRUE-HD");
    const bool showTrack = isTrueHd && codecInfo->trackID != 0;
    const bool showFile = isTrueHd;

    bool trackSet = showTrack && ui->mergeAc3TrackSpinBox->value() > 0;
    bool fileSet = showFile && !ui->mergeAc3FileLineEdit->text().trimmed().isEmpty();

    // Both at once is only reachable from a meta file that already carries both, which the engine
    // would refuse anyway. Resolve it the way the meta writer does, by preferring the track, so
    // neither control is left disabled with no way out of the state.
    if (trackSet && fileSet)
    {
        ui->mergeAc3FileLineEdit->blockSignals(true);
        ui->mergeAc3FileLineEdit->setText(QString());
        ui->mergeAc3FileLineEdit->blockSignals(false);
        codecInfo->mergeAc3File.clear();
        fileSet = false;
    }

    ui->mergeAc3TrackSpinBox->setEnabled(showTrack && !fileSet);
    ui->mergeAc3FileLineEdit->setEnabled(showFile && !trackSet);
    ui->mergeAc3FileBrowseButton->setEnabled(showFile && !trackSet);
}

void TsMuxerWindow::onEditDelayChanged(int)
{
    if (disableUpdatesCnt)
        return;
    QtvCodecInfo* codecInfo = getCurrentCodec();
    if (!codecInfo)
        return;
    codecInfo->delay = ui->editDelay->value();
    updateMetaLines();
}

void TsMuxerWindow::onPulldownCheckBoxChanged()
{
    if (disableUpdatesCnt)
        return;
    QtvCodecInfo* codecInfo = getCurrentCodec();
    if (!codecInfo)
        return;
    if (ui->checkBoxRemovePulldown->isEnabled())
    {
        if (ui->checkBoxRemovePulldown->isChecked())
        {
            codecInfo->delPulldown = 1;
            if (codecInfo->fpsTextOrig == "29.97")
            {
                ui->checkFPS->setChecked(true);
                ui->checkFPS->setEnabled(true);
                codecInfo->checkFPS = true;
                ui->comboBoxFPS->setEnabled(true);
                ui->comboBoxFPS->setCurrentIndex(3);
                codecInfo->fpsText = "24000/1001";
                setComboBoxText(ui->comboBoxFPS, "24000/1001");
            }
        }
        else
            codecInfo->delPulldown = 0;
    }
    else
        codecInfo->delPulldown = -1;
    updateMetaLines();
}

void TsMuxerWindow::addFiles(const QList<QUrl>& files)
{
    addFileList.clear();
    addFileList = files;
    addFile();
}

void TsMuxerWindow::onAddBtnClick()
{
    showAddFilesDialog(tr("Add media files"), [this]() { addFile(); });
}

void TsMuxerWindow::addFile()
{
    processAddFileList(&TsMuxerWindow::continueAddFile, &TsMuxerWindow::fileAdded, &TsMuxerWindow::addFile);
}

// The titles of the disc being added, as tsMuxeR lists them for the disc given alone
void TsMuxerWindow::listDiscTitles()
{
    newFileName = m_discSource;  // the name a refusal shows
    disconnect();
    connect(this, &TsMuxerWindow::tsMuxerSuccessFinished, this, &TsMuxerWindow::onDiscTitlesReceived);
    runInMuxMode = false;
    m_runOutput.clear();
    setProbeBusy(true);
    QStringList args;
    if (m_discOthersShown)
        args << QStringLiteral("--all-playlists");
    tsMuxerExecute(args << m_discSource);
}

// "Choose a title", then the title chosen is listed as a source: a Blu-ray title as its playlist, exactly as when the
// playlist itself is added, and a DVD title through --show-title, whose tracks then name the disc and the title's disc
// number (dvd-title=), which a saved meta file keeps whatever a later listing makes of the disc.
void TsMuxerWindow::onDiscTitlesReceived()
{
    const DiscTitleList list = parseDiscTitles(procStdOutput);
    const auto next = [this]()
    {
        if (!addFileList.isEmpty() && m_addNextFile)
            (this->*m_addNextFile)();
    };
    if (list.titles.isEmpty())
    {
        QStringList lines;
        for (const QString& line : procStdOutput)
            if (!line.trimmed().isEmpty() && !line.startsWith(QLatin1String("tsMuxeR version")))
                lines << line.trimmed();
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("No title"));
        msgBox.setText(tr("The disc \"%1\" has no title to add.").arg(QDir::toNativeSeparators(m_discSource)));
        msgBox.setInformativeText(lines.join('\n'));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.exec();
        next();
        return;
    }
    DiscTitleDialog dialog(m_discSource, list, m_discOthersShown, this);
    const int result = dialog.exec();
    if (result == DiscTitleDialog::ShowOthersChanged)
    {
        m_discOthersShown = !m_discOthersShown;
        listDiscTitles();
        return;
    }
    const int chosen = result == QDialog::Accepted ? dialog.chosen() : -1;
    if (chosen < 0)
    {
        next();
        return;
    }
    const DiscTitleRow& row = list.titles[chosen];
    m_pendingAngle = dialog.chosenAngle();
    m_pendingAngleClips.clear();
    if (m_pendingAngle > 1 && m_pendingAngle - 1 < row.angleClips.size() && !row.angleClips.isEmpty())
        for (int k = 0; k < row.angleClips[0].size() && k < row.angleClips[m_pendingAngle - 1].size(); ++k)
            m_pendingAngleClips << qMakePair(row.angleClips[0][k], row.angleClips[m_pendingAngle - 1][k]);
    QStringList args;
    if (list.dvd)
    {
        newFileName = QDir::toNativeSeparators(m_discSource);
        if (!checkFileDuplicate(newFileName, row.discTitle))
        {
            next();
            return;
        }
        m_pendingDvdTitle = row.discTitle;
        m_pendingDvdSeconds = row.seconds;
        // the number counts in the listing the title was chosen from
        if (m_discOthersShown)
            args << QStringLiteral("--all-playlists");
        args << QStringLiteral("--show-title=%1").arg(row.number);
    }
    else
    {
        newFileName = discPlaylistPath(m_discSource, row.playlist);
        if (!checkFileDuplicate(newFileName))
        {
            next();
            return;
        }
    }
    disconnect();
    connect(this, &TsMuxerWindow::tsMuxerSuccessFinished, this, &TsMuxerWindow::onTsMuxerCodecInfoReceived);
    connect(this, &TsMuxerWindow::codecListReady, this, &TsMuxerWindow::continueAddFile);
    connect(this, &TsMuxerWindow::fileAdded, this, &TsMuxerWindow::addFile);
    runInMuxMode = false;
    m_runOutput.clear();
    setProbeBusy(true);
    tsMuxerExecute(args << newFileName);
}

bool TsMuxerWindow::checkFileDuplicate(const QString& fileName, const int dvdTitle)
{
    QString t = myUnquoteStr(fileName).trimmed();
    for (int i = 0; i < ui->inputFilesLV->count(); ++i)
        if (myUnquoteStr(ui->inputFilesLV->item(i)->data(FileNameRole).toString()) == t &&
            ui->inputFilesLV->item(i)->data(DvdTitleRole).toInt() == dvdTitle)
        {
            QMessageBox msgBox(this);
            msgBox.setWindowTitle(tr("File already exists"));
            msgBox.setText(dvdTitle ? tr("DVD title %1 of \"%2\" is already added").arg(dvdTitle).arg(fileName)
                                    : tr("File \"%1\" already exists").arg(fileName));
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setStandardButtons(QMessageBox::Ok);
            msgBox.exec();
            return false;
        }
    return true;
}

void TsMuxerWindow::setComboBoxText(QComboBox* comboBox, const QString& text)
{
    for (int k = 0; k < comboBox->count(); ++k)
    {
        if (comboBox->itemText(k) == text)
        {
            comboBox->setCurrentIndex(k);
            return;
        }
    }
    comboBox->addItem(text);
    comboBox->setCurrentIndex(comboBox->count() - 1);
}

// The Matroska page of the selected track: its name and flags, usable when the output is Matroska.
void TsMuxerWindow::showMkvTrackOptions(const QtvCodecInfo* codecInfo)
{
    ui->tabWidgetTracks->addTab(ui->tabSheetMkv, TI_MKV_TAB_NAME());
    ui->mkvTrackNameEdit->setText(codecInfo->trackName);
    QCheckBox* const boxes[6] = {ui->mkvFlagForced,   ui->mkvFlagHearing,  ui->mkvFlagVisual,
                                 ui->mkvFlagTextDesc, ui->mkvFlagOriginal, ui->mkvFlagCommentary};
    for (int k = 0; k < 6; ++k) boxes[k]->setChecked(codecInfo->mkvFlags[k]);
    updateMkvTrackOptionsEnabled();
}

void TsMuxerWindow::updateMkvOutputOptions()
{
    const bool mkv = ui->radioButtonMKV->isChecked();
    ui->mkvTitleLabel->setVisible(mkv);
    ui->mkvTitleEdit->setVisible(mkv);
    ui->mkvKeepEditionsCheck->setVisible(mkv);
}

void TsMuxerWindow::updateDiscMetaOptions()
{
    const bool bluRay = ui->radioButtonBluRay->isChecked() || ui->radioButtonBluRayISO->isChecked();
    const bool fromFolder = ui->comboDiscMeta->currentIndex() == 1;
    ui->discMetaLabel->setVisible(bluRay);
    ui->comboDiscMeta->setVisible(bluRay);
    ui->discMetaDirEdit->setVisible(bluRay && fromFolder);
    ui->btnDiscMetaBrowse->setVisible(bluRay && fromFolder);
}

void TsMuxerWindow::updateMkvTrackOptionsEnabled()
{
    const bool mkv = ui->radioButtonMKV->isChecked();
    for (QWidget* w : {static_cast<QWidget*>(ui->mkvTrackNameLabel), static_cast<QWidget*>(ui->mkvTrackNameEdit),
                       static_cast<QWidget*>(ui->mkvFlagForced), static_cast<QWidget*>(ui->mkvFlagHearing),
                       static_cast<QWidget*>(ui->mkvFlagVisual), static_cast<QWidget*>(ui->mkvFlagTextDesc),
                       static_cast<QWidget*>(ui->mkvFlagOriginal), static_cast<QWidget*>(ui->mkvFlagCommentary)})
        w->setEnabled(mkv);
}

void TsMuxerWindow::onMkvTrackOptionsChanged()
{
    if (disableUpdatesCnt)
        return;
    QtvCodecInfo* codecInfo = getCurrentCodec();
    if (!codecInfo)
        return;
    codecInfo->trackName = ui->mkvTrackNameEdit->text();
    const QCheckBox* const boxes[6] = {ui->mkvFlagForced,   ui->mkvFlagHearing,  ui->mkvFlagVisual,
                                       ui->mkvFlagTextDesc, ui->mkvFlagOriginal, ui->mkvFlagCommentary};
    for (int k = 0; k < 6; ++k) codecInfo->mkvFlags[k] = boxes[k]->isChecked();
    updateMetaLines();
}

void TsMuxerWindow::trackLVItemSelectionChanged()
{
    if (disableUpdatesCnt)
        return;
    while (ui->tabWidgetTracks->count()) ui->tabWidgetTracks->removeTab(0);
    if (ui->trackLV->currentRow() == -1)
    {
        ui->tabWidgetTracks->addTab(ui->tabSheetFake, TI_DEFAULT_TAB_NAME());
        return;
    }
    QtvCodecInfo* codecInfo = getCurrentCodec();
    if (!codecInfo)
        return;
    disableUpdatesCnt++;
    if (ui->trackLV->rowCount() >= 1)
    {
        if (isVideoCodec(codecInfo->displayName))
        {
            codecInfo->addSEIMethod = ui->comboBoxSEI->currentIndex();
            codecInfo->addSPS = ui->checkBoxSPS->isChecked();

            ui->tabWidgetTracks->addTab(ui->tabSheetVideo, TI_DEFAULT_TAB_NAME());
            showMkvTrackOptions(codecInfo);

            ui->checkFPS->setChecked(codecInfo->checkFPS);
            ui->checkBoxLevel->setChecked(codecInfo->checkLevel);
            ui->comboBoxFPS->setEnabled(ui->checkFPS->isChecked());
            ui->comboBoxLevel->setEnabled(ui->checkBoxLevel->isChecked());
            ui->comboBoxSEI->setCurrentIndex(codecInfo->addSEIMethod);
            ui->checkBoxSecondaryVideo->setChecked(codecInfo->isSecondary);
            ui->checkBoxSPS->setChecked(codecInfo->addSPS);
            ui->checkBoxHdr10Override->setChecked(codecInfo->hdr10Override);
            updateHdr10OverrideEnabled(codecInfo);
            ui->checkBoxRemovePulldown->setChecked(codecInfo->delPulldown == 1);
            ui->checkBoxRemovePulldown->setEnabled(codecInfo->delPulldown >= 0);

            setComboBoxText(ui->comboBoxFPS, codecInfo->fpsText);
            if (!codecInfo->arText.isEmpty())
                setComboBoxText(ui->comboBoxAR, codecInfo->arText);
            else
                ui->comboBoxAR->setCurrentIndex(0);
            // The level can be rewritten for HEVC as well, and has been since the muxer
            // gained it, but this box never offered it: an HEVC track greyed the control out
            // and the only way to ask was to type level= into the meta by hand.
            //
            // The SEI and SPS boxes below are NOT the same question, which is why they had to
            // be separated from it rather than enabled alongside. insertSEI, forceSEI and
            // contSPS are read on an AVC track line only and do nothing at all on HEVC, so
            // offering them there would promise something that cannot happen.
            const bool isAvcTrack = codecInfo->displayName == "H.264" || codecInfo->displayName == "MVC";
            ui->checkBoxLevel->setEnabled(isAvcTrack || codecInfo->displayName == "HEVC");
            // The list holds the levels of the track's own codec. It held the H.264 levels for both, without 5.2,
            // so an HEVC track was offered 1.1, 1.2, 1.3, 2.2, 3.2 and 4.2, which HEVC does not have, and the
            // muxer refuses them now. HEVC's are those of H.265 Table A.8.
            {
                static const QStringList avcLevels = {"1.0", "1.1", "1.2", "1.3", "2.0", "2.1", "2.2",
                                                      "3.0", "3.1", "3.2", "4.0", "4.1", "4.2", "5.0",
                                                      "5.1", "5.2", "6.0", "6.1", "6.2"};
                static const QStringList hevcLevels = {"1.0", "2.0", "2.1", "3.0", "3.1", "4.0", "4.1", "5.0", "5.1",
                                                       "5.2", "6.0", "6.1", "6.2", "6.3", "7.0", "7.1", "7.2"};
                const QStringList& levels = codecInfo->displayName == "HEVC" ? hevcLevels : avcLevels;
                QStringList shown;
                for (int k = 0; k < ui->comboBoxLevel->count(); ++k) shown << ui->comboBoxLevel->itemText(k);
                if (shown != levels)
                {
                    const QSignalBlocker block(ui->comboBoxLevel);
                    ui->comboBoxLevel->clear();
                    ui->comboBoxLevel->addItems(levels);
                }
                if (ui->checkBoxLevel->isEnabled() && levels.contains(codecInfo->levelText))
                    setComboBoxText(ui->comboBoxLevel, codecInfo->levelText);
                else
                    ui->comboBoxLevel->setCurrentIndex(0);
            }
            ui->comboBoxSEI->setEnabled(isAvcTrack);
            ui->checkBoxSPS->setEnabled(isAvcTrack);
            ui->comboBoxAR->setEnabled(codecInfo->displayName == "MPEG-2");
            ui->comboBoxAR->setEnabled(true);
            ui->labelAR->setEnabled(ui->comboBoxAR->isEnabled());
            ui->checkBoxSecondaryVideo->setEnabled(codecInfo->displayName != "MVC");
            ui->videoLangComboBox->setCurrentIndex(findLangByCode(codecInfo->lang));
        }
        else
        {
            ui->tabWidgetTracks->addTab(ui->tabSheetAudio, TI_DEFAULT_TAB_NAME());
            showMkvTrackOptions(codecInfo);

            // ** IT DOES NOT CONVERT ANYTHING. ** These tracks carry a plain core inside the HD
            // stream, and all this does is keep the core and drop the HD part. Nothing is decoded
            // and nothing is re-encoded. The old wording promised a conversion, so a track with no
            // core inside it looked as though it should work, and ticking the box changed nothing.
            if (codecInfo->displayName == "DTS-HD")
                ui->dtsDwnConvert->setText(tr("Keep the DTS core, drop the HD part"));
            else if (codecInfo->displayName == "TRUE-HD")
                ui->dtsDwnConvert->setText(tr("Keep the AC-3 core, drop the TRUE-HD part"));
            else if (codecInfo->displayName == "E-AC3 (DD+)")
                ui->dtsDwnConvert->setText(tr("Keep the AC-3 core, drop the DD+ part"));
            else
                ui->dtsDwnConvert->setText(tr("Keep the core, drop the HD part"));

            // ** AND IT IS ONLY OFFERED WHEN THERE IS A CORE TO KEEP. **
            //
            // The old test excluded only "(core 0Kbps)", which is what an E-AC3 track with no core
            // reports. A TRUE-HD track that carries no core at all says nothing about a core, so it
            // passed the test and the box was offered:
            //
            //   AC3 core + TRUE-HD. Peak bitrate: 5844Kbps (core 640Kbps)   has a core
            //   TRUE-HD + ATMOS. Peak bitrate: 7626Kbps                     has none, box offered
            //   EAC3 Bitrate: 640Kbps (core 0Kbps)                          has none, box refused
            //
            // Requiring the description to mention a core covers all three the same way. DTS-HD High Resolution
            // always carries a core, but its description does not say so, and the box was greyed for it.
            ui->dtsDwnConvert->setEnabled(
                (codecInfo->descr.contains("core") && !codecInfo->descr.contains("(core 0Kbps)") &&
                 (codecInfo->displayName == "DTS-HD" || codecInfo->displayName == "TRUE-HD" ||
                  codecInfo->displayName == "E-AC3 (DD+)")) ||
                (codecInfo->displayName == "DTS-HD" && codecInfo->descr.contains("High Resolution")));
            // Greyed for a TrueHD track without a core, as from a Matroska file, it did not say why, and the AC-3 that
            // the file usually has beside it was not pointed to
            ui->dtsDwnConvert->setToolTip(
                codecInfo->displayName == "TRUE-HD" && !ui->dtsDwnConvert->isEnabled()
                    ? wrapTip(tr("This TrueHD track carries no AC-3 core. If the file has an AC-3 track, take that one "
                                 "instead."))
                    : QString());
            // drop-ac3-core applies to the disc form of a TrueHD track, the one that carries an
            // AC-3 core beside the lossless part. A TrueHD track with no core has nothing to drop,
            // and the same description test the box above uses says which is which.
            const bool hasAc3Core = codecInfo->displayName == "TRUE-HD" && codecInfo->descr.contains("core") &&
                                    !codecInfo->descr.contains("(core 0Kbps)");
            ui->dropAc3CoreCheckBox->setEnabled(hasAc3Core);
            // Writing three files out of one track only means anything when the output IS files.
            // In every other mode there is one output and nothing to split it into.
            ui->splitAc3CoreCheckBox->setEnabled(hasAc3Core && ui->radioButtonDemux->isChecked());
            // Wave64 is a container for the audio written out, so like the box above it only
            // means anything when the output IS files, and only for LPCM.
            const bool isLpcm = codecInfo->programName == QLatin1String("A_LPCM");
            ui->w64CheckBox->setEnabled(isLpcm && ui->radioButtonDemux->isChecked());
            ui->secondaryCheckBox->setEnabled(codecInfo->descr.contains("(DTS Express)") ||
                                              codecInfo->descr.contains("(DTS Express 24bit)") ||
                                              codecInfo->displayName == "E-AC3 (DD+)");

            if (!ui->secondaryCheckBox->isEnabled())
                ui->secondaryCheckBox->setChecked(false);

            ui->langComboBox->setCurrentIndex(findLangByCode(codecInfo->lang));
            ui->offsetsComboBox->setCurrentIndex(codecInfo->offsetId + 1);
            ui->dtsDwnConvert->setVisible(codecInfo->displayName != "PGS" && codecInfo->displayName != "SRT");
            ui->secondaryCheckBox->setVisible(ui->dtsDwnConvert->isVisible());
            ui->dropAc3CoreCheckBox->setVisible(ui->dtsDwnConvert->isVisible());
            ui->splitAc3CoreCheckBox->setVisible(ui->dtsDwnConvert->isVisible());
            ui->w64CheckBox->setVisible(isLpcm);
            const bool isTrueHd = (codecInfo->programName == "A_MLP" && codecInfo->displayName == "TRUE-HD");
            const bool showMergeTrack = (isTrueHd && codecInfo->trackID != 0);
            const bool showMergeFile = isTrueHd;

            ui->mergeAc3TrackLabel->setVisible(showMergeTrack);
            ui->mergeAc3TrackSpinBox->setVisible(showMergeTrack);
            ui->mergeAc3TrackSpinBox->setEnabled(showMergeTrack);
            if (showMergeTrack)
            {
                ui->mergeAc3TrackSpinBox->setValue(codecInfo->mergeAc3Track);
                updateMergeAc3Suffix();
            }
            else
                ui->mergeAc3TrackSpinBox->setValue(0);

            ui->mergeAc3FileLabel->setVisible(showMergeFile);
            ui->mergeAc3FileLineEdit->setVisible(showMergeFile);
            ui->mergeAc3FileBrowseButton->setVisible(showMergeFile);
            ui->mergeAc3FileLineEdit->setEnabled(showMergeFile);
            ui->mergeAc3FileBrowseButton->setEnabled(showMergeFile);
            if (showMergeFile)
                ui->mergeAc3FileLineEdit->setText(codecInfo->mergeAc3File);
            else
                ui->mergeAc3FileLineEdit->setText(QString());

            // Both controls have their values now, so settle which of the two applies. Selecting a
            // track has to do this as well as editing one, or a track that arrives with a merge
            // already set would show both controls enabled.
            updateMergeAc3Exclusion(codecInfo);

            bool isPGS = codecInfo->displayName == "PGS";
            ui->checkBoxKeepFps->setVisible(isPGS);
            // a text subtitle is drawn into a PG stream, which a 3D disc places by a plane as well
            const bool hasPlane = isPGS || codecInfo->displayName == "SRT";
            ui->offsetsLabel->setVisible(hasPlane);
            ui->offsetsComboBox->setVisible(hasPlane);

            ui->imageSubtitles->setVisible(!ui->dtsDwnConvert->isVisible());
            ui->imageAudio->setVisible(ui->dtsDwnConvert->isVisible());

            ui->editDelay->setValue(codecInfo->delay);
            ui->dtsDwnConvert->setChecked(codecInfo->dtsDownconvert);
            ui->dropAc3CoreCheckBox->setChecked(codecInfo->dropAc3Core);
            ui->splitAc3CoreCheckBox->setChecked(codecInfo->splitAc3Core);
            ui->w64CheckBox->setChecked(codecInfo->w64);
            ui->secondaryCheckBox->setChecked(codecInfo->isSecondary);
            ui->checkBoxKeepFps->setChecked(codecInfo->bindFps);
            ui->editDelay->setEnabled(!ui->radioButtonDemux->isChecked());
        }
    }

    disableUpdatesCnt--;
    trackLVItemChanged(0);
}

void TsMuxerWindow::trackLVItemChanged(QTableWidgetItem* item)
{
    if (disableUpdatesCnt > 0)
        return;

    Q_UNUSED(item);
    // Before updateMetaLines, because unticking a layer can take the Dolby Vision selector away and
    // the meta is written from whether that selector is showing.
    updateDvProfileVisible();
    updateAutoCoreMerge();  // a TrueHD row ticked or unticked takes its core with it
    updateMetaLines();
    updateMux3dLabel();  // a ticked or unticked 3D track makes the disc 3D or 2D
    ui->moveupBtn->setEnabled(ui->trackLV->currentItem() != 0);
    ui->movedownBtn->setEnabled(ui->trackLV->currentItem() != 0);
    ui->removeTrackBtn->setEnabled(ui->trackLV->currentItem() != 0);
    updateRemoveAllEnabled();
    if (ui->trackLV->rowCount() == 0)
        oldFileName.clear();

    disableUpdatesCnt++;
    bool checkedExist = false;
    bool uncheckedExist = false;
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        if (ui->trackLV->item(i, 0)->checkState() == Qt::Checked)
            checkedExist = true;
        else
            uncheckedExist = true;
    }
    if (checkedExist && uncheckedExist)
        m_header->setCheckState(Qt::PartiallyChecked);
    else if (checkedExist)
        m_header->setCheckState(Qt::Checked);
    else
        m_header->setCheckState(Qt::Unchecked);
    update();
    disableUpdatesCnt--;
}

// "Start muxing" with an empty track list ran tsMuxeR only for it to stop with "No tracks selected". The button is
// greyed until the list has a track, and while tsMuxeR runs. A meta file opened or written by hand is muxed as it is,
// with no track in the list: then a track line in the meta box is enough.
void TsMuxerWindow::updateMuxButton()
{
    bool hasTrack = ui->trackLV->rowCount() > 0;
    if (!hasTrack && m_metaUserOverride)
    {
        const QStringList lines = ui->memoMeta->toPlainText().split('\n');
        for (const QString& line : lines)
        {
            const QString t = line.trimmed();
            if (!t.isEmpty() && !t.startsWith(QLatin1String("MUXOPT")) && !t.startsWith('#'))
            {
                hasTrack = true;
                break;
            }
        }
    }
    ui->buttonMux->setEnabled(hasTrack && proc.state() == QProcess::NotRunning);
}

static const QDir::Filters ALL_ENTRIES = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;

// tsMuxeR removes what a failed job wrote, but a job stopped with Abort is ended at once (QProcess::kill), so it
// cannot: the output that was being written stayed. The window removes it, and only what this run made. So what was
// there is noted before the start, and tsMuxeR's own names for what it writes are used: the output itself, its split
// parts, names that begin with the output's name and a dot (an image's .layerbreak.txt and --inner-only folder), and
// the temporary names ".tsmuxer-new-<process id>" that stand for an output that was there before. A folder output
// that was there keeps everything but the new BDMV and CERTIFICATE folders of a disc and the temporary folder.
void TsMuxerWindow::noteRunOutput(const QString& output, bool discFolder)
{
    const QFileInfo out(QDir::cleanPath(QFileInfo(unquoteStr(output.trimmed())).absoluteFilePath()));
    m_runOutput = out.absoluteFilePath();
    m_runOutputExisted = out.exists();
    m_runDiscFolder = discFolder && !ui->radioButtonDemux->isChecked();
    m_runDirBefore = out.absoluteDir().entryList(ALL_ENTRIES);
    m_runOutputBefore = out.isDir() ? QDir(m_runOutput).entryList(ALL_ENTRIES) : QStringList();
    m_runMissingFolders.clear();
    // QDir::cdUp() does not go into a folder that is not there, so the names are taken apart instead
    for (QString folder = out.absolutePath(); !QFileInfo::exists(folder);)
    {
        m_runMissingFolders << folder;
        const QString up = QFileInfo(folder).absolutePath();
        if (up == folder)
            break;
        folder = up;
    }
}

QString TsMuxerWindow::removeUnfinishedOutput()
{
    if (m_runOutput.isEmpty())
        return QString();
    const QFileInfo out(m_runOutput);
    if (out.fileName().isEmpty())  // the root of a drive: nothing there is told apart safely
    {
        m_runOutput.clear();
        return tr("The unfinished output was kept: %1").arg(QDir::toNativeSeparators(out.absoluteFilePath()));
    }
    const QString tempTag = QLatin1String(".tsmuxer-new-") + QString::number(m_runPid);
    const QRegularExpression splitPart("^" + QRegularExpression::escape(out.completeBaseName() + ".split.") +
                                           "[0-9]+\\." + QRegularExpression::escape(out.suffix()) + "$",
                                       QRegularExpression::CaseInsensitiveOption);
    QStringList removed;
    QStringList kept;
    const auto removeEntry = [&](const QString& path)
    {
        const QFileInfo fi(path);
        if (fi.isDir() && !fi.isSymLink() ? QDir(path).removeRecursively() : QFile::remove(path))
            removed << path;
        else
            kept << QDir::toNativeSeparators(path);
    };
    const QDir dir = out.absoluteDir();
    for (const QString& name : dir.entryList(ALL_ENTRIES))
        if (!m_runDirBefore.contains(name) && (name == out.fileName() || name.startsWith(out.fileName() + '.') ||
                                               name.contains(tempTag) || splitPart.match(name).hasMatch()))
            removeEntry(dir.filePath(name));
    if (m_runOutputExisted && out.isDir())
    {
        const QDir outDir(m_runOutput);
        for (const QString& name : outDir.entryList(ALL_ENTRIES))
            if (!m_runOutputBefore.contains(name) &&
                (name.contains(tempTag) || (m_runDiscFolder && (name == "BDMV" || name == "CERTIFICATE"))))
                removeEntry(outDir.filePath(name));
    }
    for (const QString& folder : m_runMissingFolders) QDir().rmdir(folder);  // only when empty
    m_runOutput.clear();
    if (!kept.isEmpty())
        return tr("The unfinished output was kept: %1").arg(kept.join(", "));
    if (!removed.isEmpty())
        return tr("The unfinished output was removed.");
    return tr("No output was written.");
}

void TsMuxerWindow::onMuxStopped()
{
    const QString outputLine = removeUnfinishedOutput();
    muxForm->showStopped(ui->radioButtonDemux->isChecked() ? tr("The demux was stopped.") : tr("The mux was stopped."),
                         outputLine);
}

// The file of an item of the input list a sample can be made of (--make-sample): a file, also a clip of a playlist, and
// not a playlist, a DVD title, a disc image or a folder, whose titles a sample does not follow yet. Empty for none.
static QString sampleSource(const QListWidgetItem* itm)
{
    if (itm == nullptr || itm->data(MplsItemRole).toInt() == MPLS_PRIMARY || itm->data(DvdTitleRole).toInt() != 0)
        return {};
    const QString name = myUnquoteStr(itm->data(FileNameRole).toString());
    const QFileInfo fi(name);
    const QString ext = fi.suffix().toLower();
    if (name.isEmpty() || !fi.isFile() || ext == "iso" || ext == "img" || ext == "udf" || ext == "mpls")
        return {};
    return name;
}

void TsMuxerWindow::inputFilesLVChanged()
{
    QListWidgetItem* itm = ui->inputFilesLV->currentItem();
    ui->sampleBtn->setEnabled(sampleSource(itm).size() > 0);
    if (!itm)
    {
        ui->btnAppend->setEnabled(false);
        ui->removeFile->setEnabled(false);
        return;
    }
    // A DVD title is not joined: its meta line names the disc and the title, which a join of files cannot carry
    ui->btnAppend->setEnabled(itm->data(MplsItemRole).toInt() != MPLS_M2TS && !itm->data(DvdTitleRole).toInt() &&
                              proc.state() == QProcess::NotRunning);
    ui->removeFile->setEnabled(itm->data(MplsItemRole).toInt() != MPLS_M2TS);
}

// A film's name as a file name: a colon, which a Windows name cannot have, becomes a dash
static QString outputNameOfTitle(const QString& title)
{
    QString name = title;
    name.replace(QStringLiteral(": "), QStringLiteral(" - "));
    name.replace(QLatin1Char(':'), QLatin1Char('-'));
    name.replace(QLatin1Char('/'), QLatin1Char('-'));
    name.replace(QLatin1Char('\\'), QLatin1Char('-'));
    for (const QChar c : QStringLiteral("*?\"<>|")) name.remove(c);
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) name.chop(1);
    return name;
}

// baseName: the name of the output when the source's own does not fit, as for a DVD title
void TsMuxerWindow::modifyOutFileName(const QString fileName, const QString& baseName)
{
    QFileInfo fi(unquoteStr(fileName));
    QString name = baseName.isEmpty() ? fi.completeBaseName() : baseName;

    QString existingName = QDir::toNativeSeparators(ui->outFileName->text());
    QFileInfo fiDst(existingName);
    if (fiDst.completeBaseName() == "default" || existingName.isEmpty())
    {
        QString dstPath;
        if (!existingName.isEmpty() && existingName.contains(QDir::separator()))
            dstPath = QDir::toNativeSeparators(fiDst.absolutePath());
        else
            dstPath = getOutputDir();

        // The output folder of the window already ends in a separator, and adding another one in front of the name
        // showed two of them in the field. The folder is closed once, whichever way it came.
        const QString dir = QDir::toNativeSeparators(closeDirPath(dstPath));
        if (ui->radioButtonTS->isChecked())
            ui->outFileName->setText(dir + name + ".ts");
        else if (ui->radioButtonM2TS->isChecked())
            ui->outFileName->setText(dir + name + ".m2ts");
        else if (ui->radioButtonMKV->isChecked())
            ui->outFileName->setText(dir + name + ".mkv");
        else if (ui->radioButtonBluRayISO->isChecked())
            ui->outFileName->setText(dir + name + ".iso");
        else
            ui->outFileName->setText(dstPath);
        if (baseName.isEmpty() && isSameFile(ui->outFileName->text(), fileName))
            ui->outFileName->setText(dir + name + "_new." + fi.suffix());
    }
}

void TsMuxerWindow::continueAddFile()
{
    double fps;
    double level;
    disableUpdatesCnt++;
    bool firstWarn = true;
    int firstAddedIndex = -1;
    for (int i = 0; i < codecList.size(); ++i)
    {
        if (codecList[i].displayName == "PGS (depended view)")
            continue;

        QtvCodecInfo info = codecList[i];
        if (info.displayName.isEmpty())
        {
            QMessageBox msgBox(this);
            msgBox.setWindowTitle(tr("Unsupported format"));
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setStandardButtons(QMessageBox::Ok);
            if (codecList.size() == 0)
            {
                msgBox.setText(
                    tr("Unsupported format or all tracks are not recognized. File name: \"%1\"").arg(newFileName));
                msgBox.exec();
                disableUpdatesCnt--;
                return;
            }
            else
            {
                if (firstWarn)
                {
                    msgBox.setText(tr("Track %1 (TrackID %2) was not recognized and ignored. File name: \"%3\"")
                                       .arg(i)
                                       .arg(codecList[i].trackID)
                                       .arg(newFileName));
                    msgBox.exec();
                    firstWarn = false;
                }
                continue;
            }
        }
        info.srcByTsMuxer = listedWritingApp.startsWith(QLatin1String("tsMuxeR"), Qt::CaseInsensitive);
        info.dvdTitle = m_pendingDvdTitle;
        info.angle = m_pendingAngle;
        if (!info.ownFile.isEmpty())
        {
            info.fileList << QDir::fromNativeSeparators(info.ownFile);
            info.ownerFile = newFileName;
        }
        else if (mplsFileList.isEmpty())
            info.fileList << newFileName;
        else
        {
            info.fileList << mplsFileList[0].name;
            QFileInfo fileInfo(unquoteStr(newFileName));
            // info.mplsFile = fileInfo.baseName();
            info.mplsFiles << unquoteStr(newFileName);
        }
        // A rate the listing does not know is left to tsMuxeR, as on the command line: fps= is not written, and a PGS
        // track bound to the video gets none either. Writing 23.976 for it retimed interlaced 29.97 and 25 H.264 from a
        // TS, and an H.264 elementary stream, which the command line muxes at their own rate. 23.976 stays the choice
        // the box offers when it is ticked.
        const bool fpsUnknown = info.descr.indexOf("not found") >= 0;
        if (fpsUnknown)
            fps = 23.976;
        else
            fps = extractFloatFromDescr(info.descr, "Frame rate: ");
        info.width = extractFloatFromDescr(info.descr, "Resolution: ") + 0.5;
        info.height = extractFloatFromDescr(info.descr, QString::number(info.width) + ":") + 0.5;
        if (doubleCompare(fps, 23.976))
            info.fpsText = "24000/1001";
        else if (doubleCompare(fps, 29.97))
            info.fpsText = "30000/1001";
        else if (doubleCompare(fps, 59.94))
            info.fpsText = "60000/1001";
        else
            info.fpsText = QString::number(fps);
        info.fpsTextOrig = fpsUnknown ? QString() : QString::number(fps);
        level = extractFloatFromDescr(info.descr, "@");
        // One decimal, because that is how the list reads. Plain QString::number turns 4.0
        // into "4", which matches no entry, and setComboBoxText then ADDS a stray "4" rather
        // than selecting "4.0".
        info.levelText = QString::number(level, 'f', 1);
        if (info.descr.indexOf("pulldown") >= 0)
            info.delPulldown = 0;

        info.maxPgOffsets = extractFloatFromDescr(info.descr, "3d-pg-planes: ");
        if (info.maxPgOffsets > 0)
            m_3dMode = true;

        // The listing says "undefined" for a subtitle without an offset. The window looked for "zero", which the
        // listing never prints, and read the word as plane #0, so such a subtitle was muxed with 3d-plane=0.
        if (info.descr.contains("3d-plane: undefined"))
            info.offsetId = -1;
        else if (info.descr.contains("3d-plane: "))
            info.offsetId = extractFloatFromDescr(info.descr, "3d-plane: ");
        else if (m_3dMode)
            info.offsetId = 0;

        auto newTrackRowIdx = ui->trackLV->rowCount();
        ui->trackLV->setRowCount(newTrackRowIdx + 1);
        ui->trackLV->setRowHeight(newTrackRowIdx, 18);
        QTableWidgetItem* item = new QTableWidgetItem("");
        item->setCheckState(info.enabledByDefault ? Qt::Checked : Qt::Unchecked);
        // A new track takes the SEI and SPS choice the window shows, which is the saved one. Every track started with
        // insertSEI and contSPS switched on instead, and kept them unless it was clicked, so a file added with those
        // turned off still muxed with them on (#666).
        info.addSEIMethod = ui->comboBoxSEI->currentIndex();
        info.addSPS = ui->checkBoxSPS->isChecked();
        item->setData(Qt::UserRole, reinterpret_cast<qlonglong>(new QtvCodecInfo(info)));
        ui->trackLV->setCurrentItem(item);
        ui->trackLV->setItem(newTrackRowIdx, 0, item);
        item = new QTableWidgetItem(newFileName);
        item->setFlags(item->flags() & (~Qt::ItemIsEditable));
        ui->trackLV->setItem(newTrackRowIdx, 1, item);
        item = new QTableWidgetItem(info.displayName);
        item->setFlags(item->flags() & (~Qt::ItemIsEditable));
        ui->trackLV->setItem(newTrackRowIdx, 2, item);
        item = new QTableWidgetItem(info.lang);
        item->setFlags(item->flags() & (~Qt::ItemIsEditable));
        ui->trackLV->setItem(newTrackRowIdx, 3, item);
        // where a playlist and its clip put the track among its kind (#52); empty for another source
        for (const auto& [column, place] : {std::pair{4, info.playlistPlace}, std::pair{5, info.clipPlace}})
        {
            item = new QTableWidgetItem(place > 0 ? QString::number(place) : QString());
            item->setFlags(item->flags() & (~Qt::ItemIsEditable));
            item->setTextAlignment(Qt::AlignCenter);
            ui->trackLV->setItem(newTrackRowIdx, column, item);
        }
        item = new QTableWidgetItem(info.descr);
        item->setFlags(item->flags() & (~Qt::ItemIsEditable));
        ui->trackLV->setItem(newTrackRowIdx, 6, item);
        if (firstAddedIndex == -1)
            firstAddedIndex = newTrackRowIdx;
        colorizeCurrentRow(&info, newTrackRowIdx);
        addTrackToDefaultComboBox(newTrackRowIdx);
    }
    updateOrderColumns();
    if (firstAddedIndex >= 0)
    {
        ui->trackLV->setRangeSelected(
            QTableWidgetSelectionRange(firstAddedIndex, 0, ui->trackLV->rowCount() - 1, ui->trackLV->columnCount() - 1),
            true);
        ui->trackLV->setCurrentCell(firstAddedIndex, 0);
    }
    QString displayName = newFileName;
    if (m_pendingDvdTitle)
        displayName += QStringLiteral(" - ") + tr("DVD title %1").arg(m_pendingDvdTitle);
    if (m_pendingAngle > 1)
        displayName += QStringLiteral(" - ") + tr("angle %1").arg(m_pendingAngle);
    if (fileDuration > 0)
        displayName += QString(" (%1)").arg(floatToTime(fileDuration));
    ui->inputFilesLV->addItem(displayName);

    int index = ui->inputFilesLV->count() - 1;
    QListWidgetItem* fileItem = ui->inputFilesLV->item(index);
    if (!mplsFileList.empty())
        fileItem->setData(MplsItemRole, MPLS_PRIMARY);
    fileItem->setData(FileNameRole, newFileName);
    fileItem->setData(DvdTitleRole, m_pendingDvdTitle);
    fileItem->setData(AngleRole, m_pendingAngle);
    fileItem->setData(ChaptersRole, QVariant::fromValue(chapters));
    fileItem->setData(FileDurationRole, fileDuration);
    if (videoEnd > 0)
    {
        fileItem->setData(VideoStartRole, videoStart);
        fileItem->setData(VideoEndRole, videoEnd);
    }
    chapters.clear();
    fileDuration = 0.0;
    listedWritingApp.clear();
    videoStart = videoEnd = 0.0;

    ui->inputFilesLV->setCurrentItem(fileItem);
    if (mplsFileList.size() > 0)
        doAppendInt(mplsFileList[0].name, newFileName, mplsFileList[0].duration, false, MPLS_M2TS);
    for (int mplsCnt = 1; mplsCnt < mplsFileList.size(); ++mplsCnt)
        doAppendInt(mplsFileList[mplsCnt].name, mplsFileList[mplsCnt - 1].name, mplsFileList[mplsCnt].duration, false,
                    MPLS_M2TS);
    ui->inputFilesLV->setCurrentItem(ui->inputFilesLV->item(index));
    updateAutoCoreMerge();
    updateMetaLines();
    ui->moveupBtn->setEnabled(ui->trackLV->currentRow() >= 0);
    ui->movedownBtn->setEnabled(ui->trackLV->currentRow() >= 0);
    ui->removeTrackBtn->setEnabled(ui->trackLV->currentRow() >= 0);
    updateRemoveAllEnabled();
    if (!outFileNameModified)
    {
        // A DVD title is named after the disc and its number ("Brother title 5"); the disc alone has no name for
        // a drive, and every title of it would get the same one
        QString base = m_pendingDvdTitle ? tr("%1 title %2").arg(discName(newFileName)).arg(m_pendingDvdTitle)
                                         : outputNameOfTitle(m_listedDiscTitle);
        // The film's name of a Blu-ray: as a Matroska title and an image's label too, while those are empty
        if (!m_listedDiscTitle.isEmpty())
        {
            QString title = m_listedDiscTitle;
            title.remove(QLatin1Char('"'));
            if (ui->mkvTitleEdit->text().isEmpty())
                ui->mkvTitleEdit->setText(title);
            if (ui->DiskLabelEdit->text().isEmpty())
                ui->DiskLabelEdit->setText(title);
        }
        // another angle than the first is named so ("00200 angle 2"): its output is another film than the first's
        if (m_pendingAngle > 1)
            base = tr("%1 angle %2")
                       .arg(base.isEmpty() ? QFileInfo(unquoteStr(newFileName)).completeBaseName() : base)
                       .arg(m_pendingAngle);
        modifyOutFileName(newFileName, base);
        outFileNameModified = true;
    }
    m_pendingDvdTitle = 0;
    m_pendingAngle = 1;
    m_pendingAngleClips.clear();
    updateMaxOffsets();
    updateCustomChapters();
    updateDvProfileVisible();
    disableUpdatesCnt--;
    trackLVItemSelectionChanged();
    emit fileAdded();
}

void TsMuxerWindow::updateCustomChapters()
{
    if (ui->radioButtonCustomChapters->isChecked() && m_customChaptersUserOverride)
        return;

    // A joined file's chapters are moved to where the join puts it, which is after the VIDEO of the file before it: the
    // listing's "Video end" of that file, less the "Video start" of this one. The command line places them the same
    // way (METADemuxer::getChapters). The Durations were added up before, and a Duration is where the longest track
    // ends: a subtitle running past the picture put the next file's chapters a second late. A file the listing gave
    // no video times for counts by its Duration, as before, and a playlist always does. After a file with neither,
    // the chapters of the files joined to it are left out, as the command line leaves them out: they used to be put
    // at the join's start, a wrong place that looks right.
    QSet<qint64> chaptersSet;
    double prevEnd = 0.0;
    double offset = 0.0;
    bool placeable = true;
    for (int i = 0; i < ui->inputFilesLV->count(); ++i)
    {
        QListWidgetItem* item = ui->inputFilesLV->item(i);
        if (item->data(MplsItemRole).toInt() == MPLS_M2TS)
            continue;
        if (QtCompat::strLeft(item->text(), 4) == FILE_JOIN_PREFIX)
        {
            placeable = placeable && prevEnd > 0.0;
            offset += prevEnd - item->data(VideoStartRole).toDouble();
        }
        else
        {
            offset = 0;
            placeable = true;
        }

        ChapterList chapters = item->data(ChaptersRole).value<ChapterList>();
        if (!placeable)
            chapters.clear();
        chapters.removeIf([offset](double chapter) { return chapter + offset < 0.0; });
        // In whole milliseconds, rounded: the box shows milliseconds, so two marks that show the same are one mark.
        for (double chapter : chapters) chaptersSet << qRound64((chapter + offset) * 1000.0);
        const QVariant listedEnd = item->data(VideoEndRole);
        prevEnd = listedEnd.isValid() ? listedEnd.toDouble() : item->data(FileDurationRole).toDouble();
    }
    const QSignalBlocker blocker(ui->memoChapters);
    ui->memoChapters->clear();
    QList<qint64> mergedChapterList = chaptersSet.values();
    std::sort(std::begin(mergedChapterList), std::end(mergedChapterList));
    for (auto chapter : mergedChapterList)
        ui->memoChapters->insertPlainText(floatToTime(chapter / 1000.0) + QString('\n'));
    // The meta line carries this list, and the box's own signal is blocked above, so it is brought up to date here.
    // After a file was joined or removed, the meta line was refreshed BEFORE this list changed and kept the old one:
    // the box showed the joined file's chapters, and a mux started then got only the first file's.
    updateMetaLines();
}

void splitLines(const QString& str, QList<QString>& strList)
{
    strList = str.split('\n');
    for (int i = 0; i < strList.size(); ++i)
    {
        if (!strList[i].isEmpty() && strList[i].at(0) == '\r')
            strList[i] = QtCompat::strMid(strList[i], 1);
        else if (!strList[i].isEmpty() && strList[i].at(strList[i].length() - 1) == '\r')
            strList[i] = QtCompat::strMid(strList[i], 0, strList[i].length() - 1);
    }
}

void TsMuxerWindow::addLines(const QByteArray& arr, QList<QString>& outList, bool isError)
{
    QString str = QString::fromUtf8(arr);
    QList<QString> strList;
    splitLines(str, strList);
    QString text;
    for (int i = 0; i < strList.size(); ++i)
    {
        if (strList[i].trimmed().isEmpty())
            continue;
        int p = strList[i].indexOf("% complete");
        if (p >= 0)
        {
            int numStartPos = 0;
            for (int j = 0; j < strList[i].length(); ++j)
            {
                if (strList[i].at(j) >= '0' && strList[i].at(j) <= '9')
                {
                    numStartPos = j;
                    break;
                }
            }
            QString progress = QtCompat::strMid(strList[i], numStartPos, p - numStartPos);
            float tmpVal = progress.toFloat();
            if (qAbs(tmpVal) > 0 && runInMuxMode)
                muxForm->setProgress(int(double(tmpVal * 10) + 0.5));
        }
        else
        {
            if (runInMuxMode)
            {
                if (!text.isEmpty())
                    text += '\n';
                text += strList[i];
            }
            else
                outList << strList[i];
        }
    }
    if (runInMuxMode && !text.isEmpty())
    {
        if (isError)
            muxForm->addStdErrLine(text);
        else
            muxForm->addStdOutLine(text);
    }
}

// A read gives whatever the pipe holds at that moment, and that can end in the middle of a line, or even in the middle
// of one character of the UTF-8 text. Each read used to be taken as whole lines, so a line cut by a read was shown as
// two (15 times in one long mux), a progress line cut that way could set a wrong value for a moment, and a cut
// character became a replacement mark. The unfinished end now waits for the next read; whatever is left when the
// process ends is taken as it is (flushPendingOutput).
static QByteArray takeCompleteLines(QByteArray& pending, const QByteArray& data)
{
    pending += data;
    const auto end = pending.lastIndexOf('\n');
    if (end < 0)
        return {};
    QByteArray lines = pending.left(end + 1);
    pending.remove(0, end + 1);
    return lines;
}

void TsMuxerWindow::readFromStdout()
{
    const QByteArray lines = takeCompleteLines(m_pendingStdOut, proc.readAllStandardOutput());
    if (!lines.isEmpty())
        addLines(lines, procStdOutput, false);
}

void TsMuxerWindow::readFromStderr()
{
    const QByteArray lines = takeCompleteLines(m_pendingStdErr, proc.readAllStandardError());
    if (!lines.isEmpty())
        addLines(lines, procErrOutput, true);
}

void TsMuxerWindow::flushPendingOutput()
{
    if (!m_pendingStdOut.isEmpty())
        addLines(m_pendingStdOut, procStdOutput, false);
    if (!m_pendingStdErr.isEmpty())
        addLines(m_pendingStdErr, procErrOutput, true);
    m_pendingStdOut.clear();
    m_pendingStdErr.clear();
}

void TsMuxerWindow::myPlaySound(const QString& fileName)
{
#if defined(Q_OS_WIN)
    // The Windows build compiles Qt from qtbase alone so that the result runs on Windows 7,
    // and Multimedia is a separate module that is not built, so QSoundEffect does not exist
    // here. The old fallback was QApplication::beep(), which makes no sound at all on a
    // machine whose system sounds are off, and that is why this option has never worked on
    // Windows. PlaySound takes the wav from memory, needs no Qt module and no library that
    // is not already on the machine, and has been present since long before Windows 7.
    QFile f(fileName);
    if (f.open(QIODevice::ReadOnly))
    {
        m_soundData = f.readAll();
        if (!m_soundData.isEmpty() && PlaySoundW(reinterpret_cast<LPCWSTR>(m_soundData.constData()), nullptr,
                                                 SND_MEMORY | SND_ASYNC | SND_NODEFAULT))
            return;
    }
    QApplication::beep();
#elif QT_MULTIMEDIA_LIB
    sound.setSource(QUrl(QString("qrc%1").arg(fileName)));
    sound.play();
#else
    QApplication::beep();
#endif
}

// Reading a file to list its tracks can take a long time (a big Matroska file is read far into it), and nothing in the
// window changed meanwhile, so it looked frozen. A busy cursor shows that the file is being read. It is set only for
// that listing; the mux has its own progress window.
void TsMuxerWindow::setProbeBusy(bool busy)
{
    if (busy == m_probeBusy)
        return;
    m_probeBusy = busy;
    if (busy)
        QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    else
        QGuiApplication::restoreOverrideCursor();
}

void TsMuxerWindow::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    flushPendingOutput();
    processFinished = true;
    setProbeBusy(false);
    if (!metaName.isEmpty())
    {
        QFile::remove(metaName);
        metaName.clear();
        // After a failure the core stays on its track, for the next try
        if (exitCode == 0 && exitStatus == QProcess::NormalExit)
            removeMadeCores();
        if (ui->checkBoxSound->isChecked())
        {
            if (exitCode == 0)
                myPlaySound(":/sounds/success.wav");
            else
                myPlaySound(":/sounds/fail.wav");
        }
    }
    processExitCode = exitCode;
    processExitStatus = exitStatus;
    if (exitStatus == QProcess::NormalExit)
        m_runOutput.clear();  // ended by itself: tsMuxeR has dealt with its output
    muxForm->muxFinished(processExitCode, ui->radioButtonDemux->isChecked() ? tr("Demux") : tr("Mux"));
    updateMuxButton();
    ui->addBtn->setEnabled(true);
    m_addDiscBtn->setEnabled(true);
    inputFilesLVChanged();
    if (processExitCode == 0)
        emit tsMuxerSuccessFinished();
    else if (!runInMuxMode && exitStatus == QProcess::NormalExit)
    {
        showProbeRefusal();
        // Adding several files went on only from a file that was added, so every file chosen after a refused one
        // was dropped without a word. The rest is examined now, as if the refused one had not been chosen.
        if (!addFileList.isEmpty() && m_addNextFile)
            (this->*m_addNextFile)();
    }
}

// tsMuxeR examined a file that was being added and ended with an error. Nothing at all appeared: the file was simply
// not added, and the reason tsMuxeR gave was never shown, so a damaged recording looked like a window that ignored
// the click. tsMuxeR's own words are shown now, with the file's name.
void TsMuxerWindow::showProbeRefusal()
{
    QStringList lines;
    for (const QString& line : procErrOutput)
        if (!line.trimmed().isEmpty())
            lines << line.trimmed();
    if (lines.isEmpty())  // no error text: the last lines it printed are the best clue
        for (int i = (std::max)(0, static_cast<int>(procStdOutput.size()) - 5); i < procStdOutput.size(); ++i)
            if (!procStdOutput[i].trimmed().isEmpty())
                lines << procStdOutput[i].trimmed();
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("File not added"));
    msgBox.setText(tr("tsMuxeR could not read the file \"%1\" (error code %2).").arg(newFileName).arg(processExitCode));
    if (!lines.isEmpty())
        msgBox.setInformativeText(lines.join('\n'));
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.exec();
}

void TsMuxerWindow::onProcessError(QProcess::ProcessError error)
{
    flushPendingOutput();
    processExitCode = -1;
    if (!metaName.isEmpty())
    {
        QFile::remove(metaName);
        metaName.clear();
    }
    processFinished = true;
    setProbeBusy(false);
    QMessageBox msgBox(this);
    QString text;
    msgBox.setWindowTitle(tr("tsMuxeR error"));
    switch (error)
    {
    case QProcess::FailedToStart:
        msgBox.setText(tr("tsMuxeR not found!"));
        updateMuxButton();
        ui->addBtn->setEnabled(true);
        m_addDiscBtn->setEnabled(true);
        inputFilesLVChanged();
        break;
    case QProcess::Crashed:
        // process killed
        if (runInMuxMode)
            return;
        for (int i = 0; i < procErrOutput.size(); ++i)
        {
            if (i > 0)
                text += '\n';
            text += procErrOutput[i];
        }
        // A killed process usually says nothing, and a box with no message tells the reader
        // nothing at all: it does not even say which program stopped. Closing the window while
        // a file was still being examined produced exactly that, and it read as a fault of its
        // own rather than the consequence of ending the process.
        if (text.trimmed().isEmpty())
            text =
                tr("tsMuxeR stopped without reporting a reason.\n\n"
                   "If this appeared while a file was still being examined, or as the window was "
                   "closing, the process was ended before it could finish. Nothing has been written.");
        msgBox.setText(text);
        break;
    default:
        msgBox.setText(tr("Can't execute tsMuxeR!"));
        break;
    }
    msgBox.setIcon(QMessageBox::Critical);
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.exec();
}

static QString getTsMuxerBinaryPath()
{
    const auto applicationDirPath =
        QDir::toNativeSeparators(QCoreApplication::applicationDirPath()) + QDir::separator();
    for (auto binaryName : {"tsmuxer", "tsMuxeR"})
    {
        auto binaryPath = QStandardPaths::findExecutable(binaryName, {applicationDirPath});
        if (!binaryPath.isEmpty())
        {
            return binaryPath;
        }
    }
    return QString();
}

void TsMuxerWindow::tsMuxerExecute(const QStringList& args)
{
    const auto exePath = getTsMuxerBinaryPath();
    // Carried on EVERY call, so what the track list shows and what the mux writes agree. The
    // muxer takes it out of the argument list before it counts the arguments, so the modes are
    // unaffected, the BDMV to ISO one included. Left off entirely at the default, which keeps
    // the command line and the behaviour exactly as they were.
    QStringList allArgs;
    const QString langStyle = ui->langCodesComboBox->currentData().toString();
    if (!langStyle.isEmpty() && langStyle != QLatin1String("terminological"))
        allArgs << (QLatin1String("--lang-codes=") + langStyle);
    // The listing of a playlist gives the lengths and chapters of the parts it muxes, so it is asked the same way as
    // the mux: the window takes the chapters from it. Left off at the default.
    if (!ui->checkBoxPlaylistParts->isChecked())
        allArgs << QLatin1String("--playlist-clips=whole");
    allArgs << args;
    ui->buttonMux->setEnabled(false);
    procStdOutput.clear();
    procErrOutput.clear();
    m_pendingStdOut.clear();
    m_pendingStdErr.clear();
    processFinished = false;
    processExitCode = -1;
    proc.start(exePath, allArgs);
    m_runPid = proc.processId();
    if (muxForm->isVisible())
        muxForm->setProcess(&proc);
}

// "Make a sample...", on the Input tab and in the progress window when a job ends with an error: a part of the chosen
// source for a problem report, with the report of the last job when there is one
void TsMuxerWindow::onMakeSample()
{
    const QListWidgetItem* itm = ui->inputFilesLV->currentItem();
    if (itm == nullptr && ui->inputFilesLV->count() > 0)
        itm = ui->inputFilesLV->item(0);
    SampleDialog dialog(muxForm->isVisible() ? static_cast<QWidget*>(muxForm) : this, getTsMuxerBinaryPath(),
                        sampleSource(itm), muxForm->reportFile());
    dialog.exec();
}

void TsMuxerWindow::doAppendInt(const QString& fileName, const QString& parentFileName, double duration,
                                bool doublePrefix, MplsType mplsRole)
{
    QString itemName = FILE_JOIN_PREFIX;
    if (doublePrefix)
        itemName += FILE_JOIN_PREFIX;
    itemName += fileName;
    if (duration > 0)
        itemName += QString(" ( %1)").arg(floatToTime(duration));
    QListWidgetItem* item = new QListWidgetItem(itemName);
    ui->inputFilesLV->insertItem(ui->inputFilesLV->currentRow() + 1, item);
    item->setData(MplsItemRole, mplsRole);
    item->setData(FileNameRole, fileName);
    if (duration > 0)
        item->setData(FileDurationRole, duration);
    if (mplsRole != MPLS_M2TS && videoEnd > 0)
    {
        item->setData(VideoStartRole, videoStart);
        item->setData(VideoEndRole, videoEnd);
    }
    item->setData(ChaptersRole, QVariant::fromValue(chapters));

    ui->inputFilesLV->setCurrentItem(item);

    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        auto info = getCodecInfo(i);
        if (!info)
            continue;
        if (mplsRole == MPLS_PRIMARY)
        {
            for (int j = 0; j < info->mplsFiles.size(); ++j)
            {
                if (info->mplsFiles[j] == parentFileName)
                {
                    info->mplsFiles.insert(j + 1, fileName);
                    break;
                }
            }
        }
        else
        {
            for (int j = 0; j < info->fileList.size(); ++j)
            {
                if (info->fileList[j] == parentFileName)
                {
                    info->fileList.insert(j + 1, fileName);
                    break;
                }
            }
        }
    }
}

bool TsMuxerWindow::isVideoCropped()
{
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        auto codecInfo = getCodecInfo(i);
        if (!codecInfo)
            continue;

        if (isVideoCodec(codecInfo->displayName))
        {
            if (codecInfo->height < 1080 && codecInfo->height != 720 && codecInfo->height != 576 &&
                codecInfo->height != 480)
                return true;
        }
    }
    return false;
}

// The kinds of track a meta text has more than 32 of, as tsMuxeR counts them for a disc (MuxerManager::
// checkTracksBeforeOutput): audio, secondary audio, subtitles and secondary video, by the codec of each track line
// and its "secondary" parameter. Each kind as "47 subtitle tracks".
static QStringList kindsOverDiscLimit(const QString& meta)
{
    int audio = 0, secondaryAudio = 0, subtitles = 0, secondaryVideo = 0;
    static const QRegularExpression secondaryParam(QStringLiteral(",\\s*secondary\\s*(,|$)"));
    for (const QString& rawLine : meta.split('\n'))
    {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(QLatin1String("MUXOPT")))
            continue;
        const QString codec = line.section(',', 0, 0).trimmed();
        const bool secondary = line.contains(secondaryParam);
        if (codec.startsWith('A'))
            ++(secondary ? secondaryAudio : audio);
        else if (codec.startsWith('S'))
            ++subtitles;
        else if (codec.startsWith('V') && secondary)
            ++secondaryVideo;
    }
    QStringList over;
    if (audio > 32)
        over << TsMuxerWindow::tr("%1 audio tracks").arg(audio);
    if (secondaryAudio > 32)
        over << TsMuxerWindow::tr("%1 secondary audio tracks").arg(secondaryAudio);
    if (subtitles > 32)
        over << TsMuxerWindow::tr("%1 subtitle tracks").arg(subtitles);
    if (secondaryVideo > 32)
        over << TsMuxerWindow::tr("%1 secondary video tracks").arg(secondaryVideo);
    return over;
}

// Whether the MUXOPT line of a meta text has an option, as a word of its own
static bool muxOptHas(const QString& meta, const QString& option)
{
    for (const QString& line : meta.split('\n'))
        if (line.trimmed().startsWith(QLatin1String("MUXOPT")))
            return line.contains(QRegularExpression("(^|\\s)" + QRegularExpression::escape(option) + "(\\s|$)"));
    return false;
}

bool TsMuxerWindow::isDiskOutput() const
{
    return ui->radioButtonAVCHD->isChecked() || ui->radioButtonBluRay->isChecked() ||
           ui->radioButtonBluRayISO->isChecked();
}

QString TsMuxerWindow::getMuxOpts()
{
    QString rez = "MUXOPT --no-pcr-on-video-pid";
    if (ui->checkBoxNewAudioPes->isChecked())
        rez += " --new-audio-pes";
    else
        rez += " --no-hdmv-descriptors";
    if (ui->radioButtonBluRay->isChecked())
        rez += (ui->checkBoxV3->isChecked() ? " --blu-ray-v3" : " --blu-ray");
    else if (ui->radioButtonBluRayISO->isChecked())
    {
        rez += (ui->checkBoxV3->isChecked() ? " --blu-ray-v3" : " --blu-ray");
        if (!ui->DiskLabelEdit->text().isEmpty())
            rez += QString(" --label=\"%1\" ").arg(ui->DiskLabelEdit->text());
    }
    // More than 32 tracks of one kind on a disc (#53): with the question turned off, the option goes on the line itself,
    // so that a saved meta file writes the disc as the window does
    if (isDiskOutput() && !ui->checkBoxAskExtraTracks->isChecked() && !m_buildingMuxOpts)
    {
        m_buildingMuxOpts = true;
        const bool over = !kindsOverDiscLimit(buildMetaText()).isEmpty();
        m_buildingMuxOpts = false;
        if (over)
            rez += " --allow-extra-tracks";
    }
    // The disc's title and pictures (#30): kept from the source disc unless a folder or none is chosen
    if (ui->radioButtonBluRay->isChecked() || ui->radioButtonBluRayISO->isChecked())
    {
        const QString metaDir = ui->discMetaDirEdit->text().trimmed();
        if (ui->comboDiscMeta->currentIndex() == 1 && !metaDir.isEmpty())
            rez += QString(" --meta-dir=\"%1\"").arg(metaDir);
        else if (ui->comboDiscMeta->currentIndex() == 2)
            rez += " --meta-dir=none";
    }
    else if (ui->radioButtonAVCHD->isChecked())
        rez += " --avchd";
    else if (ui->radioButtonDemux->isChecked())
        rez += " --demux";
    else if (ui->radioButtonMKV->isChecked() && ui->comboBoxDvProfile->isVisible() &&
             ui->comboBoxDvProfile->currentIndex() == 1)
        rez += " --dv-profile=8.1";
    if (ui->radioButtonMKV->isChecked())
    {
        QString title = ui->mkvTitleEdit->text().trimmed();
        title.remove(QLatin1Char('"'));
        if (!title.isEmpty())
            rez += QString(" --title=\"%1\"").arg(title);
        // Matroska only: WebM has no editions, and the muxer refuses the option for a .webm name, so the tick that
        // was offered stopped the mux. It refuses it as well for a source that is no Matroska file (an MP4), a join,
        // a cut, a split and chapters given in the window, so the tick is greyed and not written there.
        bool keepEditionsPossible =
            !ui->outFileName->text().trimmed().endsWith(QLatin1String(".webm"), Qt::CaseInsensitive);
        bool fromMatroska = false;
        for (int i = 0; i < ui->trackLV->rowCount(); ++i)
        {
            const QtvCodecInfo* info = getCodecInfo(i);
            if (!info || info->fileList.isEmpty() || ui->trackLV->item(i, 0)->checkState() != Qt::Checked)
                continue;
            if (info->fileList.size() > 1)
                keepEditionsPossible = false;
            const QString ext = QFileInfo(unquoteStr(info->fileList[0])).suffix().toUpper();
            fromMatroska =
                fromMatroska || ext == "MKV" || ext == "MKA" || ext == "MKS" || ext == "MK3D" || ext == "WEBM";
        }
        if (!fromMatroska || (ui->checkBoxCut->isChecked() && (qTimeToMsec(ui->cutStartTimeEdit->time()) > 0 ||
                                                               qTimeToMsec(ui->cutEndTimeEdit->time()) > 0)))
            keepEditionsPossible = false;
        if (ui->splitByDuration->isChecked() || ui->splitBySize->isChecked() || ui->splitByChapters->isChecked())
            keepEditionsPossible = false;
        if (m_customChaptersUserOverride && ui->radioButtonCustomChapters->isChecked())
            keepEditionsPossible = false;
        ui->mkvKeepEditionsCheck->setEnabled(keepEditionsPossible);
        if (ui->mkvKeepEditionsCheck->isChecked() && keepEditionsPossible)
            rez += " --keep-editions";
    }
    // dual-layer guard options (Blu-ray / Blu-ray ISO output; the layer-break guard only takes effect
    // for ISO output, where tsMuxeR itself writes the image).
    if (ui->radioButtonBluRay->isChecked() || ui->radioButtonBluRayISO->isChecked())
    {
        auto* discSizeCombo = findChild<QComboBox*>("dlDiscSize");
        // The capacity goes out as an exact byte count taken from the disc's Free Sectors, not as a
        // bd50/bd100 keyword, so the 66 GB two-layer entry can be expressed at all.
        //
        // Taken from the FIELD, not from the combo entry, so a capacity the user corrected for the
        // disc actually in the drive is what the mux and the layer break are based on.
        qint64 discSectors = 0;
        if (const auto* fsEdit = findChild<QLineEdit*>("dlFreeSectors");
            fsEdit && discSizeCombo && discSizeCombo->currentIndex() > 0)
        {
            QString digits = fsEdit->text();
            digits.remove(QRegularExpression(QStringLiteral("[^0-9]")));
            discSectors = digits.toLongLong();
        }
        if (discSectors > 0)
            rez += " --disc-size=" + QString::number(discSectors * 2048LL);
        auto* guardCheck = findChild<QCheckBox*>("dlGuardCheck");
        auto* guardSpin = findChild<QSpinBox*>("dlGuardSpin");
        // Inner-only takes the place of the guard: it needs the disc and its breaks, and sizes the zeros itself.
        // ISO output and a disc of two or more layers only, as the tick box is offered.
        const auto* innerCheck = findChild<QCheckBox*>("dlInnerOnly");
        const auto* noGuardCheck = findChild<QCheckBox*>("dlNoGuard");
        if (innerCheck && innerCheck->isChecked() && ui->radioButtonBluRayISO->isChecked() && discSectors > 0 &&
            discSizeCombo->currentData().toInt() >= 2)
        {
            rez += " --inner-only";
            QStringList breaks;
            for (const qint64 b : discBreaks(discSectors, discSizeCombo->currentData().toInt()))
                breaks << QString::number(b);
            if (!breaks.isEmpty())
                rez += " --layer-break-lbn=" + breaks.join(",");
        }
        // ISO only: the guard pads the disc image. Emitting it for a Blu-ray folder would put an
        // option in the meta that the muxer ignores.
        else if (guardCheck && guardSpin && guardCheck->isChecked() && ui->radioButtonBluRayISO->isChecked())
        {
            rez += " --layer-break-guard=" + QString::number(guardSpin->value());
            // The break sectors have to be sent explicitly. Without them the engine falls back to
            // one break at half a BD-R DL disc, which is only right for a 50 GB disc: on 100 or
            // 128 GB it is not a layer boundary at all, so the guard used to protect nothing there.
            QStringList breaks;
            for (const qint64 b : discBreaks(discSectors, discSizeCombo->currentData().toInt()))
                breaks << QString::number(b);
            if (!breaks.isEmpty())
                rez += " --layer-break-lbn=" + breaks.join(",");
            auto* beforeCheck = findChild<QCheckBox*>("dlGuardBeforeCheck");
            auto* beforeSpin = findChild<QSpinBox*>("dlGuardBeforeSpin");
            if (beforeCheck && beforeSpin && beforeCheck->isChecked())
                rez += " --layer-break-guard-before=" + QString::number(beforeSpin->value());
        }
        // Without a guard ticked, an image crossing a layer break gets the guard by default; this leaves it out
        else if (noGuardCheck && noGuardCheck->isChecked() && ui->radioButtonBluRayISO->isChecked())
            rez += " --no-layer-break-guard";
        auto* oversizeCheck = findChild<QCheckBox*>("dlAllowOversize");
        if (oversizeCheck && oversizeCheck->isChecked())
            rez += " --allow-oversize";
    }
    if (ui->checkBoxCBR->isChecked())
        rez += " --cbr --bitrate=" + QString::number(ui->editCBRBitrate->value(), 'f', 3);
    else
    {
        rez += " --vbr";
        if (ui->checkBoxRVBR->isChecked())
            rez += QString(" --minbitrate=") + QString::number(ui->editMinBitrate->value(), 'f', 3);
    }

    // A playlist's clips whole, as earlier versions muxed them; the default muxes the parts the playlist plays
    if (!ui->checkBoxPlaylistParts->isChecked())
        rez += " --playlist-clips=whole";

    // ** THE CEILING IS WRITTEN ONCE. ** Two controls can ask for one: Restricted VBR here, and
    // the tick on the Blu-ray tab that holds the disc to the rate a player has to supply. Both
    // used to write their own --maxbitrate and the muxer takes the LAST one it reads, so
    // Restricted VBR silently overrode a tick whose own label promises the opposite.
    //
    // The LOWER of the two wins. The tick is a ceiling that must not be exceeded; a smaller
    // figure someone typed is still inside it and is theirs to choose. Restricted VBR is read
    // only when it is live, which is VBR and not CBR, exactly as before.
    double maxKbps = 0;
    if (!ui->checkBoxCBR->isChecked() && ui->checkBoxRVBR->isChecked())
        maxKbps = ui->editMaxBitrate->value();
    if ((ui->radioButtonBluRay->isChecked() || ui->radioButtonBluRayISO->isChecked()) &&
        ui->checkBoxRateLimit->isChecked())
    {
        const double limit = ui->checkBoxV3->isChecked() ? ui->comboRateLimit->currentData().toDouble() : 48000.0;
        maxKbps = maxKbps > 0 ? qMin(maxKbps, limit) : limit;
    }
    if (maxKbps > 0)
        rez += QString(" --maxbitrate=") + QString::number(maxKbps, 'f', 3);

    if (isDiskOutput())
    {
        if (ui->checkBoxBlankPL->isChecked() && isVideoCropped())
        {
            rez += " --insertBlankPL";
            if (ui->BlackplaylistCombo->value() != 1900)
                rez += QString(" --blankOffset=") + QString::number(ui->BlackplaylistCombo->value());
        }
        if (ui->spinBoxMplsNum->value() > 0)
            rez += " --mplsOffset=" + QString::number(ui->spinBoxMplsNum->value());
        if (ui->spinBoxM2tsNum->value() > 0)
            rez += " --m2tsOffset=" + QString::number(ui->spinBoxM2tsNum->value());
    }
    // Two different conditions, because the two options are not honoured in the same places.
    // --auto-chapters becomes playlist marks and so is written for a disc only. A custom list goes
    // to the muxer whatever the output, and Matroska writes it, so MKV gets it as well.
    if (isDiskOutput() && ui->radioButtonAutoChapter->isChecked())
        rez += " --auto-chapters=" + QString::number(ui->spinEditChapterLen->value());
    // A disc always gets the times: that is what the playlist is built from, and a playlist mark
    // has no name to lose. Matroska is different. The meta line carries times and nothing else, so
    // writing it REPLACES the source's chapter names with numbers, while writing nothing lets the
    // muxer copy the source's own chapters, names included. Measured both ways on the same file.
    // So an MKV output gets the list only when the user actually made one, which is what
    // m_customChaptersUserOverride already records.
    const bool customForThisOutput =
        isDiskOutput() || (ui->radioButtonMKV->isChecked() && m_customChaptersUserOverride);
    if (customForThisOutput && ui->radioButtonCustomChapters->isChecked())
    {
        QString custChapStr;
        QList<QString> lines;
        splitLines(ui->memoChapters->toPlainText(), lines);
        for (int i = 0; i < lines.size(); ++i)
        {
            const QString tmpStr = chapterLineTime(lines[i]);
            if (!tmpStr.isEmpty())
            {
                if (!custChapStr.isEmpty())
                    custChapStr += ';';
                custChapStr += tmpStr;
            }
        }
        rez += QString(" --custom-chapters=") + custChapStr;
    }
    // With nothing about chapters on the meta line, a Matroska output takes over the source's own chapters. That is
    // right for the other two choices and wrong for "No chapters", which therefore has to be said. A disc needs no
    // such word: its marks come only from the two options above.
    if (ui->radioButtonMKV->isChecked() && ui->radioButtonNoChapters->isChecked())
        rez += " --no-chapters";
    // Only write these where they can be honoured. The tick is remembered either way, so choosing
    // an output that cannot split hides the option rather than discarding what was set.
    if (outputCanSplit())
    {
        if (ui->splitByDuration->isChecked())
            rez += QString(" --split-duration=") + ui->spinEditSplitDuration->text();
        if (ui->splitBySize->isChecked())
            rez += QString(" --split-size=") + QString::number(ui->editSplitSize->value(), 'f', 3) +
                   ui->comboBoxMeasure->currentData().toString();
        // Takes no value: the marks come from the source, or from the chapter list on the
        // Blu-ray tab, which is already written into the meta as --custom-chapters.
        if (ui->splitByChapters->isChecked())
            rez += QString(" --split-chapters");
    }

    int startCut = qTimeToMsec(ui->cutStartTimeEdit->time());
    int endCut = qTimeToMsec(ui->cutEndTimeEdit->time());
    if (startCut > 0 && ui->checkBoxCut->isChecked())
        rez += QString(" --cut-start=%1ms").arg(startCut);
    if (endCut > 0 && ui->checkBoxCut->isChecked())
        rez += QString(" --cut-end=%1ms").arg(endCut);
    // The video of a cut holds only the pictures shown inside it, as before 3.0.0
    if ((startCut > 0 || endCut > 0) && ui->checkBoxCut->isChecked() && ui->checkBoxCutInside->isChecked())
        rez += QString(" --cut-pictures=inside");

    int vbvLen = ui->editVBVLen->value();
    if (vbvLen > 0)
        rez += " --vbv-len=" + QString::number(vbvLen);

    if (isDiskOutput() && ui->rightEyeCheckBox->isChecked())
        rez += " --right-eye";
    /*
    QString muxTimeStr = ui->muxTimeEdit->time().toString("hh:mm:ss.zzz");
    if (muxTimeStr != "00:00:00.000")
        rez += " --start-time=" + muxTimeStr;
    */
    if (ui->muxTimeClock->value())
        rez += QString(" --start-time=%1").arg(ui->muxTimeClock->value());
    return rez;
}

double TsMuxerWindow::getRendererAnimationTime() const
{
    switch (ui->comboBoxAnimation->currentIndex())
    {
    case 1:
        return 0.1;  // fast
        break;
    case 2:
        return 0.25;  // medium
        break;
    case 3:
        return 0.5;  // slow
        break;
    case 4:
        return 1.0;  // very slow
    }
    return 0.0;
}

void TsMuxerWindow::setRendererAnimationTime(double value)
{
    int index = 0;
    if (doubleCompare(value, 0.1))
        index = 1;
    else if (doubleCompare(value, 0.25))
        index = 2;
    else if (doubleCompare(value, 0.5))
        index = 3;
    else if (doubleCompare(value, 1.0))
        index = 4;

    ui->comboBoxAnimation->setCurrentIndex(index);
}

QString TsMuxerWindow::getSrtParams()
{
    // Kept as text, each subtitle is copied with its own text, tags and times: nothing that drawing
    // needs applies, and the muxer asks for none of it
    if (keepTextApplies())
        return ",keep-text";
    auto& font = fontSettingsModel->font();
    auto rez = QString(",font-name=\"%1\",font-size=%2,font-color=0x%3")
                   .arg(font.family())
                   .arg(font.pointSize())
                   .arg(fontSettingsModel->color(), 8, 16, QLatin1Char('0'));

    if (ui->lineSpacing->value() != 1.0)
        rez += ",line-spacing=" + QString::number(ui->lineSpacing->value());

    if (font.italic())
        rez += ",font-italic";
    if (font.bold())
        rez += ",font-bold";
    if (font.underline())
        rez += ",font-underline";
    if (font.strikeOut())
        // The muxer spells it font-strike-out, and has since it was documented. The window wrote
        // font-strikeout, which is not a name anything knows, so the tick warned and did nothing.
        // Measured: with the muxer's spelling the rendered subtitle really changes, and with this
        // one the output is identical to having the tick off apart from the random track UID.
        rez += ",font-strike-out";

    rez += QString(",bottom-offset=") + QString::number(ui->spinEditOffset->value()) +
           ",font-border=" + QString::number(ui->spinEditBorder->value());
    // text-align is NOT written. No build of the muxer has ever known the name, so every text
    // subtitle line produced one "unknown track parameter" warning per mux and nothing else. The
    // three radio buttons that chose it are disabled in the .ui and nothing enables them, so the
    // value could only ever be center in any case.
    //
    // The muxer has no horizontal alignment at all: the accepted list in metaDemuxer.cpp has
    // font, spacing, offset, border, charset and fade, and nothing for alignment. Adding it is a
    // feature and would mean enabling those buttons as well. Until then the window does not ask
    // for something that cannot happen.

    double animationTime = getRendererAnimationTime();
    if (animationTime > 0.0)
        rez += QString(",fadein-time=%1,fadeout-time=%1").arg(animationTime);

    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        auto codecInfo = getCodecInfo(i);
        if (!codecInfo)
            continue;

        if (isVideoCodec(codecInfo->displayName))
        {
            rez += QString(",video-width=") + QString::number(std::min(codecInfo->width, 1920));
            rez += QString(",video-height=") + QString::number(std::min(codecInfo->height, 1080));
            // The rate the video is muxed at: the one the box sets when it is ticked, else the stream's own. A video
            // whose rate the listing does not know is muxed at 25, the rate tsMuxeR gives a stream without one, and the
            // subtitles were drawn at the 23.976 the box offers, so they came at the wrong pictures.
            if (codecInfo->checkFPS || !codecInfo->fpsTextOrig.isEmpty())
                rez += QString(",fps=") + fpsTextToFpsStr(codecInfo->fpsText);
            else
                rez += QString(",fps=25");
            return rez;
        }
    }
    rez += ",video-width=1920,video-height=1080,fps=23.976";
    return rez;
}

QString TsMuxerWindow::getFileList(QtvCodecInfo* codecInfo)
{
    QString rezStr;

    // the angle of a disc title or playlist, on the line after its source (angle=)
    const QString angle = codecInfo->angle > 1 ? QStringLiteral(", angle=%1").arg(codecInfo->angle) : QString();
    // A DVD title: the disc, and the title by the disc's own number
    if (codecInfo->dvdTitle && !codecInfo->fileList.isEmpty())
        return QString("\"") + codecInfo->fileList[0] + "\", dvd-title=" + QString::number(codecInfo->dvdTitle) +
               angle;
    if (codecInfo->mplsFiles.isEmpty())
    {
        for (int i = 0; i < codecInfo->fileList.size(); ++i)
        {
            if (i > 0)
                rezStr += '+';
            rezStr += QString("\"") + codecInfo->fileList[i] + "\"";
        }
    }
    else
    {
        for (int i = 0; i < codecInfo->mplsFiles.size(); ++i)
        {
            if (i > 0)
                rezStr += '+';
            rezStr += QString("\"") + codecInfo->mplsFiles[i] + "\"";
        }
        rezStr += angle;
    }

    return rezStr;
}

QString cornerToStr(int corner)
{
    if (corner == 0)
        return "topLeft";
    else if (corner == 1)
        return "topRight";
    else if (corner == 2)
        return "bottomRight";
    else
        return "bottomLeft";
}

QString toPipScaleStr(int scaleIdx)
{
    if (scaleIdx == 0)
        return "1";
    if (scaleIdx == 1)
        return "1/2";
    else if (scaleIdx == 2)
        return "1/4";
    else if (scaleIdx == 3)
        return "1.5";
    else
        return "fullScreen";
}

QString TsMuxerWindow::getVideoMetaInfo(QtvCodecInfo* codecInfo)
{
    QString fpsStr;
    QString levelStr;
    QString rezStr = codecInfo->programName + ", ";

    rezStr += getFileList(codecInfo);

    if (codecInfo->checkFPS)
        fpsStr = fpsTextToFpsStr(codecInfo->fpsText);
    if (codecInfo->checkLevel)
        levelStr = QString::number(codecInfo->levelText.toDouble(), 'f', 1);

    if (ui->checkBoxCrop->isChecked() && ui->checkBoxCrop->isEnabled())
        rezStr += QString(", ") + "restoreCrop";
    if (!fpsStr.isEmpty())
        rezStr += QString(", ") + "fps=" + fpsStr;
    if (!levelStr.isEmpty())
        rezStr += QString(", ") + "level=" + levelStr;
    if (codecInfo->addSEIMethod == 1)
        rezStr += QString(", ") + "insertSEI";
    else if (codecInfo->addSEIMethod == 2)
        rezStr += QString(", ") + "forceSEI";
    if (codecInfo->addSPS)
        rezStr += QString(", ") + "contSPS";
    if (codecInfo->hdr10Override && codecInfo->programName == "V_MPEGH/ISO/HEVC" &&
        (ui->radioButtonBluRay->isChecked() || ui->radioButtonBluRayISO->isChecked()))
        rezStr += QString(", ") + "hdr10-override";
    if (codecInfo->delPulldown == 1)
        rezStr += QString(", ") + "delPulldown";
    if (!codecInfo->arText.isEmpty())
        rezStr += QString(", ") + "ar=" + codecInfo->arText;

    if (codecInfo->isSecondary)
    {
        rezStr += QString(", secondary");
        rezStr += QString(", pipCorner=%1").arg(cornerToStr(ui->comboBoxPipCorner->currentIndex()));
        rezStr += QString(" ,pipHOffset=%1").arg(ui->spinBoxPipOffsetH->value());
        rezStr += QString(" ,pipVOffset=%1").arg(ui->spinBoxPipOffsetV->value());
        rezStr += QString(", pipScale=%1").arg(toPipScaleStr(ui->comboBoxPipSize->currentIndex()));
        rezStr += QString(", pipLumma=%1").arg(ui->pipTransparencySpinBox->value());
    }

    return rezStr;
}

QString TsMuxerWindow::getAudioMetaInfo(QtvCodecInfo* codecInfo)
{
    QString rezStr = codecInfo->programName + ", ";
    rezStr += getFileList(codecInfo);
    if (codecInfo->delay != 0 || codecInfo->listedDelay != 0)
        rezStr += QString(", timeshift=") + QString::number(codecInfo->delay) + "ms";
    if (codecInfo->dtsDownconvert && codecInfo->programName == "A_DTS")
        rezStr += ", down-to-dts";
    else if (codecInfo->dtsDownconvert && codecInfo->programName == "A_AC3")
        rezStr += ", down-to-ac3";
    else if (codecInfo->dropAc3Core && codecInfo->programName == "A_AC3")
        rezStr += ", drop-ac3-core";
    // Not exclusive with the two above by accident but by meaning: this one asks for the pair AND
    // both halves, so it only applies when neither of the others has already narrowed the track.
    if (codecInfo->splitAc3Core && codecInfo->programName == "A_AC3" && !codecInfo->dropAc3Core &&
        !codecInfo->dtsDownconvert)
        rezStr += ", split-ac3-core";
    if (codecInfo->w64 && codecInfo->programName == "A_LPCM")
        rezStr += ", w64";
    if (codecInfo->isSecondary)
        rezStr += ", secondary";
    return rezStr;
}

void TsMuxerWindow::updateMuxTime1()
{
    QTime t = ui->muxTimeEdit->time();
    int clock = (t.hour() * 3600 + t.minute() * 60 + t.second()) * 45000ll + t.msec() * 45ll;
    ui->muxTimeClock->blockSignals(true);
    ui->muxTimeClock->setValue(clock);
    ui->muxTimeClock->blockSignals(false);
    updateMetaLines();
}

void TsMuxerWindow::updateMuxTime2()
{
    double timeF = ui->muxTimeClock->value() / 45000.0;
    ui->muxTimeEdit->blockSignals(true);
    ui->muxTimeEdit->setTime(qTimeFromFloat(timeF));
    ui->muxTimeEdit->blockSignals(false);
    updateMetaLines();
}

void TsMuxerWindow::snapMuxTimeToBdMinimum()
{
    // Below 524280 ticks of the 45 kHz clock (11.65 s) Blu-ray navigation breaks:
    // players subtract the decoder preroll near zero and their 32-bit 45 kHz
    // registers underflow. The muxer raises such values too; mirror it here so the
    // field never rests in the broken range. 0 keeps the default (600 s).
    constexpr int MIN_BD_START_TICKS = 524280;
    // Only for outputs the muxer itself gates. It exempts plain TS, Matroska and demux mode (a
    // Matroska file does not use the start time at all), so snapping there would make the GUI
    // stricter than the CLI for the same request, silently and with no way to opt out.
    if (ui->radioButtonTS->isChecked() || ui->radioButtonMKV->isChecked() || ui->radioButtonDemux->isChecked())
        return;
    const int v = ui->muxTimeClock->value();
    if (v > 0 && v < MIN_BD_START_TICKS)
        ui->muxTimeClock->setValue(MIN_BD_START_TICKS);  // valueChanged re-syncs the time edit
}

void TsMuxerWindow::onLanguageComboBoxIndexChanged(int idx)
{
    auto lang = ui->languageSelectComboBox->itemData(idx).toString();
    // Qt translates its own strings, the standard Yes, No, OK and Cancel buttons among them, and
    // those live in qtbase_<lang>.qm. That file belongs to the qttranslations module, and the
    // Windows build compiles Qt from qtbase alone so the module is not there and QLibraryInfo has
    // nothing to point at. The result is a dialog whose title and text are translated and whose
    // buttons are not. Look beside the executable first, which is where a package can put them,
    // and keep the old path for a build against a full system Qt.
    if (!qtCoreTranslator.load(QString("qtbase_%1").arg(lang), QCoreApplication::applicationDirPath()))
        (void)qtCoreTranslator.load(QString("qtbase_%1").arg(lang), QLibraryInfo::path(QLibraryInfo::TranslationsPath));
    (void)tsMuxerTranslator.load(QString("tsmuxergui_%1").arg(lang), ":/i18n");
    QFile aboutContent(QString(":/about_%1.html").arg(lang));
    if (aboutContent.open(QIODevice::ReadOnly))
    {
        ui->textEdit->setHtml(QString::fromUtf8(aboutContent.readAll()));
    }
    else
    {
        qWarning() << "Failed to open about.html for language" << lang << aboutContent.errorString();
        ui->textEdit->clear();
    }
    writeSettings();
}

void TsMuxerWindow::onMergeAc3FileBrowseClicked()
{
    if (disableUpdatesCnt)
        return;
    QtvCodecInfo* codecInfo = getCurrentCodec();
    if (!codecInfo || codecInfo->programName != "A_MLP")
        return;

    const QString startDir =
        (!codecInfo->fileList.isEmpty()) ? QFileInfo(codecInfo->fileList[0]).absolutePath() : lastInputDir;
    const QString fileName =
        QFileDialog::getOpenFileName(this, tr("Select AC-3 file"), startDir, tr("AC-3 audio (*.ac3);;All files (*)"));
    if (fileName.isEmpty())
        return;

    disableUpdatesCnt++;
    ui->mergeAc3FileLineEdit->setText(QDir::toNativeSeparators(fileName));
    disableUpdatesCnt--;
    onAudioSubtitlesParamsChanged();
}

// The box holds a row number and the muxer reports the track number, so the two disagree on any
// file where they differ, and the user is left holding a number that appears nowhere in the log.
// Both are shown, in the box itself, so "merged from track 3" lines up with something on screen.
void TsMuxerWindow::updateMergeAc3Suffix()
{
    const int row = ui->mergeAc3TrackSpinBox->value();
    if (row <= 0)
    {
        ui->mergeAc3TrackSpinBox->setSuffix(QString());
        return;
    }
    ui->mergeAc3TrackSpinBox->setSuffix(tr(" (track %1 in the file)").arg(mergeAc3TrackIdForRow(row)));
}

// The merge box is read as a row number in the track list, which is the only numbering the
// user can see. This turns it into the track number the meta needs.
int TsMuxerWindow::mergeAc3TrackIdForRow(const int row) const
{
    if (row < 1 || row > ui->trackLV->rowCount())
        return row;  // out of range: pass it through, so a hand edited value still works
    auto iCodec = ui->trackLV->item(row - 1, 0)->data(Qt::UserRole).toLongLong();
    const auto* info = reinterpret_cast<const QtvCodecInfo*>(iCodec);
    return info != nullptr && info->trackID != 0 ? info->trackID : row;
}

// The merge box of a TrueHD row holds a row number, and a row that is moved or removed takes its number with it: the
// merge went on naming the number, so after Move up, Move down or Remove another track was merged and the chosen one
// was muxed on its own. from and to are row numbers counted from 1; to is 0 when the row is removed, and a merge into
// it is then cleared.
void TsMuxerWindow::followMergeRows(const int from, const int to)
{
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        QtvCodecInfo* info = getCodecInfo(i);
        if (info == nullptr || info->programName != "A_MLP" || info->mergeAc3Track <= 0)
            continue;
        int& merge = info->mergeAc3Track;
        if (to == 0)
            merge = merge == from ? 0 : merge > from ? merge - 1 : merge;
        else if (merge == from)
            merge = to;
        else if (merge == to)
            merge = from;
    }
    const int current = ui->trackLV->currentRow();
    if (current >= 0)
        if (const QtvCodecInfo* info = getCodecInfo(current); info != nullptr && info->programName == "A_MLP")
        {
            const QSignalBlocker block(ui->mergeAc3TrackSpinBox);
            ui->mergeAc3TrackSpinBox->setValue(info->mergeAc3Track);
            updateMergeAc3Suffix();
        }
}

// ** A DISC MADE FROM TSMUXER'S OWN MATROSKA FILE LOST THE CORE OF ITS TRUEHD. **
//
// A Matroska file cannot carry the AC-3 core inside the TrueHD track the way a disc does, so tsMuxeR
// writes it as an AC-3 track of its own, on the track number straight after the TrueHD track, with
// the same language. Authored back to a disc, the two have to be braided again with merge-ac3-track=,
// and the window never set it: the disc got a TrueHD track with no core, which a Blu-ray player
// cannot rely on, and the core as a surplus AC-3 track, under a warning about MLP.
//
// The file says nothing more about the pair than that layout, so the layout is what is looked for, in
// a Matroska source only (.mkv, .mka, .mks, .mk3d, .webm) and for a TrueHD track listed without a core:
// the next track of the same file, AC-3 and not E-AC-3, in the same language, not marked as a
// commentary, with the channels a core of this TrueHD has (as many, up to 5.1). The user is asked
// about such a pair (updateAutoCoreMerge); anything else is left alone, and the box still takes any
// row by hand.
int TsMuxerWindow::coreRowOfTrueHd(const int row) const
{
    const auto codecAt = [this](const int i)
    { return reinterpret_cast<const QtvCodecInfo*>(ui->trackLV->item(i, 0)->data(Qt::UserRole).toLongLong()); };
    // "Channels: 5.1" is six, "Channels: 8" is eight
    const auto channels = [](const QString& descr)
    {
        const int p = descr.indexOf("Channels: ");
        if (p < 0)
            return 0;
        const QStringList parts = QtCompat::strMid(descr, p + 10).section(' ', 0, 0).split('.');
        int n = 0;
        for (const QString& part : parts) n += part.toInt();
        return n;
    };
    // Only a TrueHD track of a Matroska file, and only one without a core of its own. A disc or a transport stream
    // carries the core inside the TrueHD stream (listed as A_AC3, "AC3 core + TRUE-HD"), and an AC-3 track beside
    // it there is a real track of its own, such as a compatibility mix: never paired, never asked about.
    const QtvCodecInfo* thd = codecAt(row);
    if (thd == nullptr || thd->programName != "A_MLP" || thd->displayName != "TRUE-HD" || thd->trackID == 0 ||
        thd->fileList.isEmpty() || thd->descr.contains(QLatin1String("core"), Qt::CaseInsensitive) ||
        !isMatroskaOutputName(unquoteStr(thd->fileList[0])))
        return 0;
    const int coreChannels = qMin(channels(thd->descr), 6);
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        const QtvCodecInfo* ac3 = codecAt(i);
        if (i != row && ac3 != nullptr && ac3->programName == "A_AC3" && ac3->displayName == "AC3" &&
            ac3->trackID == thd->trackID + 1 && !ac3->fileList.isEmpty() && ac3->fileList[0] == thd->fileList[0] &&
            ac3->srcLang == thd->srcLang && !ac3->srcMkvFlags[5] && coreChannels > 0 &&
            channels(ac3->descr) == coreChannels)
            return i + 1;
    }
    return 0;
}

// A disc gets the pair braided when the user says the AC-3 track is the core, every other output keeps
// them as they are: a Matroska output writes the core beside the TrueHD again by itself, and a demux
// writes what the file holds. Only while the TrueHD row is ticked, so that unticking it to keep the
// AC-3 alone still keeps the AC-3.
//
// The layout alone does not prove that the AC-3 track is the core, so the window asks, once per pair:
// when a disc is first the output with the pair ticked, on adding the file or on choosing a disc. The
// answer is kept. "As the core" is offered first for a file tsMuxeR wrote, where the pair is the core
// it put there, and "As a separate track" for any other.
void TsMuxerWindow::updateAutoCoreMerge()
{
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        QtvCodecInfo* info = getCodecInfo(i);
        if (info == nullptr || info->programName != "A_MLP" || !info->mergeAc3Auto || !info->mergeAc3File.isEmpty())
            continue;
        const int core = coreRowOfTrueHd(i);
        if (core == 0 && info->mergeAc3Track == 0)
            continue;
        const bool disc = core > 0 && isDiskOutput() && ui->trackLV->item(i, 0)->checkState() == Qt::Checked;
        if (disc && info->coreAnswer == 0)
        {
            info->coreAnswer = 2;  // while the question is open, a call from its event loop does not ask again
            info->coreAnswer = askCoreOfTrueHd(i, core) ? 1 : 2;
        }
        info->mergeAc3Track = disc && info->coreAnswer == 1 ? core : 0;
        if (i == ui->trackLV->currentRow())
        {
            const QSignalBlocker block(ui->mergeAc3TrackSpinBox);
            ui->mergeAc3TrackSpinBox->setValue(info->mergeAc3Track);
            updateMergeAc3Suffix();
            updateMergeAc3Exclusion(info);
        }
    }
}

bool TsMuxerWindow::askCoreOfTrueHd(const int thdRow, const int coreRow)
{
    const QtvCodecInfo* thd = getCodecInfo(thdRow);
    const QtvCodecInfo* ac3 = getCodecInfo(coreRow - 1);
    const int p = ac3->descr.indexOf("Channels: ");
    const QString channels = p >= 0 ? QtCompat::strMid(ac3->descr, p + 10).section(' ', 0, 0) : QString();
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("TrueHD and its AC-3 core"));
    msgBox.setIcon(QMessageBox::Question);
    msgBox.setText(tr("Track %1 (AC-3 %2, %3) looks like the AC-3 core of the TrueHD track %4. Put it on the disc as "
                      "the core of the TrueHD track, or keep it as a separate audio track?")
                       .arg(ac3->trackID)
                       .arg(channels, ac3->srcLang.isEmpty() ? QStringLiteral("und") : ac3->srcLang)
                       .arg(thd->trackID));
    msgBox.setInformativeText(QDir::toNativeSeparators(unquoteStr(thd->fileList[0])));
    QPushButton* asCore = msgBox.addButton(tr("As the core"), QMessageBox::AcceptRole);
    QPushButton* asTrack = msgBox.addButton(tr("As a separate track"), QMessageBox::RejectRole);
    msgBox.setDefaultButton(thd->srcByTsMuxer ? asCore : asTrack);
    msgBox.setEscapeButton(asTrack);
    msgBox.exec();
    return msgBox.clickedButton() == asCore;
}

void TsMuxerWindow::showMergeOfRow(const int row)
{
    if (row != ui->trackLV->currentRow())
        return;
    QtvCodecInfo* info = getCodecInfo(row);
    const QSignalBlocker blockTrack(ui->mergeAc3TrackSpinBox);
    const QSignalBlocker blockFile(ui->mergeAc3FileLineEdit);
    ui->mergeAc3TrackSpinBox->setValue(info->mergeAc3Track);
    ui->mergeAc3FileLineEdit->setText(QDir::toNativeSeparators(info->mergeAc3File));
    updateMergeAc3Suffix();
    updateMergeAc3Exclusion(info);
}

double TsMuxerWindow::durationOfSource(const QString& source) const
{
    for (int i = 0; i < ui->inputFilesLV->count(); ++i)
    {
        const QListWidgetItem* item = ui->inputFilesLV->item(i);
        if (unquoteStr(item->data(FileNameRole).toString()) == source)
            return item->data(FileDurationRole).toDouble();
    }
    return 0;
}

bool TsMuxerWindow::offerAc3Cores()
{
    bool changed = false;
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        QtvCodecInfo* info = getCodecInfo(i);
        if (info == nullptr || info->programName != "A_MLP" || info->displayName != "TRUE-HD" ||
            ui->trackLV->item(i, 0)->checkState() != Qt::Checked ||
            info->descr.contains(QLatin1String("core"), Qt::CaseInsensitive) || info->mergeAc3Track > 0 ||
            !info->mergeAc3File.isEmpty() || info->noCoreWanted || info->fileList.isEmpty())
            continue;

        // The AC-3 tracks of the same file. Not one that serves another TrueHD track, and not the one the user
        // already called a separate track.
        const QString source = unquoteStr(info->fileList[0]);
        QStringList names;
        QList<int> rows;
        const int nextTrackCore = coreRowOfTrueHd(i);
        for (int j = 0; j < ui->trackLV->rowCount(); ++j)
        {
            const QtvCodecInfo* ac3 = getCodecInfo(j);
            if (j == i || ac3 == nullptr || ac3->programName != "A_AC3" || ac3->displayName != "AC3" ||
                ac3->trackID == 0 || ac3->fileList.isEmpty() || ac3->fileList[0] != info->fileList[0] ||
                isRowMergedIntoTrueHd(j) || (nextTrackCore == j + 1 && info->coreAnswer == 2))
                continue;
            const int p = ac3->descr.indexOf("Channels: ");
            names << tr("Track %1: AC-3 %2, %3")
                         .arg(ac3->trackID)
                         .arg(p >= 0 ? QtCompat::strMid(ac3->descr, p + 10).section(' ', 0, 0) : QString(),
                              ac3->srcLang.isEmpty() ? QStringLiteral("und") : ac3->srcLang);
            rows << j;
        }

        const QString sourceProblem =
            info->fileList.size() > 1 || !info->mplsFiles.isEmpty() || info->dvdTitle != 0 ||
                    !QFileInfo(source).isFile() || QFileInfo(source).suffix().compare("iso", Qt::CaseInsensitive) == 0
                ? tr("A core can only be created from a track of a single file.")
                : QString();
        const QString question =
            info->trackID != 0
                ? tr("The TrueHD track %1 of %2 has no AC-3 core. Some players will not play it.")
                      .arg(info->trackID)
                      .arg(QFileInfo(source).fileName())
                : tr("The TrueHD file %1 has no AC-3 core. Some players will not play it.")
                      .arg(QFileInfo(source).fileName());
        const QString savedBefore = m_coreEncoderPath;
        const Ac3CoreDialog::Choice choice =
            Ac3CoreDialog::ask(this, question, names, sourceProblem, &m_coreEncoderPath);
        if (m_coreEncoderPath != savedBefore)
            writeSettings();
        if (choice.what == Ac3CoreDialog::Choice::Cancel)
        {
            dropPendingCores();
            return false;
        }
        if (choice.what == Ac3CoreDialog::Choice::NoCore)
        {
            info->noCoreWanted = true;
            continue;
        }
        if (choice.what == Ac3CoreDialog::Choice::UseTrack)
        {
            info->mergeAc3Track = rows[choice.track] + 1;
            info->mergeAc3Auto = false;
        }
        else
        {
            // Beside the output, where there is room for it, under a name of its own
            QString folder = QFileInfo(ui->outFileName->text()).absolutePath();
            if (!QDir(folder).exists())
                folder = QDir::tempPath();
            const QString base = QFileInfo(source).completeBaseName() + QStringLiteral("_core");
            QString target = QDir(folder).filePath(base + QStringLiteral(".ac3"));
            // Neither a file that is there, nor the name of a core still to be made for this job
            const auto taken = [&]
            {
                return QFile::exists(target) ||
                       std::any_of(m_pendingCores.cbegin(), m_pendingCores.cend(),
                                   [&](const PendingCore& p) { return p.info->mergeAc3File == target; });
            };
            for (int n = 2; taken(); ++n) target = QDir(folder).filePath(base + QStringLiteral("_%1.ac3").arg(n));
            info->mergeAc3File = target;
            // Matroska tracks are numbered by their place in the file, a transport stream's by the PID
            const QString map = info->trackID == 0              ? QStringLiteral("0:a:0")
                                : isMatroskaOutputName(source) ? QStringLiteral("0:%1").arg(info->trackID - 1)
                                                               : QStringLiteral("0:i:%1").arg(info->trackID);
            const int p = info->descr.indexOf("Channels: ");
            int channels = 0;
            if (p >= 0)
                for (const QString& part : QtCompat::strMid(info->descr, p + 10).section(' ', 0, 0).split('.'))
                    channels += part.toInt();
            m_pendingCores.append({info, choice.encoder, map, channels});
        }
        showMergeOfRow(i);
        changed = true;
    }
    if (changed)
        updateMetaLines();
    return true;
}

bool TsMuxerWindow::makeAc3Cores()
{
    while (!m_pendingCores.isEmpty())
    {
        const PendingCore core = m_pendingCores.first();
        Ac3CoreDialog::Job job;
        job.encoder = core.encoder;
        job.source = unquoteStr(core.info->fileList[0]);
        job.map = core.map;
        job.channels = core.channels;
        job.seconds = durationOfSource(job.source);
        job.target = core.info->mergeAc3File;
        Ac3CoreDialog dialog(this, job);
        if (dialog.exec() != QDialog::Accepted)
            return false;  // the file is gone: dropPendingCores takes it off the track
        m_madeCores << job.target;
        m_pendingCores.removeFirst();
    }
    return true;
}

void TsMuxerWindow::dropPendingCores()
{
    if (m_pendingCores.isEmpty())
        return;
    for (const PendingCore& core : m_pendingCores) core.info->mergeAc3File.clear();
    m_pendingCores.clear();
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
        if (getCodecInfo(i) != nullptr)
            showMergeOfRow(i);
    updateMetaLines();
}

void TsMuxerWindow::removeMadeCores()
{
    if (m_madeCores.isEmpty())
        return;
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        QtvCodecInfo* info = getCodecInfo(i);
        if (info != nullptr && m_madeCores.contains(info->mergeAc3File))
        {
            info->mergeAc3File.clear();
            showMergeOfRow(i);
        }
    }
    for (const QString& file : m_madeCores) QFile::remove(file);
    m_madeCores.clear();
    updateMetaLines();
}

// True when this row is the one some TrueHD track is folding into itself.
bool TsMuxerWindow::isRowMergedIntoTrueHd(const int row) const
{
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        auto iCodec = ui->trackLV->item(i, 0)->data(Qt::UserRole).toLongLong();
        const auto* info = reinterpret_cast<const QtvCodecInfo*>(iCodec);
        if (info != nullptr && info->programName == "A_MLP" && info->mergeAc3Track == row + 1 && i != row)
            return true;
    }
    return false;
}

void TsMuxerWindow::updateMetaLines()
{
    // "Forced only" is a disc's choice (default=forced); a Matroska output gets default=all, so the tick did nothing
    // there and is greyed.
    ui->defaultSubTrackForcedOnlyCheckBox->setEnabled(isDiskOutput() && ui->defaultSubTrackCheckBox->isChecked());
    if (!m_updateMeta || disableUpdatesCnt > 0)
        return;

    if (m_metaUserOverride)
    {
        // The meta file edited by hand does not take the settings of the tabs: the line says so while they would give
        // another meta file than the one built before the edit, and not for a click that changes nothing
        if (m_wasShown)
            ui->metaHintLabel->setVisible(buildMetaText() != m_lastBuiltMeta);
        return;
    }

    const QString metaContent = buildMetaText();
    m_lastBuiltMeta = metaContent;
    bool wasBlocked = ui->memoMeta->blockSignals(true);
    ui->memoMeta->setPlainText(metaContent);
    ui->memoMeta->blockSignals(wasBlocked);
}

// The meta file the settings of the tabs give
QString TsMuxerWindow::buildMetaText()
{
    QString metaContent;
    metaContent.append(getMuxOpts() + '\n');
    QString tmpFps;
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        auto codecInfo = getCodecInfo(i);
        if (!codecInfo)
            continue;

        if (isVideoCodec(codecInfo->displayName))
        {
            if (codecInfo->checkFPS)
                tmpFps = fpsTextToFpsStr(codecInfo->fpsText);
            else
                tmpFps = codecInfo->fpsTextOrig;
            break;
        }
    }

    QString prefix;
    QString postfix;

    bool bluray3D = isDiskOutput() && m_3dMode;

    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        if (ui->trackLV->item(i, 0)->checkState() == Qt::Checked)
            prefix = "";
        else
            prefix = "#";
        auto codecInfo = getCodecInfo(i);
        if (!codecInfo)
            continue;

        // Force-disable audio tracks when the output format doesn't support them
        if (isCodecIncompatibleWithFormat(codecInfo->programName))
            prefix = "#";

        // A track folded into a TrueHD track travels inside it, so it must not also be muxed
        // on a line of its own. The muxer refuses that combination, and until now the
        // interface could produce it: setting the merge left the AC-3 row ticked, and both
        // lines were written. The row is left visible and ticked, because the user chose it
        // deliberately; it is its separate output line that is wrong.
        if (isRowMergedIntoTrueHd(i))
            prefix = "#";

        postfix.clear();
        if (codecInfo->programName.startsWith('S'))
        {
            if ((isDiskOutput() || ui->radioButtonMKV->isChecked()) && ui->defaultSubTrackCheckBox->isChecked() &&
                ui->defaultSubTrackComboBox->currentData().toInt() == i)
            {
                // The forced/all distinction is a disc concept: on a disc it selects which subtitle
                // the player shows by default. A Matroska output reads default=forced as the forced track
                // (FlagForced), so "forced only" marked a whole subtitle track forced, and a player showed it to
                // everyone. The track's own Forced box says that; here an .mkv gets the default track only.
                postfix += QString(", default=") +
                           (isDiskOutput() && ui->defaultSubTrackForcedOnlyCheckBox->isChecked() ? "forced" : "all");
            }
        }
        if (codecInfo->displayName == "PGS")
        {
            if (codecInfo->bindFps && !tmpFps.isEmpty())
                postfix += QString(", fps=") + tmpFps;
            // "zero" (offsetId -1) was written as no 3d-plane= at all, which gave the subtitle no offset (0xff). Since
            // the mux takes the playlist's offset when 3d-plane= is absent, "zero" on a track of a 3D playlist came
            // out at the playlist's plane. 255 is the core's own "no offset", the value it wrote for "zero" before.
            if (bluray3D)
                postfix += QString(", 3d-plane=%1").arg(codecInfo->offsetId >= 0 ? codecInfo->offsetId : 255);
        }
        if (codecInfo->displayName == "SRT")
        {
            postfix += getSrtParams();
            if (!keepTextApplies() && ui->saveSrtCheckBox->isChecked())
                postfix += QString(", save-srt=\"%1\"").arg(saveSrtFileName(codecInfo, i));
            if (keepTextApplies() && ui->radioButtonMKV->isChecked() && !ui->textStAsSrtCheckBox->isChecked() &&
                codecInfo->descr.contains(QLatin1String("TextST")))
                postfix += ", keep-textst";
            // ASS and SSA from Matroska are drawn with libass; a track of another text format takes no notice of these
            if (!keepTextApplies() && ui->assAsSrtCheckBox->isChecked())
                postfix += ", ass-as-srt";
            else if (!keepTextApplies() && !ui->fontsDirLineEdit->text().trimmed().isEmpty())
                postfix +=
                    QString(", fonts-dir=\"%1\"").arg(QDir::toNativeSeparators(ui->fontsDirLineEdit->text().trimmed()));
            // The plane was written as offset=, which the mux does not know: the drawn subtitles sat at the depth of
            // the screen whatever plane was chosen. Written as for a PGS track, with 255 for "zero".
            if (bluray3D)
                postfix += QString(", 3d-plane=%1").arg(codecInfo->offsetId >= 0 ? codecInfo->offsetId : 255);
        }
        if (codecInfo->trackID != 0)
            postfix += QString(", track=") + QString::number(codecInfo->trackID);
        if (codecInfo->programName == "A_MLP" && codecInfo->mergeAc3Track > 0)
        {
            // The box asks for a number and the only numbers on screen are the row numbers in
            // the track list, so that is what it is read as. It is NOT the same as the track
            // number the meta needs: a Dolby Vision file puts its two layers on two rows that
            // share one track, so from the row below them onwards the two numberings differ.
            // Reading 4 off an AC-3 row and writing 4 into the meta pointed at the track after
            // it. Without a second video row the two are the same number, so nothing changes
            // for a file that has none.
            const int mergedTrackId = mergeAc3TrackIdForRow(codecInfo->mergeAc3Track);
            if (mergedTrackId != 0 && mergedTrackId != codecInfo->trackID)
                postfix += QString(", merge-ac3-track=") + QString::number(mergedTrackId);
        }
        else if (codecInfo->programName == "A_MLP" && !codecInfo->mergeAc3File.isEmpty())
        {
            postfix += QString(", merge-ac3-file=") + quoteStr(codecInfo->mergeAc3File);
        }
        // A Matroska output keeps the source's language by itself, and with it a BCP 47 tag such as de-CH or pt-BR
        // that the three letter code in the box cannot carry. So lang= is written for it only when the language
        // was changed.
        const bool keepSourceLang = ui->radioButtonMKV->isChecked() && codecInfo->lang == codecInfo->srcLang;
        if (!codecInfo->lang.isEmpty() && isLangCodeForMeta(codecInfo->lang) && !keepSourceLang)
            postfix += QString(", lang=") + codecInfo->lang;
        if (codecInfo->subTrack != 0)
            postfix += QString(", subTrack=") + QString::number(codecInfo->subTrack);
        // The Matroska page: only what differs from the source, which tsMuxeR keeps by itself. An emptied name
        // leaves the track without one, an unticked flag the source had set is cleared with =0.
        if (ui->radioButtonMKV->isChecked())
        {
            if (codecInfo->trackName != codecInfo->srcTrackName)
            {
                QString name = codecInfo->trackName;
                name.remove(QLatin1Char('"'));
                postfix += QString(", track-name=") + quoteStr(name);
            }
            for (int k = 0; k < 6; ++k)
                if (codecInfo->mkvFlags[k] != codecInfo->srcMkvFlags[k])
                    postfix += QString(", ") + MKV_FLAG_KEYS[k] + (codecInfo->mkvFlags[k] ? "" : "=0");
        }
        if (isVideoCodec(codecInfo->displayName))
            metaContent.append(prefix + getVideoMetaInfo(codecInfo) + postfix + '\n');
        else
        {
            // Matroska honours "default" as of 2.16.0, so the choice must reach the meta for an
            // .mkv as well, not only for a disc. Without this the checkbox is present, is
            // settable, and silently does nothing on the one output format where a viewer would
            // notice: every player would just take the first audio track.
            if ((isDiskOutput() || ui->radioButtonMKV->isChecked()) && ui->defaultAudioTrackCheckBox->isChecked() &&
                ui->defaultAudioTrackComboBox->currentData().toInt() == i && codecInfo->programName.startsWith('A'))
            {
                postfix += QString(", default");
            }
            metaContent.append(prefix + getAudioMetaInfo(codecInfo) + postfix + '\n');
        }
    }
    return metaContent;
}

void TsMuxerWindow::onFontBtnClicked()
{
    bool ok;
    auto font = QFontDialog::getFont(&ok, fontSettingsModel->font(), this);
    if (ok)
    {
        fontSettingsModel->setFont(font);
        writeSettings();
        updateMetaLines();
    }
}

void TsMuxerWindow::onColorBtnClicked()
{
    // getColor returns an invalid colour when the dialog is cancelled, and its rgba() is opaque black: Cancel turned
    // every text subtitle drawn into pictures black, and the black was saved for the next start.
    // QColor(QRgb) takes the colour as opaque, and the dialog showed no alpha, so a semi-transparent colour such as
    // 0x80ffffff came back from OK as 0xffffffff. The colour is now given with its alpha, and the dialog shows it.
    const QColor color = QColorDialog::getColor(QColor::fromRgba(fontSettingsModel->color()), this, QString(),
                                                QColorDialog::ShowAlphaChannel);
    if (!color.isValid())
        return;
    fontSettingsModel->setColor(color.rgba());

    writeSettings();
    updateMetaLines();
}

void TsMuxerWindow::onGeneralCheckboxClicked()
{
    ui->editMaxBitrate->setEnabled(ui->checkBoxRVBR->isChecked());
    ui->editMinBitrate->setEnabled(ui->checkBoxRVBR->isChecked());
    ui->editCBRBitrate->setEnabled(ui->checkBoxCBR->isChecked());
    // The blank playlist number follows its own tick AND the output, or ticking the box would put
    // the number back for an output that never writes a playlist at all.
    const bool blankPL = ui->checkBoxBlankPL->isChecked() && isDiskOutput();
    ui->BlackplaylistCombo->setEnabled(blankPL);
    ui->BlackplaylistLabel->setEnabled(blankPL);
    updateMetaLines();
}

// The Blu-ray tab took no notice of the chosen output. Every control on it stayed live for TS, MKV
// and Demux alike, measured across all seven outputs before any of this was written: 23 controls,
// not one of which changed state. Their values were already being dropped, since getMuxOpts() only
// writes them out under isDiskOutput() and friends, so what was wrong was the tab saying otherwise.
//
// Greyed rather than hidden. Hiding suits a control whose answer is "nothing, in this mode", the way
// the Dolby Vision row and the layer break guard are already hidden, but a whole tab that empties and
// refills as the output changes is worse than one that dims. Greying does NOT silence a tooltip in
// this build, measured on two widget types, so the explanation stays reachable either way.
//
// Nothing is cleared. Every setting survives the trip, so choosing MKV and coming back to Blu-ray
// finds the tab as it was left.
void TsMuxerWindow::updateBluRayTabEnabled()
{
    const bool disk = isDiskOutput();  // Blu-ray, Blu-ray ISO, AVCHD
    const bool bd = ui->radioButtonBluRay->isChecked() || ui->radioButtonBluRayISO->isChecked();
    // Default track flags AND a chapter list are both written for Matroska, not only for a disc:
    // see getAudioMetaInfo and getSubMetaInfo, which test isDiskOutput() OR MKV, and the muxer,
    // which has written Matroska chapters since 2.16.0.
    const bool mkvOrDisk = disk || ui->radioButtonMKV->isChecked();

    // Whole groups, because everything inside them shares one condition. Disabling the group leaves
    // each child's own enabled flag untouched, so the rules that govern them individually still
    // apply when the group comes back.
    if (auto* chapters = findChild<QGroupBox*>("groupBox_3"))
        chapters->setEnabled(mkvOrDisk);  // a custom list is written for MKV as well
    if (auto* threeD = findChild<QGroupBox*>("groupBox_11"))
        threeD->setEnabled(disk);  // --right-eye
    if (auto* defTracks = findChild<QGroupBox*>("defaultTracksGroupBox"))
        defTracks->setEnabled(mkvOrDisk);  // default= on the track lines

    // Inside that group, the automatic chapters are a disc matter: --auto-chapters is turned into
    // playlist marks by createMPLSFile and is read by nothing else, so for MKV its radio and its
    // length are turned off rather than left looking live. The length also has a rule of its own in
    // onChapterParamsChanged, which tests the same two things.
    ui->radioButtonAutoChapter->setEnabled(disk);
    if (auto* autoLenLabel = findChild<QLabel*>("label_4"))
        autoLenLabel->setEnabled(disk);
    ui->spinEditChapterLen->setEnabled(disk && ui->radioButtonAutoChapter->isChecked());

    // The Options group is mixed and cannot be greyed as a whole: the mux start time applies to
    // EVERY output, and it lives in there beside options that do not.
    ui->checkBoxV3->setEnabled(bd);  // --blu-ray-v3, and AVCHD has no V3
    // The read rate ceiling only means anything for a disc, and the limit it uses depends on
    // whether that disc is V3, so it follows the same condition as the box above it.
    ui->checkBoxRateLimit->setEnabled(bd);
    // The choice exists only for V3. An HD Blu-ray has one figure, 48 Mbit/s, so there is nothing
    // to choose and the box alone says everything.
    ui->comboRateLimit->setEnabled(bd && ui->checkBoxRateLimit->isChecked() && ui->checkBoxV3->isChecked());
    ui->checkBoxRateLimit->setText(ui->checkBoxV3->isChecked() && bd
                                       ? tr("Hold the disc to the rate a player can read:")
                                       : tr("Hold the disc to the rate a player can read (48 Mbit/s)"));
    ui->checkBoxBlankPL->setEnabled(disk);  // --insertBlankPL
    const bool blankPL = disk && ui->checkBoxBlankPL->isChecked();
    ui->BlackplaylistCombo->setEnabled(blankPL);
    ui->BlackplaylistLabel->setEnabled(blankPL);
    ui->spinBoxMplsNum->setEnabled(disk);  // --mplsOffset
    ui->spinBoxM2tsNum->setEnabled(disk);  // --m2tsOffset

    // The picture in picture group is deliberately left alone. It is not decided by the output at
    // all: those parameters go out only for a track marked secondary, so its condition belongs to
    // the selected track and would need its own pass.
}

void TsMuxerWindow::onGeneralSpinboxValueChanged() { updateMetaLines(); }

void TsMuxerWindow::onChapterParamsChanged()
{
    ui->memoChapters->setEnabled(ui->radioButtonCustomChapters->isChecked());
    // A length only means something where --auto-chapters is honoured, which is BD and AVCHD.
    ui->spinEditChapterLen->setEnabled(ui->radioButtonAutoChapter->isChecked() && isDiskOutput());
    QObject* snd = sender();
    if (snd == ui->radioButtonNoChapters || snd == ui->radioButtonAutoChapter)
        m_customChaptersUserOverride = false;
    else if (snd == ui->memoChapters && ui->radioButtonCustomChapters->isChecked())
        m_customChaptersUserOverride = true;
    updateMetaLines();
}

// Demux output refuses splitting, so it cannot split. Ticking the boxes there changed nothing at all and said nothing
// about it: a 421 MB source asked to split at 50 MB came back as one whole file with a success message, while the same
// settings aimed at .ts gave nine parts. Matroska output splits by all three options, and is offered them.
bool TsMuxerWindow::outputCanSplit() const { return !ui->radioButtonDemux->isChecked(); }

// The custom chapters and the cut were written into the meta line unchecked, and only tsMuxeR refused a wrong one
// after Start. Each is now checked when it is left, with tsMuxeR's own rules: a wrong value gets a red frame and its
// reason as the tooltip, and Start shows the reason and does not run.
QString TsMuxerWindow::customChaptersProblem() const
{
    if (!ui->radioButtonCustomChapters->isChecked())
        return QString();
    QList<QString> lines;
    splitLines(ui->memoChapters->toPlainText(), lines);
    for (const QString& line : lines)
        for (const QString& time : chapterLineTime(line).split(QLatin1Char(';')))
            if (!time.trimmed().isEmpty() && !isChapterTime(time))
                return tr("Custom chapters: \"%1\" is not a time from 0 to 100000 hours, written as HH:MM:SS.mmm.")
                    .arg(time.trimmed());
    return QString();
}

QString TsMuxerWindow::cutProblem() const
{
    const int startCut = qTimeToMsec(ui->cutStartTimeEdit->time());
    const int endCut = qTimeToMsec(ui->cutEndTimeEdit->time());
    // as tsMuxeR has it: an end of 0 means no end
    if (!ui->checkBoxCut->isChecked() || endCut <= 0 || endCut > startCut)
        return QString();
    return tr("The cut end (%1) is not after the cut start (%2). Both are positions from the start of the source, so "
              "the end must be the later of the two.")
        .arg(ui->cutEndTimeEdit->text(), ui->cutStartTimeEdit->text());
}

void TsMuxerWindow::checkCustomChapters() { markWrongValue(ui->memoChapters, customChaptersProblem()); }

void TsMuxerWindow::checkCut()
{
    const QString problem = cutProblem();
    markWrongValue(ui->cutStartTimeEdit, problem);
    markWrongValue(ui->cutEndTimeEdit, problem);
}

void TsMuxerWindow::onSplitCutParamsChanged()
{
    // Grey the whole group where it cannot work, rather than leave it looking live. The tick itself
    // is kept, so switching back to an output that can split restores the setting instead of
    // quietly dropping it.
    const bool canSplit = outputCanSplit();
    ui->splitByDuration->setEnabled(canSplit);
    ui->splitBySize->setEnabled(canSplit);
    ui->splitByChapters->setEnabled(canSplit);
    ui->spinEditSplitDuration->setEnabled(canSplit && ui->splitByDuration->isChecked());
    ui->labelSplitByDur->setEnabled(canSplit && ui->splitByDuration->isChecked());
    ui->editSplitSize->setEnabled(canSplit && ui->splitBySize->isChecked());
    ui->comboBoxMeasure->setEnabled(canSplit && ui->splitBySize->isChecked());

    ui->cutStartTimeEdit->setEnabled(ui->checkBoxCut->isChecked());
    ui->cutEndTimeEdit->setEnabled(ui->checkBoxCut->isChecked());
    ui->checkBoxCutInside->setEnabled(ui->checkBoxCut->isChecked());
    updateMetaLines();
}

void TsMuxerWindow::onSavedParamChanged()
{
    writeSettings();
    updateMetaLines();
}

void TsMuxerWindow::onFontParamsChanged() { updateMetaLines(); }

// The lengths and chapters of a playlist were read when it was added, for the choice made then: a playlist added before
// the box changed keeps them until it is added again.
void TsMuxerWindow::onPlaylistPartsChanged()
{
    onSavedParamChanged();
    for (int i = 0; i < ui->inputFilesLV->count(); ++i)
    {
        const QString name = ui->inputFilesLV->item(i)->data(FileNameRole).toString();
        if (name.endsWith(QLatin1String(".mpls"), Qt::CaseInsensitive) ||
            name.endsWith(QLatin1String(".mpl"), Qt::CaseInsensitive))
        {
            QMessageBox::information(this, tr("tsMuxeR"),
                                     tr("Remove the playlist and add it again: its length and chapters were read for "
                                        "the other choice."));
            break;
        }
    }
}

void TsMuxerWindow::onRemoveBtnClick()
{
    if (!ui->inputFilesLV->currentItem())
        return;
    int idx = ui->inputFilesLV->currentRow();
    bool delMplsM2ts = false;

    if (idx < ui->inputFilesLV->count() - 1)
    {
        if (ui->inputFilesLV->currentItem()->data(MplsItemRole).toInt() == MPLS_PRIMARY)
            delMplsM2ts = true;
        else if (QtCompat::strLeft(ui->inputFilesLV->currentItem()->text(), 4) != FILE_JOIN_PREFIX)
        {
            QString text = ui->inputFilesLV->item(idx + 1)->text();
            if (text.length() >= 4 && QtCompat::strLeft(text, 4) == FILE_JOIN_PREFIX)
                ui->inputFilesLV->item(idx + 1)->setText(QtCompat::strMid(text, 4));
        }
    }

    const QString removedFile = myUnquoteStr(ui->inputFilesLV->currentItem()->data(FileNameRole).toString());
    delTracksByFileName(removedFile, ui->inputFilesLV->currentItem()->data(DvdTitleRole).toInt());
    ui->inputFilesLV->takeItem(idx);
    if (idx >= ui->inputFilesLV->count())
        idx--;
    // A meta file edited by hand that names the removed file kept naming it, and the next mux muxed the file that had
    // been removed. The meta file is built from the settings again, as "Remove all" does, and the box above it says so.
    const QString meta = ui->memoMeta->toPlainText();
    if (m_metaUserOverride && (meta.contains(QDir::toNativeSeparators(removedFile)) ||
                               meta.contains(QDir::fromNativeSeparators(removedFile))))
    {
        m_metaUserOverride = false;
        updateMetaState();
        updateMetaLines();
    }

    if (delMplsM2ts)
    {
        while (idx < ui->inputFilesLV->count())
        {
            QString text = ui->inputFilesLV->item(idx)->text();
            if (text.length() >= 4 && QtCompat::strLeft(text, 4) == FILE_JOIN_PREFIX)
            {
                delTracksByFileName(myUnquoteStr(ui->inputFilesLV->item(idx)->data(FileNameRole).toString()));
                ui->inputFilesLV->takeItem(idx);
            }
            else
                break;
        }
    }

    if (ui->inputFilesLV->count() > 0)
        ui->inputFilesLV->setCurrentRow(idx);
    updateRemoveAllEnabled();
    updateCustomChapters();
}

void TsMuxerWindow::delTracksByFileName(const QString& fileName, const int dvdTitle)
{
    for (int i = ui->trackLV->rowCount() - 1; i >= 0; --i)
    {
        // the tracks of one title of a DVD, not those of another title of the same disc
        if (auto info = getCodecInfo(i); info && info->dvdTitle == dvdTitle)
        {
            for (int j = info->fileList.count() - 1; j >= 0; --j)
            {
                if (info->fileList[j] == fileName)
                {
                    info->fileList.removeAt(j);
                    break;
                }
            }
            for (int j = info->mplsFiles.count() - 1; j >= 0; --j)
            {
                if (info->mplsFiles[j] == fileName)
                {
                    info->mplsFiles.removeAt(j);
                    break;
                }
            }
            if (info->ownerFile == fileName)
                info->fileList.clear();
            if (info->fileList.count() == 0)
                deleteTrack(i);
        }
    }
    updateMaxOffsets();
    updateMetaLines();
}

void TsMuxerWindow::updateOrderColumns()
{
    bool any = false;
    for (int row = 0; row < ui->trackLV->rowCount() && !any; ++row)
        if (const auto* info = getCodecInfo(row))
            any = info->playlistPlace > 0;
    ui->trackLV->setColumnHidden(4, !any);
    ui->trackLV->setColumnHidden(5, !any);
}

void TsMuxerWindow::deleteTrack(int idx)
{
    disableUpdatesCnt++;
    followMergeRows(idx + 1, 0);
    removeTrackFromDefaultComboBox(idx);
    delete getCodecInfo(idx);
    int lastItemIndex = idx;  // trackLV.items[idx].index;
    ui->trackLV->removeRow(idx);
    if (ui->trackLV->rowCount() == 0)
    {
        lastSourceDir.clear();
        while (ui->tabWidgetTracks->count()) ui->tabWidgetTracks->removeTab(0);
        ui->tabWidgetTracks->addTab(ui->tabSheetFake, TI_DEFAULT_TAB_NAME());
        ui->outFileName->setText(getDefaultOutputFileName());
        outFileNameModified = false;
    }
    else
    {
        if (lastItemIndex > ui->trackLV->rowCount() - 1)
            --lastItemIndex;
        if (lastItemIndex >= 0)
        {
            ui->trackLV->setCurrentCell(lastItemIndex, 0);
            ui->trackLV->setRangeSelected(
                QTableWidgetSelectionRange(lastItemIndex, 0, lastItemIndex, ui->trackLV->columnCount() - 1), true);
        }
        updateNum();
    }

    updateOrderColumns();
    updateMaxOffsets();
    // Removing a track can take away the layer that made the Dolby Vision selector apply. Without
    // this the selector stayed visible and --dv-profile=8.1 went on being written into a mux with
    // nothing left to fold.
    updateDvProfileVisible();
    updateMetaLines();
    ui->moveupBtn->setEnabled(ui->trackLV->currentItem() != 0);
    ui->movedownBtn->setEnabled(ui->trackLV->currentItem() != 0);
    ui->removeTrackBtn->setEnabled(ui->trackLV->currentItem() != 0);
    updateRemoveAllEnabled();
    disableUpdatesCnt--;
    trackLVItemSelectionChanged();
    updateTracksComboBox(ui->defaultAudioTrackComboBox);
    updateTracksComboBox(ui->defaultSubTrackComboBox);
}

void TsMuxerWindow::updateNum() {}

void TsMuxerWindow::onAppendButtonClick()
{
    QList<QtvCodecInfo> codecList;
    if (ui->inputFilesLV->currentItem() == 0)
    {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("No track selected"));
        msgBox.setText(tr("No track selected"));
        msgBox.setIcon(QMessageBox::Information);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.exec();
        return;
    }
    showAddFilesDialog(tr("Append media files"), [this]() { appendFile(); });
}

void TsMuxerWindow::appendFile()
{
    processAddFileList(&TsMuxerWindow::continueAppendFile, &TsMuxerWindow::fileAppended, &TsMuxerWindow::appendFile);
}

void TsMuxerWindow::continueAppendFile()
{
    QString parentFileName = myUnquoteStr((ui->inputFilesLV->currentItem()->data(FileNameRole).toString()));
    QFileInfo newFi(unquoteStr(newFileName));
    QFileInfo oldFi(unquoteStr(parentFileName));
    if (newFi.suffix().toUpper() != oldFi.suffix().toUpper())
    {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Invalid file extension"));
        msgBox.setText(tr("Appended file must have same file extension."));
        msgBox.setIcon(QMessageBox::Information);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.exec();
        return;
    }

    disableUpdatesCnt++;

    int idx = ui->inputFilesLV->currentRow();
    bool firstStep = true;
    bool doublePrefix = false;
    for (; idx < ui->inputFilesLV->count(); ++idx)
    {
        int mplsRole = ui->inputFilesLV->item(idx)->data(MplsItemRole).toInt();
        if ((mplsRole == MPLS_PRIMARY && firstStep) || mplsRole == MPLS_M2TS)
        {
            ui->inputFilesLV->setCurrentRow(idx);
            doublePrefix = true;
        }
        else
            break;
        firstStep = false;
    }

    doAppendInt(newFileName, parentFileName, fileDuration, false, doublePrefix ? MPLS_PRIMARY : MPLS_NONE);

    if (mplsFileList.size() > 0)
        doAppendInt(mplsFileList[0].name, newFileName, mplsFileList[0].duration, doublePrefix, MPLS_M2TS);
    for (int mplsCnt = 1; mplsCnt < mplsFileList.size(); ++mplsCnt)
        doAppendInt(mplsFileList[mplsCnt].name, mplsFileList[mplsCnt - 1].name, mplsFileList[mplsCnt].duration,
                    doublePrefix, MPLS_M2TS);

    updateMaxOffsets();
    if (!outFileNameModified)
    {
        modifyOutFileName(newFileName);
        outFileNameModified = true;
    }
    disableUpdatesCnt--;
    updateMetaLines();
    updateCustomChapters();
    emit fileAppended();
}

void TsMuxerWindow::onRemoveTrackButtonClick()
{
    if (ui->trackLV->currentItem())
        deleteTrack(ui->trackLV->currentRow());
}

// Tick or untick every track that matches the one clicked on, by type or by language.
//
// The type comes from the first letter of the codec name, V, A or S, which is how the muxer
// itself tells them apart, so no new idea of "type" is introduced here.
//
// Wrapped in the update guard for the same reason the remove all button is: updateMetaLines
// rebuilds the meta text from nothing, and doing that once per row is what made removing
// tracks one at a time slow.
void TsMuxerWindow::setChecksMatching(const int refRow, const bool byLanguage, const bool checked)
{
    const QtvCodecInfo* ref = getCodecInfo(refRow);
    if (!ref)
        return;
    const QChar refType = ref->programName.isEmpty() ? QChar() : ref->programName.at(0);
    const QString refLang = ref->lang;

    disableUpdatesCnt++;
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        const QtvCodecInfo* info = getCodecInfo(i);
        if (!info)
            continue;
        const bool match =
            byLanguage ? info->lang == refLang : (!info->programName.isEmpty() && info->programName.at(0) == refType);
        if (match)
            ui->trackLV->item(i, 0)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    disableUpdatesCnt--;

    updateMetaLines();
    trackLVItemChanged(nullptr);
}

// Asked for on the tracker together with the space bar, which shipped in 2.18.14. This acts on
// whatever was clicked rather than offering a list of every type and every language, which keeps
// it to four entries and needs no submenu: right click an English audio track and every English
// track, or every audio track, follows it.
void TsMuxerWindow::onTrackContextMenu(const QPoint& pos)
{
    const int row = ui->trackLV->rowAt(pos.y());
    if (row < 0 || row >= ui->trackLV->rowCount())
        return;
    const QtvCodecInfo* info = getCodecInfo(row);
    if (!info)
        return;

    QMenu menu(this);
#ifdef Q_OS_WIN
    // The Windows styles paint a menu's ground from the visual theme but its text in the colour scheme's menu text
    // colour. With a colour scheme the user chose, which Windows 7 still offers, that can be white text on the
    // theme's near-white ground, and the items of this menu could not be read (#27). Taking ground and text from
    // the same palette gives the scheme's own pair, whatever it is; on the default scheme the menu looks the same
    // but for a flatter frame.
    // palette() in a style sheet is the application palette. On Windows 7 Qt has no selection colour of its own
    // there, and the entry under the mouse is in the Selected Items colours only because main.cpp calls
    // useSystemColoursWhereQtHasNone() (systemcolours.cpp); without that call it is a black bar.
    // A disabled entry is drawn halfway between text and ground, as Qt's own Windows palette draws disabled text.
    // palette(mid) is darker than the ground of a dark scheme, and the disabled entries could not be seen there.
    const QPalette pal = QGuiApplication::palette();
    const QColor ink = pal.color(QPalette::WindowText);
    const QColor ground = pal.color(QPalette::Window);
    const QColor disabledInk((ink.red() + ground.red()) / 2, (ink.green() + ground.green()) / 2,
                             (ink.blue() + ground.blue()) / 2);
    menu.setStyleSheet(
        QStringLiteral(
            "QMenu { background-color: palette(window); color: palette(window-text); border: 1px solid palette(mid); }"
            "QMenu::item { padding: 3px 24px 3px 20px; }"
            "QMenu::item:selected { background-color: palette(highlight); color: palette(highlighted-text); }"
            "QMenu::item:disabled { color: %1; }"
            "QMenu::separator { height: 1px; background: palette(mid); margin: 3px 6px; }")
            .arg(disabledInk.name()));
#endif
    QAction* checkType = menu.addAction(tr("Check all tracks of this type"));
    QAction* uncheckType = menu.addAction(tr("Uncheck all tracks of this type"));
    menu.addSeparator();
    QAction* checkLang = menu.addAction(tr("Check all tracks in this language"));
    QAction* uncheckLang = menu.addAction(tr("Uncheck all tracks in this language"));
    // A track with no language of its own would otherwise match every other track that has none,
    // which is not what anyone means by "this language".
    const bool hasLang = !info->lang.isEmpty();
    checkLang->setEnabled(hasLang);
    uncheckLang->setEnabled(hasLang);

    const QAction* chosen = menu.exec(ui->trackLV->viewport()->mapToGlobal(pos));
    if (chosen == checkType)
        setChecksMatching(row, false, true);
    else if (chosen == uncheckType)
        setChecksMatching(row, false, false);
    else if (chosen == checkLang)
        setChecksMatching(row, true, true);
    else if (chosen == uncheckLang)
        setChecksMatching(row, true, false);
}

// "remove all" is offered whenever there is anything to remove. That is NOT the same condition as
// "remove", which needs a track selected: an input file whose tracks have all gone one by one
// still leaves something to clear.
void TsMuxerWindow::updateRemoveAllEnabled()
{
    ui->removeAllTracksBtn->setEnabled(ui->trackLV->rowCount() > 0 || ui->inputFilesLV->count() > 0);
}

// The thread that asked for this button also reported that removing things gets slower the more
// files there are, and the reason is in deleteTrack: every call ends with the whole refresh chain,
// and updateMetaLines rebuilds the meta text from nothing each time. Both that and
// trackLVItemSelectionChanged return early while disableUpdatesCnt is up, so holding it up across
// the whole loop leaves one refresh at the end instead of one per track.
//
// deleteTrack is used unchanged rather than reimplemented: it is what keeps the default track
// combo boxes, the Dolby Vision selector and the output name in step, and the last call through it
// is the one that resets the tabs and the output name when the list empties.
void TsMuxerWindow::onRemoveAllTracksButtonClick()
{
    if (ui->trackLV->rowCount() == 0 && ui->inputFilesLV->count() == 0)
        return;

    disableUpdatesCnt++;
    for (int i = ui->trackLV->rowCount() - 1; i >= 0; --i) deleteTrack(i);
    ui->inputFilesLV->clear();
    lastSourceDir.clear();
    disableUpdatesCnt--;
    // A fresh start, as its tooltip says: a meta file edited by hand kept naming the old files, and the next mux
    // muxed them instead of the tracks added afterwards.
    m_metaUserOverride = false;
    updateMetaState();

    ui->moveupBtn->setEnabled(false);
    ui->movedownBtn->setEnabled(false);
    ui->removeTrackBtn->setEnabled(false);
    updateRemoveAllEnabled();
    updateMaxOffsets();
    updateDvProfileVisible();
    updateCustomChapters();
    updateMetaLines();
    trackLVItemSelectionChanged();
}

void TsMuxerWindow::onMoveUpButtonCLick()
{
    if (ui->trackLV->currentItem() == 0 || ui->trackLV->currentRow() < 1)
        return;
    disableUpdatesCnt++;
    auto preMoveRow = ui->trackLV->currentRow();
    moveRow(preMoveRow, preMoveRow - 1);
    moveTrackInDefaultComboBox(preMoveRow, preMoveRow - 1);
    followMergeRows(preMoveRow + 1, preMoveRow);
    disableUpdatesCnt--;
    updateMetaLines();
    updateNum();
}

void TsMuxerWindow::onMoveDownButtonCLick()
{
    if (ui->trackLV->currentItem() == 0 || ui->trackLV->rowCount() == 0 ||
        ui->trackLV->currentRow() == ui->trackLV->rowCount() - 1)
        return;
    disableUpdatesCnt++;
    auto preMoveRow = ui->trackLV->currentRow();
    moveRow(preMoveRow, preMoveRow + 2);
    moveTrackInDefaultComboBox(preMoveRow, preMoveRow + 1);
    followMergeRows(preMoveRow + 1, preMoveRow + 2);
    disableUpdatesCnt--;
    updateMetaLines();
    updateNum();
}

void TsMuxerWindow::moveRow(int index, int index2)
{
    ui->trackLV->insertRow(index2);
    ui->trackLV->setRowHeight(index2, 18);
    if (index2 < index)
        index++;
    for (int i = 0; i < ui->trackLV->columnCount(); ++i)
        ui->trackLV->setItem(index2, i, ui->trackLV->item(index, i)->clone());
    ui->trackLV->removeRow(index);
    if (index2 > index)
        index2--;
    ui->trackLV->setRangeSelected(QTableWidgetSelectionRange(index2, 0, index2, ui->trackLV->columnCount() - 1), true);
    ui->trackLV->setCurrentCell(index2, 0);
}

// Show the Dolby Vision profile row only when it applies: Matroska output, and a source that
// actually carries a dual layer Dolby Vision track. A merged track lists as two rows, base layer
// and enhancement layer, which is exactly what the description says.
void TsMuxerWindow::updateDvProfileVisible()
{
    // Two shapes qualify and they look nothing alike.
    //
    // An already merged Matroska track arrives as two rows sharing one track number, told apart by
    // subTrack, and carrying Dolby Vision in the description.
    //
    // A DISC arrives as two independent video tracks, both with subTrack 0: a base layer that says
    // nothing about Dolby Vision, and an enhancement layer that carries the RPU. Only the first
    // shape was recognised here, so the selector never appeared for the source docs/DOLBY_VISION.md
    // describes, and a disc taken to Matroska came out as profile 7 with nothing said about it.
    //
    // The disc test mirrors the muxer's own pairing: a video track AFTER THE FIRST carrying Dolby
    // Vision data. Not "any track with an RPU", because a single layer profile 8 track has one too
    // and must be left alone.
    //
    // Unchecked rows are skipped, because they never reach the muxer: an unticked row goes into the
    // meta commented out. Counting them left the selector showing after the enhancement layer was
    // unticked, which put --dv-profile=8.1 into a mux with nothing to fold.
    bool dualLayerDv = false;
    int videoTracksSeen = 0;
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        const QTableWidgetItem* rowItem = ui->trackLV->item(i, 0);
        if (rowItem == nullptr || rowItem->checkState() != Qt::Checked)
            continue;
        const QtvCodecInfo* codecInfo = getCodecInfo(i);
        if (codecInfo == nullptr)
            continue;
        const bool carriesDv = codecInfo->descr.contains("Dolby Vision");
        if (carriesDv && codecInfo->subTrack != 0)
        {
            dualLayerDv = true;
            break;
        }
        if (isVideoCodec(codecInfo->displayName))
        {
            ++videoTracksSeen;
            if (videoTracksSeen > 1 && carriesDv)
            {
                dualLayerDv = true;
                break;
            }
        }
    }
    const bool show = ui->radioButtonMKV->isChecked() && dualLayerDv;
    ui->labelDvProfile->setVisible(show);
    ui->comboBoxDvProfile->setVisible(show);
    // The spacer at the end of this row is 0 px tall in the designer file, on purpose. A hidden label
    // and combo take no room, but a spacer always does, so at the designer default of 20 px the hidden
    // row left an empty band under the file name in every output mode. The Output group has a fixed
    // height, so that band was taken from the track list or the Meta file box. Shown, the row takes
    // its height from the combo.
}

void TsMuxerWindow::updateOutputModeTexts()
{
    if (ui->radioButtonDemux->isChecked())
        ui->buttonMux->setText(tr("Start demuxing"));
    else
        ui->buttonMux->setText(tr("Start muxing"));
    ui->FilenameLabel->setText(ui->radioButtonBluRay->isChecked() || ui->radioButtonDemux->isChecked() ||
                                       ui->radioButtonAVCHD->isChecked()
                                   ? tr("Folder")
                                   : tr("File name"));
}

void TsMuxerWindow::applyOutputMode(const bool resetAudioPes)
{
    updateOutputModeTexts();
    updateAutoCoreMerge();  // a disc braids a TrueHD track and its core again, other outputs do not
    // A chosen output gets the usual value; a restored one keeps the tick that was remembered with it
    if (resetAudioPes)
        ui->checkBoxNewAudioPes->setChecked(!ui->radioButtonTS->isChecked());
    ui->editDelay->setEnabled(!ui->radioButtonDemux->isChecked());
    ui->checkBoxNewAudioPes->setEnabled(ui->radioButtonTS->isChecked() || ui->radioButtonM2TS->isChecked());
    // The Dolby Vision profile only means anything for Matroska output, so it is hidden rather
    // than greyed out anywhere else: a disabled control still invites the question of what it is
    // for, and here the answer would be "nothing, in this mode".
    updateDvProfileVisible();
    // Whether the split controls apply depends on the output that was just chosen, so settle them
    // here too and not only when one of them is clicked. Same for the Blu-ray tab.
    onSplitCutParamsChanged();
    updateBluRayTabEnabled();
    // Writing a track out as three files is a demux only thing, so the box that asks for it has
    // to follow the output mode rather than only the selected track. Chosen in this order because
    // a box that is disabled must not keep a tick that would still reach the meta file.
    if (!ui->radioButtonDemux->isChecked() && ui->splitAc3CoreCheckBox->isChecked())
        ui->splitAc3CoreCheckBox->setChecked(false);
    ui->splitAc3CoreCheckBox->setEnabled(ui->dropAc3CoreCheckBox->isEnabled() && ui->radioButtonDemux->isChecked());
    // Same for Wave64, and for the same reason. Without this the box only followed the SELECTED
    // TRACK, so choosing Demux after picking the track left it greyed out with nothing to say why.
    if (!ui->radioButtonDemux->isChecked() && ui->w64CheckBox->isChecked())
        ui->w64CheckBox->setChecked(false);
    if (const QtvCodecInfo* info = getCurrentCodec())
        ui->w64CheckBox->setEnabled(info->programName == QLatin1String("A_LPCM") && ui->radioButtonDemux->isChecked());
    updateKeepTextControls();
}

// The .srt file save-srt writes for a track: beside the output file, named after it, or in the output folder of a disc
// or a demux, named after the source; the row of the track in the list keeps two tracks apart, and never gives the name
// of an .srt source in the same folder. An ISO image is a file: its name was taken for a folder, which the .srt made,
// and then the image of that name could not be written.
QString TsMuxerWindow::saveSrtFileName(const QtvCodecInfo* codecInfo, const int row) const
{
    const QString out = unquoteStr(ui->outFileName->text().trimmed());
    QString base;
    if ((isDiskOutput() && !ui->radioButtonBluRayISO->isChecked()) || ui->radioButtonDemux->isChecked())
    {
        QString dir = QDir::fromNativeSeparators(out);
        if (!dir.endsWith('/'))
            dir += '/';
        base =
            dir +
            QFileInfo(codecInfo->fileList.isEmpty() ? QString("subtitles") : codecInfo->fileList[0]).completeBaseName();
    }
    else
    {
        const QFileInfo fi(QDir::fromNativeSeparators(out));
        base = fi.path() + '/' + fi.completeBaseName();
    }
    return QDir::toNativeSeparators(base + QString(".track%1.srt").arg(row + 1));
}

bool TsMuxerWindow::keepTextApplies() const
{
    return ui->keepTextCheckBox->isChecked() && (ui->radioButtonMKV->isChecked() || ui->radioButtonDemux->isChecked());
}

// The tick is a preference that is remembered, so it stays while an output that cannot carry text is
// chosen; the box is greyed then, because the tick reaches nothing. While it applies, the settings that
// only drawing uses are greyed instead, so the window does not offer what cannot happen.
void TsMuxerWindow::updateKeepTextControls()
{
    ui->keepTextCheckBox->setEnabled(ui->radioButtonMKV->isChecked() || ui->radioButtonDemux->isChecked());
    const bool asText = keepTextApplies();
    // save-srt is for subtitles that are drawn: kept as text, the output or the demux holds the text already
    ui->saveSrtCheckBox->setEnabled(!asText);
    ui->textStAsSrtCheckBox->setEnabled(asText && ui->radioButtonMKV->isChecked());
    // libass draws only what is drawn, and a fonts folder is for libass alone
    ui->assAsSrtCheckBox->setEnabled(!asText);
    const bool libass = !asText && !ui->assAsSrtCheckBox->isChecked();
    ui->fontsDirLabel->setEnabled(libass);
    ui->fontsDirLineEdit->setEnabled(libass);
    ui->fontsDirBrowseButton->setEnabled(libass);
    ui->groupBox_8->setEnabled(!asText);
    ui->groupBox_9->setEnabled(!asText);
}

void TsMuxerWindow::RadioButtonMuxClick()
{
    if (outFileNameDisableChange)
        return;
    applyOutputMode();
    outFileNameDisableChange = true;
    if (ui->radioButtonBluRay->isChecked() || ui->radioButtonDemux->isChecked() || ui->radioButtonAVCHD->isChecked())
    {
        QFileInfo fi(unquoteStr(ui->outFileName->text()));
        if (!fi.suffix().isEmpty())
        {
            oldFileName = fi.fileName();
            // A name with no folder in front, as "default.ts" is while no file has been added with the output going
            // into the input's folder, was resolved against the current folder, on Windows usually the program's
            // own. That folder then stuck: the name was no longer "default", so adding a file did not replace it,
            // and the disc went into that folder (justdan96/tsMuxer#564). Such a name now gives the output folder
            // the window would use, which is empty until a file is added and is then taken from that file.
            if (fi.isRelative() && !ui->outFileName->text().contains('/') && !ui->outFileName->text().contains('\\'))
                ui->outFileName->setText(getOutputDir());
            else
                ui->outFileName->setText(QDir::toNativeSeparators(fi.absolutePath()) + QDir::separator());
        }
        ui->FilenameLabel->setText(tr("Folder"));
    }
    else
    {
        ui->FilenameLabel->setText(tr("File name"));
        if (!oldFileName.isEmpty())
        {
            ui->outFileName->setText(QDir::toNativeSeparators(ui->outFileName->text()));
            if (!ui->outFileName->text().isEmpty() &&
                QtCompat::strRight(ui->outFileName->text(), 1) != QDir::separator())
                ui->outFileName->setText(ui->outFileName->text() + QDir::separator());
            ui->outFileName->setText(ui->outFileName->text() + oldFileName);
            oldFileName.clear();
        }
        if (ui->radioButtonTS->isChecked())
            ui->outFileName->setText(changeFileExt(ui->outFileName->text(), "ts"));
        else if (ui->radioButtonMKV->isChecked())
        {
            if (!isMatroskaOutputName(ui->outFileName->text()))
                ui->outFileName->setText(changeFileExt(ui->outFileName->text(), "mkv"));
        }
        else if (ui->radioButtonBluRayISO->isChecked())
            ui->outFileName->setText(changeFileExt(ui->outFileName->text(), "iso"));
        else
            ui->outFileName->setText(changeFileExt(ui->outFileName->text(), "m2ts"));
        // The new extension could make the name one of the sources: x.m2ts with M2TS chosen, x.mkv with Matroska, and
        // the window offered the source as its own output. As when a file is added, such a name gets "_new".
        const QFileInfo proposed(ui->outFileName->text());
        bool isSource = false;
        for (int i = 0; i < ui->trackLV->rowCount() && !isSource; ++i)
            if (const QtvCodecInfo* info = getCodecInfo(i))
                for (const QString& source : info->fileList)
                    if (isSameFile(unquoteStr(source), proposed.filePath()))
                        isSource = true;
        if (isSource)
            ui->outFileName->setText(QDir::toNativeSeparators(closeDirPath(proposed.absolutePath())) +
                                     proposed.completeBaseName() + "_new." + proposed.suffix());
    }
    ui->DiskLabel->setVisible(ui->radioButtonBluRayISO->isChecked());
    ui->DiskLabelEdit->setVisible(ui->radioButtonBluRayISO->isChecked());
    updateMkvOutputOptions();
    updateDiscMetaOptions();
    ui->editDelay->setEnabled(!ui->radioButtonDemux->isChecked());
    updateMkvTrackOptionsEnabled();
    updateHdr10OverrideEnabled(getCurrentCodec());
    colorizeAllRows();
    updateMetaLines();
    outFileNameDisableChange = false;
}

// hdr10-override is offered for an HEVC track when the output is a disc, the only output that carries the mark. A disc
// with HEVC video is always a UHD one.
void TsMuxerWindow::updateHdr10OverrideEnabled(const QtvCodecInfo* codecInfo)
{
    const bool disc = ui->radioButtonBluRay->isChecked() || ui->radioButtonBluRayISO->isChecked();
    ui->checkBoxHdr10Override->setEnabled(codecInfo != nullptr && codecInfo->displayName == "HEVC" && disc);
}

void TsMuxerWindow::outFileNameChanged()
{
    // Only a name the user gives counts as theirs. The window's own changes, made while outFileNameDisableChange is
    // set (choosing another output does that), used to count too, so a name the window had put there was kept when a
    // file was added: "default.mkv" stayed after choosing Matroska first, and an output folder that was still empty
    // stayed empty, and the mux was then refused for the missing output.
    if (outFileNameDisableChange)
        return;
    outFileNameModified = true;
    if (ui->radioButtonDemux->isChecked() || ui->radioButtonBluRay->isChecked() || ui->radioButtonAVCHD->isChecked())
        return;

    outFileNameDisableChange = true;
    QFileInfo fi(unquoteStr(ui->outFileName->text().trimmed()));
    QString ext = fi.suffix().toUpper();
    if (ext.endsWith(QLatin1Char('\"')))
        ext.chop(1);

    const auto modeBefore = currentOutputMode();

    // Only a name that ends in an output's own extension chooses that output. Anything else used to
    // mean Transport Stream, so a name without an extension silently turned an ISO, Matroska or
    // M2TS output into a TS: a name typed into a file dialog that does not add the extension itself,
    // as on Linux, a name with a dot of its own such as "My.Movie.2020", and every name while it was
    // still being typed ("movie.i" is not yet "movie.iso"). The output now stays as it was until the
    // name says otherwise. A name an ISO or M2TS output cannot use is still caught when muxing
    // starts, with a message that names the extension it needs.
    if (ext == "M2TS")
        ui->radioButtonM2TS->setChecked(true);
    else if (ext == "MKV" || ext == "MKA" || ext == "MKS" || ext == "MK3D" || ext == "WEBM")
        ui->radioButtonMKV->setChecked(true);
    else if (ext == "ISO")
        ui->radioButtonBluRayISO->setChecked(true);
    else if (ext == "TS")
        ui->radioButtonTS->setChecked(true);

    ui->DiskLabel->setVisible(ui->radioButtonBluRayISO->isChecked());
    ui->DiskLabelEdit->setVisible(ui->radioButtonBluRayISO->isChecked());
    updateMkvOutputOptions();
    updateDiscMetaOptions();
    // A name whose extension chooses another output changes everything the output decides, exactly
    // as a click on that output's button does. Only the switch to or from ISO used to rebuild the
    // meta, so a name ending in .mkv typed while TS was chosen left the meta as it was for TS: a
    // FLAC track stayed commented out and the MKV was muxed without it, and .ts typed over .mkv kept
    // the FLAC track for an output that cannot carry it. The meta is what gets muxed.
    if (modeBefore != currentOutputMode())
    {
        applyOutputMode();
        colorizeAllRows();
        updateMetaLines();
    }
    // save-srt names its file after the output: a new name of the same output left the old name in the meta
    else if (ui->saveSrtCheckBox->isChecked())
        updateMetaLines();
    outFileNameDisableChange = false;
}

void TsMuxerWindow::saveFileDialog()
{
    if (m_fileDialogOpen)
        return;

    if (ui->radioButtonDemux->isChecked() || ui->radioButtonBluRay->isChecked() || ui->radioButtonAVCHD->isChecked())
    {
        auto* dialog = new QFileDialog(this, tr("Select output folder"), getExistingDialogDir(getOutputDir()));
        dialog->setFileMode(QFileDialog::Directory);
        dialog->setOption(QFileDialog::ShowDirsOnly, true);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        m_fileDialogOpen = true;
        connect(dialog, &QFileDialog::finished, this, [this](int) { m_fileDialogOpen = false; });
        connect(dialog, &QFileDialog::fileSelected, this,
                [this](const QString& folder)
                {
                    if (folder.isEmpty())
                        return;
                    auto native = QDir::toNativeSeparators(folder);
                    ui->outFileName->setText(native + QDir::separator());
                    outFileNameModified = true;
                    lastOutputDir = native;
                    writeSettings();
                });
        dialog->open();
    }
    else
    {
        auto fileName = unquoteStr(ui->outFileName->text());
        auto startDir = getExistingDialogDir(fileName.isEmpty() ? getOutputDir() : fileName);
        auto* dialog = new QFileDialog(this, tr("Select file for muxing"), startDir, currentSaveDialogFilter());
        dialog->setAcceptMode(QFileDialog::AcceptSave);
        dialog->setFileMode(QFileDialog::AnyFile);
        // The extension of the output that is selected, the same choice currentSaveDialogFilter makes.
        // The Windows dialog adds it to a typed name by itself; the dialog Qt draws on other systems
        // only does so when it is told, and returns "movie" for "movie". The window then read the name
        // without an extension as a Transport Stream and switched the output to TS.
        const QString outputExt = ui->radioButtonMKV->isChecked()         ? QStringLiteral("mkv")
                                  : ui->radioButtonBluRayISO->isChecked() ? QStringLiteral("iso")
                                  : ui->radioButtonTS->isChecked()        ? QStringLiteral("ts")
                                                                          : QStringLiteral("m2ts");
        dialog->setDefaultSuffix(outputExt);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        m_fileDialogOpen = true;
        connect(dialog, &QFileDialog::finished, this, [this](int) { m_fileDialogOpen = false; });
        // Matroska and WebM: a name typed with no extension gets the one of the file type chosen in the dialog
        if (outputExt == QLatin1String("mkv"))
            connect(dialog, &QFileDialog::filterSelected, dialog, [dialog](const QString& filter)
                    { dialog->setDefaultSuffix(filter.contains(QLatin1String("*.webm")) ? "webm" : "mkv"); });
        connect(dialog, &QFileDialog::fileSelected, this,
                [this, outputExt, dialog](const QString& selected)
                {
                    if (selected.isEmpty())
                        return;
                    const QString chosenExt = outputExt == QLatin1String("mkv") &&
                                                      dialog->selectedNameFilter().contains(QLatin1String("*.webm"))
                                                  ? QStringLiteral("webm")
                                                  : outputExt;
                    // A default suffix is only added to a name with no dot at all, so "My.Movie.2020"
                    // still came back without one. A name that does not end in an output's own
                    // extension gets the selected output's, so the output stays what was chosen. A
                    // name ending in another output's extension is left alone: it names the output wanted.
                    QString fileName = selected;
                    static const QStringList outputExts = {
                        QStringLiteral("ts"),  QStringLiteral("m2ts"), QStringLiteral("mkv"),  QStringLiteral("mka"),
                        QStringLiteral("mks"), QStringLiteral("mk3d"), QStringLiteral("webm"), QStringLiteral("iso")};
                    if (!outputExts.contains(QFileInfo(fileName).suffix(), Qt::CaseInsensitive))
                        fileName += (fileName.endsWith(QLatin1Char('.')) ? QString() : QStringLiteral(".")) + chosenExt;
                    auto native = QDir::toNativeSeparators(fileName);
                    ui->outFileName->setText(native);
                    lastOutputDir = QFileInfo(native).absolutePath();
                    writeSettings();
                });
        dialog->open();
    }
}

void TsMuxerWindow::startMuxing()
{
    checkCustomChapters();
    checkCut();
    // the chapter list stops the mux only where it goes into the meta line
    const bool chaptersWritten = isDiskOutput() || (ui->radioButtonMKV->isChecked() && m_customChaptersUserOverride);
    const QString problem =
        chaptersWritten && !customChaptersProblem().isEmpty() ? customChaptersProblem() : cutProblem();
    if (!problem.isEmpty())
    {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Wrong value"));
        msgBox.setText(problem);
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.exec();
        return;
    }
    QString outputName = unquoteStr(ui->outFileName->text().trimmed());
    ui->outFileName->setText(outputName);
    lastOutputDir = QFileInfo(outputName).absolutePath();
    writeSettings();
    // The output is never one of the sources: the writer empties its file before the source is read. With the output
    // in the source folder, 00000.M2TS could be proposed as its own output, as the names were compared with case.
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
        if (const QtvCodecInfo* info = getCodecInfo(i))
            for (const QString& source : info->fileList)
                if (isSameFile(source, outputName))
                {
                    QMessageBox msgBox(this);
                    msgBox.setWindowTitle(tr("Invalid file name"));
                    msgBox.setText(tr("The output file \"%1\" is one of the source files. Please choose another name.")
                                       .arg(outputName));
                    msgBox.setIcon(QMessageBox::Warning);
                    msgBox.setStandardButtons(QMessageBox::Ok);
                    msgBox.exec();
                    return;
                }
    if (ui->radioButtonM2TS->isChecked())
    {
        QFileInfo fi(ui->outFileName->text());
        if (fi.suffix().toUpper() != "M2TS")
        {
            QMessageBox msgBox(this);
            msgBox.setWindowTitle(tr("Invalid file name"));
            msgBox.setText(tr("The output file \"%1\" has invalid extension. Please, change file extension "
                              "to \".m2ts\"")
                               .arg(ui->outFileName->text()));
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setStandardButtons(QMessageBox::Ok);
            msgBox.exec();
            return;
        }
    }
    // TS and Matroska were not checked: tsMuxeR chooses what to write by the output's extension, and takes any other
    // extension for a folder to demux into. "My.Movie.2020" in TS or MKV mode came out as a folder of demuxed tracks
    // and the window reported success.
    else if (ui->radioButtonTS->isChecked() || ui->radioButtonMKV->isChecked())
    {
        const QString suffix = QFileInfo(ui->outFileName->text()).suffix().toUpper();
        const bool ok = ui->radioButtonTS->isChecked() ? suffix == "TS" : isMatroskaOutputName(ui->outFileName->text());
        if (!ok)
        {
            QMessageBox msgBox(this);
            msgBox.setWindowTitle(tr("Invalid file name"));
            msgBox.setText(tr("The output file \"%1\" has invalid extension. Please, change file extension to \"%2\"")
                               .arg(ui->outFileName->text())
                               .arg(ui->radioButtonTS->isChecked() ? ".ts" : ".mkv"));
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setStandardButtons(QMessageBox::Ok);
            msgBox.exec();
            return;
        }
    }
    else if (ui->radioButtonBluRayISO->isChecked())
    {
        QFileInfo fi(ui->outFileName->text());
        if (fi.suffix().toUpper() != "ISO")
        {
            QMessageBox msgBox(this);
            msgBox.setWindowTitle(tr("Invalid file name"));
            msgBox.setText(tr("The output file \"%1\" has invalid extension. Please, change file extension to \".iso\"")
                               .arg(ui->outFileName->text()));
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setStandardButtons(QMessageBox::Ok);
            msgBox.exec();
            return;
        }
    }

    // A constant rate above the disc's read rate limit cannot be held: tsMuxeR holds the stream at the limit, with a
    // warning in its log. Asked here, before the job, where the two boxes that set them are.
    if (ui->checkBoxCBR->isChecked() && (ui->radioButtonBluRay->isChecked() || ui->radioButtonBluRayISO->isChecked()) &&
        ui->checkBoxRateLimit->isChecked())
    {
        const double limit = ui->checkBoxV3->isChecked() ? ui->comboRateLimit->currentData().toDouble() : 48000.0;
        if (ui->editCBRBitrate->value() > limit &&
            QMessageBox::question(this, tr("tsMuxeR"),
                                  tr("The constant bitrate of %1 kbit/s is above the read rate limit of %2 kbit/s, so "
                                     "the stream is held at %2 kbit/s. Mux it so?")
                                      .arg(ui->editCBRBitrate->value(), 0, 'f', 0)
                                      .arg(limit, 0, 'f', 0)) != QMessageBox::Yes)
            return;
    }

    // A question below can still end this: a core that was to be made then leaves the track again. Before the #53
    // question, because a changed track builds the meta file anew and the option that question adds is not in it.
    const auto dropCores = qScopeGuard([this] { dropPendingCores(); });
    if (isDiskOutput() && !m_metaUserOverride && !offerAc3Cores())
        return;

    // More than 32 tracks of one kind on a disc (#53): tsMuxeR refuses them unless the MUXOPT line has
    // --allow-extra-tracks. Asked here, with what it means; the answer adds the option to the meta file of this job.
    if (isDiskOutput())
    {
        const QString meta = ui->memoMeta->toPlainText();
        const QStringList over = kindsOverDiscLimit(meta);
        if (!over.isEmpty() && !muxOptHas(meta, QStringLiteral("--allow-extra-tracks")))
        {
            if (ui->checkBoxAskExtraTracks->isChecked())
            {
                QMessageBox box(this);
                box.setWindowTitle(tr("More tracks than a disc has numbers for"));
                box.setIcon(QMessageBox::Question);
                box.setText(tr("This disc gets %1. A Blu-ray disc has numbers for 32 tracks of each kind.")
                                .arg(over.join(tr(" and "))));
                box.setInformativeText(
                    tr("The tracks past the 32nd get numbers outside that range. Some players show them, others leave "
                       "them out. Leave tracks out to keep within 32, or write the disc anyway."));
                QPushButton* write = box.addButton(tr("Write the disc anyway"), QMessageBox::AcceptRole);
                box.addButton(QMessageBox::Cancel);
                box.setDefaultButton(QMessageBox::Cancel);
                auto* dontAsk = new QCheckBox(tr("Don't ask again"), &box);
                dontAsk->setToolTip(tr("The question comes back with the tick on the General tab."));
                box.setCheckBox(dontAsk);
                box.exec();
                if (box.clickedButton() != write)
                    return;
                if (dontAsk->isChecked())
                {
                    ui->checkBoxAskExtraTracks->setChecked(false);
                    writeSettings();
                }
            }
            // the option on the MUXOPT line of this job's meta file, the hand-edited one too
            QStringList lines = meta.split('\n');
            for (QString& line : lines)
                if (line.trimmed().startsWith(QLatin1String("MUXOPT")))
                {
                    line = line.trimmed() + " --allow-extra-tracks";
                    break;
                }
            const bool wasBlocked = ui->memoMeta->blockSignals(true);
            ui->memoMeta->setPlainText(lines.join('\n'));
            ui->memoMeta->blockSignals(wasBlocked);
        }
    }

    // A Matroska or WebM output is a file too, and an existing one was replaced without the question the others ask.
    bool isFile = ui->radioButtonM2TS->isChecked() || ui->radioButtonTS->isChecked() ||
                  ui->radioButtonBluRayISO->isChecked() || ui->radioButtonMKV->isChecked();
    // A split writes name.split.1.ext, name.split.2.ext and on, never the name itself, so the question did not come
    // and the parts of an earlier split were replaced without a word. The meta text is what is run, so a split typed
    // into it counts as well. The number of parts is known only at the end, so any part there is asked about. Only a
    // real option counts: a label holding "--split-size" asked about parts that are not written.
    QString existingOutput = ui->outFileName->text();
    if (isFile && !ui->radioButtonBluRayISO->isChecked() && metaAsksForSplit(ui->memoMeta->toPlainText()))
    {
        const QFileInfo out(ui->outFileName->text());
        const QRegularExpression partName("^" + QRegularExpression::escape(out.completeBaseName() + ".split.") +
                                              "[0-9]+\\." + QRegularExpression::escape(out.suffix()) + "$",
                                          QRegularExpression::CaseInsensitiveOption);
        existingOutput.clear();
        for (const QString& name : out.absoluteDir().entryList(QDir::Files, QDir::Name))
            if (partName.match(name).hasMatch())
            {
                existingOutput = QDir::toNativeSeparators(out.absoluteDir().filePath(name));
                break;
            }
    }
    if (isFile && !existingOutput.isEmpty() && QFile::exists(existingOutput))
    {
        //: Used in expressions "Overwrite existing %1" and "The output %1 already exists".
        auto fileOrDir = isFile ? tr("file") : tr("directory");
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Overwrite existing %1?").arg(fileOrDir));
        msgBox.setText(
            tr("The output %1 \"%2\" already exists. Do you want to overwrite it?").arg(fileOrDir).arg(existingOutput));
        msgBox.setIcon(QMessageBox::Question);
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        if (msgBox.exec() != QMessageBox::Yes)
            return;
    }

    if (!makeAc3Cores())
        return;

    QFileInfo fi(ui->outFileName->text());
    metaName =
        QDir::toNativeSeparators(QDir::tempPath()) + QDir::separator() + "tsMuxeR_" + fi.completeBaseName() + ".meta";
    if (!saveMetaFile(metaName))
    {
        metaName.clear();
        return;
    }
    muxForm->prepare(!ui->radioButtonDemux->isChecked() ? tr("Muxing in progress") : tr("Demuxing in progress"));
    ui->buttonMux->setEnabled(false);
    ui->addBtn->setEnabled(false);
    m_addDiscBtn->setEnabled(false);
    ui->btnAppend->setEnabled(false);
    muxForm->show();
    disconnect();
    // QCoreApplication::dir
    runInMuxMode = true;
    noteRunOutput(ui->outFileName->text(), ui->radioButtonBluRay->isChecked() || ui->radioButtonAVCHD->isChecked() ||
                                               ui->radioButtonDemux->isChecked());
    QStringList args;
    // A report of the job for a problem report (--report): in the temp folder, Save report... in the progress window
    // keeps a copy
    if (ui->checkBoxReport->isChecked())
    {
        const QString report = QDir::toNativeSeparators(QDir::tempPath()) + QDir::separator() + "tsMuxeR_" +
                               fi.completeBaseName() + "_report.txt";
        QFile::remove(report);
        args << (QLatin1String("--report=") + report);
        muxForm->setReportFile(report);
    }
    tsMuxerExecute(args << metaName << quoteStr(ui->outFileName->text()));
}

// A meta file could only ever be handed to the command line version, and that is what blocks the
// other thread: somebody worked out the right setting, wrote it into a meta, and then had nowhere
// to put it. Nothing new is needed to run one. The text in the preview IS the meta the muxer is
// given, and typing in that box already switches the window to using it verbatim, so opening a
// file only has to put its contents there.
//
// What this does NOT do is fill the track list back in from the file. The list will still show
// whatever was there before, or nothing, while the mux runs from the text. That is why the button
// says what it does and why "Reset meta to auto-generated" beside it is the way back.
void TsMuxerWindow::openMetaFileBtnClick()
{
    if (m_fileDialogOpen)
        return;

    // The folder files are added from, the same as the Browse button. With no folder given the dialog opened in the
    // home folder, and Windows shows each folder in the view kept for that folder, so it showed the home folder's view,
    // such as Medium Icons, where Browse showed the user's own folder in Details.
    auto* dialog = new QFileDialog(this, tr("Open project file"), getExistingDialogDir(lastInputDir),
                                   tr("tsMuxeR project file (*.meta);;All files (*)"));
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_fileDialogOpen = true;
    connect(dialog, &QFileDialog::finished, this, [this](int) { m_fileDialogOpen = false; });
    connect(dialog, &QFileDialog::fileSelected, this,
            [this](const QString& metaName)
            {
                if (metaName.isEmpty())
                    return;
                QFile file(metaName);
                if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                {
                    QMessageBox msgBox(this);
                    msgBox.setWindowTitle(tr("Can't open meta file"));
                    msgBox.setText(tr("Can't open meta file \"%1\"").arg(metaName));
                    msgBox.setIcon(QMessageBox::Warning);
                    msgBox.setStandardButtons(QMessageBox::Ok);
                    msgBox.exec();
                    return;
                }
                const QString text = QString::fromUtf8(file.readAll());
                file.close();
                if (text.trimmed().isEmpty())
                {
                    QMessageBox msgBox(this);
                    msgBox.setWindowTitle(tr("Empty file"));
                    msgBox.setText(tr("The meta file \"%1\" is empty.").arg(metaName));
                    msgBox.setIcon(QMessageBox::Warning);
                    msgBox.setStandardButtons(QMessageBox::Ok);
                    msgBox.exec();
                    return;
                }
                lastInputDir = QFileInfo(metaName).absolutePath();
                writeSettings();
                // Deliberately NOT blocking signals: the text change is what tells the window to
                // hand this over verbatim instead of rebuilding it from the track list.
                ui->memoMeta->setPlainText(text);
            });
    dialog->open();
}

void TsMuxerWindow::saveMetaFileBtnClick()
{
    if (m_fileDialogOpen)
        return;

    auto* dialog = new QFileDialog(this, tr("Save project file"),
                                   getExistingDialogDir(changeFileExt(ui->outFileName->text(), "meta")),
                                   tr("tsMuxeR project file (*.meta);;All files (*)"));
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_fileDialogOpen = true;
    connect(dialog, &QFileDialog::finished, this, [this](int) { m_fileDialogOpen = false; });
    connect(dialog, &QFileDialog::fileSelected, this,
            [this](const QString& metaName)
            {
                if (metaName.isEmpty())
                    return;
                QFileInfo fi(metaName);
                QDir dir;
                dir.mkpath(fi.absolutePath());
                saveMetaFile(metaName);
            });
    dialog->open();
}

bool TsMuxerWindow::saveMetaFile(const QString& metaName)
{
    QFile file(metaName);
    if (!file.open(QIODevice::WriteOnly))
    {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Can't create temporary meta file"));
        msgBox.setText(tr("Can't create temporary meta file \"%1\"").arg(metaName));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.exec();
        return false;
    }
    QByteArray metaText = ui->memoMeta->toPlainText().toUtf8();
    file.write(metaText);
    file.close();
    return true;
}

void TsMuxerWindow::onMetaTextChanged()
{
    if (!m_metaUserOverride)
    {
        m_metaUserOverride = true;
        updateMetaState();
    }
    updateMuxButton();  // a meta box with tracks can be muxed with an empty track list
}

// The meta file built from the settings again: what was edited by hand is replaced
void TsMuxerWindow::onResetMetaBtnClick()
{
    m_metaUserOverride = false;
    updateMetaLines();
    updateMetaState();
    updateMuxButton();
}

// The box above the meta file, the title of its group and the line about changes not applied, as the meta file stands
void TsMuxerWindow::updateMetaState()
{
    {
        const QSignalBlocker block(ui->metaAutoCheckBox);
        ui->metaAutoCheckBox->setChecked(!m_metaUserOverride);
    }
    ui->groupBox_2->setTitle(m_metaUserOverride ? tr("Meta file (edited by hand: changes in the tabs are not applied)")
                                                : tr("Meta file"));
    if (!m_metaUserOverride)
        ui->metaHintLabel->setVisible(false);
}

void TsMuxerWindow::closeEvent(QCloseEvent* event)
{
    // Closing the window during a mux ended tsMuxeR without a word and left its unfinished output. It is asked now,
    // and Yes stops the mux as Abort does, with the same removal of the unfinished output.
    if (runInMuxMode && proc.state() != QProcess::NotRunning)
    {
        if (QMessageBox::question(this, tr("tsMuxeR"), tr("A mux is running. Stop it and close the window?")) !=
            QMessageBox::Yes)
        {
            event->ignore();
            return;
        }
        muxForm->abort();
    }
    if (!metaName.isEmpty())
    {
        QFile::remove(metaName);
        metaName.clear();
    }
    removeMadeCores();
    muxForm->close();
    event->accept();
}

void TsMuxerWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange)
    {
        ui->retranslateUi(this);
        fontSettingsModel->onLanguageChanged();
        langCodesModel->onLanguageChanged();
        for (const auto& hook : m_retranslateHooks)
            hook();  // re-translate the hand-built BDMV->ISO tab and dual-layer groupbox
    }
    QWidget::changeEvent(event);
}

// A chapter file dropped on the chapter list fills the list instead of being added as a track.
// Reads both of the formats other tools write, told apart by what is in the file:
//   CHAPTERnn= lines give up their time, CHAPTERnnNAME= lines are skipped
//   Matroska XML gives up its ChapterTimeStart values
// The box holds times and nothing else, so names are dropped here, which costs nothing for a disc. A
// Matroska output that wants to keep the names should be given --chapters-file instead.
bool TsMuxerWindow::loadChapterFileIntoBox(const QString& fileName)
{
    QFile f(fileName);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QString text = QString::fromUtf8(f.readAll());
    f.close();

    QStringList times;
    if (text.contains(QLatin1String("<ChapterTimeStart")))
    {
        // One edition, as --chapters-file reads the file (chapterFile.cpp, parseXml): the first marked
        // EditionFlagDefault, or the first when none is, without its hidden chapters. Every edition of the file was
        // taken once, so a file with two gave the times of both; then the first edition with its hidden chapters,
        // where --chapters-file took the default one.
        int scanFrom = 0;
        int scanTo = static_cast<int>(text.size());
        const QRegularExpression flagOn(QStringLiteral("^\\s*1\\s*$"));
        const auto tagIsOne = [&text, &flagOn](const QString& tag, const int from, const int to)
        {
            const int open = text.indexOf(QLatin1Char('<') + tag + QLatin1Char('>'), from);
            if (open < 0 || open >= to)
                return false;
            const int valueAt = open + static_cast<int>(tag.size()) + 2;
            const int close = text.indexOf(QLatin1String("</") + tag, valueAt);
            return close >= 0 && close <= to &&
                   flagOn.match(QtCompat::strMid(text, valueAt, close - valueAt)).hasMatch();
        };
        const int firstEdition = text.indexOf(QLatin1String("<EditionEntry"));
        for (int ed = firstEdition; ed >= 0; ed = text.indexOf(QLatin1String("<EditionEntry"), ed + 1))
        {
            const int edEnd = text.indexOf(QLatin1String("</EditionEntry>"), ed);
            const int end = edEnd < 0 ? static_cast<int>(text.size()) : edEnd;
            int firstAtom = text.indexOf(QLatin1String("<ChapterAtom"), ed);
            if (firstAtom < 0 || firstAtom > end)
                firstAtom = end;
            const bool isDefault = tagIsOne(QStringLiteral("EditionFlagDefault"), ed, firstAtom);
            if (ed == firstEdition || isDefault)
            {
                scanFrom = ed;
                scanTo = end;
            }
            if (isDefault)
                break;
        }
        const QRegularExpression re(QStringLiteral("<ChapterTimeStart>\\s*([0-9:.]+)\\s*</ChapterTimeStart>"));
        for (int atom = text.indexOf(QLatin1String("<ChapterAtom"), scanFrom); atom >= 0 && atom < scanTo;
             atom = text.indexOf(QLatin1String("<ChapterAtom"), atom + 1))
        {
            int atomEnd = text.indexOf(QLatin1String("<ChapterAtom"), atom + 1);
            const int atomClose = text.indexOf(QLatin1String("</ChapterAtom>"), atom);
            if (atomEnd < 0 || (atomClose >= 0 && atomClose < atomEnd))
                atomEnd = atomClose < 0 ? scanTo : atomClose;
            atomEnd = (std::min)(atomEnd, scanTo);
            if (tagIsOne(QStringLiteral("ChapterFlagHidden"), atom, atomEnd))
                continue;
            const QRegularExpressionMatch m = re.match(QtCompat::strMid(text, atom, atomEnd - atom));
            if (m.hasMatch())
                times << m.captured(1).trimmed();
        }
    }
    else
    {
        QList<QString> lines;
        splitLines(text, lines);
        for (QString line : lines)
        {
            line = line.trimmed();
            if (line.isEmpty())
                continue;
            if (line.startsWith(QLatin1String("CHAPTER"), Qt::CaseInsensitive))
            {
                const int eq = line.indexOf(QLatin1Char('='));
                if (eq <= 0)
                    continue;
                if (QtCompat::strLeft(line, eq).trimmed().endsWith(QLatin1String("NAME"), Qt::CaseInsensitive))
                    continue;
                line = QtCompat::strMid(line, eq + 1).trimmed();
            }
            if (!line.isEmpty())
                times << line;
        }
    }
    if (times.isEmpty())
        return false;

    // A nine digit fraction, which Matroska XML uses, is cut to milliseconds: that is what this box
    // and the meta line both carry.
    for (QString& t : times)
    {
        const int dot = t.indexOf(QLatin1Char('.'));
        if (dot > 0 && t.size() > dot + 4)
            t = QtCompat::strLeft(t, dot + 4);
    }

    // A chapter FILE is a set of positions and the order it lists them in means nothing, while
    // the order of this box means everything: blurayHelper.cpp writes the marks straight out in
    // list order without sorting them, so an unordered file would put unordered marks on the
    // disc. Duplicates go for the same reason. Typed text is left exactly as typed: this is only
    // for a loaded file, where the order carries no intent.
    // Sorted as times, not as text: "00:..." came before "0:...", so 0:00 and 0:09:00 landed after 00:02:00, and
    // 00:00:30.5 and 00:00:30.500 were two chapters.
    const auto seconds = [](const QString& t)
    {
        double s = 0;
        for (const QString& part : t.split(QLatin1Char(':'))) s = s * 60 + part.toDouble();
        return s;
    };
    std::stable_sort(times.begin(), times.end(),
                     [&](const QString& a, const QString& b) { return seconds(a) < seconds(b); });
    times.erase(std::unique(times.begin(), times.end(),
                            [&](const QString& a, const QString& b) { return qAbs(seconds(a) - seconds(b)) < 0.0005; }),
                times.end());

    ui->radioButtonCustomChapters->setChecked(true);
    ui->memoChapters->setPlainText(times.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    m_customChaptersUserOverride = true;
    onChapterParamsChanged();
    return true;
}

void TsMuxerWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat("text/plain") || event->mimeData()->hasFormat("text/uri-list"))
    {
        if (ui->addBtn->isEnabled())
        {
            opacityTimer.stop();
            setWindowOpacity(0.9);
            event->acceptProposedAction();
            QWidget* w = childAt(event->position().toPoint());
            updateBtns(w);
        }
    }
}

void TsMuxerWindow::dropEvent(QDropEvent* event)
{
    setWindowOpacity(1.0);
    updateBtns(0);
    if (event->mimeData()->hasFormat("text/uri-list"))
    {
        addFileList = event->mimeData()->urls();
        event->acceptProposedAction();
    }
    else if (event->mimeData()->hasFormat("text/plain"))
    {
        QList<QString> strList;
        addFileList.clear();
        splitLines(event->mimeData()->text(), strList);
        QList<QUrl> urls = event->mimeData()->urls();
        for (int i = 0; i < strList.size(); ++i) addFileList << QUrl::fromLocalFile(strList[i]);
        event->acceptProposedAction();
    }
    if (addFileList.isEmpty())
        return;
    auto w = childAt(event->position().toPoint());

    // Dropped on the chapter list: load it as chapters rather than adding it as a track. The chapter
    // box is a QPlainTextEdit, so childAt returns its viewport rather than the box itself.
    const bool onChapterBox = w != nullptr && (w == ui->memoChapters || w->parentWidget() == ui->memoChapters);
    // The gate is whether this OUTPUT offers chapters at all, which is what the custom radio's own
    // enabled state says. It used to test whether the BOX was enabled, and the box is disabled until
    // the custom radio is selected: so with "No chapters" chosen, which is how the window starts, a
    // dropped chapter file did nothing whatever and said nothing either. The loader selects the radio
    // itself, so there is nothing to select first.
    if (onChapterBox && addFileList.size() == 1 && ui->radioButtonCustomChapters->isEnabled())
    {
        if (loadChapterFileIntoBox(addFileList.first().toLocalFile()))
        {
            addFileList.clear();
            return;
        }
    }

    if (w && w == ui->btnAppend && w->isEnabled())
        appendFile();
    else if (ui->addBtn->isEnabled())
        addFile();
}

void TsMuxerWindow::dragMoveEvent(QDragMoveEvent* event)
{
    event->acceptProposedAction();
    QWidget* w = childAt(event->position().toPoint());
    updateBtns(w);
}

void TsMuxerWindow::updateBtns(QWidget* w)
{
    if (w)
    {
        ui->btnAppend->setDefault(w == ui->btnAppend && w->isEnabled());
        ui->addBtn->setDefault(w == ui->addBtn && w->isEnabled());
    }
    else
    {
        ui->btnAppend->setDefault(false);
        ui->addBtn->setDefault(false);
    }
    QFont font = ui->removeFile->font();
    QFont bFont(font);
    bFont.setBold(true);
    if (ui->btnAppend->isDefault())
        ui->btnAppend->setFont(bFont);
    else
        ui->btnAppend->setFont(font);
    if (ui->addBtn->isDefault())
        ui->addBtn->setFont(bFont);
    else
        ui->addBtn->setFont(font);
}

void TsMuxerWindow::dragLeaveEvent(QDragLeaveEvent*) { opacityTimer.start(100); }

void TsMuxerWindow::onOpacityTimer()
{
    opacityTimer.stop();
    setWindowOpacity(1.0);
    updateBtns(0);
}

// A 3D (MVC) track in the list and a Blu-ray folder or image as the output. An image gets its .ssif files, the names
// under which a 3D player finds both views. A folder does not: a .ssif in a folder would be a full second copy of
// the video, so it is left to whatever makes the folder into a disc, and software that does not add it makes a disc
// that plays in 2D. Said here so that it is known before the mux starts.
void TsMuxerWindow::updateMux3dLabel()
{
    if (!m_mux3dLabel)
        return;
    // Unticking the 3D track is how a 2D disc is made from a 3D title: only the left-eye view is muxed, and the
    // disc gets plain 2D playlists. The line then says so, as the confirmation of that choice.
    bool mvcTicked = false;
    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        const auto codecInfo = getCodecInfo(i);
        if (codecInfo && codecInfo->displayName == "MVC" && ui->trackLV->item(i, 0) &&
            ui->trackLV->item(i, 0)->checkState() == Qt::Checked)
            mvcTicked = true;
    }
    const bool iso = ui->radioButtonBluRayISO->isChecked();
    if (!mvcTicked)
        m_mux3dLabel->setText(
            tr("3D track unticked: the disc is made in 2D from the left-eye view, without the "
               "right-eye view or .ssif files."));
    else if (iso)
        m_mux3dLabel->setText(tr("3D video (MVC) detected: the Blu-ray disc gets its .ssif files, so it plays in 3D."));
    else
        m_mux3dLabel->setText(
            tr("3D video (MVC) detected: a Blu-ray folder gets no .ssif files, so a disc made "
               "from it plays in 3D only if the .ssif files are added. Choose Blu-ray ISO, or make "
               "the folder into an image on the BDMV folder -> ISO tab, which adds them."));
    m_mux3dLabel->setVisible(m_3dMode && (iso || ui->radioButtonBluRay->isChecked()) && !ui->groupBox_4->isHidden());
}

void TsMuxerWindow::updateMaxOffsets()
{
    int maxPGOffsets = 0;
    m_3dMode = false;

    for (int i = 0; i < ui->trackLV->rowCount(); ++i)
    {
        auto codecInfo = getCodecInfo(i);
        if (!codecInfo)
            continue;

        if (codecInfo->displayName == "MVC")
        {
            m_3dMode = true;
            maxPGOffsets = qMax(maxPGOffsets, codecInfo->maxPgOffsets);
        }
    }

    disableUpdatesCnt++;

    int oldIndex = ui->offsetsComboBox->currentIndex();
    ui->offsetsComboBox->clear();
    ui->offsetsComboBox->addItem(QString("zero"));
    for (int i = 0; i < maxPGOffsets; ++i) ui->offsetsComboBox->addItem(QString("plane #%1").arg(i));
    if (oldIndex >= 0 && oldIndex < ui->offsetsComboBox->count())
        ui->offsetsComboBox->setCurrentIndex(oldIndex);

    disableUpdatesCnt--;
    updateMux3dLabel();
}

bool TsMuxerWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == ui->label_Donate && event->type() == QEvent::MouseButtonPress)
    {
        QDesktopServices::openUrl(QUrl("https://github.com/teaching-droid/tsMuxer"));
        return true;
    }
    // The space bar ticks and unticks the highlighted track. Qt does this by itself only
    // when the CURRENT CELL is the one holding the checkbox, and the checkbox is in column
    // 0 while anyone selecting a track clicks its name in column 1. So the key appeared to
    // do nothing, which is what was reported. Toggle column 0 of the current row whichever
    // column has the focus.
    if (obj == ui->trackLV && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Space)
    {
        const int row = ui->trackLV->currentRow();
        QTableWidgetItem* const box = row >= 0 ? ui->trackLV->item(row, 0) : nullptr;
        if (box)
        {
            // setCheckState raises itemChanged, which is what keeps the header's tri-state
            // box and the meta line in step, so nothing else has to be told.
            box->setCheckState(box->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
            return true;
        }
    }
    if (obj == ui->memoChapters && event->type() == QEvent::FocusOut)
        checkCustomChapters();
    return QWidget::eventFilter(obj, event);
}

void TsMuxerWindow::at_sectionCheckstateChanged(Qt::CheckState state)
{
    if (disableUpdatesCnt > 0)
        return;

    disableUpdatesCnt++;
    for (int i = 0; i < ui->trackLV->rowCount(); ++i) ui->trackLV->item(i, 0)->setCheckState(state);
    disableUpdatesCnt--;
    trackLVItemSelectionChanged();
}

void TsMuxerWindow::writeSettings()
{
    if (disableUpdatesCnt > 0)
        return;

    disableUpdatesCnt++;

    settings->beginGroup("main");
    // settings->setValue("asyncIO", ui->checkBoxuseAsynIO->isChecked());
    settings->setValue("soundEnabled", ui->checkBoxSound->isChecked());
    settings->setValue("hdmvPES", ui->checkBoxNewAudioPes->isChecked());
    if (ui->checkBoxCrop->isEnabled())
        settings->setValue("restoreCropEnabled", ui->checkBoxCrop->isChecked());
    settings->setValue("playlistParts", ui->checkBoxPlaylistParts->isChecked());
    settings->setValue("writeReport", ui->checkBoxReport->isChecked());
    settings->setValue("askExtraTracks", ui->checkBoxAskExtraTracks->isChecked());
    settings->setValue("coreEncoder", m_coreEncoderPath);
    settings->setValue("inputDir", lastInputDir);
    settings->setValue("outputDir", lastOutputDir);
    settings->setValue("useBlankPL", ui->checkBoxBlankPL->isChecked());
    settings->setValue("blankPLNum", ui->BlackplaylistCombo->value());

    settings->setValue("outputToInputFolder", ui->radioButtonOutoutInInput->isChecked());
    settings->setValue("outputMode", currentOutputMode());
    // never an empty value: read back, it left the boxes empty on every later start
    if (!ui->languageSelectComboBox->currentText().isEmpty())
        settings->setValue("language", ui->languageSelectComboBox->currentText());
    if (!ui->langCodesComboBox->currentData().toString().isEmpty())
        settings->setValue("langCodes", ui->langCodesComboBox->currentData().toString());
    if (m_wasShown)
    {
        settings->setValue("windowSize", size());
        settings->setValue("windowPos", pos());
    }

    settings->setValue("addSEIMethod", ui->comboBoxSEI->currentIndex());
    settings->setValue("addSPS", ui->checkBoxSPS->isChecked());

    settings->endGroup();

    settings->beginGroup("subtitles");
    settings->setValue("fontBorder", ui->spinEditBorder->value());
    settings->setValue("fontLineSpacing", ui->lineSpacing->value());
    settings->setValue("offset", ui->spinEditOffset->value());
    settings->setValue("fadeTime", getRendererAnimationTime());
    settings->setValue("keepText", ui->keepTextCheckBox->isChecked());
    settings->setValue("saveSrt", ui->saveSrtCheckBox->isChecked());
    settings->setValue("assAsSrt", ui->assAsSrtCheckBox->isChecked());
    settings->setValue("textStAsSrt", ui->textStAsSrtCheckBox->isChecked());
    settings->setValue("assFontsDir", ui->fontsDirLineEdit->text().trimmed());
    settings->endGroup();
    if (fontSettingsModel)  // writeSettings also runs before the model exists, when an empty ini is first filled
        fontSettingsModel->save(*settings);

    settings->beginGroup("pip");
    settings->setValue("corner", ui->comboBoxPipCorner->currentIndex());
    settings->setValue("h_offset", ui->spinBoxPipOffsetH->value());
    settings->setValue("v_offset", ui->spinBoxPipOffsetV->value());
    settings->setValue("size", ui->comboBoxPipSize->currentIndex());
    settings->endGroup();

    saveChoices();

    disableUpdatesCnt--;
}

// A list's entry from the settings, when it is one of its entries. -1 or a number past the list was taken as it was: a
// picture in picture size of -1 selected nothing, and the track was written as fullScreen.
static void setComboIndexFromSettings(QComboBox* box, const QVariant& value)
{
    bool ok = false;
    const int index = value.toInt(&ok);
    if (value.isValid() && ok && index >= 0 && index < box->count())
        box->setCurrentIndex(index);
}

bool TsMuxerWindow::readSettings()
{
    // due to QTBUG-28893, the settings saved under "general" are not accessible
    // when using .ini files for storage - those are used on Linux by default.
    // newer GUI versions will save the settings under the "main" group to avoid
    // that. the "general" group is still read in order to import the old settings
    // on non-Linux systems where the bug doesn't occur.
    // Nothing is written while settings are read. Restoring the interface language runs its change handler, which
    // saves the settings, and it did so inside the group being read: every start wrote a copy of the settings under
    // main/main, main/subtitles and main/pip, and into the registry even when an ini was used.
    disableUpdatesCnt++;
    const bool found = readSettingsBody();
    disableUpdatesCnt--;
    return found;
}

bool TsMuxerWindow::readSettingsBody()
{
    if (!readGeneralSettings("main") && !readGeneralSettings("general"))
    {
        return false;  // no settings still written
    }
    if (fontSettingsModel)
        fontSettingsModel->load(*settings);
    // checkBoxVBR checkBoxRVBR    editMaxBitrate  editMinBitrate  checkBoxCBR
    // editCBRBitrate  editVBVLen

    settings->beginGroup("subtitles");
    ui->spinEditBorder->setValue(settings->value("fontBorder", ui->spinEditBorder->value()).toInt());
    ui->lineSpacing->setValue(settings->value("fontLineSpacing", ui->lineSpacing->value()).toDouble());
    setRendererAnimationTime(settings->value("fadeTime", getRendererAnimationTime()).toDouble());
    ui->spinEditOffset->setValue(settings->value("offset", ui->spinEditOffset->value()).toInt());
    // On unless it was turned off: Matroska carries text subtitles itself, and text stays sharp, small and editable.
    ui->keepTextCheckBox->setChecked(settings->value("keepText", true).toBool());
    ui->saveSrtCheckBox->setChecked(settings->value("saveSrt", false).toBool());
    ui->assAsSrtCheckBox->setChecked(settings->value("assAsSrt", false).toBool());
    ui->textStAsSrtCheckBox->setChecked(settings->value("textStAsSrt", true).toBool());
    ui->fontsDirLineEdit->setText(settings->value("assFontsDir", QString()).toString());
    settings->endGroup();

    settings->beginGroup("pip");
    setComboIndexFromSettings(ui->comboBoxPipCorner, settings->value("corner"));
    ui->spinBoxPipOffsetH->setValue(settings->value("h_offset", ui->spinBoxPipOffsetH->value()).toInt());
    ui->spinBoxPipOffsetV->setValue(settings->value("v_offset", ui->spinBoxPipOffsetV->value()).toInt());
    setComboIndexFromSettings(ui->comboBoxPipSize, settings->value("size"));
    settings->endGroup();

    return true;
}

// The output section, stored by name rather than by position so that adding an output later
// cannot make an old setting select the wrong one.
// Worked out when the dialog is about to open rather than kept in a member. The member was set
// once at start up and then only from RadioButtonMuxClick, which is connected to clicked() and so
// never runs when the output mode comes back from the settings: the window opened on Matroska and
// offered to save a .ts. Asking the buttons at the moment the dialog opens cannot go stale.
QString TsMuxerWindow::currentSaveDialogFilter() const
{
    if (ui->radioButtonMKV->isChecked())
        return MKV_SAVE_DIALOG_FILTER();
    if (ui->radioButtonBluRayISO->isChecked())
        return ISO_SAVE_DIALOG_FILTER();
    if (ui->radioButtonTS->isChecked())
        return TS_SAVE_DIALOG_FILTER();
    return M2TS_SAVE_DIALOG_FILTER();
}

QString TsMuxerWindow::currentOutputMode() const
{
    if (ui->radioButtonM2TS->isChecked())
        return QStringLiteral("m2ts");
    if (ui->radioButtonMKV->isChecked())
        return QStringLiteral("mkv");
    if (ui->radioButtonBluRayISO->isChecked())
        return QStringLiteral("iso");
    if (ui->radioButtonBluRay->isChecked())
        return QStringLiteral("bluray");
    if (ui->radioButtonAVCHD->isChecked())
        return QStringLiteral("avchd");
    if (ui->radioButtonDemux->isChecked())
        return QStringLiteral("demux");
    return QStringLiteral("ts");
}

void TsMuxerWindow::setOutputMode(const QString& mode)
{
    if (mode == QLatin1String("m2ts"))
        ui->radioButtonM2TS->setChecked(true);
    else if (mode == QLatin1String("mkv"))
        ui->radioButtonMKV->setChecked(true);
    else if (mode == QLatin1String("iso"))
        ui->radioButtonBluRayISO->setChecked(true);
    else if (mode == QLatin1String("bluray"))
        ui->radioButtonBluRay->setChecked(true);
    else if (mode == QLatin1String("avchd"))
        ui->radioButtonAVCHD->setChecked(true);
    else if (mode == QLatin1String("demux"))
        ui->radioButtonDemux->setChecked(true);
    else if (mode == QLatin1String("ts"))
        ui->radioButtonTS->setChecked(true);
}

bool TsMuxerWindow::readGeneralSettings(const QString& prefix)
{
    settings->beginGroup(prefix);

    auto size = settings->value("windowSize");
    if (size.isValid() && size.canConvert<QSize>())
    {
        resize(size.toSize());
    }

    // The size was remembered and the position was not, so the window came back where the system
    // felt like putting it. Restoring a position blindly is worse than not restoring one: a
    // position saved on a monitor that is no longer attached puts the window somewhere nobody can
    // reach it. So it is only used if some screen still contains it.
    auto storedPos = settings->value("windowPos");
    if (storedPos.isValid() && storedPos.canConvert<QPoint>())
    {
        const QPoint p = storedPos.toPoint();
        bool onAScreen = false;
        for (const QScreen* screen : QGuiApplication::screens())
        {
            if (screen->availableGeometry().contains(p))
            {
                onAScreen = true;
                break;
            }
        }
        if (onAScreen)
            move(p);
    }

    // A store without a language, such as an ini that is still empty, leaves the language that is already chosen: it
    // set English, so a copy made portable lost its language when the ini took over the registry's settings.
    auto lang = settings->value("language");
    if (lang.isValid())
    {
        // A language the window does not have ("xx", or an empty value) left the box empty, and it was saved back
        // empty, so the box stayed empty on every later start. It now takes the system's language when the window has
        // it, else English.
        int idx = ui->languageSelectComboBox->findText(lang.toString());
        if (idx < 0)
        {
            idx = 0;
            for (const QString& name : QLocale::system().uiLanguages())
            {
                const int found = ui->languageSelectComboBox->findData(
                    name.section(QLatin1Char('-'), 0, 0).section(QLatin1Char('_'), 0, 0).toLower());
                if (found >= 0)
                {
                    idx = found;
                    break;
                }
            }
        }
        ui->languageSelectComboBox->setCurrentIndex(idx);
    }
    else if (ui->languageSelectComboBox->currentIndex() < 0)
    {
        ui->languageSelectComboBox->setCurrentIndex(0);
    }

    // Matched on the DATA and not on the visible text, which is translated: restoring by text
    // would fail the moment the window was not in English.
    const QVariant langCodes = settings->value("langCodes");
    if (langCodes.isValid())
    {
        int idx = ui->langCodesComboBox->findData(langCodes.toString());
        if (idx < 0)  // an unknown value: the default list
            idx = ui->langCodesComboBox->findData("terminological");
        if (idx >= 0)
            ui->langCodesComboBox->setCurrentIndex(idx);
    }

    // A group with no keys is no settings. One without outputDir, as an ini written by hand may be, was taken for none
    // and the rest of it was not read.
    if (settings->childKeys().isEmpty())
    {
        settings->endGroup();
        return false;
    }

    lastInputDir = settings->value("inputDir", lastInputDir).toString();
    lastOutputDir = settings->value("outputDir").toString();

    // A key that is missing keeps what the control already shows, its default: read as 0 or off, a hand-made or older
    // ini turned off the sound and the HDMV compatible TS and set the subtitle border, line spacing and offset to 0.
    // ui->checkBoxuseAsynIO->setChecked(settings->value("asyncIO").toBool());
    ui->checkBoxSound->setChecked(settings->value("soundEnabled", ui->checkBoxSound->isChecked()).toBool());
    ui->checkBoxNewAudioPes->setChecked(settings->value("hdmvPES", ui->checkBoxNewAudioPes->isChecked()).toBool());
    ui->checkBoxCrop->setChecked(settings->value("restoreCropEnabled").toBool());
    ui->checkBoxPlaylistParts->setChecked(
        settings->value("playlistParts", ui->checkBoxPlaylistParts->isChecked()).toBool());
    ui->checkBoxReport->setChecked(settings->value("writeReport", false).toBool());
    ui->checkBoxAskExtraTracks->setChecked(settings->value("askExtraTracks", true).toBool());
    m_coreEncoderPath = settings->value("coreEncoder").toString();
    ui->checkBoxBlankPL->setChecked(settings->value("useBlankPL", ui->checkBoxBlankPL->isChecked()).toBool());
    // 0 is a playlist number like any other: it was skipped, and the box kept its default of 1900
    if (settings->contains("blankPLNum"))
        ui->BlackplaylistCombo->setValue(settings->value("blankPLNum").toInt());

    ui->radioButtonOutoutInInput->setChecked(
        settings->value("outputToInputFolder", ui->radioButtonOutoutInInput->isChecked()).toBool());
    ui->radioButtonStoreOutput->setChecked(!ui->radioButtonOutoutInInput->isChecked());

    // Someone who always demuxes had to set that back on every start. Restored last, so it wins
    // over anything above that also touches the output section.
    setOutputMode(settings->value("outputMode").toString());

    setComboIndexFromSettings(ui->comboBoxSEI, settings->value("addSEIMethod"));
    ui->checkBoxSPS->setChecked(settings->value("addSPS", ui->checkBoxSPS->isChecked()).toBool());

    settings->endGroup();
    return true;
}

template <typename OnCodecListReadyFn, typename PostActionSignal, typename PostActionFn>
void TsMuxerWindow::processAddFileList(OnCodecListReadyFn onCodecListReady, PostActionSignal postActionSignal,
                                       PostActionFn postActionFn)
{
    if (addFileList.isEmpty())
        return;
    newFileName = QDir::toNativeSeparators(addFileList[0].toLocalFile());
    if (lastSourceDir.isEmpty())
        lastSourceDir = QFileInfo(newFileName).absolutePath();
    addFileList.removeAt(0);
    m_addNextFile = postActionFn;
    m_pendingDvdTitle = 0;
    // A disc folder, drive or image gave "Unsupported format": its titles are listed now and one is chosen, whose
    // playlist or DVD title is then added as any source is (onDiscTitlesReceived). A disc is added, not joined.
    if (isDiscSource(newFileName))
    {
        if (postActionFn != &TsMuxerWindow::addFile)
        {
            QMessageBox::warning(this, tr("Join"),
                                 tr("\"%1\" is a disc. Add it with \"add\" or \"add disc\", and choose a title; a "
                                    "Blu-ray title can then be joined to another one.")
                                     .arg(newFileName));
            if (!addFileList.isEmpty())
                (this->*postActionFn)();
            return;
        }
        m_discSource = newFileName;
        m_discOthersShown = false;
        listDiscTitles();
        return;
    }
    if (!checkFileDuplicate(newFileName))
    {
        // The box has said so; the files chosen after this one are still added.
        if (!addFileList.isEmpty())
            (this->*postActionFn)();
        return;
    }
    // disconnect(this, SIGNAL(tsMuxerSuccessFinished()));
    // disconnect(this, SIGNAL(codecListReady()));
    disconnect();
    connect(this, &TsMuxerWindow::tsMuxerSuccessFinished, this, &TsMuxerWindow::onTsMuxerCodecInfoReceived);
    connect(this, &TsMuxerWindow::codecListReady, this, onCodecListReady);
    connect(this, postActionSignal, this, postActionFn);
    runInMuxMode = false;
    m_runOutput.clear();
    setProbeBusy(true);
    tsMuxerExecute(QStringList() << newFileName);
}

template <typename F>
void TsMuxerWindow::showAddFilesDialog(QString&& windowTitle, F&& windowOkFn)
{
    if (m_fileDialogOpen)
        return;

    auto* dialog = new QFileDialog(this, windowTitle, getExistingDialogDir(lastInputDir), fileDialogFilter());
    dialog->setFileMode(QFileDialog::ExistingFiles);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_fileDialogOpen = true;
    connect(dialog, &QFileDialog::finished, this, [this](int) { m_fileDialogOpen = false; });
    connect(dialog, &QFileDialog::filesSelected, this,
            [this, okFn = std::forward<F>(windowOkFn)](const QStringList& files)
            {
                if (files.isEmpty())
                    return;
                lastInputDir = QFileInfo(files.back()).absolutePath();
                writeSettings();
                addFileList.clear();
                for (const auto& f : files) addFileList << QUrl::fromLocalFile(QDir::toNativeSeparators(f));
                okFn();
            });
    dialog->open();
}
