#ifndef SAMPLE_DIALOG_H_
#define SAMPLE_DIALOG_H_

#include <QDialog>
#include <QProcess>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTimeEdit;

// "Make a sample...": a part of a source for a problem report, made by tsMuxeR --make-sample (sampleCutter.h). The
// dialog offers what the command offers for the source: an MP4 file only its start, a video stream on its own no time.
class SampleDialog : public QDialog
{
    Q_OBJECT
   public:
    // exePath: tsMuxeR itself; lastReport: the report of the last job, which can go into the zip ("" for none)
    SampleDialog(QWidget* parent, const QString& exePath, const QString& source, const QString& lastReport);

   private:
    void setSource(const QString& source);
    void onBrowseSource();
    void onBrowseTarget();
    void onShowReport();
    void onMake();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onError(QProcess::ProcessError error);
    // the controls while tsMuxeR makes the sample: none can change what it was given
    void setRunning(bool running);
    // Close, Esc or the window's X while tsMuxeR runs: it is stopped, and the zip it began is removed
    void reject() override;

    QString m_exePath;
    QString m_lastReport;
    QString m_sampleReport;  // the report of making the sample, shown when it is made
    QString m_running;       // the zip being made; empty when tsMuxeR does not run
    bool m_closing = false;
    QProcess m_proc;
    QLineEdit* m_source;
    QRadioButton* m_start;
    QRadioButton* m_end;
    QRadioButton* m_at;
    QTimeEdit* m_time;
    QSpinBox* m_size;
    QCheckBox* m_addReport;
    QLineEdit* m_target;
    QLabel* m_state;
    QPushButton* m_showReport;
    QPushButton* m_make;
    QPushButton* m_browseSource;
    QPushButton* m_browseTarget;
};

#endif
