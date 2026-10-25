#include "Commands.h"
#include <algorithm>
#include <cmath>

namespace ve {

constexpr double kStillDuration = 5.0;   // default length of an image clip

TrackFlagCommand::TrackFlagCommand(Project* p, int track, int which, bool on)
    : QUndoCommand(which == 0 ? (on ? "Hide track" : "Show track") : which == 1 ? (on ? "Lock track" : "Unlock track") : (on ? "Mute track" : "Unmute track")),
      m_p(p), m_track(track), m_which(which), m_new(on) {}

static bool isLocked(const Project& p, const Clip& c) { return c.track >= 0 && c.track < p.tracks.size() && p.tracks[c.track].locked; }

// A swoosh-in belongs to the start of a clip and a swoosh-out to its end, so splitting hands each to the right half.
static void dropEffect(Clip& c, const char* type) {
    c.effects.erase(std::remove_if(c.effects.begin(), c.effects.end(), [&](const Effect& e) { return e.type == type; }), c.effects.end());
}

// Split a clip's keyframes at clip-local time `local`: both halves get a key at the cut so the value is continuous.
static void splitKeys(const Clip& c, double local, Clip* left, Clip* right) {
    left->keys.clear(); right->keys.clear();
    for (auto it = c.keys.constBegin(); it != c.keys.constEnd(); ++it) {
        const double v = propValue(c, it.key(), local);
        QVector<Keyframe> l, r;
        for (const Keyframe& k : it.value()) {
            if (k.t < local - 1e-6) l.push_back(k);
            else if (k.t > local + 1e-6) r.push_back({k.t - local, k.v});
        }
        l.push_back({local, v});
        r.prepend({0.0, v});
        left->keys.insert(it.key(), l);
        right->keys.insert(it.key(), r);
    }
}

QUndoCommand* makeSplitCommand(Project& p, const QStringList& ids, double t) {
    constexpr double eps = 1e-3;
    QVector<Clip> before, after;
    for (const QString& id : ids) {
        const Clip* c = p.clip(id);
        if (!c || isLocked(p, *c) || t <= c->start + eps || t >= c->end() - eps) continue;
        const double local = t - c->start;
        Clip left = *c, right = *c;
        left.dur = local;
        dropEffect(left, "swoosh_out");
        dropEffect(right, "swoosh_in");
        right.id = newId();
        right.start = t;
        right.in = c->freeze ? c->in : c->in + local * c->speed;
        right.dur = c->end() - t;
        left.fadeOut = 0; right.fadeIn = 0;           // fades belong to the outer ends
        splitKeys(*c, local, &left, &right);
        before.push_back(*c);
        after.push_back(left);
        after.push_back(right);
    }
    if (before.isEmpty()) return nullptr;
    return new ChangeClipsCommand(&p, before, after, "Split clip");
}

QUndoCommand* makeDeleteCommand(Project& p, const QStringList& ids, bool ripple) {
    QVector<Clip> removed;
    QSet<QString> removedIds;
    for (const QString& id : ids)
        if (const Clip* c = p.clip(id))
            if (!isLocked(p, *c)) { removed.push_back(*c); removedIds.insert(id); }
    if (removed.isEmpty()) return nullptr;

    QVector<Clip> before = removed, after;
    if (ripple) {
        for (const Clip& c : p.clips) {
            if (removedIds.contains(c.id) || isLocked(p, c)) continue;
            double shift = 0;
            for (const Clip& r : removed)
                if (r.track == c.track && r.end() <= c.start + 1e-6) shift += r.dur;
            if (shift > 0) {
                Clip n = c;
                n.start = std::max(0.0, c.start - shift);
                before.push_back(c);
                after.push_back(n);
            }
        }
    }
    return new ChangeClipsCommand(&p, before, after, ripple ? "Ripple delete" : "Delete clip");
}

QUndoCommand* makeAddClipCommand(Project& p, const QString& assetId, int track, double start) {
    const MediaAsset* a = p.asset(assetId);
    if (!a || track < 0 || track >= p.tracks.size() || a->missing || p.tracks[track].locked) return nullptr;
    const TrackKind kind = p.tracks[track].kind;
    if (kind == TrackKind::Video && !a->hasVideo) return nullptr;
    if (kind == TrackKind::Audio && (a->hasVideo || !a->hasAudio)) return nullptr;

    Clip c;
    c.id = newId();
    c.assetId = assetId;
    c.track = track;
    c.in = 0;
    c.dur = a->isImage ? kStillDuration : a->duration;
    if (c.dur <= 0) return nullptr;
    c.start = p.findFreeStart(track, std::max(0.0, start), c.dur);
    return new ChangeClipsCommand(&p, {}, {c}, "Add clip");
}

QUndoCommand* makeSetEffectsCommand(Project& p, const QString& id, const QVector<Effect>& effects, const QString& text) {
    const Clip* c = p.clip(id);
    if (!c || isLocked(p, *c) || c->effects == effects) return nullptr;
    Clip n = *c;
    n.effects = effects;
    // keyframes of effects that no longer exist are dropped
    for (auto it = n.keys.begin(); it != n.keys.end();) {
        bool alive = !it.key().startsWith("fx.");
        for (const Effect& e : effects) alive = alive || it.key() == "fx." + e.type || it.key().startsWith("fx." + e.type + ".");
        it = alive ? std::next(it) : n.keys.erase(it);
    }
    return new ChangeClipsCommand(&p, {*c}, {n}, text);
}

QUndoCommand* makeApplyEffectCommand(Project& p, const QStringList& ids, const Effect& e) {
    QVector<Clip> before, after;
    for (const QString& id : ids) {
        const Clip* c = p.clip(id);
        if (!c || isLocked(p, *c) || (c->kind != KindMedia && c->kind != KindText) || p.tracks[c->track].kind != TrackKind::Video) continue;
        Clip n = *c;
        dropEffect(n, e.type.toUtf8().constData());
        for (auto it = n.keys.begin(); it != n.keys.end();)         // a fresh effect starts without old animation
            it = (it.key() == "fx." + e.type || it.key().startsWith("fx." + e.type + ".")) ? n.keys.erase(it) : std::next(it);
        n.effects.push_back(e);
        before.push_back(*c);
        after.push_back(n);
    }
    if (before.isEmpty()) return nullptr;
    return new ChangeClipsCommand(&p, before, after, "Apply effect");
}

QUndoCommand* makeFreezeCommand(Project& p, const QStringList& ids, double t, double hold) {
    constexpr double eps = 1e-3;
    for (const QString& id : ids) {
        const Clip* c = p.clip(id);
        if (!c || c->freeze || isLocked(p, *c) || p.tracks[c->track].kind != TrackKind::Video) continue;
        const MediaAsset* a = p.asset(c->assetId);
        if (!a || !a->hasVideo || a->isImage) continue;
        if (t < c->start - eps || t >= c->end() - eps) continue;

        const double local = std::max(0.0, t - c->start);
        const double frozenSrc = c->in + local * c->speed;
        QVector<Clip> before{*c}, after;
        Clip f = *c;                       // the freeze clip: plain transform, no audio, no effects
        f.id = newId(); f.freeze = true; f.effects.clear(); f.keys.clear(); f.dur = hold; f.speed = 1;
        f.fadeIn = f.fadeOut = 0;
        if (local > eps) {
            Clip left = *c, right = *c;
            left.dur = local;
            dropEffect(left, "swoosh_out");
            right.id = newId(); right.start = t + hold; right.in = frozenSrc; right.dur = c->end() - t;
            dropEffect(right, "swoosh_in");
            left.fadeOut = 0; right.fadeIn = 0;
            splitKeys(*c, local, &left, &right);
            f.start = t; f.in = frozenSrc;
            after = {left, f, right};
        } else {                           // freeze on the very first frame: insert before the clip
            Clip moved = *c;
            moved.start = c->start + hold;
            f.start = c->start; f.in = c->in;
            after = {f, moved};
        }
        for (const Clip& k : p.clips) {    // ripple the rest of this track
            if (k.id == c->id || k.track != c->track || k.start < c->end() - 1e-6 || isLocked(p, k)) continue;
            Clip n = k;
            n.start += hold;
            before.push_back(k);
            after.push_back(n);
        }
        return new ChangeClipsCommand(&p, before, after, "Freeze frame");
    }
    return nullptr;
}

QUndoCommand* makeSetClipCommand(Project& p, const Clip& updated, const QString& text, const QString& mergeKey) {
    const Clip* c = p.clip(updated.id);
    if (!c || isLocked(p, *c) || *c == updated) return nullptr;
    return new ChangeClipsCommand(&p, {*c}, {updated}, text, mergeKey);
}

QUndoCommand* makeSetSpeedCommand(Project& p, const QString& id, double speed, QString* err) {
    const Clip* c = p.clip(id);
    if (!c || c->freeze || isLocked(p, *c)) return nullptr;
    const MediaAsset* a = p.asset(c->assetId);
    if (!a || a->isImage) return nullptr;
    speed = std::clamp(speed, kMinSpeed, kMaxSpeed);
    if (std::abs(speed - c->speed) < 1e-9) return nullptr;
    Clip n = *c;
    n.speed = speed;
    n.dur = c->dur * c->speed / speed;                 // same source range, new length
    const double ratio = n.dur / c->dur;
    for (auto it = n.keys.begin(); it != n.keys.end(); ++it)
        for (Keyframe& k : it.value()) k.t *= ratio;   // keyframes keep their relative position in the clip
    n.fadeIn = std::min(n.fadeIn, n.dur); n.fadeOut = std::min(n.fadeOut, n.dur);
    if (p.overlaps(n)) {
        if (err) *err = "There isn't room for that speed: the clip would run into the next one. Move the next clip first.";
        return nullptr;
    }
    return new ChangeClipsCommand(&p, {*c}, {n}, "Change speed", "speed|" + id);
}

// ---- media library ----------------------------------------------------------------------------------------------

static void collectDescendants(const QVector<Folder>& folders, const QString& root, QSet<QString>* out) {
    out->insert(root);
    for (const Folder& f : folders)
        if (f.parentId == root && !out->contains(f.id)) collectDescendants(folders, f.id, out);
}

QUndoCommand* makeRemoveMediaCommand(Project& p, const QStringList& assetIds, const QStringList& folderIds) {
    LibraryState before = p.library(), after = before;
    QSet<QString> doomedFolders;
    for (const QString& f : folderIds) collectDescendants(p.folders, f, &doomedFolders);
    QSet<QString> doomedAssets(assetIds.begin(), assetIds.end());
    for (const MediaAsset& a : p.assets)
        if (doomedFolders.contains(a.folderId)) doomedAssets.insert(a.id);
    if (doomedAssets.isEmpty() && doomedFolders.isEmpty()) return nullptr;
    for (const QString& id : doomedAssets) after.assets.remove(id);
    after.folders.erase(std::remove_if(after.folders.begin(), after.folders.end(), [&](const Folder& f) { return doomedFolders.contains(f.id); }), after.folders.end());
    after.clips.erase(std::remove_if(after.clips.begin(), after.clips.end(), [&](const Clip& c) { return doomedAssets.contains(c.assetId); }), after.clips.end());
    return new LibraryCommand(&p, before, after, "Remove media");
}

QUndoCommand* makeMoveToFolderCommand(Project& p, const QStringList& assetIds, const QStringList& folderIds, const QString& target) {
    LibraryState before = p.library(), after = before;
    bool changed = false;
    for (const QString& id : assetIds) {
        auto it = after.assets.find(id);
        if (it != after.assets.end() && it->folderId != target) { it->folderId = target; changed = true; }
    }
    for (const QString& id : folderIds) {
        QSet<QString> sub;
        collectDescendants(p.folders, id, &sub);
        if (sub.contains(target)) continue;               // can't move a folder into itself
        for (Folder& f : after.folders)
            if (f.id == id && f.parentId != target) { f.parentId = target; changed = true; }
    }
    if (!changed) return nullptr;
    return new LibraryCommand(&p, before, after, "Move to folder");
}

QUndoCommand* makeNewFolderCommand(Project& p, const QString& name, const QString& parentId, QString* newFolderId) {
    LibraryState before = p.library(), after = before;
    Folder f{newId(), name.trimmed().isEmpty() ? "New folder" : name.trimmed(), parentId};
    after.folders.push_back(f);
    if (newFolderId) *newFolderId = f.id;
    return new LibraryCommand(&p, before, after, "New folder");
}

QUndoCommand* makeRenameFolderCommand(Project& p, const QString& folderId, const QString& name) {
    LibraryState before = p.library(), after = before;
    bool changed = false;
    for (Folder& f : after.folders)
        if (f.id == folderId && f.name != name.trimmed() && !name.trimmed().isEmpty()) { f.name = name.trimmed(); changed = true; }
    return changed ? new LibraryCommand(&p, before, after, "Rename folder") : nullptr;
}

} // namespace ve

