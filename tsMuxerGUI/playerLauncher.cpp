#include "playerLauncher.h"

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

#include <algorithm>
#include <cmath>
#include <tuple>

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QUrl>
#include <QWidget>

#include "muxForm.h"

// The texts are the window's, so that they are translated with the others (tsmuxergui_*.ts, context TsMuxerWindow)
static QString wtr(const char* text) { return QCoreApplication::translate("TsMuxerWindow", text); }

static const QString FOLDER_PREFIX = QStringLiteral("tsMuxeR_preview_");

// The process is still running. OpenProcess with PROCESS_QUERY_LIMITED_INFORMATION is there since Windows Vista.
static bool processRuns(const qint64 pid)
{
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return GetLastError() == ERROR_ACCESS_DENIED;  // there, but of another user
    DWORD code = 0;
    const bool runs = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return runs;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

PlayerLauncher::PlayerLauncher(const QString& tsMuxerPath, QWidget* parent) : QObject(parent), m_tsMuxer(tsMuxerPath)
{
}

PlayerLauncher::~PlayerLauncher()
{
    if (m_proc)
    {
        m_proc->disconnect();
        m_proc->kill();
        m_proc->waitForFinished();
    }
    removeFolder();
}

void PlayerLauncher::setPlayer(const QString& program, const QString& arguments)
{
    m_defaultProgram = program.trimmed();
    m_defaultArguments = arguments.trimmed();
}

void PlayerLauncher::setTitlePlayers(const QString& bluRayProgram, const QString& bluRayArguments,
                                     const QString& dvdProgram, const QString& dvdArguments)
{
    m_bluRayProgram = bluRayProgram.trimmed();
    m_bluRayArguments = bluRayArguments.trimmed();
    m_dvdProgram = dvdProgram.trimmed();
    m_dvdArguments = dvdArguments.trimmed();
}

QPair<QString, QString> PlayerLauncher::playerOf(const PreviewSource& source) const
{
    // a clip or any other file is a file: the player of every preview
    if (source.file.isEmpty() || !QFileInfo(source.file).isFile())
    {
        if (!source.playlist.isEmpty() && !m_bluRayProgram.isEmpty())
            return {m_bluRayProgram, m_bluRayArguments};
        if (source.dvdTitle > 0 && !m_dvdProgram.isEmpty())
            return {m_dvdProgram, m_dvdArguments};
    }
    return {m_defaultProgram, m_defaultArguments};
}

QString PlayerLauncher::folder() const
{
    return QDir(QDir::tempPath()).filePath(FOLDER_PREFIX + QString::number(QCoreApplication::applicationPid()));
}

void PlayerLauncher::removeFolder()
{
    QDir dir(folder());
    if (dir.exists())
        dir.removeRecursively();
}

void PlayerLauncher::removeOldFolders()
{
    const QDir temp(QDir::tempPath());
    const QDateTime weekAgo = QDateTime::currentDateTime().addDays(-7);
    for (const QFileInfo& info : temp.entryInfoList({FOLDER_PREFIX + '*'}, QDir::Dirs | QDir::NoDotAndDotDot))
    {
        bool ok = false;
        const qint64 pid = info.fileName().mid(FOLDER_PREFIX.size()).toLongLong(&ok);
        if (!ok || pid == QCoreApplication::applicationPid())
            continue;
        // the folder of a window that still runs stays, unless it is a week old: its number then most likely belongs
        // to another program by now
        if (processRuns(pid) && info.lastModified() > weekAgo)
            continue;
        QDir(info.absoluteFilePath()).removeRecursively();
    }
}

double PlayerLauncher::defaultStart(const double seconds)
{
    return std::floor((std::min)(seconds / 10, 120.0));
}

PlayerLauncher::Player PlayerLauncher::player() const
{
    if (m_program.isEmpty())
        return Player::Other;
    const QString name = QFileInfo(m_program).completeBaseName().toLower();
    if (name == QLatin1String("mpv"))
        return Player::Mpv;
    if (name == QLatin1String("vlc"))
        return Player::Vlc;
    return Player::Other;
}

bool PlayerLauncher::launch(const QString& what, const QStringList& presetArguments, QWidget* parent)
{
    if (m_program.isEmpty())
    {
        if (QDesktopServices::openUrl(QUrl::fromLocalFile(what)))
            return true;
        QMessageBox::warning(parent, wtr("Preview"),
                             wtr("The file \"%1\" could not be opened in the system's default player.")
                                 .arg(QDir::toNativeSeparators(what)));
        return false;
    }
    QStringList arguments = QProcess::splitCommand(m_arguments);
    bool placed = false;
    for (QString& argument : arguments)
        if (argument.contains(QLatin1String("{file}")))
        {
            argument.replace(QLatin1String("{file}"), what);
            placed = true;
        }
    if (!placed)
        arguments << what;
    arguments << presetArguments;
    if (QProcess::startDetached(m_program, arguments))
        return true;
    QMessageBox::warning(parent, wtr("Preview"),
                         wtr("The player \"%1\" could not be started.").arg(QDir::toNativeSeparators(m_program)));
    return false;
}

bool PlayerLauncher::launchDirect(const PreviewSource& source, QWidget* parent)
{
    const QString start = QString::number(source.start, 'f', 3);
    if (player() == Player::Mpv && !source.playlist.isEmpty())
    {
        // the disc of BDMV/PLAYLIST/<number>.mpls, a folder or an image, and the playlist by its number
        QString root = QDir::fromNativeSeparators(source.playlist);
        for (int up = 0; up < 3 && root.lastIndexOf('/') > 0; ++up) root.truncate(root.lastIndexOf('/'));
        bool ok = false;
        const int number = QFileInfo(source.playlist).completeBaseName().toInt(&ok);
        if (!ok)
            return false;
        QStringList preset{QStringLiteral("--bluray-device=") + QDir::toNativeSeparators(root)};
        if (source.angle > 1)
            preset << QStringLiteral("--bluray-angle=%1").arg(source.angle);
        if (source.start > 0)
            preset << QStringLiteral("--start=") + start;
        launch(QStringLiteral("bd://mpls/%1").arg(number), preset, parent);
        return true;
    }
    // VLC 3.0 opens a DVD title, from its start: tried with VLC 3.0.24, a title opened with --dvdnav-angle (or with
    // dvdsimple:// and --dvdread-angle) still played the first angle, and --start-time on a dvd:// address ended VLC
    // with a crash. Another angle is previewed by tsMuxeR.
    if (player() == Player::Vlc && source.dvdTitle > 0 && source.angle == 1)
    {
        launch(QStringLiteral("dvd:///%1#%2").arg(QDir::fromNativeSeparators(source.disc)).arg(source.dvdTitle), {},
               parent);
        return true;
    }
    return false;
}

void PlayerLauncher::preview(const PreviewSource& source, QWidget* parent)
{
    if (busy())
        return;
    // The player is fixed here, for the whole of this preview: a setting changed while tsMuxeR makes it does not move
    // the preview to another player
    std::tie(m_program, m_arguments) = playerOf(source);
    // a file is opened as it is: any player plays a whole clip or file, and can go anywhere in it
    if (!source.file.isEmpty() && QFileInfo(source.file).isFile())
    {
        launch(QDir::toNativeSeparators(source.file), {}, parent);
        return;
    }
    if (launchDirect(source, parent))
        return;
    m_source = source;
    m_parent = parent;
    m_stopped = false;
    m_form = new MuxForm(parent);
    m_form->setAttribute(Qt::WA_DeleteOnClose);
    m_form->setSampleOffered(false);
    m_form->setWindowModality(Qt::ApplicationModal);
    m_form->prepare(wtr("Making a preview"));
    m_form->show();
    if (m_source.tracks.isEmpty())
        startList();
    else
        startMux();
}

void PlayerLauncher::startList()
{
    m_lines.clear();
    m_form->addStdOutLine(wtr("Reading the tracks of the title..."));
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::finished, this, &PlayerLauncher::onListFinished);
    m_proc->start(m_tsMuxer, m_source.listArguments);
    m_form->setProcess(m_proc);
}

