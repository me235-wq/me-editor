#include "MainWindow.h"
#include "PreviewWidget.h"
#include "TimelineWidget.h"
#include "core/Commands.h"
#include "core/Effects.h"
#include "core/TextRender.h"
#include "core/Util.h"
#include "media/Decoder.h"
#include "media/Exporter.h"
#include "media/Proxy.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QPlainTextEdit>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <functional>
#include <numeric>

namespace ve {

static const char* kAssetMime = "application/x-ve-asset";
static const char* kEffectMime = "application/x-ve-effect";

static bool isMediaFile(const QString& path) {
    static const QSet<QString> ext = {
        "mp4", "m4v", "mov", "mkv", "webm", "avi", "flv", "wmv", "mpg", "mpeg", "mts", "m2ts", "ts", "3gp", "ogv", "gif",   // video
        "mp3", "wav", "aac", "flac", "ogg", "oga", "opus", "m4a", "wma", "aiff", "aif",                                    // audio
        "png", "jpg", "jpeg", "webp", "bmp", "tif", "tiff"};                                                               // images
    return ext.contains(QFileInfo(path).suffix().toLower());
}

// ---------------------------------------------------------------------------------------------------------
// Small widgets
// ---------------------------------------------------------------------------------------------------------

// Keyframe button: ◇ none · ◇(gold outline) the property is animated · ◆ there's a key at the playhead.
class DiamondButton : public QAbstractButton {
public:
    explicit DiamondButton(QWidget* parent = nullptr) : QAbstractButton(parent) {
        setFixedSize(24, 24);
        setCursor(Qt::PointingHandCursor);
        setToolTip("Add or remove a keyframe at the playhead");
        setFocusPolicy(Qt::NoFocus);
    }
    void setState(int s) { if (s != m_state) { m_state = s; update(); } }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF c(width() / 2.0, height() / 2.0);
        const QPolygonF d({QPointF(c.x(), c.y() - 7), QPointF(c.x() + 7, c.y()), QPointF(c.x(), c.y() + 7), QPointF(c.x() - 7, c.y())});
        const QColor gold("#ffd54a");
        if (!isEnabled()) { p.setPen(QPen(QColor("#555559"), 1.2)); p.setBrush(Qt::NoBrush); }
        else if (m_state == 2) { p.setPen(QPen(QColor("#a98400"), 1.2)); p.setBrush(gold); }
        else if (m_state == 1) { p.setPen(QPen(gold, 1.6)); p.setBrush(Qt::NoBrush); }
        else { p.setPen(QPen(underMouse() ? QColor("#c8c8cd") : QColor("#85858c"), 1.4)); p.setBrush(Qt::NoBrush); }
        p.drawPolygon(d);
    }
private:
    int m_state = 0;
};

class EffectList : public QListWidget {
public:
    using QListWidget::QListWidget;
protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        auto* m = new QMimeData;
        if (!items.isEmpty()) m->setData(kEffectMime, items.first()->data(Qt::UserRole).toString().toUtf8());
        return m;
    }
};

// Media panel: folders and files. Drag a file to the timeline, drag files/folders onto folders to organise,
// drop files from your file manager to import.
class MediaTree : public QTreeWidget {
    Q_OBJECT
public:
    explicit MediaTree(QWidget* parent = nullptr) : QTreeWidget(parent) {
        setHeaderHidden(true);
        setIconSize(QSize(64, 36));
        setIndentation(14);
        setSelectionMode(ExtendedSelection);
        setDragEnabled(true);
        setAcceptDrops(true);
        setDragDropMode(DragDrop);
        setDropIndicatorShown(true);
        setEditTriggers(EditKeyPressed);
        setContextMenuPolicy(Qt::CustomContextMenu);
    }
    static QString idOf(const QTreeWidgetItem* it) { return it->data(0, Qt::UserRole).toString(); }
    static bool isFolder(const QTreeWidgetItem* it) { return it->data(0, Qt::UserRole + 1).toString() == "folder"; }
signals:
    void moveRequested(const QStringList& assets, const QStringList& folders, const QString& target);
    void urlsDropped(const QStringList& paths, const QString& target);
protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
        auto* m = new QMimeData;
        for (const QTreeWidgetItem* it : items)
            if (!isFolder(it)) { m->setData(kAssetMime, idOf(it).toUtf8()); break; }
        return m;
    }
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData()->hasUrls() || e->source() == this) e->acceptProposedAction(); else e->ignore();
    }
    void dragMoveEvent(QDragMoveEvent* e) override {
        if (e->mimeData()->hasUrls() || e->source() == this) { QTreeWidget::dragMoveEvent(e); e->acceptProposedAction(); } else e->ignore();
    }
    void dropEvent(QDropEvent* e) override {
        const QTreeWidgetItem* it = itemAt(e->position().toPoint());
        const QString target = it ? (isFolder(it) ? idOf(it) : (it->parent() ? idOf(it->parent()) : QString())) : QString();
        if (e->mimeData()->hasUrls()) {
            QStringList paths;
            for (const QUrl& u : e->mimeData()->urls()) if (u.isLocalFile()) paths << u.toLocalFile();
            emit urlsDropped(paths, target);
            e->acceptProposedAction();
        } else if (e->source() == this) {
            QStringList assets, folders;
            for (const QTreeWidgetItem* s : selectedItems()) (isFolder(s) ? folders : assets) << idOf(s);
            emit moveRequested(assets, folders, target);
            e->acceptProposedAction();
        }
    }
};

struct AssetJob {
    MediaAsset asset;
    AssetOverview overview;
    QImage thumb;
    bool ok = false, probed = false;
    QString err;
};

// ---------------------------------------------------------------------------------------------------------
// Project settings: aspect ratio presets (Shorts, YouTube, ...), custom ratio, resolution, frame rate
// ---------------------------------------------------------------------------------------------------------

struct RatioPreset { const char* label; int w, h; };
static const RatioPreset kRatios[] = {
    {"YouTube / landscape  (16:9)", 16, 9}, {"Shorts / Reels / TikTok  (9:16)", 9, 16}, {"Square  (1:1)", 1, 1},
    {"Portrait  (4:5)", 4, 5}, {"Classic  (4:3)", 4, 3}, {"Cinema  (21:9)", 21, 9},
};
struct ResPreset { const char* label; int side; };
static const ResPreset kRes[] = {{"480p", 480}, {"720p", 720}, {"1080p  (Full HD)", 1080}, {"1440p  (2K)", 1440}, {"2160p  (4K)", 2160}};

