#pragma once
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVector>

namespace ve {

inline QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

enum class TrackKind { Video, Audio };

// A bin in the media panel. parentId empty = top level.
struct Folder {
    QString id, name, parentId;
};

// A source file. The original is never modified (non-destructive editing).
struct MediaAsset {
    QString id;
    QString path;
    QString folderId;             // media-panel folder ("" = top level)
    double duration = 0;          // seconds; 0 for still images
    int width = 0, height = 0;    // display size (rotation already applied)
    double fps = 0;
    bool hasVideo = false, hasAudio = false, isImage = false;
    int rotation = 0;             // clockwise degrees from display-matrix metadata
    bool missing = false;
    QString name() const;
};

struct Track {
    QString name;
    TrackKind kind = TrackKind::Video;
    bool hidden = false;   // not rendered / not heard
    bool locked = false;   // can't be edited
    bool muted = false;    // audio off (picture stays)
};

// A per-clip effect. type: "shake" | "swoosh_in" | "swoosh_out".
struct Effect {
    QString type;
    double amount = 0.5;      // shake intensity 0..1
    double duration = 0.45;   // swoosh length in seconds
    int direction = 0;        // swoosh side: 0 left, 1 right, 2 top, 3 bottom
    QMap<QString, double> p;  // named parameters of the newer effects (shadow offset, blur softness, key colour, adjustment values ...)
    bool operator==(const Effect& o) const {
        return type == o.type && amount == o.amount && duration == o.duration && direction == o.direction && p == o.p;
    }
};

// Rectangle mask on a clip (project pixels, relative to the canvas centre). On a picture it crops it;
// on an effect or adjustment layer it limits where the effect applies.
struct Mask {
    bool on = false, invert = false;
    double x = 0, y = 0, w = 0, h = 0, rot = 0, feather = 0;
    bool operator==(const Mask& o) const {
        return on == o.on && invert == o.invert && x == o.x && y == o.y && w == o.w && h == o.h && rot == o.rot && feather == o.feather;
    }
};

enum ClipKind { KindMedia = 0, KindEffectLayer = 1, KindAdjustment = 2, KindText = 3 };

// A text object. Position, size, rotation and opacity are the clip's own (so they can be keyframed and dragged in the preview).
struct TextStyle {
    QString text = "Text";
    QString font = "Sans Serif";
    double size = 120;                   // pixels, in project coordinates
    bool bold = false, italic = false, underline = false;
    double r = 1, g = 1, b = 1;          // fill colour
    double strokeW = 0;                  // outline width in pixels (0 = none)
    double sr = 0, sg = 0, sb = 0;       // outline colour
    bool operator==(const TextStyle& o) const {
        return text == o.text && font == o.font && size == o.size && bold == o.bold && italic == o.italic && underline == o.underline &&
               r == o.r && g == o.g && b == o.b && strokeW == o.strokeW && sr == o.sr && sg == o.sg && sb == o.sb;
    }
};

struct Keyframe {
    double t = 0;   // seconds from the start of the clip (timeline seconds, independent of speed)
    double v = 0;
    bool operator==(const Keyframe& o) const { return t == o.t && v == o.v; }
};
using KeyMap = QMap<QString, QVector<Keyframe>>;   // property name → keyframes sorted by time

// A clip is only an instruction: "show source X from `in` for `dur` seconds, starting at `start`".
struct Clip {
    QString id, assetId;
    int track = 0;
    double start = 0;    // position on the timeline
    double in = 0;       // in-point inside the source (for a freeze clip: the frozen frame)
    double dur = 0;      // length on the timeline (the source range used is dur * speed)
    bool freeze = false; // hold the frame at `in` for `dur` seconds (silent)
    int kind = KindMedia;  // media clip, effect / adjustment layer (no media; effects[0] is what it does), or a text object
    TextStyle text;        // used when kind == KindText
    Mask mask;
    // transform (x/y in project pixels from the centre, scale 1 = fit to canvas)
    double x = 0, y = 0, scale = 1, rotation = 0, opacity = 1;
    double speed = 1;
    // audio
    double volume = 1;
    bool muted = false;
    double fadeIn = 0, fadeOut = 0;
    QVector<Effect> effects;
    KeyMap keys;
    double end() const { return start + dur; }
    bool operator==(const Clip& o) const {
        return id == o.id && assetId == o.assetId && track == o.track && start == o.start && in == o.in && dur == o.dur &&
               freeze == o.freeze && kind == o.kind && text == o.text && mask == o.mask && x == o.x && y == o.y && scale == o.scale && rotation == o.rotation && opacity == o.opacity &&
               speed == o.speed && volume == o.volume && muted == o.muted && fadeIn == o.fadeIn && fadeOut == o.fadeOut &&
               effects == o.effects && keys == o.keys;
    }
    bool operator!=(const Clip& o) const { return !(*this == o); }
};

// ---- animatable properties: "x" "y" "scale" "rotation" "opacity" "volume" "fx.shake" -----------------------------
QStringList animatableProps();
double propBase(const Clip& c, const QString& prop);
void setPropBase(Clip& c, const QString& prop, double v);
double propValue(const Clip& c, const QString& prop, double local);   // keyframed value if the property has keys, else the base
bool hasKeyAt(const Clip& c, const QString& prop, double local, double tol);
void setKeyAt(Clip& c, const QString& prop, double local, double v, double tol);
void removeKeyAt(Clip& c, const QString& prop, double local, double tol);   // removing the last key bakes its value into the base

struct ProjectSettings {
    int width = 1920, height = 1080;
    double fps = 30.0;
};

// Result of evaluating a clip's transform + effects at one instant. Distances are in project pixels.
struct LayerFx {
    double dx = 0, dy = 0;          // translation
    double scale = 1;               // uniform zoom
    double rotation = 0;            // degrees, clockwise
    double blurX = 0, blurY = 0;    // motion-blur vector (full shutter length)
    double opacity = 1;
    bool flipX = false;             // mirror effect: the picture is flipped left <-> right
};

// One effect, fully evaluated (keyframes resolved) for a single instant. This is what the GPU consumes.
struct FxPass {
    QString type;                 // shadow border blur mirror chromakey blackflash adjust
    double strength = 1;          // 0..1
    QMap<QString, double> p;
};

// Evaluated mask (project pixels).
struct MaskFx {
    bool on = false, invert = false;
    double x = 0, y = 0, w = 0, h = 0, rot = 0, feather = 0;
};

// What must be drawn at a given timeline time (bottom layer first).
struct ActiveLayer {
    QString path;                 // empty for effect / adjustment layers
    double sourceTime = 0;
    int track = 0;
    QString clipId;
    LayerFx fx;
    QVector<FxPass> passes;       // effects to run (on the picture itself, or on everything below for a layer)
    MaskFx mask;
    bool isPass = false;          // effect / adjustment layer: acts on the composite of everything beneath
    TextStyle text;
    bool isText = false;          // a text object: drawn from `text`, no media file
};

// Plain, copyable project state + read-only queries. The exporter and audio engine work on snapshots of this.
struct ProjectData {
    ProjectSettings settings;
    QVector<Track> tracks;            // stored in display order: V2, V1, A1, A2 (lower index = on top)
    QMap<QString, MediaAsset> assets;
    QVector<Folder> folders;
    QVector<Clip> clips;

