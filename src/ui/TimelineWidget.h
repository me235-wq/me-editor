#pragma once
#include "core/Project.h"
#include "media/Overview.h"
#include <QHash>
#include <QSet>
#include <QUndoStack>
#include <QWidget>

namespace ve {

// Custom-painted multi-track timeline: ruler, playhead, drag/trim/snap, drag-box selection, zoom,
// per-track hide / lock / mute, and drag & drop from the media and effects panels.
class TimelineWidget : public QWidget {
    Q_OBJECT
public:
    TimelineWidget(Project* project, QUndoStack* stack, QWidget* parent = nullptr);

    double playhead() const { return m_playhead; }
    void setPlayhead(double t);
    QStringList selectedIds() const { return QStringList(m_sel.begin(), m_sel.end()); }
    void clearSelection();
    void selectAll();
    void selectOnly(const QString& clipId);
    void selectIds(const QStringList& ids);
    void setOverviewStore(const QHash<QString, AssetOverview>* store) { m_overview = store; update(); }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void playheadMoved(double t);   // user scrubbed
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    enum class Mode { None, Scrub, Move, TrimL, TrimR, Band, KeyMove };
    enum class Zone { None, Body, Left, Right };
    struct Hit { int index = -1; Zone zone = Zone::None; };

    double xFor(double t) const;
    double tFor(double x) const;
    int rowAt(double y) const;
    QRectF clipRect(const Clip& c) const;
    QRectF headerButton(int row, int which) const;
    int headerButtonAt(const QPointF& p, int* row) const;   // which button (0 hide, 1 lock, 2 mute) or -1
    Hit hitTest(const QPointF& p) const;
    QVector<double> snapTargets(const QSet<QString>& exclude) const;
    double snap(double t, const QSet<QString>& exclude);
    bool candidateValid(const QVector<Clip>& cand, const QSet<QString>& selection) const;
    void scrubTo(double x);
    void ensureVisible(double t);
    bool dropAllowed(const QString& assetId, int row) const;
    QRectF chipRect(const QRectF& clip, int index, int count) const;
    void drawFilmstrip(QPainter& p, const Clip& c, const QRectF& r, const AssetOverview& ov) const;
    void drawWaveform(QPainter& p, const Clip& c, const QRectF& r, const AssetOverview& ov, bool strip, bool dim) const;
    QSet<QString> clipsInBand(const QRectF& r) const;
    bool keyHit(const QPointF& p, QString* clipId, double* keyTime) const;   // is there a keyframe diamond under the cursor?

    const QHash<QString, AssetOverview>* m_overview = nullptr;
    Project* m_project;
    QUndoStack* m_stack;
    QSet<QString> m_sel, m_selBase;
    double m_playhead = 0, m_scroll = 0, m_pps = 80;   // pps = pixels per second
    Mode m_mode = Mode::None;
    QString m_anchorId;
    QString m_collapseTo;   // plain click inside a multi-selection: narrow to this clip on release unless it was a drag
    double m_pressTime = 0;
    QPointF m_pressPos;
    double m_keyT0 = 0;      // clip-local time of the keyframe being dragged
    QRectF m_band;
    QVector<Clip> m_orig, m_live;
    double m_snapLine = -1;
    QString m_dropAsset; int m_dropRow = -1; double m_dropTime = 0;
    QString m_fxTarget;   // clip highlighted while an effect preset is dragged over it
    int m_layerRow = -1; double m_layerTime = 0;   // where a dragged effect-layer preset would land
};

} // namespace ve