static bool askSettings(QWidget* parent, ProjectSettings* s, const QString& title) {
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    auto* form = new QFormLayout(&dlg);

    const int g = std::gcd(s->width, s->height);
    const int rw = s->width / std::max(1, g), rh = s->height / std::max(1, g);
    auto* ratio = new QComboBox;
    int ratioIdx = int(std::size(kRatios));                       // default: custom
    for (int i = 0; i < int(std::size(kRatios)); ++i) {
        ratio->addItem(kRatios[i].label);
        if (kRatios[i].w * s->height == kRatios[i].h * s->width) ratioIdx = i;
    }
    ratio->addItem("Custom…");
    ratio->setCurrentIndex(ratioIdx);
    auto* cw = new QSpinBox, *ch = new QSpinBox;
    for (QSpinBox* b : {cw, ch}) b->setRange(1, 1000);
    cw->setValue(rw <= 1000 ? rw : 16); ch->setValue(rh <= 1000 ? rh : 9);
    auto* customRow = new QWidget;
    auto* cl = new QHBoxLayout(customRow);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->addWidget(cw); cl->addWidget(new QLabel(":")); cl->addWidget(ch); cl->addStretch(1);

    auto* res = new QComboBox;
    const int side = std::min(s->width, s->height);
    int resIdx = -1;
    for (int i = 0; i < int(std::size(kRes)); ++i) { res->addItem(kRes[i].label, kRes[i].side); if (kRes[i].side == side) resIdx = i; }
    if (resIdx < 0) { res->addItem(QString("Current  (%1p)").arg(side), side); resIdx = res->count() - 1; }
    res->setCurrentIndex(resIdx);

    auto* fps = new QComboBox;
    for (double r : {24.0, 25.0, 30.0, 50.0, 60.0}) { fps->addItem(QString("%1 fps").arg(r), r); if (r == s->fps) fps->setCurrentIndex(fps->count() - 1); }
    auto* sizeLabel = new QLabel;

    auto size = [&](int* w, int* h) {
        const int i = ratio->currentIndex();
        const int a = i < int(std::size(kRatios)) ? kRatios[i].w : cw->value(), b = i < int(std::size(kRatios)) ? kRatios[i].h : ch->value();
        sizeForRatio(a, b, res->currentData().toInt(), w, h);
    };
    auto refresh = [&] {
        customRow->setEnabled(ratio->currentIndex() >= int(std::size(kRatios)));
        int w, h; size(&w, &h);
        sizeLabel->setText(QString("<b>%1 × %2</b> pixels").arg(w).arg(h));
    };
    QObject::connect(ratio, &QComboBox::currentIndexChanged, &dlg, refresh);
    QObject::connect(res, &QComboBox::currentIndexChanged, &dlg, refresh);
    QObject::connect(cw, &QSpinBox::valueChanged, &dlg, refresh);
    QObject::connect(ch, &QSpinBox::valueChanged, &dlg, refresh);
    form->addRow("Aspect ratio", ratio);
    form->addRow("Custom ratio", customRow);
    form->addRow("Resolution", res);
    form->addRow("Frame rate", fps);
    form->addRow("Canvas", sizeLabel);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    refresh();
    if (dlg.exec() != QDialog::Accepted) return false;
    size(&s->width, &s->height);
    s->fps = fps->currentData().toDouble();
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------------------------

MainWindow::MainWindow(bool useOpenGl, QWidget* parent) : QMainWindow(parent) {
    setAcceptDrops(true);
    resize(1440, 880);
    buildUi(useOpenGl);
    buildActions();

    connect(&m_frames, &FrameServer::framesReady, this, [this](const QVector<RenderLayer>& layers) { if (m_playing) ++m_delivered; m_preview->setLayers(layers); });
    connect(&m_project, &Project::changed, this, [this] {
        requestPreview();
        updateProps();
        if (m_playing) m_audio.updateProject(m_project);
    });
    connect(&m_project, &Project::assetsChanged, this, &MainWindow::rebuildMediaList);
    connect(&m_project, &Project::settingsChanged, this, &MainWindow::applyProjectSettings);
    connect(&m_stack, &QUndoStack::cleanChanged, this, &MainWindow::updateTitle);
    connect(&m_audio, &AudioPlayer::message, this, [this](const QString& m) { statusBar()->showMessage(m, 12000); });
    connect(&m_playTimer, &QTimer::timeout, this, [this] {
        double t = m_playFrom + m_clock.nsecsElapsed() / 1e9;
        const double end = m_project.duration();
        if (t >= end) { t = end; stopPlay(); }
        setPosition(t);
    });
    m_playTimer.setTimerType(Qt::PreciseTimer);
    m_playTimer.setInterval(8);
    connect(&m_autosaveTimer, &QTimer::timeout, this, &MainWindow::autosave);
    m_autosaveTimer.start(60 * 1000);
    // automatic proxies: only when playback really can't keep up with a heavy video
    m_proxy = new ProxyManager(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/proxies", this);
    connect(m_proxy, &ProxyManager::ready, this, [this](const QString& id, const QString& path) {
        m_proxyPaths.insert(id, path);
        statusBar()->showMessage("Lightweight preview copy ready: playback will be smoother. (Export still uses the original file.)", 10000);
        requestPreview();
    });
    connect(m_proxy, &ProxyManager::failed, this, [this](const QString&, const QString& why) { statusBar()->showMessage("Couldn't make a preview copy: " + why, 10000); });
    connect(&m_lagTimer, &QTimer::timeout, this, &MainWindow::checkLag);
    m_lagTimer.start(1000);

    applyProjectSettings();
    updateTitle();
    updateProps();
    statusBar()->showMessage(QString("Preview: %1").arg(m_preview->name()));
    checkRecovery();
}

void MainWindow::buildUi(bool useOpenGl) {
    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_timeline = new TimelineWidget(&m_project, &m_stack);
    if (useOpenGl) m_preview = new GlPreview; else m_preview = new SoftwarePreview;
    m_preview->transformOverlay()->onDrag = [this](TransformOverlay::Phase ph, const TransformDrag& d) { onTransformDrag(ph, d); };
    m_preview->transformOverlay()->onClick = [this](const QPointF& p) { selectAtPoint(p); };

    // left: media panel (folders) + effects
    m_media = new MediaTree;
    auto* mediaBox = new QWidget;
    auto* ml = new QVBoxLayout(mediaBox);
    ml->setContentsMargins(6, 6, 6, 6);
    auto* btnRow = new QHBoxLayout;
    auto* bFiles = new QPushButton("Import files");
    auto* bFolder = new QPushButton("Import folder");
    auto* bNew = new QPushButton("New folder");
    for (QPushButton* b : {bFiles, bFolder, bNew}) btnRow->addWidget(b);
    connect(bFiles, &QPushButton::clicked, this, &MainWindow::importDialog);
    connect(bFolder, &QPushButton::clicked, this, &MainWindow::importFolderDialog);
    connect(bNew, &QPushButton::clicked, this, &MainWindow::newFolder);
    ml->addLayout(btnRow);
    ml->addWidget(m_media, 1);
    auto* mhint = new QLabel("Drop files or folders here. Drag files onto folders to organise them. Del removes.");
    mhint->setWordWrap(true);
    mhint->setStyleSheet("color: #8d8d93;");
    ml->addWidget(mhint);
    connect(m_media, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int) {
        if (!MediaTree::isFolder(it)) addAssetAtPlayhead(MediaTree::idOf(it));
    });
    connect(m_media, &MediaTree::urlsDropped, this, [this](const QStringList& paths, const QString& folder) { importPaths(paths, folder); });
    connect(m_media, &MediaTree::moveRequested, this, [this](const QStringList& assets, const QStringList& folders, const QString& target) {
        if (QUndoCommand* c = makeMoveToFolderCommand(m_project, assets, folders, target)) m_stack.push(c);
    });
    connect(m_media, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int) {      // inline rename (F2)
        if (!MediaTree::isFolder(it)) return;
        if (QUndoCommand* c = makeRenameFolderCommand(m_project, MediaTree::idOf(it), it->text(0))) m_stack.push(c);
    });
    connect(m_media, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* it) { if (MediaTree::isFolder(it)) m_expanded.insert(MediaTree::idOf(it)); });
    connect(m_media, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* it) { if (MediaTree::isFolder(it)) m_expanded.remove(MediaTree::idOf(it)); });
    connect(m_media, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu;
        QTreeWidgetItem* it = m_media->itemAt(pos);
        menu.addAction("New folder", this, &MainWindow::newFolder);
        if (it && MediaTree::isFolder(it)) menu.addAction("Rename", [this, it] { m_media->editItem(it, 0); });
        if (it) menu.addAction("Remove from project", this, &MainWindow::deleteMediaSelection);
        menu.exec(m_media->viewport()->mapToGlobal(pos));
    });

    auto* effectsBox = new QWidget;
    auto* el = new QVBoxLayout(effectsBox);
    el->setContentsMargins(6, 6, 6, 6);
    auto* hint = new QLabel("Clip effects: drag onto a clip (or select a clip and double-click).\nEffect layers and the adjustment layer: drag onto a track (or double-click). They affect everything below them.");
    hint->setStyleSheet("color: #8d8d93;");
    el->addWidget(hint);
    auto* effects = new EffectList;
    effects->setDragEnabled(true);
    effects->setDragDropMode(QAbstractItemView::DragOnly);
    QString lastGroup;
    for (const EffectPreset& p : effectPresets()) {
        if (p.group != lastGroup) {
            lastGroup = p.group;
            auto* hd = new QListWidgetItem(p.group.toUpper());
            hd->setFlags(Qt::ItemIsEnabled);
            QFont f = hd->font(); f.setBold(true); f.setPointSizeF(f.pointSizeF() * 0.9); hd->setFont(f);
            hd->setForeground(QColor("#8d8d93"));
            effects->addItem(hd);
        }
        auto* it = new QListWidgetItem("   " + p.label);
        it->setData(Qt::UserRole, p.id);
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        effects->addItem(it);
    }
    el->addWidget(effects, 1);
    connect(effects, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        const QString id = it->data(Qt::UserRole).toString();
        if (!id.isEmpty()) applyPresetToSelection(id);
    });
    m_tabs = new QTabWidget;
    m_tabs->addTab(mediaBox, "Media");
    m_tabs->addTab(effectsBox, "Effects");

    // centre: preview + transport
    auto* centre = new QWidget;
    auto* cl = new QVBoxLayout(centre);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    cl->addWidget(m_preview->widget(), 1);
    auto* transport = new QHBoxLayout;
    transport->setContentsMargins(8, 4, 8, 4);
    m_playBtn = new QPushButton("▶");
    m_playBtn->setFixedWidth(44);
    m_playBtn->setFocusPolicy(Qt::NoFocus);
    connect(m_playBtn, &QPushButton::clicked, this, &MainWindow::togglePlay);
    m_timecode = new QLabel;
    m_timecode->setStyleSheet("font-family: monospace;");
    m_quality = new QComboBox;
    m_quality->addItem("Preview: Full", 1.0);
    m_quality->addItem("Preview: 1/2", 0.5);
    m_quality->addItem("Preview: 1/4", 0.25);
    m_quality->setFocusPolicy(Qt::NoFocus);
    connect(m_quality, &QComboBox::currentIndexChanged, this, [this] { requestPreview(); });
    transport->addWidget(m_playBtn);
    transport->addWidget(m_timecode);
    transport->addStretch(1);
    transport->addWidget(m_quality);
    cl->addLayout(transport);

    // right: inspector
    m_propsBox = new QWidget;
    m_propsLayout = new QVBoxLayout(m_propsBox);
    m_propsLayout->setAlignment(Qt::AlignTop);
    m_propsLayout->setContentsMargins(10, 10, 10, 10);
    m_propsBox->setMinimumWidth(270);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(m_propsBox);

    auto* top = new QSplitter(Qt::Horizontal);
    top->addWidget(m_tabs);
    top->addWidget(centre);
    top->addWidget(scroll);
    top->setStretchFactor(0, 0); top->setStretchFactor(1, 1); top->setStretchFactor(2, 0);
    top->setSizes({290, 840, 310});

    auto* vs = new QSplitter(Qt::Vertical);
    vs->addWidget(top);
    auto* tlScroll = new QScrollArea;                  // vertical scrolling when there are many tracks
    tlScroll->setWidgetResizable(true);
    tlScroll->setFrameShape(QFrame::NoFrame);
    tlScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tlScroll->setWidget(m_timeline);
    m_timeline->setOverviewStore(&m_overview);
    vs->addWidget(tlScroll);
    vs->setStretchFactor(0, 1); vs->setStretchFactor(1, 0);
    vs->setSizes({560, 270});
    root->addWidget(vs);
    setCentralWidget(central);

    connect(m_timeline, &TimelineWidget::playheadMoved, this, [this](double t) { setPosition(t, true); });
    connect(m_timeline, &TimelineWidget::selectionChanged, this, &MainWindow::updateProps);
}

