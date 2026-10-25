#include "Project.h"
#include "Effects.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <cmath>

namespace ve {

QString MediaAsset::name() const { return QFileInfo(path).fileName(); }

static QVector<Track> defaultTracks() {
    return { {"V3", TrackKind::Video}, {"V2", TrackKind::Video}, {"V1", TrackKind::Video}, {"A1", TrackKind::Audio}, {"A2", TrackKind::Audio} };
}

// ---- keyframes --------------------------------------------------------------------------------------------------

QStringList animatableProps() { return {"x", "y", "scale", "rotation", "opacity", "volume", "fx.shake"}; }

static QStringList splitProp(const QString& p) { return p.split('.'); }

double propBase(const Clip& c, const QString& p) {
    if (p.startsWith("mask.")) {
        const QString n = p.mid(5);
        return n == "x" ? c.mask.x : n == "y" ? c.mask.y : n == "w" ? c.mask.w : n == "h" ? c.mask.h : n == "rot" ? c.mask.rot : c.mask.feather;
    }
    if (p.startsWith("fx.")) {
        const QStringList parts = splitProp(p);
        for (const Effect& e : c.effects)
            if (e.type == parts[1]) return parts.size() == 2 ? e.amount : e.p.value(parts[2], effectParamDefault(e.type, parts[2]));
        return parts.size() == 2 ? 0.0 : effectParamDefault(parts[1], parts[2]);
    }
    if (p == "x") return c.x;
    if (p == "y") return c.y;
    if (p == "scale") return c.scale;
    if (p == "rotation") return c.rotation;
    if (p == "opacity") return c.opacity;
    if (p == "volume") return c.volume;
    return 0;
}

void setPropBase(Clip& c, const QString& p, double v) {
    if (p.startsWith("mask.")) {
        const QString n = p.mid(5);
        (n == "x" ? c.mask.x : n == "y" ? c.mask.y : n == "w" ? c.mask.w : n == "h" ? c.mask.h : n == "rot" ? c.mask.rot : c.mask.feather) = v;
        return;
    }
    if (p.startsWith("fx.")) {
        const QStringList parts = splitProp(p);
        for (Effect& e : c.effects)
            if (e.type == parts[1]) { if (parts.size() == 2) e.amount = v; else e.p[parts[2]] = v; }
        return;
    }
    if (p == "x") c.x = v;
    else if (p == "y") c.y = v;
    else if (p == "scale") c.scale = v;
    else if (p == "rotation") c.rotation = v;
    else if (p == "opacity") c.opacity = v;
    else if (p == "volume") c.volume = v;
}

double propValue(const Clip& c, const QString& p, double local) {
    const auto it = c.keys.constFind(p);
    if (it == c.keys.constEnd() || it->isEmpty()) return propBase(c, p);
    const QVector<Keyframe>& k = *it;
    if (local <= k.first().t) return k.first().v;
    if (local >= k.last().t) return k.last().v;
    for (int i = 1; i < k.size(); ++i) {
        if (local <= k[i].t) {
            const double span = k[i].t - k[i - 1].t;
            const double f = span > 0 ? (local - k[i - 1].t) / span : 1.0;
            return k[i - 1].v + (k[i].v - k[i - 1].v) * f;     // linear
        }
    }
    return k.last().v;
}

bool hasKeyAt(const Clip& c, const QString& p, double local, double tol) {
    for (const Keyframe& k : c.keys.value(p)) if (std::abs(k.t - local) <= tol) return true;
    return false;
}

void setKeyAt(Clip& c, const QString& p, double local, double v, double tol) {
    QVector<Keyframe>& k = c.keys[p];
    for (Keyframe& e : k) if (std::abs(e.t - local) <= tol) { e.v = v; return; }
    k.push_back({local, v});
    std::sort(k.begin(), k.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
}

void removeKeyAt(Clip& c, const QString& p, double local, double tol) {
    auto it = c.keys.find(p);
    if (it == c.keys.end()) return;
    QVector<Keyframe>& k = *it;
    double removed = 0; bool any = false;
    for (int i = 0; i < k.size(); ++i)
        if (std::abs(k[i].t - local) <= tol) { removed = k[i].v; k.removeAt(i); any = true; break; }
    if (any && k.isEmpty()) { c.keys.erase(it); setPropBase(c, p, removed); }   // no keys left: keep the value that was showing
}

// ---- queries ----------------------------------------------------------------------------------------------------

const MediaAsset* ProjectData::asset(const QString& id) const {
    auto it = assets.constFind(id);
    return it == assets.constEnd() ? nullptr : &it.value();
}

int ProjectData::indexOf(const QString& id) const {
    for (int i = 0; i < clips.size(); ++i)
        if (clips[i].id == id) return i;
    return -1;
}

const Clip* ProjectData::clip(const QString& id) const {
    const int i = indexOf(id);
    return i < 0 ? nullptr : &clips[i];
}

QVector<Clip> ProjectData::clipsOnTrack(int track) const {
    QVector<Clip> out;
    for (const Clip& c : clips)
        if (c.track == track) out.push_back(c);
    std::sort(out.begin(), out.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    return out;
}

double ProjectData::duration() const {
    double d = 0;
    for (const Clip& c : clips) d = std::max(d, c.end());
    return d;
}

bool ProjectData::overlaps(const Clip& c, const QSet<QString>& ignore) const {
    constexpr double eps = 1e-6;
    for (const Clip& o : clips) {
        if (o.id == c.id || o.track != c.track || ignore.contains(o.id)) continue;
        if (c.start < o.end() - eps && c.end() > o.start + eps) return true;
    }
    return false;
}

double ProjectData::findFreeStart(int track, double start, double dur) const {
    const QVector<Clip> list = clipsOnTrack(track);
    bool moved = true;
    while (moved) {
        moved = false;
        for (const Clip& c : list) {
            if (start < c.end() - 1e-6 && start + dur > c.start + 1e-6) { start = c.end(); moved = true; }
        }
    }
    return start;
}

QVector<ActiveLayer> ProjectData::videoLayersAt(double t) const {
    QVector<ActiveLayer> out;
    for (int tr = tracks.size() - 1; tr >= 0; --tr) {      // bottom track first
        if (tracks[tr].kind != TrackKind::Video || tracks[tr].hidden) continue;
        for (const Clip& c : clips) {
            if (c.track != tr || t < c.start || t >= c.end()) continue;
            const double local = t - c.start;
            ActiveLayer L;
            L.track = tr; L.clipId = c.id;
            L.passes = resolvePasses(c, local, settings);
            L.mask = resolveMask(c, local);
            if (c.kind == KindText) {                        // text object: drawn from its style, can carry effects like a picture
                L.isText = true;
                L.text = c.text;
                L.fx = evalLayer(c, local, settings);
                out.push_back(L);
                continue;
            }
            if (c.kind != KindMedia) {                       // effect / adjustment layer: no picture of its own
                if (L.passes.isEmpty()) continue;
                L.isPass = true;
                out.push_back(L);
                continue;
            }
            const MediaAsset* a = asset(c.assetId);
            if (!a || a->missing || !a->hasVideo) continue;
            L.path = a->path;
            L.sourceTime = c.freeze ? c.in : c.in + local * c.speed;
            L.fx = evalLayer(c, local, settings);
            out.push_back(L);
        }
    }
    return out;
}

bool ProjectData::usesEffects() const {
    for (const Clip& c : clips)
        if (c.freeze || c.kind != KindMedia || c.mask.on || !c.effects.isEmpty() || !c.keys.isEmpty() || c.x != 0 || c.y != 0 || c.scale != 1 || c.rotation != 0 || c.opacity != 1) return true;
    return false;
}

// ---- mutation ---------------------------------------------------------------------------------------------------

Project::Project(QObject* parent) : QObject(parent) { tracks = defaultTracks(); }

void Project::reset(const ProjectSettings& s) {
    settings = s;
    tracks = defaultTracks();
    assets.clear();
    folders.clear();
    clips.clear();
    filePath.clear();
    emit assetsChanged();
    emit settingsChanged();
    emit changed();
}

void Project::replace(const QVector<Clip>& from, const QVector<Clip>& to) {
    QSet<QString> keep;
    for (const Clip& c : to) keep.insert(c.id);
    for (const Clip& c : from) {
        if (keep.contains(c.id)) continue;
        const int i = indexOf(c.id);
        if (i >= 0) clips.removeAt(i);
    }
    for (const Clip& c : to) {
        const int i = indexOf(c.id);
        if (i >= 0) clips[i] = c; else clips.push_back(c);
    }
    emit changed();
}

void Project::addAsset(const MediaAsset& a) {
    assets.insert(a.id, a);
    emit assetsChanged();
}

void Project::addFolder(const Folder& f) {
    folders.push_back(f);
    emit assetsChanged();
}

LibraryState Project::library() const { return {assets, folders, clips}; }

void Project::setLibrary(const LibraryState& s) {
    assets = s.assets;
    folders = s.folders;
    clips = s.clips;
    emit assetsChanged();
    emit changed();
}

void Project::setTimeline(const TimelineState& s) {
    tracks = s.tracks;
    clips = s.clips;
    emit changed();
}

void Project::setTrackFlag(int track, int which, bool on) {
    if (track < 0 || track >= tracks.size()) return;
    Track& t = tracks[track];
    (which == 0 ? t.hidden : which == 1 ? t.locked : t.muted) = on;
    emit changed();
}

QStringList Project::missingMedia() const {
    QStringList out;
    for (const MediaAsset& a : assets)
        if (a.missing) out << a.path;
    return out;
}

// ---- save / load ------------------------------------------------------------------------------------------------

bool Project::save(const QString& path, QString* err) {
    QJsonObject root;
    root["version"] = 2;
    root["settings"] = QJsonObject{{"width", settings.width}, {"height", settings.height}, {"fps", settings.fps}};
    QJsonArray as;
    for (const MediaAsset& a : assets)
        as.append(QJsonObject{{"id", a.id}, {"path", a.path}, {"folderId", a.folderId}, {"duration", a.duration}, {"width", a.width},
                              {"height", a.height}, {"fps", a.fps}, {"hasVideo", a.hasVideo}, {"hasAudio", a.hasAudio},
                              {"isImage", a.isImage}, {"rotation", a.rotation}});
    root["assets"] = as;
    QJsonArray fs;
    for (const Folder& f : folders) fs.append(QJsonObject{{"id", f.id}, {"name", f.name}, {"parentId", f.parentId}});
    root["folders"] = fs;
    QJsonArray ts;
    for (const Track& t : tracks)
        ts.append(QJsonObject{{"name", t.name}, {"kind", t.kind == TrackKind::Video ? "video" : "audio"},
                              {"hidden", t.hidden}, {"locked", t.locked}, {"muted", t.muted}});
    root["tracks"] = ts;
    QJsonArray cs;
    for (const Clip& c : clips) {
        QJsonArray fx;
        for (const Effect& e : c.effects)
        {
            QJsonObject pj;
            for (auto it = e.p.constBegin(); it != e.p.constEnd(); ++it) pj[it.key()] = it.value();
            fx.append(QJsonObject{{"type", e.type}, {"amount", e.amount}, {"duration", e.duration}, {"direction", e.direction}, {"params", pj}});
        }
        QJsonObject keys;
        for (auto it = c.keys.constBegin(); it != c.keys.constEnd(); ++it) {
            QJsonArray arr;
            for (const Keyframe& k : it.value()) arr.append(QJsonArray{k.t, k.v});
            keys[it.key()] = arr;
        }
        cs.append(QJsonObject{{"id", c.id}, {"assetId", c.assetId}, {"track", c.track}, {"start", c.start}, {"in", c.in},
                              {"dur", c.dur}, {"freeze", c.freeze}, {"kind", c.kind},
                              {"text", QJsonObject{{"text", c.text.text}, {"font", c.text.font}, {"size", c.text.size}, {"bold", c.text.bold}, {"italic", c.text.italic},
                                                    {"underline", c.text.underline}, {"r", c.text.r}, {"g", c.text.g}, {"b", c.text.b}, {"strokeW", c.text.strokeW},
                                                    {"sr", c.text.sr}, {"sg", c.text.sg}, {"sb", c.text.sb}}},
                              {"mask", QJsonObject{{"on", c.mask.on}, {"invert", c.mask.invert}, {"x", c.mask.x}, {"y", c.mask.y}, {"w", c.mask.w},
                                                    {"h", c.mask.h}, {"rot", c.mask.rot}, {"feather", c.mask.feather}}}, {"x", c.x}, {"y", c.y}, {"scale", c.scale},
                              {"rotation", c.rotation}, {"opacity", c.opacity}, {"speed", c.speed}, {"volume", c.volume},
                              {"muted", c.muted}, {"fadeIn", c.fadeIn}, {"fadeOut", c.fadeOut}, {"effects", fx}, {"keys", keys}});
    }
    root["clips"] = cs;

    QSaveFile f(path);   // atomic write: a crash can never leave a half-written project
    if (!f.open(QIODevice::WriteOnly)) { if (err) *err = f.errorString(); return false; }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) { if (err) *err = f.errorString(); return false; }
    return true;
}

bool Project::load(const QString& path, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = f.errorString(); return false; }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (doc.isNull() || !doc.isObject()) { if (err) *err = "This isn't a valid project file (" + pe.errorString() + ")."; return false; }
    const QJsonObject root = doc.object();

    ProjectSettings s;
    const QJsonObject so = root["settings"].toObject();
    s.width = so["width"].toInt(1920); s.height = so["height"].toInt(1080); s.fps = so["fps"].toDouble(30.0);
    settings = s;

    assets.clear(); clips.clear(); tracks.clear(); folders.clear();
    const QDir projDir = QFileInfo(path).absoluteDir();
    for (const QJsonValue v : root["assets"].toArray()) {
        const QJsonObject o = v.toObject();
        MediaAsset a;
        a.id = o["id"].toString(); a.path = o["path"].toString(); a.folderId = o["folderId"].toString();
        a.duration = o["duration"].toDouble(); a.width = o["width"].toInt(); a.height = o["height"].toInt();
        a.fps = o["fps"].toDouble(); a.hasVideo = o["hasVideo"].toBool(); a.hasAudio = o["hasAudio"].toBool();
        a.isImage = o["isImage"].toBool(); a.rotation = o["rotation"].toInt();
        if (!QFileInfo::exists(a.path)) {   // auto-relink: same file name next to the project
            const QString alt = projDir.filePath(QFileInfo(a.path).fileName());
            if (QFileInfo::exists(alt)) a.path = alt;
        }
        a.missing = !QFileInfo::exists(a.path);
        assets.insert(a.id, a);
    }
    for (const QJsonValue v : root["folders"].toArray()) {
        const QJsonObject o = v.toObject();
        folders.push_back({o["id"].toString(), o["name"].toString(), o["parentId"].toString()});
    }
    for (const QJsonValue v : root["tracks"].toArray()) {
        const QJsonObject o = v.toObject();
        Track t;
        t.name = o["name"].toString();
        t.kind = o["kind"].toString() == "audio" ? TrackKind::Audio : TrackKind::Video;
        t.hidden = o["hidden"].toBool(); t.locked = o["locked"].toBool(); t.muted = o["muted"].toBool();
        tracks.push_back(t);
    }
    if (tracks.isEmpty()) tracks = defaultTracks();
    for (const QJsonValue v : root["clips"].toArray()) {
        const QJsonObject o = v.toObject();
        Clip c;
        c.id = o["id"].toString(); c.assetId = o["assetId"].toString(); c.track = o["track"].toInt();
        c.start = o["start"].toDouble(); c.in = o["in"].toDouble(); c.dur = o["dur"].toDouble();
        c.freeze = o["freeze"].toBool();
        c.kind = o["kind"].toInt();
        if (o.contains("text")) {
            const QJsonObject to = o["text"].toObject();
            c.text.text = to["text"].toString("Text"); c.text.font = to["font"].toString("Sans Serif"); c.text.size = to["size"].toDouble(120);
            c.text.bold = to["bold"].toBool(); c.text.italic = to["italic"].toBool(); c.text.underline = to["underline"].toBool();
            c.text.r = to["r"].toDouble(1); c.text.g = to["g"].toDouble(1); c.text.b = to["b"].toDouble(1); c.text.strokeW = to["strokeW"].toDouble();
            c.text.sr = to["sr"].toDouble(); c.text.sg = to["sg"].toDouble(); c.text.sb = to["sb"].toDouble();
        }
        { const QJsonObject mo = o["mask"].toObject();
          c.mask = {mo["on"].toBool(), mo["invert"].toBool(), mo["x"].toDouble(), mo["y"].toDouble(), mo["w"].toDouble(), mo["h"].toDouble(), mo["rot"].toDouble(), mo["feather"].toDouble()}; }
        c.x = o["x"].toDouble(); c.y = o["y"].toDouble(); c.scale = o["scale"].toDouble(1.0);
        c.rotation = o["rotation"].toDouble(); c.opacity = o["opacity"].toDouble(1.0);
        c.speed = o["speed"].toDouble(1.0); c.volume = o["volume"].toDouble(1.0);
        c.muted = o["muted"].toBool(); c.fadeIn = o["fadeIn"].toDouble(); c.fadeOut = o["fadeOut"].toDouble();
        for (const QJsonValue ev : o["effects"].toArray()) {
            const QJsonObject eo = ev.toObject();
            Effect e;
            e.type = eo["type"].toString(); e.amount = eo["amount"].toDouble(0.5);
            e.duration = eo["duration"].toDouble(0.45); e.direction = eo["direction"].toInt();
            const QJsonObject pj = eo["params"].toObject();
            for (auto it = pj.begin(); it != pj.end(); ++it) e.p.insert(it.key(), it.value().toDouble());
            c.effects.push_back(e);
        }
        const QJsonObject ko = o["keys"].toObject();
        for (auto it = ko.begin(); it != ko.end(); ++it) {
            QVector<Keyframe> kv;
            for (const QJsonValue kk : it.value().toArray()) { const QJsonArray a = kk.toArray(); kv.push_back({a.at(0).toDouble(), a.at(1).toDouble()}); }
            if (!kv.isEmpty()) c.keys.insert(it.key(), kv);
        }
        if ((c.kind != KindMedia || assets.contains(c.assetId)) && c.track >= 0 && c.track < tracks.size()) clips.push_back(c);
    }
    filePath = path;
    emit assetsChanged(); emit settingsChanged(); emit changed();
    return true;
}

} // namespace ve
