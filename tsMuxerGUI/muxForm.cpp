// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "muxForm.h"

#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui_muxForm.h"

// The window used to end tsMuxeR itself once 10,000 lines had come in on its error output ("Too many errors!
// tsMuxeR is terminated."). Warnings arrive there too, and some are printed once per frame, so a long mux that the
// command line finishes was cut off part way and its output file left short. tsMuxeR ends by itself on a real
// error. So the window now only stops drawing: it shows the first MAX_SHOWN_LINES lines, counts the rest, and when
// the mux ends shows how many were left out and the last TAIL_LINES of them, where a final error message would be.
const static int MAX_SHOWN_LINES = 10000;
const static int TAIL_LINES = 20;

MuxForm::MuxForm(QWidget* parent) : QDialog(parent, Qt::WindowMaximizeButtonHint), ui(new Ui_muxForm), muxProcess(0)
{
    ui->setupUi(this);
    connect(ui->progressBar, &QProgressBar::valueChanged, this, &MuxForm::onProgressChanged);
    connect(ui->abortBtn, &QPushButton::clicked, this, &MuxForm::onAbort);
    connect(ui->okBtn, &QPushButton::clicked, this, &MuxForm::close);
    connect(ui->reportBtn, &QPushButton::clicked, this, &MuxForm::onSaveReport);
    ui->reportBtn->setVisible(false);
    connect(ui->sampleBtn, &QPushButton::clicked, this, &MuxForm::makeSampleRequested);
    ui->sampleBtn->setVisible(false);
}

void MuxForm::closeEvent(QCloseEvent* event)
{
    onAbort();
    event->accept();
}

void MuxForm::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange)
    {
        ui->retranslateUi(this);
    }
    QDialog::changeEvent(event);
}

void MuxForm::prepare(const QString& label)
{
    muxProcess = 0;
    errCnt = 0;
    m_hiddenLines = 0;
    m_hiddenTail.clear();
    setWindowTitle(label);
    ui->muxLabel->setText(label + '.');
    ui->progressBar->setValue(0);
    onProgressChanged();  // the line an Abort left
    ui->stdoutText->clear();
    ui->stderrText->clear();
    ui->abortBtn->setEnabled(true);
    ui->okBtn->setEnabled(false);
    ui->okBtn->setText(QCoreApplication::translate("muxForm", "OK"));
    setReportFile(QString());
    ui->sampleBtn->setVisible(false);
}

void MuxForm::setReportFile(const QString& fileName)
{
    m_reportFile = fileName;
    ui->reportBtn->setVisible(!fileName.isEmpty());
    ui->reportBtn->setEnabled(false);  // until the job has ended and the report is complete
}