void MainWindow::buildActions() {
    auto make = [this](const QString& text, const QKeySequence& ks, auto slot) {
        auto* a = new QAction(text, this);
        a->setShortcut(ks);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
        return a;
    };
    auto* aNew = make("New project", QKeySequence::New, [this] { newProject(); });
    auto* aOpen = make("Open…", QKeySequence::Open, [this] { openDialog(); });
    auto* aSave = make("Save", QKeySequence::Save, [this] { save(); });
    auto* aSaveAs = make("Save as…", QKeySequence::SaveAs, [this] { saveAs(); });
    auto* aImport = make("Import files…", QKeySequence("Ctrl+I"), [this] { importDialog(); });
    auto* aImportFolder = make("Import folder…", QKeySequence("Ctrl+Shift+I"), [this] { importFolderDialog(); });
    auto* aBeep = make("Test audio output", QKeySequence(), [this] { m_audio.beep(); });
    auto* aExport = make("Export…", QKeySequence("Ctrl+E"), [this] { exportDialog(); });
    auto* aSettings = make("Canvas / aspect ratio…", QKeySequence(), [this] {
        ProjectSettings s = m_project.settings;
        if (askSettings(this, &s, "Canvas and frame rate")) { m_project.settings = s; m_project.notifySettingsChanged(); m_extraDirty = true; updateTitle(); requestPreview(); }
    });
    auto* aUndo = m_stack.createUndoAction(this, "Undo"); aUndo->setShortcut(QKeySequence::Undo); addAction(aUndo);
    auto* aRedo = m_stack.createRedoAction(this, "Redo"); aRedo->setShortcut(QKeySequence("Ctrl+Shift+Z")); addAction(aRedo);
    auto* aSplit = make("Split", QKeySequence(Qt::Key_S), [this] { doSplit(); });
    auto* aFreeze = make("Freeze frame", QKeySequence(Qt::Key_F), [this] { doFreeze(); });
    auto* aDel = make("Delete", QKeySequence(Qt::Key_Delete), [this] { doDelete(false); });
    auto* aRip = make("Ripple delete", QKeySequence("Shift+Delete"), [this] { doDelete(true); });
    auto* aCopy = make("Copy", QKeySequence::Copy, [this] { doCopy(); });
    auto* aCut = make("Cut", QKeySequence::Cut, [this] { doCut(); });
    auto* aPaste = make("Paste", QKeySequence::Paste, [this] { doPaste(); });
    auto* aDup = make("Duplicate", QKeySequence("Ctrl+D"), [this] { doDuplicate(); });
    auto* aText = make("Add text", QKeySequence(Qt::Key_T), [this] { addTextAtPlayhead(); });
    auto* aTrackV = make("Add video track", QKeySequence(), [this] { if (QUndoCommand* c = makeInsertTrackCommand(m_project, TrackKind::Video, 0)) m_stack.push(c); });
    auto* aTrackA = make("Add audio track", QKeySequence(), [this] { if (QUndoCommand* c = makeInsertTrackCommand(m_project, TrackKind::Audio, int(m_project.tracks.size()))) m_stack.push(c); });
    auto* aProxy = new QAction("Automatic preview proxies", this);
    aProxy->setCheckable(true);
    aProxy->setChecked(true);
    aProxy->setToolTip("Only when playback can't keep up with a heavy (1440p+) video: make a light copy for the preview. Export always uses the original.");
    connect(aProxy, &QAction::toggled, this, [this](bool on) { m_autoProxy = on; requestPreview(); });
    auto* aAll = make("Select all", QKeySequence::SelectAll, [this] { selectAll(); });
    make("Play / Pause", QKeySequence(Qt::Key_Space), [this] { togglePlay(); });
    make("Previous frame", QKeySequence(Qt::Key_Left), [this] { stopPlay(); setPosition(m_pos - 1.0 / m_project.settings.fps, true); });
    make("Next frame", QKeySequence(Qt::Key_Right), [this] { stopPlay(); setPosition(m_pos + 1.0 / m_project.settings.fps, true); });
    make("Go to start", QKeySequence(Qt::Key_Home), [this] { setPosition(0, true); });
    make("Go to end", QKeySequence(Qt::Key_End), [this] { setPosition(m_project.duration(), true); });

    auto* file = menuBar()->addMenu("&File");
    file->addActions({aNew, aOpen, aSave, aSaveAs}); file->addSeparator();
    file->addActions({aImport, aImportFolder, aSettings, aProxy, aBeep, aExport});
    auto* edit = menuBar()->addMenu("&Edit");
    edit->addActions({aUndo, aRedo}); edit->addSeparator(); edit->addActions({aCopy, aCut, aPaste, aDup}); edit->addSeparator();
    edit->addActions({aSplit, aFreeze, aDel, aRip, aAll}); edit->addSeparator(); edit->addActions({aText, aTrackV, aTrackA});

    auto* tb = addToolBar("Main");
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    tb->addAction(aImport); tb->addSeparator();
    tb->addAction(aUndo); tb->addAction(aRedo); tb->addSeparator();
    tb->addAction(aSplit); tb->addAction(aFreeze); tb->addAction(aText); tb->addAction(aDel);
    auto* spacer = new QWidget; spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    tb->addAction(aExport);
}

// ---- project lifecycle ------------------------------------------------------------------------

void MainWindow::applyProjectSettings() {
    m_preview->setCanvasSize(QSize(m_project.settings.width, m_project.settings.height));
    setPosition(m_pos);
}

void MainWindow::updateTitle() {
    const QString name = m_project.filePath.isEmpty() ? "Untitled" : QFileInfo(m_project.filePath).fileName();
    setWindowTitle(QString("%1%2 — Video Editor").arg(name, isModified() ? " *" : ""));
}

void MainWindow::newProject() {
    if (!maybeSave()) return;
    ProjectSettings s;
    if (!askSettings(this, &s, "New project")) return;
    stopPlay();
    m_stack.clear();
    m_thumbs.clear();
    m_project.reset(s);
    m_timeline->clearSelection();
    m_recovered = false; m_extraDirty = false;
    m_stack.setClean();
    setPosition(0, true);
    updateTitle();
}

void MainWindow::openDialog() {
    if (!maybeSave()) return;
    const QString f = QFileDialog::getOpenFileName(this, "Open project", QString(), "Video project (*.vproj)");
    if (!f.isEmpty()) openProject(f);
}

void MainWindow::openProject(const QString& path) {
    stopPlay();
    QString err;
    if (!m_project.load(path, &err)) {
        QMessageBox::warning(this, "Couldn't open project", err);
        return;
    }
    m_stack.clear();
    m_stack.setClean();
    m_recovered = false; m_extraDirty = false;
    m_thumbs.clear();
    m_timeline->clearSelection();
    for (const MediaAsset& a : m_project.assets) if (!a.missing) startAssetJob(a, false);
    setPosition(0, true);
    updateTitle();
    const QStringList missing = m_project.missingMedia();
    if (!missing.isEmpty())
        QMessageBox::warning(this, "Media missing",
                             "Some media files couldn't be found:\n\n" + missing.join("\n") +
                                 "\n\nMove them back to where they were (or next to the project file) and re-open the project.");
}

bool MainWindow::save() {
    if (m_project.filePath.isEmpty()) return saveAs();
    QString err;
    if (!m_project.save(m_project.filePath, &err)) { QMessageBox::warning(this, "Couldn't save", err); return false; }
    m_stack.setClean();
    m_recovered = false; m_extraDirty = false;
    QFile::remove(autosavePath());
    updateTitle();
    return true;
}

bool MainWindow::saveAs() {
    QString f = QFileDialog::getSaveFileName(this, "Save project", QString(), "Video project (*.vproj)");
    if (f.isEmpty()) return false;
    if (!f.endsWith(".vproj")) f += ".vproj";
    m_project.filePath = f;
    return save();
}

bool MainWindow::maybeSave() {
    if (!isModified()) return true;
    const auto r = QMessageBox::question(this, "Unsaved changes", "Save changes to this project first?",
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Save) return save();
    return r == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!maybeSave()) { e->ignore(); return; }
    stopPlay();
    QFile::remove(autosavePath());
    e->accept();
}

QString MainWindow::autosavePath() const {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/autosave.vproj";
}

void MainWindow::autosave() {
    if (isModified() && !m_project.clips.isEmpty()) {
        const QString keep = m_project.filePath;          // save() would overwrite the project's own path
        m_project.save(autosavePath());
        m_project.filePath = keep;
    }
}

void MainWindow::checkRecovery() {
    if (!QFileInfo::exists(autosavePath())) return;
    if (QMessageBox::question(this, "Recover project",
                              "The editor didn't close properly last time. Recover your unsaved work?") == QMessageBox::Yes) {
        openProject(autosavePath());
        m_project.filePath.clear();   // force "Save as" so the recovered work gets a real name
        m_recovered = true;
        updateTitle();
    } else {
        QFile::remove(autosavePath());
    }
}

// ---- import (background) ----------------------------------------------------------------------

void MainWindow::importDialog() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Import media", QString(),
        "Media (*.mp4 *.m4v *.mov *.mkv *.webm *.avi *.flv *.wmv *.mpg *.mpeg *.ts *.mts *.m2ts *.3gp *.ogv *.gif "
        "*.mp3 *.wav *.aac *.flac *.ogg *.oga *.opus *.m4a *.wma *.aiff *.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff);;All files (*)");
    importFiles(files, QString());
}

void MainWindow::importFolderDialog() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Import a whole folder");
    if (!dir.isEmpty()) importDirectory(dir, QString());
}

void MainWindow::importPaths(const QStringList& paths, const QString& folderId) {
    QStringList files;
    for (const QString& p : paths) {
        if (QFileInfo(p).isDir()) importDirectory(p, folderId);
        else files << p;
    }
    importFiles(files, folderId);
}

void MainWindow::importFiles(const QStringList& files, const QString& folderId) {
    for (const QString& f : files) {
        bool dup = false;
        for (const MediaAsset& a : m_project.assets) dup = dup || a.path == f;
        if (dup) continue;
        MediaAsset seed;
        seed.path = f;
        seed.folderId = folderId;
        startAssetJob(seed, true);
    }
}

// Mirrors a directory tree as folders in the media panel and imports every supported file in it.
void MainWindow::importDirectory(const QString& dir, const QString& parentFolderId) {
    const QDir d(dir);
    Folder f{newId(), d.dirName().isEmpty() ? dir : d.dirName(), parentFolderId};
    m_project.addFolder(f);
    m_expanded.insert(f.id);
    m_extraDirty = true;
    updateTitle();
    QStringList files;
    for (const QFileInfo& fi : d.entryInfoList(QDir::Files | QDir::Readable, QDir::Name))
        if (isMediaFile(fi.absoluteFilePath())) files << fi.absoluteFilePath();
    importFiles(files, f.id);
    for (const QFileInfo& sub : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name))
        importDirectory(sub.absoluteFilePath(), f.id);
}

void MainWindow::startAssetJob(MediaAsset seed, bool probe) {
    auto* w = new QFutureWatcher<AssetJob>(this);
    connect(w, &QFutureWatcher<AssetJob>::finished, this, [this, w] {
        AssetJob r = w->result();
        w->deleteLater();
        if (--m_jobs <= 0) statusBar()->showMessage(QString("Preview: %1").arg(m_preview->name()));
        if (!r.ok) {
            if (r.probed)
                QMessageBox::warning(this, "Couldn't import",
                                     QString("Couldn't read “%1”.\n\n%2\n\nTry converting it to H.264 MP4, or check whether the file is corrupted.")
                                         .arg(QFileInfo(r.asset.path).fileName(), r.err));
            return;
        }
        if (r.probed) { r.asset.id = newId(); m_project.addAsset(r.asset); m_extraDirty = true; updateTitle(); }
        m_thumbs.insert(r.asset.id, r.thumb);
        m_overview.insert(r.asset.id, r.overview);
        m_timeline->update();
        if (m_proxy) { const QString ex = m_proxy->existing(r.asset); if (!ex.isEmpty()) m_proxyPaths.insert(r.asset.id, ex); }   // a preview copy from an earlier session
        rebuildMediaList();
    });
    ++m_jobs;
    statusBar()->showMessage(QString("Importing %1…").arg(QFileInfo(seed.path).fileName()));
    w->setFuture(QtConcurrent::run([seed, probe]() -> AssetJob {
        AssetJob r;
        r.asset = seed;
        r.probed = probe;
        if (probe) {
            if (!probeMedia(seed.path, &r.asset, &r.err)) return r;
            r.asset.folderId = seed.folderId;
        }
        if (r.asset.hasVideo) {
            Decoder d;
            if (d.open(seed.path, nullptr, false)) {
                d.setMaxSize(QSize(192, 108));
                r.thumb = d.frameAt(r.asset.isImage ? 0 : std::min(1.0, r.asset.duration * 0.1));
            }
        }
        r.overview = buildOverview(r.asset);        // filmstrip + waveform for the timeline
        r.ok = true;
        return r;
    }));
}

