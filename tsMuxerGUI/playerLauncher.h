#ifndef PLAYER_LAUNCHER_H_
#define PLAYER_LAUNCHER_H_

#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

class MuxForm;
class QWidget;

// What to preview: a file that a player opens as it is, or a title of a disc in one of its angles, from a place in it
struct PreviewSource
{
    QString file;      // a file as it is (also a clip of a disc folder); empty for a disc title
    QString playlist;  // a Blu-ray title: its playlist file, the full path (inside an image: x.iso/BDMV/PLAYLIST/...)
    QString disc;      // a DVD title: the disc folder or image
    int dvdTitle = 0;  // a DVD title: the disc's own number of it (dvd-title=)
    // The arguments that list its tracks (tsMuxeR <arguments>): the playlist, --show-title=<n> and the disc, or the
    // file. Not needed when tracks is given.
    QStringList listArguments;
    QList<QPair<QString, int>> tracks;  // the codec and track number of its video and its first audio, when known
    int angle = 1;
    double start = 0;      // where the preview of a title starts, in seconds of the title
    double fileStart = 0;  // where the preview of a file starts, in seconds of the file
};

// A preview of a title or file in a player outside the window: the one set on the General tab (a program and its
// arguments), or the system's default player. A file is opened as it is. A disc title is opened by the player directly
// where the player can show its angle: mpv a Blu-ray playlist (bd://mpls/<number> with --bluray-angle, which mpv 0.41
// and later know), VLC a DVD title in its first angle (dvd://; VLC has no Blu-ray angle switch, and its DVD angle
// switch did not work, see launchDirect). For any other player and title, tsMuxeR first makes a short .ts of the title in its angle (angle= with --cut-start and --cut-end), which
// every player can open, Windows 7 included, also from a disc image. It runs with the progress window of a mux and can be stopped there. The previews go into a folder of this
// session in the system's temporary folder, removed when the window closes, or at the next start when it could not be
// removed then (a player may still have a file open).
class PlayerLauncher : public QObject
{
   public:
    static constexpr double PREVIEW_SECONDS = 30;

    PlayerLauncher(const QString& tsMuxerPath, QWidget* parent);
    ~PlayerLauncher() override;
    // program empty: the system's default player
    void setPlayer(const QString& program, const QString& arguments);
    // Plays the source, or makes its preview and plays that. parent: the window the progress and the messages show over.
    void preview(const PreviewSource& source, QWidget* parent);
    // A preview is being made
    [[nodiscard]] bool busy() const { return m_proc != nullptr; }
    // Removes this session's folder, as far as no player holds a file of it
    void removeFolder();
    // Removes the preview folders that earlier sessions left behind, of windows that have ended
    static void removeOldFolders();
    // The start of a preview of a title of `seconds` with no place given: a tenth of the way in, at most 2 minutes,
    // past the logos the start of a film usually holds. Not further: a cut reads the title from its start.
    static double defaultStart(double seconds);

   private:
    enum class Player
    {
        Other,
        Mpv,
        Vlc
    };
    // The kind of player, by the name of its program
    [[nodiscard]] Player player() const;
    // The player with what to play in place of {file} (or last), and the arguments of a preset after the user's own
    bool launch(const QString& what, const QStringList& presetArguments, QWidget* parent);
    // mpv or VLC opens the title itself, in its angle and from its start: false for any other player or source
    bool launchDirect(const PreviewSource& source, QWidget* parent);
    void startList();
    void startMux();
    void onListFinished(int exitCode, QProcess::ExitStatus status);
    void onMuxFinished(int exitCode, QProcess::ExitStatus status);
    void onOutput(bool error);
    void finish();
    [[nodiscard]] QString folder() const;

    QString m_tsMuxer;
    QString m_program;
    QString m_arguments;
    PreviewSource m_source;
    QPointer<QWidget> m_parent;
    QProcess* m_proc = nullptr;
    MuxForm* m_form = nullptr;
    QString m_output;     // the .ts being made
    QString m_meta;       // its meta file
    QStringList m_lines;  // the listing, while it is read
    QByteArray m_pending[2];
    bool m_stopped = false;
    int m_count = 0;  // the previews of this session, for the names of their files
};

#endif  // PLAYER_LAUNCHER_H_
