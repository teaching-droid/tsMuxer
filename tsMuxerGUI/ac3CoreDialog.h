#ifndef AC3_CORE_DIALOG_H_
#define AC3_CORE_DIALOG_H_

#include <QDialog>
#include <QProcess>
#include <QString>
#include <QStringList>

class QLabel;
class QProgressBar;

// A TrueHD track without an AC-3 core on a disc: the question what to do, and the making of the core with the user's
// own ffmpeg (tsMuxeR comes without one). ffmpeg stays out of sight: this window shows its progress, and Cancel, Esc
// or the window's X stops it and removes the file it began.
class Ac3CoreDialog : public QDialog
{
    Q_OBJECT
   public:
    struct Choice
    {
        enum What
        {
            Cancel,    // no mux
            UseTrack,  // the AC-3 track number `track` of the list given to ask()
            Create,    // a new core, by the program at `encoder`
            NoCore
        };
        What what = Cancel;
        int track = -1;
        QString encoder;
    };

    // What the job needs to make one core
    struct Job
    {
        QString encoder;
        QString source;
        QString map;         // the track in the source, as the encoder's -map takes it
        int channels = 0;    // of the TrueHD track, 0 when not known
        double seconds = 0;  // its length, 0 when not known
        QString target;      // the AC-3 file to write
    };

    // question: the sentence that names the TrueHD track; ac3Tracks: the AC-3 tracks of the same file that can serve as
    // the core; sourceProblem: why a core cannot be made from this source, "" when it can; savedEncoder: the path in
    // the settings, changed when the user chooses another program
    static Choice ask(QWidget* parent, const QString& question, const QStringList& ac3Tracks,
                      const QString& sourceProblem, QString* savedEncoder);

    explicit Ac3CoreDialog(QWidget* parent, const Job& job);

    // Starts the encoder as the dialog opens; Accepted when the file is complete
    int exec() override;

   private:
    void startEncoder();
    void onOutput();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onError(QProcess::ProcessError error);
    void fail(const QString& details);
    void reject() override;

    Job m_job;
    bool m_closing = false;
    QByteArray m_pending;  // the end of the progress text that has no line end yet
    QProcess m_proc;
    QProgressBar* m_bar;
    QLabel* m_state;
};

#endif
