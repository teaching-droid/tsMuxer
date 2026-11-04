#include "ac3CoreDialog.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
// A program that cannot start, a build for a newer Windows or one that misses a library, must not bring up the system's
// error box in front of the window. A child takes this mode over from the parent.
class QuietStart
{
   public:
#ifdef _WIN32
    QuietStart() : m_old(SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX)) {}
    ~QuietStart() { SetErrorMode(m_old); }

   private:
    UINT m_old;
#else
    QuietStart() {}  // a constructor of its own: the variable is not an unused one on the other systems
#endif
};

// The program's standard output for these arguments, or empty when it did not start or end by itself in time
QString askProgram(const QString& path, const QStringList& args)
{
    const QuietStart quiet;
    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(path, args);
    if (!proc.waitForStarted(5000))
        return QString();
    if (!proc.waitForFinished(10000))
    {
        proc.kill();
        proc.waitForFinished(2000);
        return QString();
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return QString();
    return QString::fromLocal8Bit(proc.readAllStandardOutput());
}

// A line of -decoders or -encoders: flags, the name, the description. The flags of an audio codec start with A.
bool listsAudioCodec(const QString& list, const QString& name)
{
    const QRegularExpression line(QStringLiteral("^\\s*A\\S*\\s+%1\\s").arg(name),
                                  QRegularExpression::MultilineOption);
    return line.match(list).hasMatch();
}

QString timeText(const double seconds)
{
    const int s = static_cast<int>(seconds);
    return QStringLiteral("%1:%2:%3")
        .arg(s / 3600)
        .arg(s / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(s % 60, 2, 10, QLatin1Char('0'));
}

// "00:12:30.500000" of the progress text, -1 for anything else (it says N/A before the first frame)
double progressSeconds(const QByteArray& value)
{
    const QList<QByteArray> parts = value.trimmed().split(':');
    if (parts.size() != 3)
        return -1;
    bool ok[3];
    const double seconds =
        parts[0].toInt(&ok[0]) * 3600.0 + parts[1].toInt(&ok[1]) * 60.0 + parts[2].toDouble(&ok[2]);
    return ok[0] && ok[1] && ok[2] ? seconds : -1;
}
}  // namespace

// The first program that can do the job, looked for beside tsMuxeR, on the PATH and at the path in the settings.
// What it can do is asked, not read from a version number: builds differ in what they leave out.
static QString findEncoder(const QString& savedPath, QString* problem)
{
    QStringList candidates;
    const QString beside =
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg"), {QCoreApplication::applicationDirPath()});
    if (!beside.isEmpty())
        candidates << beside;
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!onPath.isEmpty() && !candidates.contains(onPath))
        candidates << onPath;
    if (!savedPath.isEmpty() && !candidates.contains(savedPath))
        candidates << savedPath;

    problem->clear();
    for (const QString& path : candidates)
    {
        const QString name = QDir::toNativeSeparators(path);
        if (!askProgram(path, {QStringLiteral("-hide_banner"), QStringLiteral("-version")})
                 .startsWith(QLatin1String("ffmpeg")))
            *problem = Ac3CoreDialog::tr("%1 does not start here.").arg(name);
        else if (!listsAudioCodec(askProgram(path, {QStringLiteral("-hide_banner"), QStringLiteral("-decoders")}),
                                  QStringLiteral("truehd")))
            *problem = Ac3CoreDialog::tr("%1 cannot read TrueHD.").arg(name);
        else if (!listsAudioCodec(askProgram(path, {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")}),
                                  QStringLiteral("ac3")))
            *problem = Ac3CoreDialog::tr("%1 cannot write AC-3.").arg(name);
        else
        {
            problem->clear();
            return path;
        }
    }
    if (candidates.isEmpty())
        *problem = Ac3CoreDialog::tr("ffmpeg was not found beside tsMuxeR or on the PATH.");
    return QString();
}

Ac3CoreDialog::Choice Ac3CoreDialog::ask(QWidget* parent, const QString& question, const QStringList& ac3Tracks,
                                         const QString& sourceProblem, QString* savedEncoder)
{
    QDialog box(parent);
    box.setWindowTitle(tr("TrueHD track without an AC-3 core"));
    auto* layout = new QVBoxLayout(&box);

    auto* text = new QLabel(question, &box);
    text->setWordWrap(true);
    layout->addWidget(text);

    QRadioButton* use = nullptr;
    QComboBox* tracks = nullptr;
    if (!ac3Tracks.isEmpty())
    {
        auto* row = new QHBoxLayout;
        use = new QRadioButton(tr("Use an AC-3 track of the file as the core:"), &box);
        tracks = new QComboBox(&box);
        tracks->addItems(ac3Tracks);
        row->addWidget(use);
        row->addWidget(tracks, 1);
        layout->addLayout(row);
    }

    auto* create = new QRadioButton(tr("Create an AC-3 core"), &box);
    layout->addWidget(create);
    auto* createInfo = new QLabel(&box);
    createInfo->setWordWrap(true);
    createInfo->setContentsMargins(20, 0, 0, 0);
    layout->addWidget(createInfo);
    auto* choose = new QPushButton(tr("Choose ffmpeg..."), &box);
    auto* chooseRow = new QHBoxLayout;
    chooseRow->setContentsMargins(20, 0, 0, 0);
    chooseRow->addWidget(choose);
    chooseRow->addStretch();
    layout->addLayout(chooseRow);

    auto* without = new QRadioButton(tr("Continue without a core"), &box);
    layout->addWidget(without);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &box);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &box, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &box, &QDialog::reject);

    QString encoder;
    const auto showEncoder = [&]
    {
        QString problem;
        encoder = sourceProblem.isEmpty() ? findEncoder(*savedEncoder, &problem) : QString();
        const bool usable = !encoder.isEmpty();
        create->setEnabled(usable);
        if (!usable && create->isChecked())
            (use != nullptr ? use : without)->setChecked(true);
        choose->setVisible(!usable && sourceProblem.isEmpty());
        createInfo->setEnabled(usable);
        createInfo->setText(usable ? tr("640 kbit/s, 48 kHz, at most 5.1 channels. Made with %1.")
                                         .arg(QDir::toNativeSeparators(encoder))
                                   : (sourceProblem.isEmpty() ? problem : sourceProblem));
    };
    showEncoder();
    connect(choose, &QPushButton::clicked, &box,
            [&]
            {
                const QString path = QFileDialog::getOpenFileName(&box, tr("Choose ffmpeg"));
                if (path.isEmpty())
                    return;
                *savedEncoder = path;
                showEncoder();
                if (create->isEnabled())
                    create->setChecked(true);
            });

    (use != nullptr ? use : (create->isEnabled() ? create : without))->setChecked(true);
    box.resize(560, box.sizeHint().height());

    Choice choice;
    if (box.exec() != QDialog::Accepted)
        return choice;
    if (use != nullptr && use->isChecked())
    {
        choice.what = Choice::UseTrack;
        choice.track = tracks->currentIndex();
    }
    else if (create->isChecked())
    {
        choice.what = Choice::Create;
        choice.encoder = encoder;
    }
    else
        choice.what = Choice::NoCore;
    return choice;
}

