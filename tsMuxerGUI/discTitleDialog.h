#ifndef DISC_TITLE_DIALOG_H_
#define DISC_TITLE_DIALOG_H_

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

class QCheckBox;
class QTreeWidget;

// The titles of a Blu-ray or DVD disc folder, drive or image, read from the listing tsMuxeR prints for it
// (tsMuxeR <disc>, see discTitles.h and dvdTitles.h of the command line).
struct DiscTitleRow
{
    int number = 0;     // its place in the listing (title=<number>, --show-title=<number>)
    QString playlist;   // Blu-ray: the playlist, 00800.mpls
    int discTitle = 0;  // DVD: the disc's own number of the title (dvd-title=)
    double seconds = 0;
    QString length;  // as listed, 1:39:36
    int chapters = 0;
    int clips = 0;      // Blu-ray only
    QString audio;      // the languages, as listed
    QString subtitles;  // DVD only
    QString note;       // "main film", "version of title 1", why a title is left out ...
    bool isMain = false;
    bool encrypted = false;
    int angles = 1;  // of a title with several angles; the dialog shows a row for each (angle=)
};

struct DiscTitleList
{
    bool dvd = false;
    QString video;     // DVD: "MPEG-2 720x576 25 fps 4:3"
    QString mainLine;  // "The disc has 4 main titles: ..."
    QList<DiscTitleRow> titles;
    QStringList leftOut;  // the lines under "Left out:"
};

// Reads the listing; no titles when the output holds none.
DiscTitleList parseDiscTitles(const QList<QString>& output);

// True for a path tsMuxeR lists titles for: a disc image (.iso), or a folder or drive holding BDMV or VIDEO_TS, or that
// folder itself.
bool isDiscSource(const QString& path);

// The name of a disc for an output named after it: the image's or the folder's, the volume label of a drive.
QString discName(const QString& disc);

// The playlist file of a Blu-ray title, also inside an image: the path to add as a source.
QString discPlaylistPath(const QString& disc, const QString& playlist);

// "Choose a title": the titles of the disc, the main titles bold, the first of them chosen. The tick shows the left-out
// and short titles too, for which the disc is listed again: exec() then ends with ShowOthersChanged.
class DiscTitleDialog : public QDialog
{
   public:
    enum
    {
        ShowOthersChanged = 2
    };
    DiscTitleDialog(const QString& disc, const DiscTitleList& list, bool othersShown, QWidget* parent);
    [[nodiscard]] int chosen() const;       // an index into list.titles, -1 for none
    [[nodiscard]] int chosenAngle() const;  // the angle of the row chosen, from 1

   private:
    QTreeWidget* m_tree = nullptr;
    QCheckBox* m_showOthers = nullptr;
};

#endif  // DISC_TITLE_DIALOG_H_