void PlayerLauncher::onListFinished(const int exitCode, const QProcess::ExitStatus status)
{
    const QString text = QString::fromUtf8(m_proc->readAllStandardOutput());
    if (status != QProcess::NormalExit || exitCode != 0)
    {
        if (status == QProcess::NormalExit && m_form)
            m_form->addStdErrLine(QString::fromUtf8(m_proc->readAllStandardError()).trimmed());
        finish();
        return;
    }
    // the first video track and the first audio track: "Track ID:    4113", then "Stream ID:   V_MPEG4/ISO/AVC"
    int track = 0;
    QPair<QString, int> video, audio;
    for (QString line : text.split('\n'))
    {
        line = line.trimmed();
        if (line.startsWith(QLatin1String("Track ID:")))
            track = line.mid(9).trimmed().toInt();
        else if (line.startsWith(QLatin1String("Stream ID:")) && track > 0)
        {
            const QString codec = line.mid(10).trimmed();
            if (codec.startsWith(QLatin1String("V_")) && video.first.isEmpty())
                video = {codec, track};
            else if (codec.startsWith(QLatin1String("A_")) && audio.first.isEmpty())
                audio = {codec, track};
            track = 0;
        }
    }
    if (video.first.isEmpty())
    {
        if (m_form)
            m_form->addStdErrLine(wtr("tsMuxeR found no video in this title, so no preview can be made."));
        finish();
        return;
    }
    m_source.tracks = {video};
    if (!audio.first.isEmpty())
        m_source.tracks << audio;
    m_proc->deleteLater();
    m_proc = nullptr;
    startMux();
}