Ac3CoreDialog::Ac3CoreDialog(QWidget* parent, const Job& job) : QDialog(parent), m_job(job)
{
    setWindowTitle(tr("Creating the AC-3 core"));
    auto* layout = new QVBoxLayout(this);
    auto* name = new QLabel(QDir::toNativeSeparators(job.source), this);
    name->setWordWrap(true);
    layout->addWidget(name);
    m_bar = new QProgressBar(this);
    if (job.seconds > 0)
        m_bar->setRange(0, 1000);
    else
        m_bar->setRange(0, 0);  // moves without a share of the whole
    layout->addWidget(m_bar);
    m_state = new QLabel(tr("Starting..."), this);
    layout->addWidget(m_state);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    void (QProcess::*finished)(int, QProcess::ExitStatus) = &QProcess::finished;
    connect(&m_proc, finished, this, &Ac3CoreDialog::onFinished);
    connect(&m_proc, &QProcess::errorOccurred, this, &Ac3CoreDialog::onError);
    connect(&m_proc, &QProcess::readyReadStandardOutput, this, &Ac3CoreDialog::onOutput);
    m_proc.setProcessChannelMode(QProcess::SeparateChannels);
    resize(520, sizeHint().height());
}

int Ac3CoreDialog::exec()
{
    QTimer::singleShot(0, this, &Ac3CoreDialog::startEncoder);
    return QDialog::exec();
}