// ---- media panel ------------------------------------------------------------------------------

void MainWindow::rebuildMediaList() {
    QSignalBlocker block(m_media);
    m_media->clear();
    QHash<QString, QTreeWidgetItem*> fitems;
    QVector<Folder> pending = m_project.folders;
    std::sort(pending.begin(), pending.end(), [](const Folder& a, const Folder& b) { return a.name.toLower() < b.name.toLower(); });
    const QIcon dirIcon = style()->standardIcon(QStyle::SP_DirIcon);
    bool progress = true;
    while (!pending.isEmpty() && progress) {                       // parents before children
        progress = false;
        for (int i = 0; i < pending.size(); ++i) {
            const Folder f = pending[i];
            QTreeWidgetItem* parent = f.parentId.isEmpty() ? nullptr : fitems.value(f.parentId);
            if (!f.parentId.isEmpty() && !parent) {
                bool parentExists = false;
                for (const Folder& o : m_project.folders) parentExists = parentExists || o.id == f.parentId;
                if (parentExists) continue;                           // wait for its parent
            }
            auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_media);
            it->setText(0, f.name);
            it->setIcon(0, dirIcon);
            it->setData(0, Qt::UserRole, f.id);
            it->setData(0, Qt::UserRole + 1, "folder");
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsEditable);
            fitems.insert(f.id, it);
            pending.removeAt(i--);
            progress = true;
        }
    }
    QVector<MediaAsset> sorted(m_project.assets.begin(), m_project.assets.end());
    std::sort(sorted.begin(), sorted.end(), [](const MediaAsset& x, const MediaAsset& y) { return x.name().toLower() < y.name().toLower(); });
    for (const MediaAsset& a : sorted) {
        QTreeWidgetItem* parent = fitems.value(a.folderId);
        auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_media);
        QString sub = a.missing ? "MISSING" : a.isImage ? "Image" : a.hasVideo ? "Video" : "Audio";
        if (!a.isImage && !a.missing) sub += QString("  %1:%2").arg(int(a.duration) / 60).arg(int(a.duration) % 60, 2, 10, QChar('0'));
        it->setText(0, QString("%1\n%2").arg(a.name(), sub));
        it->setData(0, Qt::UserRole, a.id);
        it->setData(0, Qt::UserRole + 1, "asset");
        it->setToolTip(0, a.path);
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        if (a.missing) it->setForeground(0, QColor("#ff6b6b"));
        if (m_thumbs.contains(a.id) && !m_thumbs[a.id].isNull())
            it->setIcon(0, QPixmap::fromImage(m_thumbs[a.id]).scaled(64, 36, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        else if (!a.hasVideo)
            it->setIcon(0, style()->standardIcon(QStyle::SP_MediaVolume));
    }
    for (auto it = fitems.begin(); it != fitems.end(); ++it) it.value()->setExpanded(m_expanded.contains(it.key()));
}

void MainWindow::newFolder() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, "New folder", "Folder name:", QLineEdit::Normal, "New folder", &ok);
    if (!ok) return;
    QString parent;                                              // create inside the selected folder, if any
    const auto sel = m_media->selectedItems();
    if (!sel.isEmpty()) parent = MediaTree::isFolder(sel[0]) ? MediaTree::idOf(sel[0]) : (sel[0]->parent() ? MediaTree::idOf(sel[0]->parent()) : QString());
    QString id;
    if (QUndoCommand* c = makeNewFolderCommand(m_project, name, parent, &id)) { m_expanded.insert(parent); m_stack.push(c); }
}

static void collectFolderIds(const QVector<Folder>& all, const QString& root, QSet<QString>* out) {
    out->insert(root);
    for (const Folder& f : all) if (f.parentId == root && !out->contains(f.id)) collectFolderIds(all, f.id, out);
}

void MainWindow::deleteMediaSelection() {
    QStringList assets, folders;
    for (const QTreeWidgetItem* it : m_media->selectedItems()) (MediaTree::isFolder(it) ? folders : assets) << MediaTree::idOf(it);
    if (assets.isEmpty() && folders.isEmpty()) return;
    QSet<QString> doomedFolders;
    for (const QString& f : folders) collectFolderIds(m_project.folders, f, &doomedFolders);
    QSet<QString> doomedAssets(assets.begin(), assets.end());
    for (const MediaAsset& a : m_project.assets) if (doomedFolders.contains(a.folderId)) doomedAssets.insert(a.id);
    int clips = 0;
    for (const Clip& c : m_project.clips) if (doomedAssets.contains(c.assetId)) ++clips;
    QString msg = QString("Remove %1 file(s)%2 from the project?").arg(doomedAssets.size()).arg(doomedFolders.isEmpty() ? "" : QString(" and %1 folder(s)").arg(doomedFolders.size()));
    if (clips) msg += QString("\n\nThey are used by %1 clip(s) on the timeline; those clips will be removed too.").arg(clips);
    msg += "\n\nYour original files on disk are not touched, and you can undo this with Ctrl+Z.";
    if (QMessageBox::question(this, "Remove media", msg) != QMessageBox::Yes) return;
    if (QUndoCommand* c = makeRemoveMediaCommand(m_project, assets, folders)) m_stack.push(c);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
void MainWindow::dropEvent(QDropEvent* e) {
    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls()) if (u.isLocalFile()) paths << u.toLocalFile();
    importPaths(paths, QString());
}

// ---- editing ----------------------------------------------------------------------------------

void MainWindow::addAssetAtPlayhead(const QString& assetId) {
    const MediaAsset* a = m_project.asset(assetId);
    if (!a) return;
    int row = -1;
    for (int i = m_project.tracks.size() - 1; i >= 0 && row < 0; --i) {   // prefer V1 (lowest video track), then A1
        if (m_project.tracks[i].locked) continue;
        if ((m_project.tracks[i].kind == TrackKind::Video) == a->hasVideo) row = i;
    }
    if (row < 0) { statusBar()->showMessage("Every matching track is locked.", 4000); return; }
    if (QUndoCommand* c = makeAddClipCommand(m_project, assetId, row, m_pos)) m_stack.push(c);
}

void MainWindow::selectAll() {
    if (m_media->hasFocus()) m_media->selectAll(); else m_timeline->selectAll();
}

void MainWindow::doSplit() {
    QStringList ids = m_timeline->selectedIds();
    if (ids.isEmpty())
        for (const Clip& c : m_project.clips) if (m_pos > c.start && m_pos < c.end()) ids << c.id;
    if (QUndoCommand* c = makeSplitCommand(m_project, ids, m_pos)) m_stack.push(c);
}

void MainWindow::doDelete(bool ripple) {
    if (m_media->hasFocus()) { deleteMediaSelection(); return; }
    if (QUndoCommand* c = makeDeleteCommand(m_project, m_timeline->selectedIds(), ripple)) m_stack.push(c);
}

void MainWindow::doFreeze() {
    QStringList ids = m_timeline->selectedIds();
    if (ids.isEmpty())
        for (const Clip& c : m_project.clips) if (m_pos > c.start && m_pos < c.end()) ids << c.id;
    std::sort(ids.begin(), ids.end(), [this](const QString& a, const QString& b) {   // topmost track first
        const Clip *x = m_project.clip(a), *y = m_project.clip(b);
        return x && y && x->track < y->track;
    });
    if (QUndoCommand* c = makeFreezeCommand(m_project, ids, m_pos, 2.0)) {
        m_stack.push(c);
        statusBar()->showMessage("Froze a 2 second frame. Drag the right edge of the freeze clip to change its length.", 6000);
    } else {
        statusBar()->showMessage("Move the playhead onto an unlocked video clip to freeze a frame.", 4000);
    }
}

void MainWindow::applyPresetToSelection(const QString& presetId) {
    const EffectPreset* p = findPreset(presetId);
    if (!p) return;
    if (p->layer) { addLayerAtPlayhead(p->effect); return; }
    if (QUndoCommand* c = makeApplyEffectCommand(m_project, m_timeline->selectedIds(), p->effect)) m_stack.push(c);
    else statusBar()->showMessage("Select a video clip first, then double-click the preset.", 4000);
}

// An effect / adjustment layer goes on the topmost video track that is free at the playhead and covers what is on the timeline.
// ---- direct manipulation in the preview ---------------------------------------------------------------------------------

// The frame of a picture on the canvas, from its own position / size / rotation at the playhead (effects like shake are ignored,
// so the box stays steady while you edit). False when it isn't a visible, editable picture right now.
bool MainWindow::clipBox(const Clip& c, TransformBox* b) const {
    if ((c.kind != KindMedia && c.kind != KindText) || m_pos < c.start - 1e-6 || m_pos >= c.end()) return false;
    const Track& tr = m_project.tracks[c.track];
    if (tr.kind != TrackKind::Video || tr.hidden || tr.locked) return false;
    if (c.kind == KindText) {                                // a text object: the frame is the text (plus outline) at its own size
        const double lt = std::clamp(m_pos - c.start, 0.0, c.dur);
        const QSizeF sz = textBounds(c.text);
        const double sc = std::max(0.0, propValue(c, "scale", lt));
        *b = {true, propValue(c, "x", lt), propValue(c, "y", lt), sz.width() * sc, sz.height() * sc, propValue(c, "rotation", lt)};
        return true;
    }
    const MediaAsset* a = m_project.asset(c.assetId);
    if (!a || a->missing || !a->hasVideo || a->width <= 0 || a->height <= 0) return false;
    const double local = std::clamp(m_pos - c.start, 0.0, c.dur);
    const double fit = std::min(m_project.settings.width / double(a->width), m_project.settings.height / double(a->height));
    const double s = std::max(0.0, propValue(c, "scale", local));
    *b = {true, propValue(c, "x", local), propValue(c, "y", local), a->width * fit * s, a->height * fit * s, propValue(c, "rotation", local)};
    return true;
}

