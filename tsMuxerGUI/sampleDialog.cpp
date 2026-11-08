// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "sampleDialog.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimeEdit>
#include <QVBoxLayout>

#include "muxForm.h"

namespace
{
// An MP4 or QuickTime file: tsMuxeR takes only its start (its index would have to be made anew for another part)
bool startOnly(const QString& ext)
{
    static const QStringList kinds = {"mp4", "m4v", "m4a", "mov", "3gp", "3g2"};
    return kinds.contains(ext);
}

// A video stream on its own holds no times, so no part around a time can be found in it
bool noTimes(const QString& ext)
{
    static const QStringList kinds = {"264", "h264", "avc", "jsv", "mvc", "265", "h265", "hevc", "hvc", "m2v", "mpv",
                                      "m1v", "vc1",  "av1", "obu", "ivf", "mp4", "m4v",  "m4a",  "mov", "3gp", "3g2"};
    return kinds.contains(ext);
}

QString freeZipName()
{
    QString folder = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (folder.isEmpty())
        folder = QDir::homePath();
    for (int n = 1;; ++n)
    {
        const QString name = QDir(folder).filePath(QStringLiteral("tsMuxeR sample %1.zip").arg(n));
        if (!QFile::exists(name))
            return QDir::toNativeSeparators(name);
    }
}
}  // namespace

