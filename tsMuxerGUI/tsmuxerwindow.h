// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef TSMUXER_H_
#define TSMUXER_H_

#include <QHeaderView>
#include <QLineEdit>
#include <QProcess>
#include <QSet>
#include <QTimer>
#include <QTranslator>
#include <QWidget>

#include <functional>
#include <vector>

#ifdef Q_OS_WIN
#include <QByteArray>
#else
#include <QTemporaryFile>

#include <map>
#include <memory>
#endif

#include "codecinfo.h"

class QTableWidgetItem;
class QListWidgetItem;
class QComboBox;
class QLabel;
class QCheckBox;
class QPushButton;

class MuxForm;
class PlayerLauncher;
namespace Ui
{
class TsMuxerWindow;
}

class QnCheckBoxedHeaderView;
class FontSettingsTableModel;
class LangCodesModel;
class QSettings;

typedef QList<double> ChapterList;

// The tsMuxerGUI.ini that makes the window portable: in the program's own folder. While it exists every setting is kept
// in it and the registry is not written at all.
QString tsMuxerGuiIniName();

enum MplsType
{
    MPLS_NONE,
    MPLS_PRIMARY,
    MPLS_M2TS,
};

class TsMuxerWindow : public QWidget
{
    Q_OBJECT
   public:
    TsMuxerWindow();
    ~TsMuxerWindow() override;

    void addFiles(const QList<QUrl>& files);
   signals:
    void tsMuxerSuccessFinished();
    void codecListReady();
    void fileAdded();
    void fileAppended();

   private:
    template <typename F>
    void showAddFilesDialog(QString&& windowTitle, F&& windowOkFn);
    void onAddBtnClick();
    void readFromStdout();
    void readFromStderr();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void showProbeRefusal();
    void setProbeBusy(bool busy);
    void trackLVItemSelectionChanged();
    void inputFilesLVChanged();
    void noteRunOutput(const QString& output, bool discFolder);
    QString removeUnfinishedOutput();
    void onMuxStopped();
    void updateMuxButton();
    void trackLVItemChanged(QTableWidgetItem* item);
    void onVideoCheckBoxChanged();
    void onPulldownCheckBoxChanged();
    void onVideoComboBoxChanged(int index);
    void onAudioSubtitlesParamsChanged();
    void onMkvTrackOptionsChanged();
    void onMergeAc3FileBrowseClicked();
    void onEditDelayChanged(int i);
    void onFontBtnClicked();
    void onColorBtnClicked();
    void onGeneralCheckboxClicked();
    void onGeneralSpinboxValueChanged();
    void onChapterParamsChanged();
    void onSplitCutParamsChanged();
    void checkCustomChapters();
    void checkCut();
    void onSavedParamChanged();
    void onFontParamsChanged();
    void onPlaylistPartsChanged();
    void onRemoveBtnClick();
    void onMakeSample();
    // "Make a sample...": enabled for the chosen input that a sample can be made of, and its tooltip
    void updateSampleButton();
    void onAppendButtonClick();
    void onRemoveTrackButtonClick();
    void onRemoveAllTracksButtonClick();
    void updateRemoveAllEnabled();
    void onTrackContextMenu(const QPoint& pos);
    // "Play" on the input list: the item in the player for previews (playerLauncher.h)
    void onInputContextMenu(const QPoint& pos);
    void playInputItem(const QListWidgetItem* itm);
    // "Preview with its delay" on the track list: the track with the video, as the meta file muxes them
    void previewTrackDelay(int row);
    // The launcher with the player of the General tab as it is set now
    PlayerLauncher* previewPlayer();
    void setChecksMatching(int refRow, bool byLanguage, bool checked);
    void onMoveUpButtonCLick();
    void onMoveDownButtonCLick();
    void RadioButtonMuxClick();
    void outFileNameChanged();
    void saveFileDialog();
    void startMuxing();
    void saveMetaFileBtnClick();
    void onResetMetaBtnClick();
    void updateMetaState();
    QString buildMetaText();
    void openMetaFileBtnClick();
    void onMetaTextChanged();
    void continueAppendFile();
    void continueAddFile();
    void onTsMuxerCodecInfoReceived();
    void listDiscTitles();
    void updateOrderColumns();
    void onDiscTitlesReceived();
    void addFile();
    void appendFile();
    void onOpacityTimer();
    void updateMetaLines();
    void updateSizeEstimate();
    void at_sectionCheckstateChanged(Qt::CheckState state);
    void updateMuxTime1();
    void updateMuxTime2();
    void snapMuxTimeToBdMinimum();
    void onLanguageComboBoxIndexChanged(int);
    template <typename OnCodecListReadyFn, typename PostActionSignal, typename PostActionFn>
    void processAddFileList(OnCodecListReadyFn onCodecListReady, PostActionSignal postActionSignal,
                            PostActionFn postActionFn);

   protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    QString customChaptersProblem() const;
    QString cutProblem() const;
    void updateMaxOffsets();
    void updateMux3dLabel();
    void updateCustomChapters();

    void writeSettings();
    bool readSettings();
    bool readSettingsBody();
    [[nodiscard]] QString currentOutputMode() const;
    [[nodiscard]] QString currentSaveDialogFilter() const;
    void setOutputMode(const QString& mode);
    bool readGeneralSettings(const QString& prefix);
    QString getOutputDir() const;

   private:
    struct MPLSFileInfo
    {
        MPLSFileInfo(const QString& _name, double _duration) : name(_name), duration(_duration) {}
        MPLSFileInfo() : duration(0) {}
        QString name;
        double duration;
    };

    bool checkFileDuplicate(const QString& fileName, int dvdTitle = 0);
    void tsMuxerExecute(const QStringList& args);
    void addLines(const QByteArray& arr, QList<QString>& outList, bool isError);
    void flushPendingOutput();
    void doAppendInt(const QString& fileName, const QString& parentFileName, double duration, bool doublePrefix,
                     MplsType mplsRole);
    bool isDiskOutput() const;
    QString getMuxOpts();
    QString getFileList(QtvCodecInfo* codecInfo);
    QString getAudioMetaInfo(QtvCodecInfo* codecInfo);
    QString getVideoMetaInfo(QtvCodecInfo* codecInfo);
    QString getFileList();
    QString getSrtParams(bool asText);
    int findLangByCode(const QString& code);
    bool isLangCodeForMeta(const QString& code);
    void showMkvTrackOptions(const QtvCodecInfo* codecInfo);
    void updateMkvTrackOptionsEnabled();
    void updateHdr10OverrideEnabled(const QtvCodecInfo* codecInfo);
    QString saveSrtFileName(const QtvCodecInfo* codecInfo, int row) const;
    void updateMkvOutputOptions();
    void updateDiscMetaOptions();
    QString langForTrack(const QComboBox* box, const QString& current);
    void setComboBoxText(QComboBox* comboBox, const QString& text);
    QtvCodecInfo* getCodecInfo(int idx);
    // Show the Dolby Vision profile row only for Matroska output of a dual layer source.
    void updateDvProfileVisible();
    QtvCodecInfo* getCurrentCodec();
    // merge-ac3-track and merge-ac3-file are mutually exclusive; grey out whichever does not apply.
    void updateMergeAc3Exclusion(QtvCodecInfo* codecInfo);
    [[nodiscard]] int mergeAc3TrackIdForRow(int row) const;
    [[nodiscard]] bool isRowMergedIntoTrueHd(int row) const;
    void updateMergeAc3Suffix();
    void followMergeRows(int from, int to);
    // The row (from 1) of the AC-3 track that is the core of the TrueHD track on this row, 0 for none
    [[nodiscard]] int coreRowOfTrueHd(int row) const;
    // Sets or clears the merge of every TrueHD row whose merge the window chooses, for the output chosen
    void updateAutoCoreMerge();
    // Asks whether the AC-3 track on coreRow (from 1) goes on the disc as the core of the TrueHD track on thdRow
    bool askCoreOfTrueHd(int thdRow, int coreRow);
    // At Start muxing, for each ticked TrueHD track without an AC-3 core on a disc output: what to do about it. False
    // when the user cancels. A core to make goes onto the meta line now and is made by makeAc3Cores, after the other
    // questions of Start muxing, so that a No to one of them has not cost the time of an encode.
    bool offerAc3Cores();
    // ** SOUND A BLU-RAY DOES NOT CARRY. ** A track of such a kind that can be converted for a disc: AAC, MPEG audio,
    // Opus, Vorbis, FLAC, ALAC, WavPack, or LPCM at a rate a disc has not, of one file (not a join, a playlist or a
    // disc title, which ffmpeg cannot read as one file)
    [[nodiscard]] bool convertibleForDisc(const QtvCodecInfo* info) const;
    // The words ", convert=..., stereo=..." of the track's meta line on a disc output: its own answer, else the
    // General tab's setting; empty where the track stays as it is
    [[nodiscard]] QString convertWords(const QtvCodecInfo* info) const;
    // The track is converted, or still to be asked about ("Ask me"): it is not shown as left out of a disc
    [[nodiscard]] bool mayBeConverted(const QtvCodecInfo* info) const;
    // At Start muxing with "Ask me": the box for the ticked tracks not yet answered. False when the user cancels.
    bool askAudioConversion();
    // The General tab's setting changed: the earlier answers of the box give way to it
    void onConvertSettingChanged();
    void updateConvertSettingEnabled();
    bool makeAc3Cores();
    void dropPendingCores();
    // The cores made for the job that ended well, or for a window that closes: they were only there for the mux
    void removeMadeCores();
    void showMergeOfRow(int row);
    [[nodiscard]] double durationOfSource(const QString& source) const;
    // Splitting is produced by the transport stream writer, so not every output can do it.
    [[nodiscard]] bool outputCanSplit() const;
    // Grey the Blu-ray tab's controls that the chosen output does not use.
    void updateBluRayTabEnabled();
    // Everything that follows from the chosen output except its name. A click on an output button
    // and a typed name whose extension chooses the output both come through here.
    // At start, with resetAudioPes false, the remembered new audio PES tick is kept.
    void applyOutputMode(bool resetAudioPes = true);
    // The mux button and the name label say what the chosen output does; kept through a language change
    void updateOutputModeTexts();
    // An SRT track is copied as text only into Matroska or by a demux, and only when asked for
    [[nodiscard]] bool keepTextApplies() const;
    void updateKeepTextControls();
    void delTracksByFileName(const QString& fileName, int dvdTitle = 0);
    void deleteTrack(int idx);
    void updateNum();
    void modifyOutFileName(const QString fileName, const QString& baseName = QString());
    QString m_listedDiscTitle;  // the film's name the listing of the file being added gave
    void moveRow(int index, int index2);
    bool saveMetaFile(const QString& metaName);
    void updateBtns(QWidget* w);
    double getRendererAnimationTime() const;
    void setRendererAnimationTime(double value);
    void setTextItemColor(QString str);
    QString getDefaultOutputFileName() const;
    void updateCurrentColor(int dr, int dg, int db, int rowIndex);
    void colorizeCurrentRow(const QtvCodecInfo* codecInfo, int rowIndex = -1);
    void colorizeAllRows();
    /// Returns true if the given audio codec is incompatible with the
    /// currently selected output format (e.g. FLAC in TS, Opus in M2TS).
    bool isCodecIncompatibleWithFormat(const QString& programName) const;

