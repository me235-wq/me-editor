#pragma once
#include "Project.h"
#include <QUndoCommand>

namespace ve {

// Every clip edit is one command: it swaps a set of clips for another set. Undo swaps them back.
// Commands with a merge key (inspector edits) fold into the previous command with the same key,
// so dragging a spin box is a single undo step.
class ChangeClipsCommand : public QUndoCommand {
public:
    ChangeClipsCommand(Project* p, QVector<Clip> before, QVector<Clip> after, const QString& text, const QString& mergeKey = QString())
        : QUndoCommand(text), m_p(p), m_before(std::move(before)), m_after(std::move(after)), m_key(mergeKey) {}
    void redo() override { m_p->replace(m_before, m_after); }
    void undo() override { m_p->replace(m_after, m_before); }
    int id() const override { return m_key.isEmpty() ? -1 : 7001; }
    bool mergeWith(const QUndoCommand* other) override {
        const auto* o = dynamic_cast<const ChangeClipsCommand*>(other);
        if (!o || o->m_key != m_key || m_key.isEmpty()) return false;
        m_after = o->m_after;
        return true;
    }
private:
    Project* m_p;
    QVector<Clip> m_before, m_after;
    QString m_key;
};

class TrackFlagCommand : public QUndoCommand {
public:
    TrackFlagCommand(Project* p, int track, int which, bool on);   // which: 0 hidden, 1 locked, 2 muted
    void redo() override { m_p->setTrackFlag(m_track, m_which, m_new); }
    void undo() override { m_p->setTrackFlag(m_track, m_which, !m_new); }
private:
    Project* m_p; int m_track, m_which; bool m_new;
};

// Tracks added / removed together with the clips' track numbers.
class TimelineCommand : public QUndoCommand {
public:
    TimelineCommand(Project* p, TimelineState before, TimelineState after, const QString& text)
        : QUndoCommand(text), m_p(p), m_before(std::move(before)), m_after(std::move(after)) {}
    void redo() override { m_p->setTimeline(m_after); }
    void undo() override { m_p->setTimeline(m_before); }
private:
    Project* m_p;
    TimelineState m_before, m_after;
};

// Media-panel operations (add/move/rename/delete folders and assets) as one snapshot swap.
class LibraryCommand : public QUndoCommand {
public:
    LibraryCommand(Project* p, LibraryState before, LibraryState after, const QString& text)
        : QUndoCommand(text), m_p(p), m_before(std::move(before)), m_after(std::move(after)) {}
    void redo() override { m_p->setLibrary(m_after); }
    void undo() override { m_p->setLibrary(m_before); }
private:
    Project* m_p;
    LibraryState m_before, m_after;
};

// Factories return nullptr when there is nothing to do (caller pushes the result on the undo stack).
// Clips on locked tracks are never touched.
QUndoCommand* makeSplitCommand(Project& p, const QStringList& ids, double t);
QUndoCommand* makeDeleteCommand(Project& p, const QStringList& ids, bool ripple);
QUndoCommand* makeAddClipCommand(Project& p, const QString& assetId, int track, double start);

// Effects: applying a preset replaces any existing effect of the same type on that clip.
QUndoCommand* makeApplyEffectCommand(Project& p, const QStringList& ids, const Effect& e);
QUndoCommand* makeSetEffectsCommand(Project& p, const QString& id, const QVector<Effect>& effects, const QString& text);

// Create a separate effect / adjustment layer (acts on everything below it). Spans the existing timeline from `start`.
QUndoCommand* makeAddLayerCommand(Project& p, const Effect& e, int track, double start);

// Freeze frame: at time `t` on the first suitable clip, split it and insert a silent `hold`-second still of the
// frame shown at `t`. Later clips on the same track move right by `hold`.
QUndoCommand* makeFreezeCommand(Project& p, const QStringList& ids, double t, double hold);

// Replace one clip by an edited copy (position, volume, keyframes ...). Pass a mergeKey for continuous edits.
QUndoCommand* makeSetClipCommand(Project& p, const Clip& updated, const QString& text, const QString& mergeKey = QString());
// Change playback speed: the clip keeps its source range, so its length on the timeline changes. Fails (nullptr + err)
// if it would run into the next clip.
QUndoCommand* makeSetSpeedCommand(Project& p, const QString& id, double speed, QString* err = nullptr);

// Tracks. Video tracks always stay above audio tracks; names are renumbered (V3 V2 V1 / A1 A2 ...). At least one of each kind stays.
QUndoCommand* makeInsertTrackCommand(Project& p, TrackKind kind, int index);
QUndoCommand* makeRemoveTrackCommand(Project& p, int index, bool alsoRemoveClips, QString* err = nullptr);
constexpr int kMaxTracksPerKind = 12;

// Copy / paste. `copyClips` snapshots clips (timeline order); `makePasteCommand` puts them back with new ids, keeping their
// distances from each other and their tracks, starting at `at` (or after whatever is in the way).
QVector<Clip> copyClips(const Project& p, const QStringList& ids);
QUndoCommand* makePasteCommand(Project& p, const QVector<Clip>& clips, double at, QStringList* newIds = nullptr);

// Text object on a video track.
QUndoCommand* makeAddTextCommand(Project& p, int track, double start, const TextStyle& style = TextStyle());

// Media library
QUndoCommand* makeRemoveMediaCommand(Project& p, const QStringList& assetIds, const QStringList& folderIds);   // also removes dependent clips
QUndoCommand* makeMoveToFolderCommand(Project& p, const QStringList& assetIds, const QStringList& folderIds, const QString& target);
QUndoCommand* makeNewFolderCommand(Project& p, const QString& name, const QString& parentId, QString* newFolderId = nullptr);
QUndoCommand* makeRenameFolderCommand(Project& p, const QString& folderId, const QString& name);

constexpr double kMinSpeed = 0.1, kMaxSpeed = 20.0;

} // namespace ve
