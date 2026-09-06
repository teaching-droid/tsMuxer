#include <QApplication>
#include <QFileInfo>
#include <QUrl>

#include "tsmuxerwindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName("Network Optix");
    app.setApplicationName("tsMuxeR");
    TsMuxerWindow win;
    win.show();
    QList<QUrl> files;
    for (int i = 1; i < argc; ++i)
    {
        const QString arg = QString::fromUtf8(argv[i]);
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