void PlayerLauncher::startMux()
{
    QDir dir(folder());
    if (!dir.exists() && !QDir().mkpath(dir.path()))
    {
        m_form->addStdErrLine(wtr("The temporary folder \"%1\" could not be made.").arg(QDir::toNativeSeparators(dir.path())));
        finish();
        return;
    }
    // the earlier previews of this session, as far as no player holds them
    for (const QString& old : dir.entryList({QStringLiteral("preview_*")}, QDir::Files)) dir.remove(old);
    ++m_count;
    m_output = dir.filePath(QStringLiteral("preview_%1.ts").arg(m_count));
    m_meta = dir.filePath(QStringLiteral("preview_%1.meta").arg(m_count));
    // the title, or the file, as a meta line names it, in the angle chosen
    QString source;
    double start = m_source.start;
    if (!m_source.file.isEmpty())
    {
        source = QStringLiteral("\"%1\"").arg(m_source.file);
        start = m_source.fileStart;
    }
    else if (!m_source.playlist.isEmpty())
        source = QStringLiteral("\"%1\"").arg(m_source.playlist);
    else
        source = QStringLiteral("\"%1\", dvd-title=%2").arg(m_source.disc).arg(m_source.dvdTitle);
    const QString angle = m_source.angle > 1 && m_source.file.isEmpty() ? QStringLiteral(", angle=%1").arg(m_source.angle)
                                                                         : QString();
    // A cut reads the title from its start up to the cut. Only the pictures inside it are kept, which spares the
    // second reading that keeping the pictures before it takes (--cut-pictures).
    const qint64 from = qRound64(start * 1000);
    QString meta = QStringLiteral("MUXOPT --no-pcr-on-video-pid --new-audio-pes --vbr --vbv-len=500");
    if (from > 0)
        meta += QStringLiteral(" --cut-start=%1ms --cut-pictures=inside").arg(from);
    meta += QStringLiteral(" --cut-end=%1ms\n").arg(from + qRound64(PREVIEW_SECONDS * 1000));
    for (const auto& [codec, track] : m_source.tracks)
        meta += QStringLiteral("%1, %2, track=%3%4\n").arg(codec, source).arg(track).arg(angle);
    QFile file(m_meta);
    if (!file.open(QIODevice::WriteOnly) || file.write(meta.toUtf8()) < 0)
    {
        m_form->addStdErrLine(wtr("The temporary folder \"%1\" could not be made.").arg(QDir::toNativeSeparators(dir.path())));
        finish();
        return;
    }
    file.close();
    m_pending[0].clear();
    m_pending[1].clear();
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, [this]() { onOutput(false); });
    connect(m_proc, &QProcess::readyReadStandardError, this, [this]() { onOutput(true); });
    connect(m_proc, &QProcess::finished, this, &PlayerLauncher::onMuxFinished);
    m_proc->start(m_tsMuxer, {m_meta, m_output});
    m_form->setProcess(m_proc);
}

// The lines tsMuxeR prints, in the progress window: "12.3% complete" moves the bar, the rest is shown
void PlayerLauncher::onOutput(const bool error)
{
    QByteArray& pending = m_pending[error ? 1 : 0];
    pending += error ? m_proc->readAllStandardError() : m_proc->readAllStandardOutput();
    const auto end = pending.lastIndexOf('\n');
    if (end < 0 || !m_form)
        return;
    const QString text = QString::fromUtf8(pending.left(end + 1));
    pending.remove(0, end + 1);
    for (QString line : text.split('\n'))
    {
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        const int at = line.indexOf(QLatin1String("% complete"));
        if (at >= 0)
            m_form->setProgress(qRound(line.left(at).trimmed().toDouble() * 10));
        else if (error)
            m_form->addStdErrLine(line);
        else
            m_form->addStdOutLine(line);
    }
}

void PlayerLauncher::onMuxFinished(const int exitCode, const QProcess::ExitStatus status)
{
    onOutput(false);
    onOutput(true);
    QFile::remove(m_meta);
    if (status != QProcess::NormalExit || exitCode != 0)
    {
        QFile::remove(m_output);
        if (status == QProcess::NormalExit && m_form)
            m_form->muxFinished(exitCode, QString());
        finish();
        return;
    }
    m_proc->deleteLater();
    m_proc = nullptr;
    const QString output = m_output;
    QWidget* parent = m_parent;
    if (m_form)
        m_form->close();
    m_form = nullptr;
    launch(QDir::toNativeSeparators(output), {}, parent);
}

// The preview was stopped, or could not be made: the progress window says so and stays until it is closed
void PlayerLauncher::finish()
{
    const bool killed = m_proc && m_proc->exitStatus() != QProcess::NormalExit;
    if (m_proc)
    {
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    if (m_form)
    {
        if (killed)
            m_form->showStopped(wtr("The preview was stopped."), wtr("No preview was made."));
        else
            m_form->muxFinished(1, QString());
        m_form->setProcess(nullptr);
    }
    m_form = nullptr;
}