SampleDialog::SampleDialog(QWidget* parent, const QString& exePath, const QString& source, const QString& lastReport)
    : QDialog(parent), m_exePath(exePath), m_lastReport(lastReport)
{
    setWindowTitle(tr("Make a sample for a problem report"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("A part of the source, copied as it is, to attach to a problem report in place of the "
                                "whole file. The .zip holds the sample and a report of making it, and stays under "
                                "25 MB, the most a file on GitHub may be."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* grid = new QGridLayout();
    grid->addWidget(new QLabel(tr("Source:"), this), 0, 0);
    m_source = new QLineEdit(this);
    m_source->setReadOnly(true);
    m_browseSource = new QPushButton(tr("..."), this);
    auto* browseSource = m_browseSource;
    auto* sourceRow = new QHBoxLayout();
    sourceRow->addWidget(m_source);
    sourceRow->addWidget(browseSource);
    grid->addLayout(sourceRow, 0, 1);

    grid->addWidget(new QLabel(tr("Take:"), this), 1, 0, Qt::AlignTop);
    auto* takes = new QVBoxLayout();
    m_start = new QRadioButton(tr("the start"), this);
    m_end = new QRadioButton(tr("the end"), this);
    m_at = new QRadioButton(tr("around this time (where the problem is):"), this);
    m_time = new QTimeEdit(this);
    m_time->setDisplayFormat(QStringLiteral("H:mm:ss"));
    m_start->setChecked(true);
    auto* atRow = new QHBoxLayout();
    atRow->addWidget(m_at);
    atRow->addWidget(m_time);
    atRow->addStretch();
    takes->addWidget(m_start);
    takes->addWidget(m_end);
    takes->addLayout(atRow);
    grid->addLayout(takes, 1, 1);

    grid->addWidget(new QLabel(tr("Size:"), this), 2, 0);
    m_size = new QSpinBox(this);
    m_size->setRange(1, 24);
    m_size->setValue(20);
    m_size->setSuffix(tr(" MB"));
    auto* sizeRow = new QHBoxLayout();
    sizeRow->addWidget(m_size);
    sizeRow->addWidget(new QLabel(tr("(the .zip stays under 25 MB)"), this));
    sizeRow->addStretch();
    grid->addLayout(sizeRow, 2, 1);

    m_addReport = new QCheckBox(tr("Add the report of the last job"), this);
    m_addReport->setEnabled(!m_lastReport.isEmpty() && QFile::exists(m_lastReport));
    m_addReport->setChecked(m_addReport->isEnabled());
    if (!m_addReport->isEnabled())
        m_addReport->setToolTip(
            tr("There is no report of a job: tick \"Write a report of each mux for a problem "
               "report\" in the options and run the job again to have one."));
    grid->addWidget(m_addReport, 3, 1);

    grid->addWidget(new QLabel(tr("Save as:"), this), 4, 0);
    m_target = new QLineEdit(freeZipName(), this);
    m_browseTarget = new QPushButton(tr("..."), this);
    auto* browseTarget = m_browseTarget;
    auto* targetRow = new QHBoxLayout();
    targetRow->addWidget(m_target);
    targetRow->addWidget(browseTarget);
    grid->addLayout(targetRow, 4, 1);
    layout->addLayout(grid);

    m_state = new QLabel(this);
    m_state->setWordWrap(true);
    layout->addWidget(m_state);

    auto* buttons = new QDialogButtonBox(this);
    m_showReport = buttons->addButton(tr("Show report"), QDialogButtonBox::ActionRole);
    m_showReport->setEnabled(m_addReport->isEnabled());
    m_make = buttons->addButton(tr("Make sample"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    connect(browseSource, &QPushButton::clicked, this, &SampleDialog::onBrowseSource);
    connect(browseTarget, &QPushButton::clicked, this, &SampleDialog::onBrowseTarget);
    connect(m_showReport, &QPushButton::clicked, this, &SampleDialog::onShowReport);
    connect(m_make, &QPushButton::clicked, this, &SampleDialog::onMake);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_at, &QRadioButton::toggled, m_time, &QWidget::setEnabled);
    void (QProcess::*finished)(int, QProcess::ExitStatus) = &QProcess::finished;
    connect(&m_proc, finished, this, &SampleDialog::onFinished);
    connect(&m_proc, &QProcess::errorOccurred, this, &SampleDialog::onError);
    m_proc.setProcessChannelMode(QProcess::MergedChannels);
    m_time->setEnabled(false);
    setSource(source);
    resize(640, sizeHint().height());
}

// What the source offers: the end and a time where tsMuxeR can take them, with the reason where not
void SampleDialog::setSource(const QString& source)
{
    m_source->setText(QDir::toNativeSeparators(source));
    const QString ext = QFileInfo(source).suffix().toLower();
    const bool end = !startOnly(ext);
    const bool at = !noTimes(ext);
    m_end->setEnabled(end);
    m_at->setEnabled(at);
    m_end->setToolTip(end ? QString()
                          : tr("An MP4 or QuickTime file keeps the place of every frame in one index, so only its "
                               "start can be taken. Mux it to MKV first to take another part."));
    m_at->setToolTip(at               ? QString()
                     : startOnly(ext) ? m_end->toolTip()
                                      : tr("A video stream on its own holds no times. Take its start or its end."));
    if ((!end && m_end->isChecked()) || (!at && m_at->isChecked()))
        m_start->setChecked(true);
    m_make->setEnabled(!source.isEmpty());
}

void SampleDialog::onBrowseSource()
{
    const QString name = QFileDialog::getOpenFileName(this, tr("Source of the sample"), m_source->text());
    if (!name.isEmpty())
        setSource(name);
}

void SampleDialog::onBrowseTarget()
{
    const QString name =
        QFileDialog::getSaveFileName(this, tr("Save the sample as"), m_target->text(), tr("Zip files (*.zip)"));
    if (!name.isEmpty())
        m_target->setText(QDir::toNativeSeparators(name.endsWith(".zip", Qt::CaseInsensitive) ? name : name + ".zip"));
}

void SampleDialog::onShowReport()
{
    QFile file(m_lastReport);
    if (file.open(QIODevice::ReadOnly))
        showReportPreview(this, tr("Report of the last job"), QString::fromUtf8(file.readAll()), false);
}

void SampleDialog::onMake()
{
    QStringList args = {QStringLiteral("--make-sample")};
    if (m_end->isChecked())
        args << QStringLiteral("--from-end");
    else if (m_at->isChecked())
        args << (QStringLiteral("--at=") + m_time->time().toString(QStringLiteral("H:mm:ss")));
    else
        args << QStringLiteral("--from-start");
    args << QStringLiteral("--size=%1").arg(m_size->value());
    // a name of this window's own: two windows making samples do not share it
    m_sampleReport =
        QDir(QDir::tempPath())
            .filePath(QStringLiteral("tsMuxeR_sample_report_%1.txt").arg(QCoreApplication::applicationPid()));
    QFile::remove(m_sampleReport);
    args << (QStringLiteral("--report=") + QDir::toNativeSeparators(m_sampleReport));
    if (m_addReport->isEnabled() && m_addReport->isChecked())
        args << (QStringLiteral("--add-report=") + QDir::toNativeSeparators(m_lastReport));
    args << QDir::toNativeSeparators(m_source->text()) << m_target->text();
    m_running = m_target->text();
    setRunning(true);
    m_state->setText(tr("Making the sample..."));
    m_proc.start(m_exePath, args);
}

void SampleDialog::setRunning(const bool running)
{
    if (!running)
        m_running.clear();
    m_make->setEnabled(!running && !m_source->text().isEmpty());
    m_browseSource->setEnabled(!running);
    m_browseTarget->setEnabled(!running);
    m_target->setReadOnly(running);
    m_showReport->setEnabled(!running && m_addReport->isEnabled());
}

void SampleDialog::onError(const QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart || m_closing)
        return;  // the others end the process, and onFinished says so
    setRunning(false);
    m_state->setText(tr("tsMuxeR could not be started, so no sample was made."));
    QFile::remove(m_sampleReport);
}

void SampleDialog::reject()
{
    if (!m_running.isEmpty() && m_proc.state() != QProcess::NotRunning)
    {
        m_closing = true;
        m_proc.kill();
        m_proc.waitForFinished(5000);
        QFile::remove(m_running);  // the zip it began
        QFile::remove(m_sampleReport);
    }
    QDialog::reject();
}

void SampleDialog::onFinished(const int exitCode, const QProcess::ExitStatus status)
{
    if (m_closing)
        return;  // stopped by Close: nothing to say
    const QString made = m_running;
    setRunning(false);
    const QString output = QString::fromUtf8(m_proc.readAll());
    if (status != QProcess::NormalExit || exitCode != 0)
    {
        m_state->setText(tr("No sample was made."));
        // the lines that say why: tsMuxeR's own, without its version line
        QStringList lines;
        for (const QString& line : output.split('\n'))
            if (!line.trimmed().isEmpty() && !line.startsWith(QLatin1String("tsMuxeR version")))
                lines << line.trimmed();
        QMessageBox::warning(this, windowTitle(), lines.join('\n'));
        QFile::remove(m_sampleReport);
        return;
    }
    QFile file(m_sampleReport);
    if (file.open(QIODevice::ReadOnly))
    {
        showReportPreview(this, tr("Report of making the sample"), QString::fromUtf8(file.readAll()), false);
        file.close();
    }
    QFile::remove(m_sampleReport);
    m_state->setText(tr("The sample is saved as \"%1\". Attach this .zip file to the problem report.").arg(made));
    m_target->setText(freeZipName());
}