namespace ve {

QUndoCommand* makeAddLayerCommand(Project& p, const Effect& e, int track, double start) {
    if (track < 0 || track >= p.tracks.size() || p.tracks[track].locked || p.tracks[track].kind != TrackKind::Video) return nullptr;
    Clip c;
    c.id = newId();
    c.kind = e.type == "adjust" ? KindAdjustment : KindEffectLayer;
    c.track = track;
    c.effects = {e};
    start = std::max(0.0, start);
    c.dur = p.duration() - start > 1.0 ? p.duration() - start : 5.0;      // cover what is already on the timeline
    c.start = p.findFreeStart(track, start, c.dur);
    return new ChangeClipsCommand(&p, {}, {c}, c.kind == KindAdjustment ? "Add adjustment layer" : "Add effect layer");
}

} // namespace ve

namespace ve {

// ---- tracks -----------------------------------------------------------------------------------------------------

static void renumberTracks(QVector<Track>& t) {
    int nv = 0;
    for (const Track& x : t) if (x.kind == TrackKind::Video) ++nv;
    int v = nv, a = 1;
    for (Track& x : t) x.name = x.kind == TrackKind::Video ? QString("V%1").arg(v--) : QString("A%1").arg(a++);
}

QUndoCommand* makeInsertTrackCommand(Project& p, TrackKind kind, int index) {
    int firstAudio = p.tracks.size(), count = 0;
    for (int i = 0; i < p.tracks.size(); ++i) {
        if (p.tracks[i].kind == TrackKind::Audio) firstAudio = std::min(firstAudio, i);
        if (p.tracks[i].kind == kind) ++count;
    }
    if (count >= kMaxTracksPerKind) return nullptr;
    index = kind == TrackKind::Video ? std::clamp(index, 0, firstAudio) : std::clamp(index, firstAudio, int(p.tracks.size()));
    TimelineState before = p.timeline(), after = before;
    Track t; t.kind = kind;
    after.tracks.insert(index, t);
    for (Clip& c : after.clips) if (c.track >= index) ++c.track;
    renumberTracks(after.tracks);
    return new TimelineCommand(&p, before, after, kind == TrackKind::Video ? "Add video track" : "Add audio track");
}

QUndoCommand* makeRemoveTrackCommand(Project& p, int index, bool alsoRemoveClips, QString* err) {
    if (index < 0 || index >= p.tracks.size()) return nullptr;
    int same = 0;
    for (const Track& t : p.tracks) if (t.kind == p.tracks[index].kind) ++same;
    if (same <= 1) { if (err) *err = QString("At least one %1 track has to stay.").arg(p.tracks[index].kind == TrackKind::Video ? "video" : "audio"); return nullptr; }
    if (p.tracks[index].locked) { if (err) *err = "Unlock the track first."; return nullptr; }
    bool hasClips = false;
    for (const Clip& c : p.clips) hasClips = hasClips || c.track == index;
    if (hasClips && !alsoRemoveClips) { if (err) *err = "The track still has clips on it."; return nullptr; }
    TimelineState before = p.timeline(), after = before;
    after.clips.erase(std::remove_if(after.clips.begin(), after.clips.end(), [&](const Clip& c) { return c.track == index; }), after.clips.end());
    for (Clip& c : after.clips) if (c.track > index) --c.track;
    after.tracks.removeAt(index);
    renumberTracks(after.tracks);
    return new TimelineCommand(&p, before, after, "Delete track");
}

// ---- copy / paste -----------------------------------------------------------------------------------------------

QVector<Clip> copyClips(const Project& p, const QStringList& ids) {
    QVector<Clip> out;
    for (const QString& id : ids) if (const Clip* c = p.clip(id)) out.push_back(*c);
    std::sort(out.begin(), out.end(), [](const Clip& a, const Clip& b) { return a.start != b.start ? a.start < b.start : a.track < b.track; });
    return out;
}

static bool trackSuits(const Project& p, const Clip& c) {
    if (c.track < 0 || c.track >= p.tracks.size() || p.tracks[c.track].locked) return false;
    const TrackKind k = p.tracks[c.track].kind;
    if (c.kind != KindMedia) return k == TrackKind::Video;
    const MediaAsset* a = p.asset(c.assetId);
    if (!a || a->missing) return false;
    return k == TrackKind::Video ? a->hasVideo : (!a->hasVideo && a->hasAudio);
}

QUndoCommand* makePasteCommand(Project& p, const QVector<Clip>& clips, double at, QStringList* newIds) {
    QVector<Clip> valid;
    for (const Clip& c : clips) if (trackSuits(p, c)) valid.push_back(c);
    if (valid.isEmpty()) return nullptr;
    double minStart = 1e18;
    for (const Clip& c : valid) minStart = std::min(minStart, c.start);
    double shift = std::max(0.0, at) - minStart;
    for (int iter = 0; iter < 64; ++iter) {            // slide the group right until it fits among the existing clips
        double bump = 0;
        for (const Clip& c : valid) {
            Clip t = c; t.start += shift;
            for (const Clip& o : p.clips)
                if (o.track == t.track && t.start < o.end() - 1e-6 && t.end() > o.start + 1e-6) bump = std::max(bump, o.end() - t.start);
        }
        if (bump <= 0) break;
        shift += bump;
    }
    QVector<Clip> after;
    for (Clip c : valid) { c.id = newId(); c.start += shift; after.push_back(c); if (newIds) newIds->push_back(c.id); }
    return new ChangeClipsCommand(&p, {}, after, after.size() > 1 ? "Paste clips" : "Paste clip");
}

QUndoCommand* makeAddTextCommand(Project& p, int track, double start, const TextStyle& style) {
    if (track < 0 || track >= p.tracks.size() || p.tracks[track].locked || p.tracks[track].kind != TrackKind::Video) return nullptr;
    Clip c;
    c.id = newId();
    c.kind = KindText;
    c.track = track;
    c.text = style;
    c.dur = 5.0;
    c.start = p.findFreeStart(track, std::max(0.0, start), c.dur);
    return new ChangeClipsCommand(&p, {}, {c}, "Add text");
}

} // namespace ve