    int indexOf(const QString& id) const;
    const MediaAsset* asset(const QString& id) const;
    const Clip* clip(const QString& id) const;
    QVector<Clip> clipsOnTrack(int track) const;   // sorted by start
    double duration() const;
    bool overlaps(const Clip& c, const QSet<QString>& ignore = {}) const;
    double findFreeStart(int track, double start, double dur) const;
    QVector<ActiveLayer> videoLayersAt(double t) const;
    bool usesEffects() const;
};

// Everything the media panel can change (assets, folders, and the clips that depend on them).
struct LibraryState {
    QMap<QString, MediaAsset> assets;
    QVector<Folder> folders;
    QVector<Clip> clips;
};

// Tracks and the clips on them (changed together when tracks are added or removed).
struct TimelineState {
    QVector<Track> tracks;
    QVector<Clip> clips;
};

class Project : public QObject, public ProjectData {
    Q_OBJECT
public:
    explicit Project(QObject* parent = nullptr);

    QString filePath;

    void reset(const ProjectSettings& s = ProjectSettings());

    // Core mutation used by every undoable clip command: remove clips in `from` that are not in `to`, upsert `to`.
    void replace(const QVector<Clip>& from, const QVector<Clip>& to);
    void addAsset(const MediaAsset& a);
    void addFolder(const Folder& f);
    TimelineState timeline() const { return {tracks, clips}; }
    void setTimeline(const TimelineState& s);
    LibraryState library() const;
    void setLibrary(const LibraryState& s);
    void setTrackFlag(int track, int which, bool on);   // which: 0 hidden, 1 locked, 2 muted
    void notifySettingsChanged() { emit settingsChanged(); }

    bool save(const QString& path, QString* err = nullptr);
    bool load(const QString& path, QString* err = nullptr);
    QStringList missingMedia() const;

signals:
    void changed();
    void assetsChanged();
    void settingsChanged();
};

} // namespace ve