void MainWindow::updateTransformBox() {
    TransformBox b;
    const QStringList ids = m_timeline->selectedIds();
    if (!m_picking && ids.size() == 1)
        if (const Clip* c = m_project.clip(ids[0])) if (!clipBox(*c, &b)) b = TransformBox();
    m_preview->transformOverlay()->set(b, QSize(m_project.settings.width, m_project.settings.height));
}

void MainWindow::onTransformDrag(TransformOverlay::Phase ph, const TransformDrag& d) {
    const QStringList ids = m_timeline->selectedIds();
    if (ids.size() != 1) return;
    const Clip* c = m_project.clip(ids[0]);
    if (!c) return;
    if (ph == TransformOverlay::Begin) {
        ++m_gesture;
        m_gestScale0 = propValue(*c, "scale", std::clamp(m_pos - c->start, 0.0, c->dur));
        return;
    }
    if (ph == TransformOverlay::End) { statusBar()->clearMessage(); return; }
    QMap<QString, double> vals;
    if (d.moved) { vals["x"] = d.x; vals["y"] = d.y; statusBar()->showMessage(QString("Position  X %1   Y %2").arg(d.x, 0, 'f', 1).arg(d.y, 0, 'f', 1)); }
    if (d.scaled) {
        const double s = std::round(std::clamp(m_gestScale0 * d.scaleFactor, 0.01, 50.0) * 1000) / 1000;
        vals["scale"] = s;
        statusBar()->showMessage(QString("Size  %1 %").arg(s * 100, 0, 'f', 1));
    }
    if (d.rotated) { vals["rotation"] = d.rot; statusBar()->showMessage(QString("Rotation  %1 °").arg(d.rot, 0, 'f', 1)); }
    applyTransform(c->id, vals);
}

// Writes the values like the inspector does: a property that has keyframes gets a keyframe at the playhead, any other one just changes.
// The whole mouse gesture is one undo step.
void MainWindow::applyTransform(const QString& id, const QMap<QString, double>& vals) {
    const Clip* c = m_project.clip(id);
    if (!c) return;
    const double local = clipLocal(*c), tol = keyTol();
    editClip(id, QString("xform%1").arg(m_gesture), [&](Clip& n) {
        for (auto it = vals.constBegin(); it != vals.constEnd(); ++it) {
            if (n.keys.value(it.key()).isEmpty()) setPropBase(n, it.key(), it.value());
            else setKeyAt(n, it.key(), local, it.value(), tol);
        }
    });
}

// Clicking a picture in the preview selects its clip (the top-most one under the cursor).
void MainWindow::selectAtPoint(const QPointF& P) {
    for (int tr = 0; tr < m_project.tracks.size(); ++tr) {                    // top track first
        for (const Clip& c : m_project.clips) {
            TransformBox b;
            if (c.track != tr || !clipBox(c, &b)) continue;
            const QPointF d = P - QPointF(b.x, b.y);
            const double a = b.rot * M_PI / 180.0;
            const double lx = d.x() * std::cos(a) + d.y() * std::sin(a), ly = -d.x() * std::sin(a) + d.y() * std::cos(a);
            if (std::abs(lx) <= b.w / 2 && std::abs(ly) <= b.h / 2) { m_timeline->selectOnly(c.id); return; }
        }
    }
}

// ---- text, copy / paste ---------------------------------------------------------------------------------------------

void MainWindow::addTextAtPlayhead() {
    int row = -1;
    for (int i = 0; i < m_project.tracks.size() && row < 0; ++i) {         // topmost video track that is free for 5 s from the playhead
        if (m_project.tracks[i].kind != TrackKind::Video || m_project.tracks[i].locked) continue;
        Clip probe; probe.id = "probe"; probe.track = i; probe.start = m_pos; probe.dur = 5.0;
        if (!m_project.overlaps(probe)) row = i;
    }
    if (row < 0) { statusBar()->showMessage("No free video track at the playhead: unlock one, add a track, or move the playhead.", 5000); return; }
    TextStyle st;
    st.size = m_project.settings.height / 9.0;                             // 120 px on a 1080p canvas
    if (QUndoCommand* cmd = makeAddTextCommand(m_project, row, m_pos, st)) {
        m_stack.push(cmd);
        m_timeline->selectOnly(m_project.clips.last().id);
        statusBar()->showMessage("Text added. Type it in the panel on the right, and drag it in the preview.", 7000);
    }
}

void MainWindow::doCopy() {
    const QStringList ids = m_timeline->selectedIds();
    if (ids.isEmpty()) { statusBar()->showMessage("Select one or more clips first.", 3000); return; }
    m_clipboard = copyClips(m_project, ids);
    statusBar()->showMessage(QString("Copied %1 clip(s). Move the playhead and press Ctrl+V to paste.").arg(m_clipboard.size()), 5000);
}

void MainWindow::doCut() {
    const QStringList ids = m_timeline->selectedIds();
    if (ids.isEmpty()) return;
    m_clipboard = copyClips(m_project, ids);
    if (QUndoCommand* c = makeDeleteCommand(m_project, ids, false)) m_stack.push(c);
    statusBar()->showMessage(QString("Cut %1 clip(s).").arg(m_clipboard.size()), 4000);
}

void MainWindow::doPaste() {
    if (m_clipboard.isEmpty()) { statusBar()->showMessage("Nothing copied yet: select clips and press Ctrl+C.", 3000); return; }
    QStringList ids;
    if (QUndoCommand* c = makePasteCommand(m_project, m_clipboard, m_pos, &ids)) { m_stack.push(c); m_timeline->selectIds(ids); }
    else statusBar()->showMessage("Nothing could be pasted: the tracks are locked, or no longer suit these clips.", 5000);
}

void MainWindow::doDuplicate() {
    const QStringList ids = m_timeline->selectedIds();
    if (ids.isEmpty()) return;
    const QVector<Clip> clips = copyClips(m_project, ids);
    double end = 0;
    for (const Clip& c : clips) end = std::max(end, c.end());
    QStringList made;
    if (QUndoCommand* c = makePasteCommand(m_project, clips, end, &made)) { m_stack.push(c); m_timeline->selectIds(made); }
}

void MainWindow::checkLag() {
    if (!m_playing || !m_autoProxy) { m_delivered = 0; m_slowSecs = 0; return; }
    const double fps = std::max(1.0, m_project.settings.fps);
    m_slowSecs = m_delivered < 0.7 * fps ? m_slowSecs + 1 : 0;           // the preview delivered well under the project's frame rate
    m_delivered = 0;
    if (m_slowSecs < 2) return;
    m_slowSecs = 0;
    for (const Clip& c : m_project.clips) {
        if (c.kind != KindMedia || m_pos < c.start || m_pos >= c.end()) continue;
        const MediaAsset* a = m_project.asset(c.assetId);
        if (!a || m_proxyPaths.contains(a->id) || m_proxyTried.contains(a->id) || !ProxyManager::worthProxying(*a)) continue;
        m_proxyTried.insert(a->id);
        if (m_proxy->request(*a))
            statusBar()->showMessage(QString("Playback is struggling with “%1”: making a lightweight preview copy in the background (export still uses the original).").arg(a->name()), 15000);
    }
}

// Eyedropper for the chroma key: shows the picture untouched, then the next click on it sets the key colour.
void MainWindow::startPick(const QString& clipId) {
    m_picking = true;
    updateTransformBox();                                  // the picker needs the preview to itself
    requestPreview();
    statusBar()->showMessage("Click the colour in the preview that you want to remove.", 0);
    m_preview->setPickMode(true, [this, clipId](const QColor& col) {
        m_picking = false;
        updateTransformBox();
        editClip(clipId, QString(), [&](Clip& n) {
            for (Effect& f : n.effects) if (f.type == "chromakey") { f.p["r"] = col.redF(); f.p["g"] = col.greenF(); f.p["b"] = col.blueF(); }
        });
        statusBar()->showMessage(QString("Chroma key will remove %1 (and close shades of it).").arg(col.name()), 8000);
        requestPreview();
    });
}

void MainWindow::addLayerAtPlayhead(const Effect& e) {
    const double dur = m_project.duration() - m_pos > 1.0 ? m_project.duration() - m_pos : 5.0;
    int row = -1;
    for (int i = 0; i < m_project.tracks.size() && row < 0; ++i) {
        if (m_project.tracks[i].kind != TrackKind::Video || m_project.tracks[i].locked) continue;
        Clip probe; probe.id = "probe"; probe.track = i; probe.start = m_pos; probe.dur = dur;
        if (!m_project.overlaps(probe)) row = i;
    }
    if (row < 0) { statusBar()->showMessage("No free video track at the playhead: unlock one, or move the playhead.", 5000); return; }
    if (QUndoCommand* cmd = makeAddLayerCommand(m_project, e, row, m_pos)) {
        m_stack.push(cmd);
        statusBar()->showMessage("Layer added. It affects everything below it on the timeline; drag its edges to change where it applies.", 7000);
    }
}

// ---- playback ---------------------------------------------------------------------------------

QSize MainWindow::previewMaxSize() const {
    const double f = m_quality->currentData().toDouble();
    return QSize(int(m_project.settings.width * f * 1.5), int(m_project.settings.height * f * 1.5));   // headroom for zoomed layers
}

void MainWindow::requestPreview() {
    m_preview->setRenderScale(m_quality->currentData().toDouble());
    const QVector<ActiveLayer> layers = m_project.videoLayersAt(m_pos);
    if (layers.isEmpty()) { m_preview->setLayers({}); return; }
    QVector<LayerRequest> req;
    for (const ActiveLayer& l : layers) {
        QVector<FxPass> passes = l.passes;
        if (m_picking)                                         // while picking a key colour, show the untouched picture
            passes.erase(std::remove_if(passes.begin(), passes.end(), [](const FxPass& f) { return f.type == "chromakey"; }), passes.end());
        QString path = l.path;
        if (m_autoProxy && !l.isText && !l.isPass)           // a lightweight copy for the preview, if one exists
            if (const Clip* cl = m_project.clip(l.clipId)) { const auto it = m_proxyPaths.constFind(cl->assetId); if (it != m_proxyPaths.constEnd()) path = it.value(); }
        req.push_back({path, l.sourceTime, l.fx, passes, l.mask, l.isPass, l.text, l.isText});
    }
    m_frames.request(req, previewMaxSize(), QSize(m_project.settings.width, m_project.settings.height));
}

void MainWindow::setPosition(double t, bool fromUser) {
    m_pos = std::clamp(t, 0.0, std::max(0.0, m_project.duration() + 3600.0));
    if (fromUser && m_playing) { m_playFrom = m_pos; m_clock.restart(); m_audio.play(m_project, m_pos); }
    m_timeline->setPlayhead(m_pos);
    m_timecode->setText(QString("%1   /   %2").arg(formatTimecode(m_pos, m_project.settings.fps), formatTimecode(m_project.duration(), m_project.settings.fps)));
    requestPreview();
    syncProps();
}

