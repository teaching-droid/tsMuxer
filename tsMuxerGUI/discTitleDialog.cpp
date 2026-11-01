#include "discTitleDialog.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTreeWidget>
#include <QVBoxLayout>

// The texts are the window's, so that they are translated with the others (tsmuxergui_*.ts, context TsMuxerWindow)
static QString wtr(const char* text) { return QCoreApplication::translate("TsMuxerWindow", text); }

static double hmsSeconds(const QString& text)
{
    double seconds = 0;
    for (const QString& part : text.split(QLatin1Char(':'))) seconds = seconds * 60 + part.toDouble();
    return seconds;
}

// The note a listing line ends with, in brackets: taken off the rest of the line
static QString takeNote(QString& rest)
{
    static const QRegularExpression note(QStringLiteral("^(.*?)\\s*\\(([^()]*)\\)\\s*$"));
    const QRegularExpressionMatch m = note.match(rest);
    if (!m.hasMatch())
    {
        rest = rest.trimmed();
        return QString();
    }
    rest = m.captured(1).trimmed();
    return m.captured(2);
}

DiscTitleList parseDiscTitles(const QList<QString>& output)
{
    // "Title  1  00800.mpls    1:39:36   17 chapters     1 clip   deu eng  3D  5 angles  (main film)"
    static const QRegularExpression bluray(
        QStringLiteral("^Title\\s+(\\d+)\\s+(\\S+\\.mpls)\\s+(\\d+:\\d\\d:\\d\\d)\\s+(\\d+)\\s+chapters?\\s+(\\d+)\\s+"
                       "clips?(.*)$"),
        QRegularExpression::CaseInsensitiveOption);
    // "Title 1  DVD title  2    0:48:06    7 chapters  audio en AC-3 2ch  subtitles en  ENCRYPTED  (main title)"
    static const QRegularExpression dvd(
        QStringLiteral("^Title\\s+(\\d+)\\s+DVD title\\s+(\\d+)\\s+(\\d+:\\d\\d:\\d\\d)\\s+(\\d+)\\s+chapters?(.*)$"));
    static const QRegularExpression extras(QStringLiteral("(?:^|\\s)(3D|\\d+ angles)(?=\\s|$)"));
    DiscTitleList list;
    bool leftOut = false;
    for (const QString& line : output)
    {
        if (line.startsWith(QLatin1String("DVD-Video, ")))
        {
            list.dvd = true;
            list.video = line.mid(11).trimmed();
        }
        else if (line.startsWith(QLatin1String("The disc has ")))
            list.mainLine = line.trimmed();
        else if (line.startsWith(QLatin1String("Left out:")))
            leftOut = true;
        else if (leftOut && line.startsWith(QLatin1String("  ")))
            list.leftOut << line.trimmed();
        else if (const QRegularExpressionMatch m = bluray.match(line); m.hasMatch())
        {
            DiscTitleRow row;
            row.number = m.captured(1).toInt();
            row.playlist = m.captured(2);
            row.length = m.captured(3);
            row.seconds = hmsSeconds(row.length);
            row.chapters = m.captured(4).toInt();
            row.clips = m.captured(5).toInt();
            QString rest = m.captured(6);
            row.note = takeNote(rest);
            row.isMain = row.note.startsWith(QLatin1String("main film"));
            // 3D and the angles go with the note, so that the audio column holds the languages only
            QStringList extra;
            for (auto it = extras.globalMatch(rest); it.hasNext();) extra << it.next().captured(1);
            rest.replace(extras, QString());
            if (!extra.isEmpty())
                row.note = extra.join(QLatin1String(", ")) + (row.note.isEmpty() ? QString() : "; " + row.note);
            row.audio = rest.simplified();
            list.titles << row;
        }
        else if (const QRegularExpressionMatch d = dvd.match(line); d.hasMatch())
        {
            DiscTitleRow row;
            list.dvd = true;
            row.number = d.captured(1).toInt();
            row.discTitle = d.captured(2).toInt();
            row.length = d.captured(3);
            row.seconds = hmsSeconds(row.length);
            row.chapters = d.captured(4).toInt();
            QString rest = d.captured(5);
            row.note = takeNote(rest);
            row.isMain = row.note.startsWith(QLatin1String("main title"));
            // the parts are two spaces apart: "audio ...", "subtitles ...", "ENCRYPTED"
            for (const QString& part : rest.split(QRegularExpression(QStringLiteral("\\s{2,}")), Qt::SkipEmptyParts))
            {
                if (part.startsWith(QLatin1String("audio ")))
                    row.audio = part.mid(6);
                else if (part.startsWith(QLatin1String("subtitles ")))
                    row.subtitles = part.mid(10);
                else if (part == QLatin1String("ENCRYPTED"))
                    row.encrypted = true;
            }
            list.titles << row;
        }
    }
    return list;
}

