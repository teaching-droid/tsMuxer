#ifndef CODECINFO_H
#define CODECINFO_H

#include <QList>
#include <QString>

struct QtvCodecInfo
{
    QtvCodecInfo() = default;
    QtvCodecInfo(const QtvCodecInfo&) = default;

    int trackID = 0;
    int width = 0;
    int height = 0;
    QString displayName;
    QString programName;
    QString descr;
    QString lang;
    int delay = 0;
    int subTrack = 0;
    bool dtsDownconvert = false;
    // For a disc TrueHD track (A_AC3 reported as TRUE-HD, which carries an AC-3 core beside the
    // lossless part): adds drop-ac3-core to the meta line, leaving the core out. The opposite of
    // dtsDownconvert, which keeps the core and drops the lossless part, so only one can apply.
    bool dropAc3Core = false;
    bool isSecondary = false;
    // For A_MLP (TrueHD) only: when set (>0), adds merge-ac3-track=<n> to the meta line
    // to interleave a separate AC-3 compatibility track from the same MKV.
    int mergeAc3Track = 0;
    // For A_MLP (TrueHD) only: when non-empty, adds merge-ac3-file="path" to the meta line
    // to interleave an external AC-3 elementary stream with a standalone TrueHD file.
    QString mergeAc3File;
    int offsetId = -1;
    int maxPgOffsets = 0;
    QList<QString> fileList;
    bool checkFPS = false;
    bool checkLevel = false;
    int addSEIMethod = 1;
    bool addSPS = true;
    int delPulldown = -1;
    QString fpsText;
    QString fpsTextOrig;
    QString levelText;
    // for append
    int nested = 0;
    QtvCodecInfo* parent = nullptr;
    QtvCodecInfo* child = nullptr;
    bool bindFps = true;
    QStringList mplsFiles;
    QString arText;
    bool enabledByDefault = true;
};

#endif