    void addTrackToDefaultComboBox(int trackRowIdx);
    void removeTrackFromDefaultComboBox(int trackRowIdx);
    void removeTrackFromDefaultComboBox(QComboBox*, QCheckBox*, int comboBoxIdx, int trackRowIdx);
    void updateTracksComboBox(QComboBox*);
    void moveTrackInDefaultComboBox(int oldIndex, int newIndex);
    void postMoveComboBoxUpdate(QComboBox*, const QVariant& preMoveIndex, int oldIndex, int newIndex);
    void setUiMetaItemsData();

    QString metaName;
    // The output of the job that runs, as it was before the start: what an Abort may remove (removeUnfinishedOutput)
    QString m_runOutput;
    bool m_runOutputExisted = false;
    bool m_runDiscFolder = false;
    QStringList m_runDirBefore;       // the entries of the folder that holds the output
    QStringList m_runOutputBefore;    // the entries of an output folder that was there
    QStringList m_runMissingFolders;  // the folders above the output that were not there, the deepest first
    qint64 m_runPid = 0;
    Ui::TsMuxerWindow* ui;
    int disableUpdatesCnt;
    bool processFinished;
    int processExitCode;
    QProcess::ExitStatus processExitStatus;
    QProcess::ProcessError processError;
    QList<MPLSFileInfo> mplsFileList;
    QProcess proc;
    QList<QString> procStdOutput;
    QList<QString> procErrOutput;
    QByteArray m_pendingStdOut;  // the end of the last read, when the read stopped inside a line
    QByteArray m_pendingStdErr;
    bool outFileNameModified;
    QString oldFileName;
    bool outFileNameDisableChange;
    MuxForm* muxForm;
    QString newFileName;
    // A disc folder, drive or image being added: its titles are listed, the left-out ones too when m_discOthersShown,
    // and the title chosen is added (listDiscTitles). A DVD title is added by its disc number, m_pendingDvdTitle, and
    // its length, m_pendingDvdSeconds, while its tracks are listed; 0 for any other source.
    QString m_discSource;
    QPushButton* m_addDiscBtn = nullptr;  // "add disc", beside "add" and enabled with it
    // The queue of jobs (tsMuxeR --batch): "Add to queue" keeps the job the window shows, as its meta file and a line
    // of the job list in the queue folder, and "Queue" shows the list and runs it. The folder is on disk, so the queue
    // is still there after the window was closed.
    // "Languages to take" on the General tab: the sound and subtitle tracks of other languages are added unticked
    QLineEdit* m_languagesEdit = nullptr;
    QLabel* m_sizeLabel = nullptr;  // "Estimated size: about 23.4 GB", in the Output box
    [[nodiscard]] QStringList takeLanguages() const;
    [[nodiscard]] bool languageWanted(const QtvCodecInfo& info, const QStringList& wanted) const;
    // "Make the second Dolby Vision layer" on the Blu-ray tab: unticked, --no-dv-layer on the MUXOPT line
    QCheckBox* m_dvLayerCheck = nullptr;
    // "Sound a Blu-ray does not carry" on the General tab: Ask me, AC-3, LPCM, Leave it as it is; the channels; the
    // AC-3 rate of a stereo track and of a track of more channels; the LPCM bits. Index 0 of each is the start value.
    QComboBox* m_convertAudioCombo = nullptr;
    QComboBox* m_convertChannelsCombo = nullptr;
    QComboBox* m_convertRateStereoCombo = nullptr;
    QComboBox* m_convertRateMultiCombo = nullptr;
    QComboBox* m_convertBitsCombo = nullptr;
    bool m_convertSetByBox = false;  // the box sets the setting ("Remember my choice"): the answers stay
    QPushButton* m_queueAddBtn = nullptr;
    QPushButton* m_queueBtn = nullptr;
    bool m_queueStopOnError = false;  // "Stop at the first job that fails" in the queue: --batch-stop-on-error
    bool m_addToQueue = false;  // startMuxing was asked for the queue: it stops after the checks and the meta file
    // "Scan": the job is run with --scan, which writes nothing and tells the size of the output
    QPushButton* m_scanBtn = nullptr;
    // What tsMuxeR printed is its list of the discs of a folder ("... holds 3 discs ..."): the box that says so is
    // shown, and true returned
    bool folderOfDiscsBox(const QString& folder);
    // the text of the meta box as it stands, a no-break space kept (see its definition)
    [[nodiscard]] QString metaBoxText() const;
    // The text subtitles are kept as text and go in drawn as well, as a second track of each (the tick under "Keep
    // SRT subtitles as text"): the line of each text track is then written twice
    [[nodiscard]] bool alsoDrawnApplies() const;
    bool m_scanRun = false;         // startMuxing was asked for a scan
    bool m_askNonDiscCodec = true;  // the question about a codec a Blu-ray does not have; off until the window closes
    bool m_scanRunning = false;     // the run in the progress window is a scan
    bool m_queueRun = false;        // the run in the progress window is the queue
    QSet<int> m_queueDone;          // the jobs of that run that ended well, counted from 1
    [[nodiscard]] QString queueDir() const;
    [[nodiscard]] QStringList queueLines() const;
    void writeQueueLines(const QStringList& lines);
    void removeQueueMeta(const QString& line) const;
    void updateQueueButton();
    void showQueue();
    void startQueue();
    bool m_discOthersShown = false;
    int m_pendingDvdTitle = 0;
    int m_pendingAngle = 1;  // the angle chosen with the title, while its tracks are listed
    // Blu-ray, another angle than the first: for each part of several angles, where it starts in the title (in
    // seconds, as the disc listing gives it), the first angle's clip there and this angle's, which takes its place
    struct PendingAngleClip
    {
        double start = 0;
        QString first;
        QString clip;
    };
    QList<PendingAngleClip> m_pendingAngleClips;
    double m_pendingDvdSeconds = 0.0;
    // The step that takes the next file of addFileList (addFile or appendFile), so that a file that is refused, or
    // already in the list, does not end the adding of the files chosen after it.
    void (TsMuxerWindow::*m_addNextFile)() = nullptr;
    QList<QtvCodecInfo> codecList;
    ChapterList chapters;
    double fileDuration;
    // The "Writing application:" line of the listing of the file being added (a Matroska file's WritingApp)
    QString listedWritingApp;
    // The listing's "Video start" and "Video end": where a join puts the file after this one. 0 when not listed.
    double videoStart = 0.0;
    double videoEnd = 0.0;
    bool runInMuxMode;
    // The busy cursor of a track listing is shown (setProbeBusy).
    bool m_probeBusy = false;
    QString lastInputDir;
    QString lastOutputDir;
    // Retranslation hooks for hand-built (non-designer) widgets; run from changeEvent(LanguageChange).
    std::vector<std::function<void()>> m_retranslateHooks;
    QList<QUrl> addFileList;
    QTimer opacityTimer;
    bool m_updateMeta;
    bool m_3dMode;
    int m_maxPgOffsets = 0;          // the most planes (3d-pg-planes) an MVC track of the list has
    QLabel* m_mux3dLabel = nullptr;  // says that a 3D (MVC) track gives the disc its .ssif files
    bool m_fileDialogOpen;
    /// When true, custom chapter text was edited by the user; do not overwrite from input file marks.
    bool m_customChaptersUserOverride;
    /// When true, meta file was edited by the user; do not auto-regenerate.
    bool m_metaUserOverride;
    bool m_buildingMuxOpts = false;  // getMuxOpts asks buildMetaText, which asks getMuxOpts: once only
    struct PendingCore
    {
        QtvCodecInfo* info;
        QString encoder;
        QString map;
        int channels;
    };
    QList<PendingCore> m_pendingCores;
    QStringList m_madeCores;
    QString m_coreEncoderPath;  // the program chosen by the user, kept in the settings
    QString m_lastBuiltMeta;    // the meta file last built from the settings
    QnCheckBoxedHeaderView* m_header;
    PlayerLauncher* m_player = nullptr;  // made at the first preview (previewPlayer)
    QString lastSourceDir;