// A name in a folder whatever its case, as discs and copies of them write it
static QString childNoCase(const QString& folder, const QString& name)
{
    const QDir dir(folder);
    for (const QString& entry : dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
        if (entry.compare(name, Qt::CaseInsensitive) == 0)
            return dir.filePath(entry);
    return QString();
}

bool isDiscSource(const QString& path)
{
    const QFileInfo info(path);
    if (info.isFile())
        return info.suffix().compare(QLatin1String("iso"), Qt::CaseInsensitive) == 0;
    if (!info.isDir())
        return false;
    const QString name = info.fileName();
    if (name.compare(QLatin1String("BDMV"), Qt::CaseInsensitive) == 0 ||
        name.compare(QLatin1String("VIDEO_TS"), Qt::CaseInsensitive) == 0)
        return true;
    return !childNoCase(path, QStringLiteral("BDMV")).isEmpty() ||
           !childNoCase(path, QStringLiteral("VIDEO_TS")).isEmpty();
}

QString discName(const QString& disc)
{
    const QFileInfo info(disc);
    if (info.isFile())
        return info.completeBaseName();
    QDir dir(disc);
    if (dir.dirName().compare(QLatin1String("BDMV"), Qt::CaseInsensitive) == 0 ||
        dir.dirName().compare(QLatin1String("VIDEO_TS"), Qt::CaseInsensitive) == 0)
        dir.cdUp();
    if (!dir.isRoot())
        return dir.dirName();
    const QString label = QStorageInfo(dir.absolutePath()).name();
    return label.isEmpty() ? QStringLiteral("disc") : label;
}

QString discPlaylistPath(const QString& disc, const QString& playlist)
{
    const QFileInfo info(disc);
    QString bdmv;
    if (info.isFile())  // an image: tsMuxeR finds the names inside it whatever their case
        bdmv = disc + QStringLiteral("/BDMV");
    else if (info.fileName().compare(QLatin1String("BDMV"), Qt::CaseInsensitive) == 0)
        bdmv = disc;
    else
        bdmv = childNoCase(disc, QStringLiteral("BDMV"));
    const QString playlistDir = info.isFile() ? bdmv + QStringLiteral("/PLAYLIST") : childNoCase(bdmv, "PLAYLIST");
    const QString file = info.isFile() ? QString() : childNoCase(playlistDir, playlist);
    return QDir::toNativeSeparators(file.isEmpty() ? playlistDir + QLatin1Char('/') + playlist : file);
}

DiscTitleDialog::DiscTitleDialog(const QString& disc, const DiscTitleList& list, const bool othersShown,
                                 QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(wtr("Choose a title"));
    auto* layout = new QVBoxLayout(this);
    QString head = QStringLiteral("<b>%1</b>").arg(QDir::toNativeSeparators(disc).toHtmlEscaped());
    if (!list.video.isEmpty())
        head += QStringLiteral("<br>") + wtr("DVD-Video, %1").arg(list.video.toHtmlEscaped());
    int mains = 0;
    for (const DiscTitleRow& row : list.titles) mains += row.isMain ? 1 : 0;
    if (mains > 1)
        head += QStringLiteral("<br>") +
                wtr("This disc has %1 main titles, the episodes or films it holds. They are shown in bold.").arg(mains);
    auto* headLabel = new QLabel(head, this);
    headLabel->setTextFormat(Qt::RichText);
    headLabel->setWordWrap(true);
    layout->addWidget(headLabel);

    m_tree = new QTreeWidget(this);
    m_tree->setRootIsDecorated(false);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setUniformRowHeights(true);
    if (list.dvd)
        m_tree->setHeaderLabels({wtr("Title"), wtr("DVD title"), wtr("Length"), wtr("Chapters"), wtr("Audio"),
                                 wtr("Subtitles"), wtr("Note")});
    else
        m_tree->setHeaderLabels(
            {wtr("Title"), wtr("Playlist"), wtr("Length"), wtr("Chapters"), wtr("Clips"), wtr("Audio"), wtr("Note")});
    QTreeWidgetItem* first = nullptr;
    for (int i = 0; i < list.titles.size(); ++i)
    {
        const DiscTitleRow& row = list.titles[i];
        QStringList cells{QString::number(row.number)};
        if (list.dvd)
            cells << QString::number(row.discTitle) << row.length << QString::number(row.chapters) << row.audio
                  << row.subtitles << row.note;
        else
            cells << row.playlist << row.length << QString::number(row.chapters) << QString::number(row.clips)
                  << row.audio << row.note;
        auto* item = new QTreeWidgetItem(m_tree, cells);
        item->setData(0, Qt::UserRole, i);
        for (const int c : {0, 2, 3}) item->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
        if (!list.dvd)
            item->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        if (row.isMain)
        {
            QFont bold = item->font(0);
            bold.setBold(true);
            for (int c = 0; c < cells.size(); ++c) item->setFont(c, bold);
        }
        if (row.encrypted)
        {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            for (int c = 0; c < cells.size(); ++c)
                item->setToolTip(c, wtr("The video of this title is still encrypted, and tsMuxeR cannot read it."));
        }
        else if (!first || (row.isMain && !list.titles[first->data(0, Qt::UserRole).toInt()].isMain))
            first = item;
    }
    for (int c = 0; c < m_tree->columnCount(); ++c) m_tree->resizeColumnToContents(c);
    if (first)
        m_tree->setCurrentItem(first);
    layout->addWidget(m_tree);

    if (!othersShown && !list.leftOut.isEmpty())
    {
        auto* leftLabel = new QLabel(wtr("Some playlists or titles are left out, such as copies, one clip repeated, or "
                                         "titles under 2 minutes. Tick the box below to list them with the reason."),
                                     this);
        leftLabel->setWordWrap(true);
        layout->addWidget(leftLabel);
    }
    m_showOthers = new QCheckBox(wtr("Show the left-out and short titles too"), this);
    m_showOthers->setChecked(othersShown);
    connect(m_showOthers, &QCheckBox::toggled, this, [this]() { done(ShowOthersChanged); });
    layout->addWidget(m_showOthers);

    auto* buttons = new QDialogButtonBox(this);
    QPushButton* add = buttons->addButton(wtr("Add title"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    add->setEnabled(first != nullptr);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item)
            {
                if (item && (item->flags() & Qt::ItemIsEnabled))
                    accept();
            });
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [add](QTreeWidgetItem* item) { add->setEnabled(item && (item->flags() & Qt::ItemIsEnabled)); });
    layout->addWidget(buttons);

    int width = 60;
    for (int c = 0; c < m_tree->columnCount(); ++c) width += m_tree->columnWidth(c);
    resize(qBound(640, width, 1200), qBound(320, 200 + 22 * static_cast<int>(list.titles.size()), 720));
}

int DiscTitleDialog::chosen() const
{
    const QTreeWidgetItem* item = m_tree->currentItem();
    return item && (item->flags() & Qt::ItemIsEnabled) ? item->data(0, Qt::UserRole).toInt() : -1;
}
