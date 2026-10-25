#pragma once
#include "AudioPlayer.h"
#include "PreviewWidget.h"
#include "media/Overview.h"
#include "core/Project.h"
#include "media/FrameServer.h"
#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QTimer>
#include <QUndoStack>

class QCheckBox;
class QFontComboBox;
class QPlainTextEdit;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTabWidget;
class QTreeWidgetItem;
class QVBoxLayout;

namespace ve {

class DiamondButton;
class ProxyManager;
class MediaTree;
class PreviewSurface;
class TimelineWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bool useOpenGl, QWidget* parent = nullptr);
    void openProject(const QString& path);
    const Project& project() const { return m_project; }   // read-only access (tests/automation)
    TransformOverlay* previewOverlay() { return m_preview->transformOverlay(); }
    void seekTo(double t) { setPosition(t, true); }   // jump the playhead (also used by tests/automation)

protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    struct PropRow { QString prop; QDoubleSpinBox* spin; DiamondButton* key; double mult; };
    struct ChoiceRow { QComboBox* box; QString type, name; };
    struct ColourBtn { QPushButton* btn; QString type; };

    void buildUi(bool useOpenGl);
    void buildActions();
    void newProject();
    void openDialog();
    bool save();
    bool saveAs();
    bool maybeSave();
    void importDialog();
    void importFolderDialog();
    void importFiles(const QStringList& files, const QString& folderId);
    void importDirectory(const QString& dir, const QString& parentFolderId);
    void importPaths(const QStringList& paths, const QString& folderId);
    void startAssetJob(MediaAsset seed, bool probe);
    void rebuildMediaList();
    void newFolder();
    void deleteMediaSelection();
    void exportDialog();
    void doSplit();
    void doDelete(bool ripple);
    void doFreeze();
    void selectAll();
    void applyPresetToSelection(const QString& presetId);
    void addLayerAtPlayhead(const Effect& e);
    void startPick(const QString& clipId);
    void addTextAtPlayhead();
    void doCopy();
    void doCut();
    void doPaste();
    void doDuplicate();
    void checkLag();
    // direct manipulation in the preview
    bool clipBox(const Clip& c, TransformBox* b) const;
    void updateTransformBox();
    void onTransformDrag(TransformOverlay::Phase ph, const TransformDrag& d);
    void applyTransform(const QString& id, const QMap<QString, double>& vals);
    void selectAtPoint(const QPointF& projectPt);
    void addAssetAtPlayhead(const QString& assetId);
    void togglePlay();
    void stopPlay();
    void setPosition(double t, bool fromUser = false);
    void requestPreview();
    // inspector
    void updateProps();
    void syncProps();
    double clipLocal(const Clip& c) const;
    double keyTol() const;
    void editProp(const QString& id, const QString& prop, double value);
    void toggleKey(const QString& id, const QString& prop);
    void editClip(const QString& id, const QString& key, const std::function<void(Clip&)>& fn);
    PropRow addPropRow(QVBoxLayout* into, const QString& id, const QString& label, const QString& prop, double lo, double hi, double step,
                       int decimals, double mult, const QString& suffix);
    void updateTitle();
    void applyProjectSettings();
    QSize previewMaxSize() const;
    bool isModified() const { return !m_stack.isClean() || m_recovered || m_extraDirty; }
    QString autosavePath() const;
    void autosave();
    void checkRecovery();

    Project m_project;
    QUndoStack m_stack;
    FrameServer m_frames;
    AudioPlayer m_audio;
    PreviewSurface* m_preview = nullptr;
    TimelineWidget* m_timeline = nullptr;
    MediaTree* m_media = nullptr;
    QTabWidget* m_tabs = nullptr;
    QLabel* m_timecode = nullptr;
    QWidget* m_propsBox = nullptr;
    QVBoxLayout* m_propsLayout = nullptr;
    QPushButton* m_playBtn = nullptr;
    QComboBox* m_quality = nullptr;
    QTimer m_playTimer, m_autosaveTimer;
    QElapsedTimer m_clock;
    QHash<QString, QImage> m_thumbs;
    QSet<QString> m_expanded;
    // inspector state
    QString m_inspSig;
    QVector<PropRow> m_rows;
    QDoubleSpinBox *m_speedSpin = nullptr, *m_fadeInSpin = nullptr, *m_fadeOutSpin = nullptr;
    QCheckBox* m_muteCheck = nullptr;
    QHash<QString, QDoubleSpinBox*> m_fxSpins;   // swoosh length by effect type
    QVector<ChoiceRow> m_choices;
    QVector<ColourBtn> m_colourBtns;
    QCheckBox *m_maskOn = nullptr, *m_maskInv = nullptr;
    double m_pos = 0, m_playFrom = 0;
    bool m_picking = false;
    QVector<Clip> m_clipboard;
    QHash<QString, AssetOverview> m_overview;     // thumbnails + waveform of every asset (drawn inside timeline clips)
    ProxyManager* m_proxy = nullptr;
    bool m_autoProxy = true;
    QHash<QString, QString> m_proxyPaths;          // asset id -> lightweight preview copy
    QSet<QString> m_proxyTried;
    QTimer m_lagTimer;
    int m_delivered = 0, m_slowSecs = 0;           // preview frames delivered this second / consecutive slow seconds
    QPlainTextEdit* m_textEdit = nullptr;
    QFontComboBox* m_fontBox = nullptr;
    QDoubleSpinBox *m_fontSize = nullptr, *m_strokeW = nullptr;
    QCheckBox *m_bold = nullptr, *m_italic = nullptr, *m_underline = nullptr;
    QPushButton *m_fillBtn = nullptr, *m_strokeBtn = nullptr;
    int m_gesture = 0;           // counts mouse gestures in the preview: each one is a single undo step
    double m_gestScale0 = 1;     // the clip's size when the current gesture began
    bool m_playing = false, m_recovered = false, m_extraDirty = false, m_warnedNoAudio = false;
    int m_jobs = 0;
};

} // namespace ve