void MainWindow::togglePlay() {
    if (m_playing) { stopPlay(); return; }
    if (m_project.clips.isEmpty()) return;
    if (m_pos >= m_project.duration() - 1e-3) m_pos = 0;
    m_playFrom = m_pos;
    m_clock.restart();
    m_playing = true;
    m_playBtn->setText("⏸");
    m_audio.play(m_project, m_pos);
    m_playTimer.start();
    if (!m_audio.available() && !m_warnedNoAudio) {
        m_warnedNoAudio = true;
        statusBar()->showMessage("No sound output available: preview is silent (the export still has audio).", 8000);
    }
}

void MainWindow::stopPlay() {
    if (!m_playing) return;
    m_playing = false;
    m_playTimer.stop();
    m_audio.stop();
    m_playBtn->setText("▶");
}

// ---- inspector ----------------------------------------------------------------------------------

double MainWindow::keyTol() const { return 0.5 / std::max(1.0, m_project.settings.fps); }

double MainWindow::clipLocal(const Clip& c) const {
    const double fps = std::max(1.0, m_project.settings.fps);
    const double l = std::clamp(m_pos - c.start, 0.0, c.dur);
    return std::min(c.dur, std::round(l * fps) / fps);          // keyframes sit on whole frames
}

void MainWindow::editClip(const QString& id, const QString& key, const std::function<void(Clip&)>& fn) {
    const Clip* c = m_project.clip(id);
    if (!c) return;
    Clip n = *c;
    fn(n);
    if (QUndoCommand* cmd = makeSetClipCommand(m_project, n, "Edit clip", key.isEmpty() ? QString() : id + "|" + key)) m_stack.push(cmd);
}

void MainWindow::editProp(const QString& id, const QString& prop, double value) {
    const Clip* c = m_project.clip(id);
    if (!c) return;
    const double local = clipLocal(*c), tol = keyTol();
    editClip(id, prop, [&](Clip& n) {
        if (n.keys.value(prop).isEmpty()) setPropBase(n, prop, value);     // not animated: just set the value
        else setKeyAt(n, prop, local, value, tol);                          // animated: editing creates/updates a keyframe at the playhead
    });
}

void MainWindow::toggleKey(const QString& id, const QString& prop) {
    const Clip* c = m_project.clip(id);
    if (!c) return;
    const double local = clipLocal(*c), tol = keyTol();
    editClip(id, QString(), [&](Clip& n) {
        if (hasKeyAt(n, prop, local, tol)) removeKeyAt(n, prop, local, tol);
        else setKeyAt(n, prop, local, propValue(n, prop, local), tol);
    });
}

MainWindow::PropRow MainWindow::addPropRow(QVBoxLayout* into, const QString& id, const QString& label, const QString& prop, double lo, double hi,
                                           double step, int decimals, double mult, const QString& suffix) {
    auto* row = new QWidget;
    auto* l = new QHBoxLayout(row);
    l->setContentsMargins(0, 0, 0, 0);
    auto* name = new QLabel(label);
    name->setMinimumWidth(78);
    auto* spin = new QDoubleSpinBox;
    spin->setRange(lo, hi);
    spin->setDecimals(decimals);
    spin->setSingleStep(step);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);                  // typing commits on Enter; arrows commit immediately
    spin->setFocusPolicy(Qt::StrongFocus);             // the mouse wheel over the panel scrolls it instead of changing values by accident
    auto* key = new DiamondButton;
    l->addWidget(name);
    l->addWidget(spin, 1);
    l->addWidget(key);
    into->addWidget(row);
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this, id, prop, mult](double v) { editProp(id, prop, v / mult); });
    connect(key, &QAbstractButton::clicked, this, [this, id, prop] { toggleKey(id, prop); });
    PropRow r{prop, spin, key, mult};
    m_rows.push_back(r);
    return r;
}

