#include "discTitleDialog.h"

#include <algorithm>

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

#include "playerLauncher.h"

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
    // "Title  1  00011.mpls    1:52:14   24 chapters     1 clip   deu eng  3D  2 angles  (main film)"
    static const QRegularExpression bluray(
        QStringLiteral("^Title\\s+(\\d+)\\s+(\\S+\\.mpls)\\s+(\\d+:\\d\\d:\\d\\d)\\s+(\\d+)\\s+chapters?\\s+(\\d+)\\s+"
                       "clips?(.*)$"),
        QRegularExpression::CaseInsensitiveOption);
    // "Title 1  DVD title  2    0:48:06    7 chapters  audio en AC-3 2ch  subtitles en  ENCRYPTED  (main title)"
    static const QRegularExpression dvd(
        QStringLiteral("^Title\\s+(\\d+)\\s+DVD title\\s+(\\d+)\\s+(\\d+:\\d\\d:\\d\\d)\\s+(\\d+)\\s+chapters?(.*)$"));
    static const QRegularExpression extras(QStringLiteral("(?:^|\\s)(3D|\\d+ angles)(?=\\s|$)"));
    // the picture, the last part before the note: "SDR", "HDR10", "HDR10+", "Dolby Vision profile 7", or the first
    // three with a Dolby Vision after ", "
    static const QRegularExpression picture(QStringLiteral(
        "^(?:SDR|HDR10\\+?|Dolby Vision(?: profile [0-9.]+)?)(?:, Dolby Vision(?: profile [0-9.]+)?)?$"));
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
        // under a title of several angles: where they differ, and the clips of each angle there
        else if (!list.titles.isEmpty() && line.startsWith(QLatin1String("    angles differ at ")))
            list.titles.last().angleParts = line.mid(21).trimmed().split(QStringLiteral(", "));
        else if (!list.titles.isEmpty() && line.startsWith(QLatin1String("    angle ")) && line.contains(':'))
            list.titles.last().angleClips << line.section(':', 1).trimmed().split(QStringLiteral(", "));
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
            // the picture is the last part, two spaces after the one before it (the rest is trimmed: on a title without
            // languages, 3D or angles it is all of it)
            const int at = rest.lastIndexOf(QLatin1String("  "));
            if (picture.match(rest.mid(at < 0 ? 0 : at + 2).trimmed()).hasMatch())
            {
                row.picture = rest.mid(at < 0 ? 0 : at + 2).trimmed();
                rest.truncate(at < 0 ? 0 : at);
            }
            // 3D and the angles go with the note, so that the audio column holds the languages only
            QStringList extra;
            for (auto it = extras.globalMatch(rest); it.hasNext();) extra << it.next().captured(1);
            for (const QString& e : extra)
                if (e.endsWith(QLatin1String(" angles")))
                    row.angles = e.section(' ', 0, 0).toInt();
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
            // the parts are two spaces apart: "2 angles", "audio ...", "subtitles ...", "ENCRYPTED". The angles go
            // with the note, as on a Blu-ray line: a DVD title of several angles was shown without them.
            static const QRegularExpression angles(QStringLiteral("^\\d+ angles$"));
            for (const QString& part : rest.split(QRegularExpression(QStringLiteral("\\s{2,}")), Qt::SkipEmptyParts))
            {
                if (part.startsWith(QLatin1String("audio ")))
                    row.audio = part.mid(6);
                else if (part.startsWith(QLatin1String("subtitles ")))
                    row.subtitles = part.mid(10);
                else if (part == QLatin1String("ENCRYPTED"))
                    row.encrypted = true;
                else if (picture.match(part).hasMatch())
                    row.picture = part;
                else if (angles.match(part).hasMatch())
                {
                    row.note = part + (row.note.isEmpty() ? QString() : "; " + row.note);
                    row.angles = part.section(' ', 0, 0).toInt();
                }
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
                                 QWidget* parent, PlayerLauncher* player)
    : QDialog(parent), m_disc(disc), m_list(list), m_othersShown(othersShown), m_player(player)
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
                                 wtr("Subtitles"), wtr("Picture"), wtr("Note")});
    else
        m_tree->setHeaderLabels({wtr("Title"), wtr("Playlist"), wtr("Length"), wtr("Chapters"), wtr("Clips"),
                                 wtr("Audio"), wtr("Picture"), wtr("Note")});
    QTreeWidgetItem* first = nullptr;
    for (int i = 0; i < list.titles.size(); ++i)
      // a title of several angles is a row for each angle: each is muxed on its own (angle=)
      for (int angle = 1; angle <= (std::max)(1, list.titles[i].angles); ++angle)
    {
        const DiscTitleRow& row = list.titles[i];
        QString note = row.note;
        if (row.angles > 1)
        {
            const QString main = row.note.section(QLatin1String("; "), 1);
            note = wtr("angle %1 of %2").arg(angle).arg(row.angles) + (main.isEmpty() ? QString() : "; " + main);
        }
        QStringList cells{QString::number(row.number)};
        if (list.dvd)
            cells << QString::number(row.discTitle) << row.length << QString::number(row.chapters) << row.audio
                  << row.subtitles << row.picture << note;
        else
            cells << row.playlist << row.length << QString::number(row.chapters) << QString::number(row.clips)
                  << row.audio << row.picture << note;
        auto* item = new QTreeWidgetItem(m_tree, cells);
        item->setData(0, Qt::UserRole, i);
        item->setData(0, Qt::UserRole + 1, angle);
        // Where this angle differs, under its row and in its tooltip: the times, and on a Blu-ray the clip it plays
        // there, in bold where it is not the clip of the first angle. A line under a row chooses that row.
        if (row.angles > 1 && !row.angleParts.isEmpty())
        {
            QStringList tip;
            for (int k = 0; k < row.angleParts.size(); ++k)
            {
                const QString clip = angle - 1 < row.angleClips.size() && k < row.angleClips[angle - 1].size()
                                         ? row.angleClips[angle - 1][k]
                                         : QString();
                const bool own = angle > 1 && !clip.isEmpty() && !row.angleClips.isEmpty() &&
                                 k < row.angleClips[0].size() && row.angleClips[0][k] != clip;
                QStringList sub;
                for (int c = 0; c < cells.size(); ++c) sub << QString();
                sub[list.dvd ? 2 : 1] = list.dvd ? row.angleParts[k] : clip;
                if (!list.dvd)
                    sub[2] = row.angleParts[k];
                // A play item can name fewer clips than it has angles: the listing gives the first angle's clip for
                // the others, and the line then says so rather than that the angle differs there
                sub.last() = angle == 1                            ? wtr("the first angle")
                             : !list.dvd && !clip.isEmpty() && !own ? wtr("the same clip as angle 1")
                                                                    : wtr("differs from angle 1");
                auto* part = new QTreeWidgetItem(item, sub);
                part->setData(0, Qt::UserRole, i);
                part->setData(0, Qt::UserRole + 1, angle);
                part->setData(0, Qt::UserRole + 2, k + 1);  // the part, from 1: a preview starts there
                if (own)
                {
                    QFont bold = part->font(1);
                    bold.setBold(true);
                    for (int c = 0; c < sub.size(); ++c) part->setFont(c, bold);
                }
                // A Blu-ray angle's clip there is a link: a click plays that clip as it is (in an image, a short
                // preview of it)
                if (m_player && !list.dvd && !clip.isEmpty())
                {
                    QFont link = part->font(1);
                    link.setUnderline(true);
                    part->setFont(1, link);
                    part->setForeground(1, palette().brush(QPalette::Link));
                    part->setToolTip(1, wtr("Click to play this clip in the player for previews."));
                }
                tip << (clip.isEmpty() ? row.angleParts[k] : row.angleParts[k] + "  " + clip);
            }
            const QString text = wtr("Angle %1 differs from the others at:").arg(angle) + "\n" + tip.join('\n');
            for (int c = 0; c < cells.size(); ++c) item->setToolTip(c, text);
            // the lines are shown at once: they are what tells the angles apart
            m_tree->setRootIsDecorated(true);
            item->setExpanded(true);
        }
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
    QPushButton* previewButton = nullptr;
    if (m_player)
    {
        previewButton = buttons->addButton(wtr("Preview"), QDialogButtonBox::ActionRole);
        previewButton->setToolTip(
            wtr("Play the title chosen, in its angle, in the player for previews (General tab). Where the angles of a "
                "title differ, it starts there."));
        previewButton->setEnabled(first != nullptr);
        connect(previewButton, &QPushButton::clicked, this, [this]() { preview(m_tree->currentItem()); });
        connect(m_tree, &QTreeWidget::itemClicked, this,
                [this](QTreeWidgetItem* item, const int column)
                {
                    if (!m_list.dvd && column == 1 && item && item->data(0, Qt::UserRole + 2).toInt() > 0 &&
                        !item->text(1).isEmpty() && (item->flags() & Qt::ItemIsEnabled))
                        preview(item);
                });
    }
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
            [add, previewButton](QTreeWidgetItem* item)
            {
                add->setEnabled(item && (item->flags() & Qt::ItemIsEnabled));
                if (previewButton)
                    previewButton->setEnabled(add->isEnabled());
            });
    layout->addWidget(buttons);

    int width = 60;
    for (int c = 0; c < m_tree->columnCount(); ++c) width += m_tree->columnWidth(c);
    resize(qBound(640, width, 1200), qBound(320, 200 + 22 * static_cast<int>(list.titles.size()), 720));
}

