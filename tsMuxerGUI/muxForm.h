// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef MUX_FORM_H_
#define MUX_FORM_H_

#include <QDialog>
#include <QStringList>
class QProcess;
class Ui_muxForm;

// A report as it is attached to a problem report, read-only, with Copy and Close, and Save... when withSave. True when
// Save... was chosen.
bool showReportPreview(QWidget* parent, const QString& title, const QString& text, bool withSave);

class MuxForm : public QDialog  // QWidget
{
    Q_OBJECT
   public:
    explicit MuxForm(QWidget* parent);
    void prepare(const QString& label);
    void setProgress(int value);
    void addStdOutLine(const QString& line);
    void addStdErrLine(const QString& line);
    void muxFinished(int exitCode, const QString& prefix);
    void setProcess(QProcess* proc);
    void showStopped(const QString& label, const QString& outputLine);
    void abort() { onAbort(); }  // as the Abort button
    // The report tsMuxeR writes for this job (--report), or none: Save report... keeps a copy when the job has ended
    void setReportFile(const QString& fileName);
    [[nodiscard]] QString reportFile() const { return m_reportFile; }

   signals:
    // tsMuxeR was stopped with Abort, or with the dialog closed, and has ended
    void stopped();
    // "Make a sample...", offered when a job ends with an error
    void makeSampleRequested();

   protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
   private slots:
    void onProgressChanged();
    void onAbort();
    void onSaveReport();

   private:
    int errCnt;
    int m_hiddenLines = 0;     // error-output lines that came in after the shown ones
    QStringList m_hiddenTail;  // the last of those, shown when the mux ends
    Ui_muxForm* ui;
    QProcess* muxProcess;
    QString m_reportFile;
};

#endif