void MainWindow::updateProps() {
    const QStringList ids = m_timeline->selectedIds();
    const Clip* c = ids.size() == 1 ? m_project.clip(ids[0]) : nullptr;
    QString sig;
    if (c) {                                             // rebuild only when the *shape* of the inspector changes
        const MediaAsset* a = m_project.asset(c->assetId);
        sig = c->id + (c->freeze ? "|F" : "|-") + (a && a->hasAudio ? "A" : "-") + (a && a->hasVideo ? "V" : "-") + (a && a->isImage ? "I" : "-") +
              QString::number(c->kind) + (c->mask.on ? "M" : "m");
        for (const Effect& e : c->effects) sig += "|" + e.type + QString::number(e.direction);
        if (sig == m_inspSig) { syncProps(); return; }   // otherwise a spin box would be destroyed while the user drags it
    }
    m_inspSig = sig;
    while (QLayoutItem* it = m_propsLayout->takeAt(0)) {
        if (QWidget* w = it->widget()) { w->hide(); w->deleteLater(); }
        delete it;
    }
    m_rows.clear(); m_fxSpins.clear(); m_choices.clear(); m_colourBtns.clear();
    m_speedSpin = m_fadeInSpin = m_fadeOutSpin = nullptr; m_muteCheck = nullptr; m_maskOn = m_maskInv = nullptr;
    m_textEdit = nullptr; m_fontBox = nullptr; m_fontSize = m_strokeW = nullptr; m_bold = m_italic = m_underline = nullptr; m_fillBtn = m_strokeBtn = nullptr;

    auto label = [this](const QString& html) { auto* l = new QLabel(html); l->setWordWrap(true); l->setTextFormat(Qt::RichText); m_propsLayout->addWidget(l); return l; };
    auto section = [&](const QString& t) { label(QString("<br><b>%1</b>").arg(t)); };

    if (!c) {
        m_preview->setMaskOutline(MaskFx());
        updateTransformBox();
        if (ids.size() > 1) label(QString("<b>%1 clips selected</b><br><span style='color:#8d8d93'>Select a single clip to edit its properties.<br>Delete removes them, Shift+Delete ripples.</span>").arg(ids.size()));
        else label(QString("<b>Project</b><br>%1 × %2 @ %3 fps<br>Length: %4 s<br>Clips: %5<br><br>"
                           "<span style='color:#8d8d93'>Click a clip, or drag a box in the timeline to select several.<br><br>"
                           "Space play · S split · F freeze frame<br>Del delete · Shift+Del ripple delete<br>Ctrl+A select all · Ctrl+wheel zoom<br><br>"
                           "Next to each value is a ◇ button: click it to add a keyframe at the playhead.<br><br>"
                           "Effects tab: clip effects go on a clip; effect layers and the adjustment layer go on a track and affect everything below.</span>")
                       .arg(m_project.settings.width).arg(m_project.settings.height).arg(m_project.settings.fps)
                       .arg(m_project.duration(), 0, 'f', 1).arg(m_project.clips.size()));
        m_propsLayout->addStretch(1);
        return;
    }

    const QString id = c->id;
    const MediaAsset* a = m_project.asset(c->assetId);
    const bool isText = c->kind == KindText;
    const bool isLayer = c->kind == KindEffectLayer || c->kind == KindAdjustment;

    // ---- effects with their own strength + parameters (shadow, border, blur, mirror, chroma key, black flash, adjustment)
    auto buildEffect = [&](const Effect& e, bool removable) {
        const EffectDef* d = effectDef(e.type);
        if (!d) return;
        const QString type = e.type;
        if (!isLayer) label(QString("<span style='color:#3d9a95'>●</span> <b>%1</b>").arg(d->label));
        addPropRow(m_propsLayout, id, d->strengthLabel, "fx." + type, 0, 100, 1, 0, 100, " %");
        bool hasColour = false;
        for (const ParamDef& pd : d->params) {
            if (pd.colour) { hasColour = true; continue; }
            if (!pd.choices.isEmpty()) {
                auto* row = new QWidget; auto* l = new QHBoxLayout(row); l->setContentsMargins(0, 0, 0, 0);
                auto* name = new QLabel(pd.label); name->setMinimumWidth(78);
                auto* box = new QComboBox; box->addItems(pd.choices);
                l->addWidget(name); l->addWidget(box, 1);
                m_propsLayout->addWidget(row);
                m_choices.push_back({box, type, pd.name});
                const QString pname = pd.name;
                connect(box, &QComboBox::activated, this, [this, id, type, pname](int idx) {
                    editClip(id, QString(), [&](Clip& n) { for (Effect& f : n.effects) if (f.type == type) f.p[pname] = idx; });
                });
                continue;
            }
            addPropRow(m_propsLayout, id, pd.label, "fx." + type + "." + pd.name, pd.lo * pd.mult, pd.hi * pd.mult, pd.mult > 1 ? 1.0 : pd.step, pd.decimals, pd.mult, pd.suffix);
        }
        if (type == "chromakey") {
            auto* pick = new QPushButton("Pick the colour to remove from the preview");
            pick->setToolTip("Click here, then click on the colour in the picture that should disappear");
            m_propsLayout->addWidget(pick);
            connect(pick, &QPushButton::clicked, this, [this, id] { startPick(id); });
        }
        if (hasColour) {
            auto* btn = new QPushButton("Colour…");
            m_propsLayout->addWidget(btn);
            m_colourBtns.push_back({btn, type});
            connect(btn, &QPushButton::clicked, this, [this, id, type] {
                const Clip* cur = m_project.clip(id);
                if (!cur) return;
                QColor start = Qt::white;
                for (const Effect& f : cur->effects) if (f.type == type)
                    start = QColor::fromRgbF(float(f.p.value("r")), float(f.p.value("g")), float(f.p.value("b")));
                const QColor picked = QColorDialog::getColor(start, this, "Choose a colour");
                if (!picked.isValid()) return;
                editClip(id, QString(), [&](Clip& n) { for (Effect& f : n.effects) if (f.type == type) { f.p["r"] = picked.redF(); f.p["g"] = picked.greenF(); f.p["b"] = picked.blueF(); } });
            });
        }
        if (removable) {
            auto* rm = new QPushButton("Remove " + d->label.toLower());
            m_propsLayout->addWidget(rm);
            connect(rm, &QPushButton::clicked, this, [this, id, type] {
                editClip(id, QString(), [&](Clip& n) {
                    n.effects.erase(std::remove_if(n.effects.begin(), n.effects.end(), [&](const Effect& f) { return f.type == type; }), n.effects.end());
                    for (auto it = n.keys.begin(); it != n.keys.end();) it = (it.key() == "fx." + type || it.key().startsWith("fx." + type + ".")) ? n.keys.erase(it) : std::next(it);
                });
            });
        }
    };

    // ---- rectangle mask: crops a picture, or limits where an effect / adjustment layer applies
    auto maskSection = [&](const QString& title, const QString& hint) {
        section(title);
        label(QString("<span style='color:#8d8d93'>%1</span>").arg(hint));
        m_maskOn = new QCheckBox("Enable rectangle mask");
        m_propsLayout->addWidget(m_maskOn);
        connect(m_maskOn, &QCheckBox::toggled, this, [this, id](bool on) {
            editClip(id, QString(), [&](Clip& n) {
                n.mask.on = on;
                if (on && n.mask.w <= 0) { n.mask.w = m_project.settings.width / 2.0; n.mask.h = m_project.settings.height / 2.0; n.mask.feather = 30; }
            });
        });
        if (!c->mask.on) return;
        m_maskInv = new QCheckBox("Invert (apply outside the rectangle)");
        m_propsLayout->addWidget(m_maskInv);
        connect(m_maskInv, &QCheckBox::toggled, this, [this, id](bool on) { editClip(id, QString(), [on](Clip& n) { n.mask.invert = on; }); });
        addPropRow(m_propsLayout, id, "Position X", "mask.x", -20000, 20000, 1, 0, 1, " px");
        addPropRow(m_propsLayout, id, "Position Y", "mask.y", -20000, 20000, 1, 0, 1, " px");
        addPropRow(m_propsLayout, id, "Width", "mask.w", 0, 32000, 1, 0, 1, " px");
        addPropRow(m_propsLayout, id, "Height", "mask.h", 0, 32000, 1, 0, 1, " px");
        addPropRow(m_propsLayout, id, "Rotation", "mask.rot", -7200, 7200, 1, 1, 1, " °");
        addPropRow(m_propsLayout, id, "Feather", "mask.feather", 0, 5000, 1, 0, 1, " px");
    };

    if (isLayer) {
        label(QString("<b>%1</b><br><span style='color:#8d8d93'>Track %2 · start %3 s · length %4 s<br>Acts on everything below it on the timeline. Drag its edges to choose where it applies.</span>")
                  .arg(c->kind == KindAdjustment ? "Adjustment layer" : "Effect layer", m_project.tracks[c->track].name).arg(c->start, 0, 'f', 2).arg(c->dur, 0, 'f', 2));
        section(c->kind == KindAdjustment ? "Adjustments" : "Effect");
        for (const Effect& e : c->effects) buildEffect(e, false);
        maskSection("Where it applies", "Turn on the mask to limit it to a rectangle: for example to blur only a small part of the picture.");
        m_propsLayout->addStretch(1);
        syncProps();
        return;
    }

    label(QString("<b>%1</b><br>%2<br><span style='color:#8d8d93'>Track %3 · start %4 s · length %5 s</span>")
              .arg(isText ? "Text" : (c->freeze ? "Freeze frame" : "Clip"), isText ? c->text.text.section('\n', 0, 0) : (a ? a->name() : "?"), m_project.tracks[c->track].name)
              .arg(c->start, 0, 'f', 2).arg(c->dur, 0, 'f', 2));
    if (c->freeze) label("<span style='color:#8d8d93'>Holds one frame. Drag the clip's right edge to change how long.</span>");

    if (isText) {
        section("Text");
        m_textEdit = new QPlainTextEdit;
        m_textEdit->setFixedHeight(76);
        m_textEdit->setPlaceholderText("Type your text…");
        m_propsLayout->addWidget(m_textEdit);
        connect(m_textEdit, &QPlainTextEdit::textChanged, this, [this, id] {
            const QString t = m_textEdit ? m_textEdit->toPlainText() : QString();
            editClip(id, "text", [&](Clip& n) { n.text.text = t; });
        });
        auto row = [&](const QString& name, QWidget* w) {
            auto* r = new QWidget; auto* l = new QHBoxLayout(r); l->setContentsMargins(0, 0, 0, 0);
            auto* nm = new QLabel(name); nm->setMinimumWidth(78);
            l->addWidget(nm); l->addWidget(w, 1);
            m_propsLayout->addWidget(r);
        };
        m_fontBox = new QFontComboBox;
        m_fontBox->setFocusPolicy(Qt::StrongFocus);
        row("Font", m_fontBox);
        connect(m_fontBox, &QFontComboBox::currentFontChanged, this, [this, id](const QFont& f) { editClip(id, "font", [&](Clip& n) { n.text.font = f.family(); }); });
        m_fontSize = new QDoubleSpinBox;
        m_fontSize->setRange(1, 3000); m_fontSize->setDecimals(0); m_fontSize->setSingleStep(1); m_fontSize->setSuffix(" px");
        m_fontSize->setKeyboardTracking(false); m_fontSize->setFocusPolicy(Qt::StrongFocus);
        row("Font size", m_fontSize);
        connect(m_fontSize, &QDoubleSpinBox::valueChanged, this, [this, id](double v) { editClip(id, "fsize", [&](Clip& n) { n.text.size = v; }); });
        auto* styleRow = new QWidget; auto* sl = new QHBoxLayout(styleRow); sl->setContentsMargins(0, 0, 0, 0);
        m_bold = new QCheckBox("Bold"); m_italic = new QCheckBox("Italic"); m_underline = new QCheckBox("Underline");
        for (QCheckBox* b : {m_bold, m_italic, m_underline}) sl->addWidget(b);
        m_propsLayout->addWidget(styleRow);
        connect(m_bold, &QCheckBox::toggled, this, [this, id](bool on) { editClip(id, QString(), [on](Clip& n) { n.text.bold = on; }); });
        connect(m_italic, &QCheckBox::toggled, this, [this, id](bool on) { editClip(id, QString(), [on](Clip& n) { n.text.italic = on; }); });
        connect(m_underline, &QCheckBox::toggled, this, [this, id](bool on) { editClip(id, QString(), [on](Clip& n) { n.text.underline = on; }); });
        m_fillBtn = new QPushButton("Text colour…");
        m_propsLayout->addWidget(m_fillBtn);
        connect(m_fillBtn, &QPushButton::clicked, this, [this, id] {
            const Clip* cur = m_project.clip(id);
            if (!cur) return;
            const QColor col = QColorDialog::getColor(QColor::fromRgbF(float(cur->text.r), float(cur->text.g), float(cur->text.b)), this, "Text colour");
            if (col.isValid()) editClip(id, QString(), [&](Clip& n) { n.text.r = col.redF(); n.text.g = col.greenF(); n.text.b = col.blueF(); });
        });
        m_strokeW = new QDoubleSpinBox;
        m_strokeW->setRange(0, 500); m_strokeW->setDecimals(0); m_strokeW->setSingleStep(1); m_strokeW->setSuffix(" px");
        m_strokeW->setKeyboardTracking(false); m_strokeW->setFocusPolicy(Qt::StrongFocus);
        row("Outline width", m_strokeW);
        connect(m_strokeW, &QDoubleSpinBox::valueChanged, this, [this, id](double v) { editClip(id, "stroke", [&](Clip& n) { n.text.strokeW = v; }); });
        m_strokeBtn = new QPushButton("Outline colour…");
        m_propsLayout->addWidget(m_strokeBtn);
        connect(m_strokeBtn, &QPushButton::clicked, this, [this, id] {
            const Clip* cur = m_project.clip(id);
            if (!cur) return;
            const QColor col = QColorDialog::getColor(QColor::fromRgbF(float(cur->text.sr), float(cur->text.sg), float(cur->text.sb)), this, "Outline colour");
            if (col.isValid()) editClip(id, QString(), [&](Clip& n) { n.text.sr = col.redF(); n.text.sg = col.greenF(); n.text.sb = col.blueF(); });
        });
    }
    if ((a && a->hasVideo) || isText) {
        section(isText ? "Position, size and look" : "Video");
        addPropRow(m_propsLayout, id, "Position X", "x", -20000, 20000, 1, 0, 1, " px");
        addPropRow(m_propsLayout, id, "Position Y", "y", -20000, 20000, 1, 0, 1, " px");
        addPropRow(m_propsLayout, id, "Size", "scale", 0, 2000, 1, 1, 100, " %");
        addPropRow(m_propsLayout, id, "Rotation", "rotation", -7200, 7200, 1, 1, 1, " °");
        addPropRow(m_propsLayout, id, "Opacity", "opacity", 0, 100, 1, 1, 100, " %");
    }
    if (a && !c->freeze && !a->isImage) {
        section("Speed");
        m_speedSpin = new QDoubleSpinBox;
        m_speedSpin->setRange(kMinSpeed, kMaxSpeed); m_speedSpin->setDecimals(2); m_speedSpin->setSingleStep(0.1); m_speedSpin->setSuffix(" ×");
        m_speedSpin->setKeyboardTracking(false);
        m_propsLayout->addWidget(m_speedSpin);
        connect(m_speedSpin, &QDoubleSpinBox::valueChanged, this, [this, id](double v) {
            QString err;
            if (QUndoCommand* cmd = makeSetSpeedCommand(m_project, id, v, &err)) m_stack.push(cmd);
            else { if (!err.isEmpty()) statusBar()->showMessage(err, 6000); syncProps(); }
        });
    }
    if (a && a->hasAudio && !c->freeze && !a->isImage) {
        section(a->hasVideo ? "Audio of this video" : "Audio");
        addPropRow(m_propsLayout, id, "Volume", "volume", 0, 400, 1, 0, 100, " %");
        m_muteCheck = new QCheckBox("Mute");
        m_propsLayout->addWidget(m_muteCheck);
        connect(m_muteCheck, &QCheckBox::toggled, this, [this, id](bool on) { editClip(id, QString(), [on](Clip& n) { n.muted = on; }); });
        auto fade = [&](const QString& text, bool in) {
            auto* row = new QWidget; auto* l = new QHBoxLayout(row); l->setContentsMargins(0, 0, 0, 0);
            auto* name = new QLabel(text); name->setMinimumWidth(78);
            auto* s = new QDoubleSpinBox; s->setRange(0, 30); s->setDecimals(1); s->setSingleStep(0.1); s->setSuffix(" s"); s->setKeyboardTracking(false);
            l->addWidget(name); l->addWidget(s, 1);
            m_propsLayout->addWidget(row);
            connect(s, &QDoubleSpinBox::valueChanged, this, [this, id, in](double v) {
                editClip(id, in ? "fadeIn" : "fadeOut", [&](Clip& n) { (in ? n.fadeIn : n.fadeOut) = std::min(v, n.dur); });
            });
            return s;
        };
        m_fadeInSpin = fade("Fade in", true);
        m_fadeOutSpin = fade("Fade out", false);
    }
    if (((a && a->hasVideo) || isText) && m_project.tracks[c->track].kind == TrackKind::Video) {
        maskSection("Mask", "Crop this picture to a rectangle with soft edges.");
        if (!c->freeze || !c->effects.isEmpty()) {
            section("Effects");
            if (c->effects.isEmpty()) label("<span style='color:#8d8d93'>None. Open the Effects tab and drag a preset onto this clip.</span>");
        }
        for (const Effect& e : c->effects) {
            const QString type = e.type;
            if (effectDef(type)) { buildEffect(e, true); continue; }
            QWidget* removeHost = nullptr;
            if (type == "mirror") {
                label("<span style='color:#3d9a95'>●</span> <b>Mirror</b><br><span style='color:#8d8d93'>The picture is flipped left ↔ right.</span>");
            } else if (type == "shake") {
                label(QString("<span style='color:#d9822b'>●</span> %1").arg(effectLabel(e)));
                addPropRow(m_propsLayout, id, "Strength", "fx.shake", 0, 100, 1, 0, 100, " %");
            } else {
                auto* row = new QWidget; auto* l = new QHBoxLayout(row); l->setContentsMargins(0, 0, 0, 0);
                auto* name = new QLabel(QString("<span style='color:#8a5fd6'>●</span> %1").arg(effectLabel(e))); name->setTextFormat(Qt::RichText);
                auto* s = new QDoubleSpinBox; s->setRange(0.05, 10.0); s->setDecimals(2); s->setSingleStep(0.05); s->setSuffix(" s"); s->setKeyboardTracking(false);
                s->setToolTip("How long the swoosh lasts");
                l->addWidget(name, 1); l->addWidget(s);
                m_propsLayout->addWidget(row);
                m_fxSpins.insert(type, s);
                connect(s, &QDoubleSpinBox::valueChanged, this, [this, id, type](double v) {
                    editClip(id, "fx." + type, [&](Clip& n) { for (Effect& f : n.effects) if (f.type == type) f.duration = v; });
                });
                removeHost = row;
            }
            auto* rm = new QPushButton("Remove " + QString(type == "shake" ? "shake" : type == "mirror" ? "mirror" : type == "swoosh_in" ? "swoosh in" : "swoosh out"));
            m_propsLayout->addWidget(rm);
            (void)removeHost;
            connect(rm, &QPushButton::clicked, this, [this, id, type] {
                editClip(id, QString(), [&](Clip& n) {
                    n.effects.erase(std::remove_if(n.effects.begin(), n.effects.end(), [&](const Effect& f) { return f.type == type; }), n.effects.end());
                    if (type == "shake") n.keys.remove("fx.shake");
                });
            });
        }
    }
    m_propsLayout->addStretch(1);
    syncProps();
}