// The row chosen in the player: the title in its angle, from the start of the part of the line chosen under an angle,
// or from where the angles differ first, or a part of the way into a title of one angle
void DiscTitleDialog::preview(const QTreeWidgetItem* item)
{
    const int index = item && (item->flags() & Qt::ItemIsEnabled) ? item->data(0, Qt::UserRole).toInt() : -1;
    if (!m_player || index < 0 || index >= m_list.titles.size())
        return;
    const DiscTitleRow& row = m_list.titles[index];
    PreviewSource source;
    source.angle = (std::max)(1, item->data(0, Qt::UserRole + 1).toInt());
    if (m_list.dvd)
    {
        source.disc = m_disc;
        source.dvdTitle = row.discTitle;
        // the number counts in the listing the title was chosen from
        if (m_othersShown)
            source.listArguments << QStringLiteral("--all-playlists");
        source.listArguments << QStringLiteral("--show-title=%1").arg(row.number) << m_disc;
    }
    else
    {
        source.playlist = discPlaylistPath(m_disc, row.playlist);
        source.listArguments << source.playlist;
    }
    // the part of several angles: the one of the line chosen, or the first
    const int chosenPart = item->data(0, Qt::UserRole + 2).toInt();
    const int part = chosenPart > 0 ? chosenPart - 1 : row.angles > 1 && !row.angleParts.isEmpty() ? 0 : -1;
    if (part >= 0 && part < row.angleParts.size())
    {
        source.start = hmsSeconds(row.angleParts[part].section(QLatin1Char('-'), 0, 0));
        // On a Blu-ray the angle plays a clip of its own there, which begins where the part begins: that clip is
        // opened as it is (or its preview made from its start, in an image), which a cut of the playlist at the part
        // would first read the title up to
        const int angle = source.angle;
        if (!m_list.dvd && angle - 1 < row.angleClips.size() && part < row.angleClips[angle - 1].size())
        {
            const QString playlistDir = QFileInfo(source.playlist).absolutePath();
            const QString streamDir = QFileInfo(playlistDir).absolutePath() + QStringLiteral("/STREAM");
            const QFileInfo info(m_disc);
            const QString stream =
                info.isFile() ? streamDir : childNoCase(QFileInfo(playlistDir).absolutePath(), QStringLiteral("STREAM"));
            QString clip = info.isFile() ? QString() : childNoCase(stream, row.angleClips[angle - 1][part]);
            if (clip.isEmpty())
                clip = (stream.isEmpty() ? streamDir : stream) + QLatin1Char('/') + row.angleClips[angle - 1][part];
            source.file = QDir::toNativeSeparators(clip);
            source.listArguments = QStringList{source.file};
        }
    }
    else
        source.start = PlayerLauncher::defaultStart(row.seconds);
    m_player->preview(source, this);
}

int DiscTitleDialog::chosen() const
{
    const QTreeWidgetItem* item = m_tree->currentItem();
    return item && (item->flags() & Qt::ItemIsEnabled) ? item->data(0, Qt::UserRole).toInt() : -1;
}

int DiscTitleDialog::chosenAngle() const
{
    const QTreeWidgetItem* item = m_tree->currentItem();
    return item ? (std::max)(1, item->data(0, Qt::UserRole + 1).toInt()) : 1;
}