void Ac3CoreDialog::startEncoder()
{
    QStringList args = {QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-nostats"),
                        QStringLiteral("-loglevel"),    QStringLiteral("error"),    QStringLiteral("-progress"),
                        QStringLiteral("pipe:1"),       QStringLiteral("-y")};
    // 7.1 holds a 5.1 presentation of its own, made by the mastering. Without this the encoder folds the eight
    // channels down by itself, and without a word.
    if (m_job.channels > 6)
        args << QStringLiteral("-downmix") << QStringLiteral("5.1(side)");
    args << QStringLiteral("-i") << QDir::toNativeSeparators(m_job.source) << QStringLiteral("-map") << m_job.map
         << QStringLiteral("-c:a") << QStringLiteral("ac3") << QStringLiteral("-b:a") << QStringLiteral("640k")
         // An AC-3 encoder runs 256 samples late: they are taken from the start, so the core sits exactly on the
         // TrueHD, and added at the end, so that it keeps the length. At 48 kHz, as the rate an AC-3 core must have:
         // the encoder would leave a 44.1 kHz source at 44.1 kHz.
         << QStringLiteral("-af") << QStringLiteral("aresample=48000,atrim=start_sample=256,apad=pad_len=256")
         << QStringLiteral("-ar") << QStringLiteral("48000") << QStringLiteral("-f") << QStringLiteral("ac3")
         << QDir::toNativeSeparators(m_job.target);
    const QuietStart quiet;
    m_proc.start(m_job.encoder, args);
}

void Ac3CoreDialog::onOutput()
{
    m_pending += m_proc.readAllStandardOutput();
    int end;
    while ((end = m_pending.indexOf('\n')) >= 0)
    {
        const QByteArray line = m_pending.left(end);
        m_pending.remove(0, end + 1);
        if (!line.startsWith("out_time="))
            continue;
        const double seconds = progressSeconds(line.mid(9));
        if (seconds < 0)
            continue;
        if (m_job.seconds > 0)
        {
            m_bar->setValue(qBound(0, static_cast<int>(seconds * 1000 / m_job.seconds), 1000));
            m_state->setText(tr("%1 of %2").arg(timeText(seconds), timeText(m_job.seconds)));
        }
        else
            m_state->setText(timeText(seconds));
    }
}

void Ac3CoreDialog::fail(const QString& details)
{
    QFile::remove(m_job.target);
    QMessageBox::warning(this, windowTitle(), tr("The AC-3 core could not be created.") +
                                                  (details.isEmpty() ? QString() : QStringLiteral("\n\n") + details));
    QDialog::reject();
}

void Ac3CoreDialog::onError(const QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart || m_closing)
        return;  // the others end the process, and onFinished says so
    fail(tr("ffmpeg could not be started."));
}

void Ac3CoreDialog::onFinished(const int exitCode, const QProcess::ExitStatus status)
{
    if (m_closing)
        return;  // stopped by Cancel: nothing to say
    const QString errors = QString::fromLocal8Bit(m_proc.readAllStandardError()).trimmed();
    if (status != QProcess::NormalExit || exitCode != 0 || QFileInfo(m_job.target).size() == 0)
    {
        fail(errors.right(600));
        return;
    }
    accept();
}

void Ac3CoreDialog::reject()
{
    if (m_proc.state() != QProcess::NotRunning)
    {
        m_closing = true;
        m_proc.kill();
        m_proc.waitForFinished(5000);
    }
    QFile::remove(m_job.target);
    QDialog::reject();
}
