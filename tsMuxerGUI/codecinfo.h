// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
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
    // The delay the listing gave. Where it is not 0, a delay set to 0 in the window is written as timeshift=0ms:
    // without a timeshift the track kept the start its file gives, so 0 in the window did not mean 0.
    int listedDelay = 0;
    int subTrack = 0;
    bool dtsDownconvert = false;
    // For a disc TrueHD track (A_AC3 reported as TRUE-HD, which carries an AC-3 core beside the
    // lossless part): adds drop-ac3-core to the meta line, leaving the core out. The opposite of
    // dtsDownconvert, which keeps the core and drops the lossless part, so only one can apply.
    bool dropAc3Core = false;
    // For the same track, on a demux only: adds split-ac3-core, which writes the pair, the
    // lossless part and the core as three files in one pass instead of three runs.
    bool splitAc3Core = false;

    // On a demux only, and only for LPCM: write the audio as Sony Wave64 instead of RIFF
    // WAVE, which has no 4 GiB ceiling so a long track stays one file.
    bool w64 = false;
    bool isSecondary = false;
    // For A_MLP (TrueHD) only: when set (>0), adds merge-ac3-track=<n> to the meta line
    // to interleave a separate AC-3 compatibility track from the same MKV.
    int mergeAc3Track = 0;
    // True while the merge above is the window's own choice: a TrueHD track of a Matroska file with
    // what looks like its AC-3 core on the next track, which is how tsMuxeR writes the pair, is merged
    // on a disc output when the user answered "As the core", and left apart on any other output. A
    // number set or cleared by hand ends it.
    bool mergeAc3Auto = true;
    // The answer to the question whether that AC-3 track is the core: 0 not asked yet, 1 as the core,
    // 2 as a separate track. Asked once per pair, not again when the output changes.
    int coreAnswer = 0;
    // The source is a Matroska file tsMuxeR wrote (its WritingApp), where such a pair is the core
    bool srcByTsMuxer = false;
    // For A_MLP (TrueHD) only: when non-empty, adds merge-ac3-file="path" to the meta line
    // to interleave an external AC-3 elementary stream with a standalone TrueHD file.
    QString mergeAc3File;
    int offsetId = -1;
    int maxPgOffsets = 0;
    QList<QString> fileList;
    // A title of a DVD folder, drive or image, by the disc's own number: fileList holds the disc, and the meta line
    // names the title with dvd-title=
    int dvdTitle = 0;
    bool checkFPS = false;
    bool checkLevel = false;
    int addSEIMethod = 1;
    bool addSPS = true;
    bool hdr10Override = false;  // hdr10-override: an SDR HEVC track marked as HDR10 on a UHD disc
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
    // For a Matroska output: the track's name (track-name=) and its six flags (forced, hearing-impaired,
    // visual-impaired, text-descriptions, original, commentary, in that order). The src values are what the source
    // said, which tsMuxeR keeps by itself, so the meta carries only what was changed.
    QString trackName;
    QString srcTrackName;
    // The language tsMuxeR listed for the source track. A Matroska output left without lang= keeps the source's own
    // language, its BCP 47 tag included, which a three letter lang= would replace.
    QString srcLang;
    bool mkvFlags[6] = {};
    bool srcMkvFlags[6] = {};
};

#endif
