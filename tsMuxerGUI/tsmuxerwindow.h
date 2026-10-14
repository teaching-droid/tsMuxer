#ifndef TSMUXER_H_
#define TSMUXER_H_

#include <QHeaderView>
#include <QProcess>
#include <QTimer>
#include <QTranslator>
#include <QWidget>

#include <functional>
#include <vector>

#if QT_MULTIMEDIA_LIB
#include <QSoundEffect>
#endif
#ifdef Q_OS_WIN
#include <QByteArray>
#endif

#include "codecinfo.h"

class QTableWidgetItem;
class QComboBox;
class QLabel;
class QCheckBox;

class MuxForm;
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
    void trackLVItemSelectionChanged();
    void inputFilesLVChanged();
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
    void onSavedParamChanged();
    void onFontParamsChanged();
    void onRemoveBtnClick();
    void onAppendButtonClick();
    void onRemoveTrackButtonClick();
    void onRemoveAllTracksButtonClick();
    void updateRemoveAllEnabled();
    void onTrackContextMenu(const QPoint& pos);
    void setChecksMatching(int refRow, bool byLanguage, bool checked);
    void onMoveUpButtonCLick();
    void onMoveDownButtonCLick();
    void RadioButtonMuxClick();
    void outFileNameChanged();
    void saveFileDialog();
    void startMuxing();
    void saveMetaFileBtnClick();
    void onResetMetaBtnClick();
    void openMetaFileBtnClick();
    void onMetaTextChanged();
    void continueAppendFile();
    void continueAddFile();
    void onTsMuxerCodecInfoReceived();
    void addFile();
    void appendFile();
    void onOpacityTimer();
    void updateMetaLines();
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

    bool checkFileDuplicate(const QString& fileName);
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
    QString getSrtParams();
    int findLangByCode(const QString& code);
    bool isLangCodeForMeta(const QString& code);
    void showMkvTrackOptions(const QtvCodecInfo* codecInfo);
    void updateMkvTrackOptionsEnabled();
    void updateHdr10OverrideEnabled(const QtvCodecInfo* codecInfo);
    void updateMkvOutputOptions();
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
    void delTracksByFileName(const QString& fileName);
    void deleteTrack(int idx);
    void updateNum();
    void modifyOutFileName(const QString fileName);
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
    // The step that takes the next file of addFileList (addFile or appendFile), so that a file that is refused, or
    // already in the list, does not end the adding of the files chosen after it.
    void (TsMuxerWindow::*m_addNextFile)() = nullptr;
    QList<QtvCodecInfo> codecList;
    ChapterList chapters;
    double fileDuration;
    // The listing's "Video start" and "Video end": where a join puts the file after this one. 0 when not listed.
    double videoStart = 0.0;
    double videoEnd = 0.0;
    bool runInMuxMode;
    QString lastInputDir;
    QString lastOutputDir;
    // Retranslation hooks for hand-built (non-designer) widgets; run from changeEvent(LanguageChange).
    std::vector<std::function<void()>> m_retranslateHooks;
    QList<QUrl> addFileList;
    QTimer opacityTimer;
    bool m_updateMeta;
    bool m_3dMode;
    QLabel* m_mux3dLabel = nullptr;  // says that a 3D (MVC) track gives the disc its .ssif files
    bool m_fileDialogOpen;
    /// When true, custom chapter text was edited by the user; do not overwrite from input file marks.
    bool m_customChaptersUserOverride;
    /// When true, meta file was edited by the user; do not auto-regenerate.
    bool m_metaUserOverride;
    QnCheckBoxedHeaderView* m_header;
    QString lastSourceDir;

    QTranslator qtCoreTranslator;
    QTranslator tsMuxerTranslator;

    // A chapter file dropped on the chapter list is loaded into it rather than added as a track.
    bool loadChapterFileIntoBox(const QString& fileName);
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;

#if QT_MULTIMEDIA_LIB
    QSoundEffect sound;
#endif
    void myPlaySound(const QString& fileName);
#ifdef Q_OS_WIN
    // PlaySound reads the wav straight out of this while it plays, and it plays
    // asynchronously, so the bytes have to outlive the call.
    QByteArray m_soundData;
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
