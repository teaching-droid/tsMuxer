#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QUrl>

#include "systemcolours.h"
#include "tsmuxerwindow.h"

// The window kept its settings under the name of the company the original program came from: on Windows the registry
// key HKEY_CURRENT_USER/Software/Network Optix/tsMuxeR. They are kept under tsMuxeR now. On the first start under the
// new name the old settings are copied over once, so nothing chosen before is lost; the old place is left as it is,
// so an older version still finds its own.
static void copySettingsFromOldName()
{
    // With a tsMuxerGUI.ini the registry is neither read for this nor written: the window keeps everything in the ini.
    if (QFile::exists(tsMuxerGuiIniName()))
        return;
    QSettings current;
    if (!current.allKeys().isEmpty())
        return;  // already in use under the new name
    QSettings old(QStringLiteral("Network Optix"), QStringLiteral("tsMuxeR"));
    const QStringList keys = old.allKeys();
    for (const QString& key : keys) current.setValue(key, old.value(key));
    current.sync();
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName("tsMuxeR");
    app.setApplicationName("tsMuxeR");
    copySettingsFromOldName();
    // Before any window exists. On Windows 7 Qt paints selections black; see systemcolours.cpp.
    useSystemColoursWhereQtHasNone();
    TsMuxerWindow win;
    win.show();
    QList<QUrl> files;
    // The names come from QApplication: on Windows argv is in the ANSI code page, so a file opened
    // with "Open with" or "Send to" whose name has a character outside it (even a German umlaut was read wrongly
    // as UTF-8) was refused as a file that does not exist. Dropping the same file worked.
    const QStringList args = QApplication::arguments();
    for (int i = 1; i < args.size(); ++i)
    {
        const QString& arg = args.at(i);
        // Every argument became a file name, so "tsMuxerGUI --version" opened a dialog saying it
        // could not recognise the format of a file called --version. Anything starting with a
        // dash that is not actually a file on disk is an option and is not a track. Checking the
        // disk as well keeps a real file whose name begins with a dash working.
        if (arg.startsWith(QLatin1Char('-')) && !QFileInfo::exists(arg))
            continue;
        files << QUrl::fromLocalFile(arg);
    }
    if (!files.isEmpty())
        win.addFiles(files);
    return app.exec();
}