    QTranslator qtCoreTranslator;
    QTranslator tsMuxerTranslator;

    // A chapter file dropped on the chapter list is loaded into it rather than added as a track.
    bool loadChapterFileIntoBox(const QString& fileName);
    // "Load..." and "Save..." under the chapter list
    void loadChapterFileByDialog();
    void saveChapterFileByDialog();
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;

    void myPlaySound(const QString& fileName);
#ifdef Q_OS_WIN
    // PlaySound reads the wav straight out of this while it plays, and it plays
    // asynchronously, so the bytes have to outlive the call.
    QByteArray m_soundData;
#else
    // The wav of each sound, written out once for the system's player, which reads a file and
    // plays on after the call. Removed when the window closes.
    std::map<QString, std::unique_ptr<QTemporaryFile>> m_soundFiles;
#endif
    bool isVideoCropped();

    FontSettingsTableModel* fontSettingsModel = nullptr;
    LangCodesModel* langCodesModel;

    // The program choices of the window that are not job content (see CHOICE_WIDGETS in the .cpp). They are saved only
    // after they have been read back, or the first save, which runs while the window is still being built, would
    // overwrite the stored choices with the defaults.
    void saveChoices();
    void restoreChoices(QSettings& from);
    bool m_choicesRestored = false;
    // Set while an ini that is still empty takes over the registry's settings: the registry is then read, never
    // written.
    QSettings* m_seedSettings = nullptr;
    // The size and position are only worth saving once the window has been shown: before that, pos() is (0,0).
    bool m_wasShown = false;
};

Q_DECLARE_METATYPE(ChapterList);

#endif
