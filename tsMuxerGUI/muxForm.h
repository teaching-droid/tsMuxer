#ifndef MUX_FORM_H_
#define MUX_FORM_H_

#include <QDialog>
#include <QStringList>
class QProcess;
class Ui_muxForm;

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

   signals:
    // tsMuxeR was stopped with Abort, or with the dialog closed, and has ended
    void stopped();

   protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
   private slots:
    void onProgressChanged();
    void onAbort();

   private:
    int errCnt;
    int m_hiddenLines = 0;     // error-output lines that came in after the shown ones
    QStringList m_hiddenTail;  // the last of those, shown when the mux ends
    Ui_muxForm* ui;
    QProcess* muxProcess;
};

#endif