// Refresh the values (and keyframe diamonds) of the current inspector without rebuilding it.
void MainWindow::syncProps() {
    updateTransformBox();
    const QStringList ids = m_timeline->selectedIds();
    const Clip* c = ids.size() == 1 ? m_project.clip(ids[0]) : nullptr;
    if (!c) { m_preview->setMaskOutline(MaskFx()); return; }
    const double local = clipLocal(*c);
    const bool inside = m_pos >= c->start - 1e-6 && m_pos <= c->end() + 1e-6;
    for (PropRow& r : m_rows) {
        QSignalBlocker b(r.spin);
        r.spin->setValue(propValue(*c, r.prop, local) * r.mult);
        r.key->setState(hasKeyAt(*c, r.prop, local, keyTol()) ? 2 : (c->keys.value(r.prop).isEmpty() ? 0 : 1));
        r.key->setEnabled(inside);
    }
    if (m_speedSpin) { QSignalBlocker b(m_speedSpin); m_speedSpin->setValue(c->speed); }
    if (m_muteCheck) { QSignalBlocker b(m_muteCheck); m_muteCheck->setChecked(c->muted); }
    if (m_fadeInSpin) { QSignalBlocker b(m_fadeInSpin); m_fadeInSpin->setValue(c->fadeIn); }
    if (m_fadeOutSpin) { QSignalBlocker b(m_fadeOutSpin); m_fadeOutSpin->setValue(c->fadeOut); }
    if (m_textEdit) {
        const TextStyle& ts = c->text;
        if (m_textEdit->toPlainText() != ts.text) { QSignalBlocker b(m_textEdit); m_textEdit->setPlainText(ts.text); }
        if (m_fontBox && m_fontBox->currentFont().family() != QFont(ts.font).family()) { QSignalBlocker b(m_fontBox); m_fontBox->setCurrentFont(QFont(ts.font)); }
        if (m_fontSize) { QSignalBlocker b(m_fontSize); m_fontSize->setValue(ts.size); }
        if (m_strokeW) { QSignalBlocker b(m_strokeW); m_strokeW->setValue(ts.strokeW); }
        if (m_bold) { QSignalBlocker b(m_bold); m_bold->setChecked(ts.bold); }
        if (m_italic) { QSignalBlocker b(m_italic); m_italic->setChecked(ts.italic); }
        if (m_underline) { QSignalBlocker b(m_underline); m_underline->setChecked(ts.underline); }
        auto paint = [](QPushButton* btn, double r, double g, double bl) {
            if (!btn) return;
            const QColor col = QColor::fromRgbF(float(r), float(g), float(bl));
            btn->setStyleSheet(QString("background: %1; color: %2; border: 1px solid #666; padding: 4px;").arg(col.name(), col.lightness() > 128 ? "black" : "white"));
        };
        paint(m_fillBtn, ts.r, ts.g, ts.b);
        paint(m_strokeBtn, ts.sr, ts.sg, ts.sb);
    }
    if (m_maskOn) { QSignalBlocker b(m_maskOn); m_maskOn->setChecked(c->mask.on); }
    if (m_maskInv) { QSignalBlocker b(m_maskInv); m_maskInv->setChecked(c->mask.invert); }
    for (auto it = m_fxSpins.begin(); it != m_fxSpins.end(); ++it)
        for (const Effect& e : c->effects)
            if (e.type == it.key()) { QSignalBlocker b(it.value()); it.value()->setValue(e.duration); }
    for (const ChoiceRow& r : m_choices)
        for (const Effect& e : c->effects)
            if (e.type == r.type) { QSignalBlocker b(r.box); r.box->setCurrentIndex(int(std::lround(e.p.value(r.name, effectParamDefault(r.type, r.name))))); }
    for (const ColourBtn& cb : m_colourBtns)
        for (const Effect& e : c->effects)
            if (e.type == cb.type) {
                const QColor col = QColor::fromRgbF(float(e.p.value("r")), float(e.p.value("g")), float(e.p.value("b")));
                cb.btn->setStyleSheet(QString("background: %1; color: %2; border: 1px solid #666; padding: 4px;").arg(col.name(), col.lightness() > 128 ? "black" : "white"));
            }
    m_preview->setMaskOutline(c->mask.on ? resolveMask(*c, local) : MaskFx());   // yellow rectangle on the preview while editing a mask
}

// ---- export -----------------------------------------------------------------------------------

void MainWindow::exportDialog() {
    if (m_project.clips.isEmpty()) { QMessageBox::information(this, "Export", "The timeline is empty. Add some clips first."); return; }
    const QVector<EncoderChoice> encs = Exporter::availableEncoders();
    if (encs.isEmpty()) {
        QMessageBox::warning(this, "FFmpeg not found",
                             "Export needs FFmpeg. Install it (Linux: your package manager; Windows: put ffmpeg.exe next to this app or in PATH) and try again.");
        return;
    }
    stopPlay();
    QDialog dlg(this);
    dlg.setWindowTitle("Export");
    auto* form = new QFormLayout(&dlg);
    form->addRow("Video", new QLabel(QString("%1 × %2 @ %3 fps · %4 s  (change in File → Canvas / aspect ratio)")
                                         .arg(m_project.settings.width).arg(m_project.settings.height).arg(m_project.settings.fps).arg(m_project.duration(), 0, 'f', 1)));
    auto* encBox = new QComboBox;
    for (const EncoderChoice& e : encs) encBox->addItem(e.label, e.encoder);
    auto* qual = new QComboBox;
    qual->addItem("High", 18); qual->addItem("Medium", 23); qual->addItem("Small file", 28);
    qual->setCurrentIndex(1);
    auto* path = new QLineEdit(QDir::homePath() + "/export.mp4");
    auto* browse = new QPushButton("Browse…");
    auto* pathRow = new QHBoxLayout; pathRow->addWidget(path, 1); pathRow->addWidget(browse);
    connect(browse, &QPushButton::clicked, &dlg, [&] {
        const QString f = QFileDialog::getSaveFileName(&dlg, "Export to", path->text(), "MP4 (*.mp4);;MOV (*.mov);;MKV (*.mkv)");
        if (!f.isEmpty()) path->setText(f);
    });
    form->addRow("Encoder", encBox);
    form->addRow("Quality", qual);
    form->addRow("Save to", pathRow);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* go = bb->addButton("Export", QDialogButtonBox::AcceptRole);
    go->setDefault(true);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    ExportOptions o;
    o.outPath = path->text();
    o.encoder = encBox->currentData().toString();
    o.quality = qual->currentData().toInt();

    Exporter ex;
    QProgressDialog prog("Exporting…", "Cancel", 0, 1000, this);
    prog.setWindowModality(Qt::WindowModal);
    prog.setMinimumDuration(0);
    QEventLoop loop;
    bool ok = false; QString msg;
    connect(&ex, &Exporter::progress, &prog, [&](double f) { prog.setValue(int(f * 1000)); });
    connect(&ex, &Exporter::finished, &loop, [&](bool k, const QString& m) { ok = k; msg = m; loop.quit(); });
    connect(&prog, &QProgressDialog::canceled, &ex, &Exporter::cancel);
    QString err;
    if (!ex.start(m_project, o, &err)) { QMessageBox::warning(this, "Couldn't export", err); return; }
    loop.exec();
    prog.close();
    if (ok) {
        QString note;
        if (!ex.usedGpu() && m_project.usesEffects())
            note = "\n\nNote: this computer can't run the GPU renderer, so position, size, rotation, opacity, keyframes, shake and swoosh were not applied "
                   "(cuts, speed, freeze frames and audio were). Tip: on Linux you can try LIBGL_ALWAYS_SOFTWARE=1.";
        QMessageBox::information(this, "Export", msg + "\n\nSaved to:\n" + o.outPath + note);
    } else if (msg != "Export cancelled.") {
        QMessageBox::warning(this, "Couldn't export", msg);
    }
}

} // namespace ve

#include "MainWindow.moc"