bool showReportPreview(QWidget* parent, const QString& title, const QString& text, const bool withSave)
{
    QDialog preview(parent, Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
    preview.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&preview);
    auto* note = new QLabel(MuxForm::tr("File names, disc titles, track names and chapter names are replaced by "
                                        "placeholders. This is the text that is attached."),
                            &preview);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* view = new QPlainTextEdit(text, &preview);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(view);
    auto* buttons = new QDialogButtonBox(&preview);
    if (withSave)
    {
        QPushButton* save = buttons->addButton(MuxForm::tr("Save..."), QDialogButtonBox::AcceptRole);
        QObject::connect(save, &QPushButton::clicked, &preview, &QDialog::accept);
    }
    QPushButton* copy = buttons->addButton(MuxForm::tr("Copy"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    QObject::connect(copy, &QPushButton::clicked, &preview, [text]() { QGuiApplication::clipboard()->setText(text); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);
    layout->addWidget(buttons);
    preview.resize(900, 600);
    return preview.exec() == QDialog::Accepted;
}

// Save report... shows the report first, as it will be attached: what it holds can be read before it leaves the
// computer. The file is saved under a name of its own, not one built from the output's name.
void MuxForm::onSaveReport()
{
    QFile file(m_reportFile);
    if (m_reportFile.isEmpty() || !file.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(this, tr("Save report"), tr("tsMuxeR wrote no report for this job."));
        return;
    }
    const QString text = QString::fromUtf8(file.readAll());
    file.close();
    if (!showReportPreview(this, tr("Save report"), text, true))
        return;

    const QString target = QFileDialog::getSaveFileName(
        this, tr("Save report"), QDir::home().filePath(QStringLiteral("tsMuxeR_report.txt")), tr("Text files (*.txt)"));
    if (target.isEmpty())
        return;
    if (QFile::exists(target))
        QFile::remove(target);
    if (!QFile::copy(m_reportFile, target))
    {
        QMessageBox::warning(this, tr("Save report"), tr("The report could not be saved as \"%1\".").arg(target));
        return;
    }
    QMessageBox::information(this, tr("Save report"),
                             tr("The report is saved as \"%1\". Attach this file to the problem report.")
                                 .arg(QDir::toNativeSeparators(target)));
}

void MuxForm::onProgressChanged()
{
    ui->progressLabel->setText(QObject::tr("Progress: ") + QString::number(ui->progressBar->value() / 10.0, 'f', 1) +
                               '%');
}

void MuxForm::setProgress(int value) { ui->progressBar->setValue(value); }

void MuxForm::addStdOutLine(const QString& line)
{
    ui->stdoutText->append(line);
    QTextCursor c = ui->stdoutText->textCursor();
    c.movePosition(QTextCursor::End);
    ui->stdoutText->setTextCursor(c);
}

void MuxForm::addStdErrLine(const QString& line)
{
    if (errCnt >= MAX_SHOWN_LINES)
    {
        const QStringList lines = line.split('\n');
        m_hiddenLines += lines.size();
        m_hiddenTail += lines;
        if (m_hiddenTail.size() > TAIL_LINES)
            m_hiddenTail.erase(m_hiddenTail.begin(), m_hiddenTail.end() - TAIL_LINES);
        return;
    }
    ui->stderrText->append(line);
    errCnt = ui->stderrText->document()->blockCount();
    if (errCnt >= MAX_SHOWN_LINES)
    {
        ui->stderrText->append("---------------------------------------");
        ui->stderrText->append(QObject::tr("Only the first %1 lines are shown here. tsMuxeR goes on, and the last "
                                           "lines are shown when it ends.")
                                   .arg(MAX_SHOWN_LINES));
    }
    QTextCursor c = ui->stderrText->textCursor();
    c.movePosition(QTextCursor::End);
    ui->stderrText->setTextCursor(c);
}

void MuxForm::muxFinished(int exitCode, const QString&)
{
    if (m_hiddenLines > 0)
    {
        ui->stderrText->append("---------------------------------------");
        ui->stderrText->append(QObject::tr("Lines not shown: %1. The last of them:").arg(m_hiddenLines));
        for (const QString& hidden : m_hiddenTail) ui->stderrText->append(hidden);
        m_hiddenLines = 0;
        m_hiddenTail.clear();
        QTextCursor c = ui->stderrText->textCursor();
        c.movePosition(QTextCursor::End);
        ui->stderrText->setTextCursor(c);
    }
    if (muxProcess && ui->abortBtn->isEnabled())
    {
        if (exitCode == 0)
            setWindowTitle(QObject::tr("tsMuxeR successfully finished"));
        else
            setWindowTitle(QObject::tr("tsMuxeR finished with error code %1").arg(exitCode));
        ui->muxLabel->setText(windowTitle() + '.');
        ui->abortBtn->setEnabled(false);
        ui->okBtn->setEnabled(true);
    }
    ui->reportBtn->setEnabled(!m_reportFile.isEmpty());
    // a job that went wrong: a sample of its source may be what the problem report needs
    ui->sampleBtn->setVisible(exitCode != 0);
}

void MuxForm::onAbort()
{
    // only a job that still runs: closing the dialog after the end must not take its output away
    if (muxProcess == nullptr || muxProcess->state() == QProcess::NotRunning)
        return;
    ui->abortBtn->setEnabled(false);
    ui->okBtn->setEnabled(true);
    setWindowTitle(QObject::tr("terminating tsMuxeR..."));
    muxProcess->kill();
    muxProcess->waitForFinished();
    setWindowTitle(QObject::tr("tsMuxeR is terminated"));
    muxProcess = nullptr;
    ui->reportBtn->setEnabled(!m_reportFile.isEmpty());  // the lines up to the stop are in it
    emit stopped();
}

// After Abort the dialog still read "Muxing in progress. Progress: 33.0%". It now says that the job was stopped and
// what became of its unfinished output, and the button closes it.
void MuxForm::showStopped(const QString& label, const QString& outputLine)
{
    ui->muxLabel->setText(label);
    ui->progressLabel->setText(outputLine);
    ui->okBtn->setText(QObject::tr("Close"));
}

void MuxForm::setProcess(QProcess* proc) { muxProcess = proc; }
