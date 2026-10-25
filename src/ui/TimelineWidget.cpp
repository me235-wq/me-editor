#include "TimelineWidget.h"
#include "core/Commands.h"
#include "core/Effects.h"
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QMenu>
#include <QMessageBox>
#include <QFontMetrics>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace ve {

static constexpr int kHeaderW = 120, kRulerH = 26, kTrackH = 46, kGap = 2;
static constexpr double kMinDur = 0.1, kSnapPx = 8.0, kStill = 5.0;
static const char* kAssetMime = "application/x-ve-asset";
static const char* kEffectMime = "application/x-ve-effect";

TimelineWidget::TimelineWidget(Project* project, QUndoStack* stack, QWidget* parent)
    : QWidget(parent), m_project(project), m_stack(stack) {
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::ClickFocus);
    connect(m_project, &Project::changed, this, [this] {
        QSet<QString> alive;
        for (const Clip& c : m_project->clips)
            if (!m_project->tracks[c.track].locked) alive.insert(c.id);    // locked clips can't stay selected
        const int before = m_sel.size();
        m_sel.intersect(alive);
        if (m_sel.size() != before) emit selectionChanged();
        updateGeometry();                    // tracks may have been added or removed
        update();
    });
}

QSize TimelineWidget::minimumSizeHint() const { return {400, kRulerH + int(m_project->tracks.size()) * (kTrackH + kGap) + 6}; }
QSize TimelineWidget::sizeHint() const { return minimumSizeHint(); }

double TimelineWidget::xFor(double t) const { return kHeaderW + (t - m_scroll) * m_pps; }
double TimelineWidget::tFor(double x) const { return m_scroll + (x - kHeaderW) / m_pps; }

int TimelineWidget::rowAt(double y) const {
    const int r = int(std::floor((y - kRulerH) / (kTrackH + kGap)));
    return (r >= 0 && r < m_project->tracks.size()) ? r : -1;
}

QRectF TimelineWidget::clipRect(const Clip& c) const {
    const double y = kRulerH + c.track * (kTrackH + kGap);
    return QRectF(xFor(c.start), y + 2, c.dur * m_pps, kTrackH - 4);
}

QRectF TimelineWidget::headerButton(int row, int which) const {
    const double y = kRulerH + row * (kTrackH + kGap);
    return QRectF(40 + which * 26, y + (kTrackH - 20) / 2.0, 22, 20);
}

int TimelineWidget::headerButtonAt(const QPointF& p, int* row) const {
    *row = rowAt(p.y());
    if (*row < 0 || p.x() >= kHeaderW) return -1;
    for (int w = 0; w < 3; ++w)
        if (headerButton(*row, w).contains(p)) return w;
    return -1;
}

void TimelineWidget::setPlayhead(double t) {
    m_playhead = std::max(0.0, t);
    ensureVisible(m_playhead);
    update();
}

void TimelineWidget::ensureVisible(double t) {
    const double visible = (width() - kHeaderW) / m_pps;
    if (t < m_scroll) m_scroll = std::max(0.0, t - 0.1 * visible);
    else if (t > m_scroll + visible) m_scroll = t - 0.1 * visible;
}

void TimelineWidget::clearSelection() {
    if (m_sel.isEmpty()) return;
    m_sel.clear();
    emit selectionChanged();
    update();
}

void TimelineWidget::selectOnly(const QString& clipId) {
    const Clip* c = m_project->clip(clipId);
    if (!c || m_project->tracks[c->track].locked) return;
    m_sel = {clipId};
    emit selectionChanged();
    update();
}

void TimelineWidget::selectIds(const QStringList& ids) {
    m_sel.clear();
    for (const QString& id : ids) if (const Clip* c = m_project->clip(id)) if (!m_project->tracks[c->track].locked) m_sel.insert(id);
    emit selectionChanged();
    update();
}

void TimelineWidget::selectAll() {
    m_sel.clear();
    for (const Clip& c : m_project->clips)
        if (!m_project->tracks[c.track].locked) m_sel.insert(c.id);
    emit selectionChanged();
    update();
}

TimelineWidget::Hit TimelineWidget::hitTest(const QPointF& p) const {
    for (int i = m_project->clips.size() - 1; i >= 0; --i) {
        const Clip& c = m_project->clips[i];
        if (m_project->tracks[c.track].locked) continue;
        const QRectF r = clipRect(c);
        if (!r.contains(p) || p.x() < kHeaderW) continue;
        Hit h; h.index = i; h.zone = Zone::Body;
        const double edge = r.width() > 30 ? 7.0 : 0.0;
        if (p.x() < r.left() + edge) h.zone = Zone::Left;
        else if (p.x() > r.right() - edge) h.zone = Zone::Right;
        return h;
    }
    return {};
}

bool TimelineWidget::keyHit(const QPointF& p, QString* clipId, double* keyTime) const {
    for (int i = m_project->clips.size() - 1; i >= 0; --i) {
        const Clip& c = m_project->clips[i];
        if (c.keys.isEmpty() || m_project->tracks[c.track].locked) continue;
        const QRectF r = clipRect(c);
        if (p.y() < r.bottom() - 16 || p.y() > r.bottom() + 1 || p.x() < kHeaderW) continue;
        double best = 8.0; bool found = false;
        for (auto it = c.keys.constBegin(); it != c.keys.constEnd(); ++it)
            for (const Keyframe& k : it.value()) {
                if (k.t < -1e-6 || k.t > c.dur + 1e-6) continue;
                const double d = std::abs(p.x() - (r.left() + k.t * m_pps));
                if (d <= best) { best = d; *keyTime = k.t; found = true; }
            }
        if (found) { *clipId = c.id; return true; }
    }
    return false;
}

QSet<QString> TimelineWidget::clipsInBand(const QRectF& r) const {
    QSet<QString> out;
    for (const Clip& c : m_project->clips)
        if (!m_project->tracks[c.track].locked && clipRect(c).intersects(r)) out.insert(c.id);
    return out;
}

// ---- snapping ---------------------------------------------------------------------------------

QVector<double> TimelineWidget::snapTargets(const QSet<QString>& exclude) const {
    QVector<double> t{0.0, m_playhead};
    for (const Clip& c : m_project->clips)
        if (!exclude.contains(c.id)) { t << c.start << c.end(); }
    return t;
}

double TimelineWidget::snap(double t, const QSet<QString>& exclude) {
    const double thr = kSnapPx / m_pps;
    double best = thr, out = t;
    m_snapLine = -1;
    for (double tg : snapTargets(exclude)) {
        if (std::abs(tg - t) < best) { best = std::abs(tg - t); out = tg; m_snapLine = tg; }
    }
    return out;
}

bool TimelineWidget::candidateValid(const QVector<Clip>& cand, const QSet<QString>& selection) const {
    for (int i = 0; i < cand.size(); ++i) {
        if (m_project->overlaps(cand[i], selection)) return false;
        for (int j = i + 1; j < cand.size(); ++j)
            if (cand[i].track == cand[j].track && cand[i].start < cand[j].end() - 1e-6 && cand[i].end() > cand[j].start + 1e-6) return false;
    }
    return true;
}

// ---- painting ---------------------------------------------------------------------------------

// CapCut-style filmstrip: thumbnails tile the clip, each showing the frame at that point of the source.
void TimelineWidget::drawFilmstrip(QPainter& p, const Clip& c, const QRectF& r, const AssetOverview& ov) const {
    if (ov.thumbs.isEmpty()) return;
    const double th = r.height(), tw = th * 16.0 / 9.0;
    const double x1 = std::min(r.right(), double(width()));
    for (int i = std::max(0, int(std::floor((std::max(r.left(), double(kHeaderW)) - r.left()) / tw))); r.left() + i * tw < x1; ++i) {
        const QRectF tile(r.left() + i * tw, r.top(), tw, th);
        const double local = std::clamp((tile.center().x() - r.left()) / m_pps, 0.0, c.dur);
        const double src = c.freeze ? c.in : c.in + local * c.speed;
        const QImage& im = ov.thumbs[std::clamp(int(src / ov.thumbStep), 0, int(ov.thumbs.size()) - 1)];
        if (im.isNull()) continue;
        const double ia = im.width() / double(im.height()), ta = tw / th;      // centre-crop the thumbnail to the tile
        const QRectF srcR = ia > ta ? QRectF((im.width() - im.height() * ta) / 2, 0, im.height() * ta, im.height())
                                    : QRectF(0, (im.height() - im.width() / ta) / 2, im.width(), im.width() / ta);
        p.drawImage(tile, im, srcR);
    }
    p.fillRect(r, QColor(0, 0, 0, 80));                                         // darken a little so the name stays readable
}

// Audio waveform: along the bottom of a video clip, or filling an audio clip. One vertical line per pixel column = the loudest
// moment in that stretch of the source (so it follows speed changes and trims).
void TimelineWidget::drawWaveform(QPainter& p, const Clip& c, const QRectF& r, const AssetOverview& ov, bool strip, bool dim) const {
    if (ov.peaks.isEmpty()) return;
    const double h = strip ? 14.0 : r.height() - 12.0, cy = strip ? r.bottom() - 9.0 : r.center().y();
    if (strip) p.fillRect(QRectF(r.left(), r.bottom() - 17, r.width(), 17), QColor(0, 0, 0, 95));
    p.setPen(QPen(dim ? QColor(255, 255, 255, 70) : (strip ? QColor(255, 255, 255, 215) : QColor(214, 255, 232, 235)), 1));
    const int n = ov.peaks.size();
    for (int x = int(std::ceil(std::max(r.left(), double(kHeaderW)))); x < std::min(r.right(), double(width())); ++x) {
        const double t0 = (x - r.left()) / m_pps;
        const double s0 = c.in + t0 * c.speed, s1 = c.in + (t0 + 1.0 / m_pps) * c.speed;
        const int i0 = std::clamp(int(s0 / ov.peakStep), 0, n - 1), i1 = std::clamp(std::max(i0, int(s1 / ov.peakStep)), 0, n - 1);
        int pk = 0;
        for (int i = i0; i <= i1; ++i) pk = std::max<int>(pk, ov.peaks[i]);
        const double amp = std::max(0.6, pk / 255.0 * h / 2);
        p.drawLine(QPointF(x + 0.5, cy - amp), QPointF(x + 0.5, cy + amp));
    }
}

QRectF TimelineWidget::chipRect(const QRectF& clip, int index, int count) const {
    const double w = 50, gap = 3;
    const double x1 = clip.right() - 6 - (count - 1 - index) * (w + gap);
    return QRectF(x1 - w, clip.top() + 5, w, 15);
}

static void drawTrackIcon(QPainter& p, const QRectF& r, int which, bool on, bool hot) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor accent = which == 0 ? QColor("#6fb1ff") : which == 1 ? QColor("#ffb74d") : QColor("#ff6b6b");
    const QColor ink = on ? accent : QColor(hot ? "#c8c8cd" : "#7d7d84");
    if (on) { p.setPen(Qt::NoPen); p.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 38)); p.drawRoundedRect(r, 4, 4); }
    p.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    const QPointF c = r.center();
    if (which == 0) {                                   // eye
        QPainterPath eye;
        eye.moveTo(c.x() - 7, c.y());
        eye.quadTo(c.x(), c.y() - 8, c.x() + 7, c.y());
        eye.quadTo(c.x(), c.y() + 8, c.x() - 7, c.y());
        p.drawPath(eye);
        p.setBrush(ink); p.drawEllipse(c, 2.2, 2.2);
        if (on) { p.setPen(QPen(ink, 1.8, Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(c.x() - 7, c.y() + 6), QPointF(c.x() + 7, c.y() - 6)); }
    } else if (which == 1) {                            // padlock
        p.setBrush(on ? ink : Qt::NoBrush);
        p.drawRoundedRect(QRectF(c.x() - 5.5, c.y() - 1, 11, 8), 1.5, 1.5);
        p.setBrush(Qt::NoBrush);
        QPainterPath sh;
        sh.moveTo(c.x() - 3.5, c.y() - 1);
        sh.lineTo(c.x() - 3.5, c.y() - 4);
        sh.arcTo(QRectF(c.x() - 3.5, c.y() - 8, 7, 7), 180, -180);
        if (on) sh.lineTo(c.x() + 3.5, c.y() - 1);
        else sh.lineTo(c.x() + 3.5, c.y() - 5.5);
        p.drawPath(sh);
    } else {                                            // speaker
        p.setBrush(ink);
        p.drawPolygon(QPolygonF({QPointF(c.x() - 7, c.y() - 2.5), QPointF(c.x() - 3.5, c.y() - 2.5), QPointF(c.x() + 0.5, c.y() - 6),
                                 QPointF(c.x() + 0.5, c.y() + 6), QPointF(c.x() - 3.5, c.y() + 2.5), QPointF(c.x() - 7, c.y() + 2.5)}));
        p.setBrush(Qt::NoBrush);
        if (!on) { p.drawArc(QRectF(c.x() - 3, c.y() - 4, 8, 8), -50 * 16, 100 * 16); p.drawArc(QRectF(c.x() - 3, c.y() - 7, 14, 14), -50 * 16, 100 * 16); }
        else p.drawLine(QPointF(c.x() + 3, c.y() - 5), QPointF(c.x() + 9, c.y() + 5));
    }
    p.restore();
}

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#1c1c1e"));
    QFont small = font(); small.setPointSizeF(8.5); p.setFont(small);
    const QPointF hoverPos = mapFromGlobal(QCursor::pos());

    for (int i = 0; i < m_project->tracks.size(); ++i) {
        const Track& tr = m_project->tracks[i];
        const int y = kRulerH + i * (kTrackH + kGap);
        p.fillRect(QRect(kHeaderW, y, width() - kHeaderW, kTrackH), i % 2 ? QColor("#232326") : QColor("#262629"));
        if (tr.hidden) p.fillRect(QRect(kHeaderW, y, width() - kHeaderW, kTrackH), QColor(0, 0, 0, 70));
        p.fillRect(QRect(0, y, kHeaderW - 2, kTrackH), QColor("#2e2e32"));
        p.setPen(tr.hidden ? QColor("#6d6d73") : QColor("#d0d0d5"));
        p.drawText(QRect(10, y, 28, kTrackH), Qt::AlignVCenter | Qt::AlignLeft, tr.name);
        const bool flags[3] = {tr.hidden, tr.locked, tr.muted};
        for (int w = 0; w < 3; ++w) drawTrackIcon(p, headerButton(i, w), w, flags[w], headerButton(i, w).contains(hoverPos));
    }

    p.setClipRect(QRect(kHeaderW, kRulerH, width() - kHeaderW, height() - kRulerH));
    const QFontMetrics fm(small);
    for (const Clip& c : m_project->clips) {
        const QRectF r = clipRect(c);
        if (r.right() < kHeaderW || r.left() > width()) continue;
        const Track& tr = m_project->tracks[c.track];
        const MediaAsset* a = m_project->asset(c.assetId);
        const bool video = tr.kind == TrackKind::Video;
        const bool sel = m_sel.contains(c.id);
        p.setOpacity(tr.hidden ? 0.4 : 1.0);
        QColor fill = (a && a->missing) ? QColor("#9a3b3b") : c.kind == KindText ? QColor("#2f8f9d") : c.kind == KindAdjustment ? QColor("#b08a2a") : c.kind == KindEffectLayer ? QColor("#7d5bbf")
                      : c.freeze ? QColor("#3c8fa8") : video ? QColor("#3f6aa8") : QColor("#368a69");
        if (sel) fill = fill.lighter(125);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(r, 4, 4);
        if (a && m_overview && !a->missing) {                                       // filmstrip + waveform inside the clip's rounded outline
            const auto ovIt = m_overview->constFind(c.assetId);
            if (ovIt != m_overview->constEnd()) {
                p.save();
                QPainterPath outline; outline.addRoundedRect(r, 4, 4);
                p.setClipPath(outline, Qt::IntersectClip);
                if (video && a->hasVideo) drawFilmstrip(p, c, r, *ovIt);
                if (a->hasAudio && !c.freeze && !a->isImage) drawWaveform(p, c, r, *ovIt, a->hasVideo, c.muted || tr.muted);
                if (sel) p.fillRect(r, QColor(255, 255, 255, 38));
                p.restore();
            }
        }
        if (c.freeze) { p.setBrush(QBrush(QColor(255, 255, 255, 40), Qt::BDiagPattern)); p.drawRoundedRect(r, 4, 4); }
        if (tr.locked) { p.setBrush(QBrush(QColor(0, 0, 0, 90), Qt::FDiagPattern)); p.drawRoundedRect(r, 4, 4); }
        p.setBrush(QColor(255, 255, 255, 45));            // trim handles
        if (r.width() > 30 && !tr.locked) { p.drawRect(QRectF(r.left(), r.top(), 3, r.height())); p.drawRect(QRectF(r.right() - 3, r.top(), 3, r.height())); }
        if (sel) { p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor("#ffd54a"), 2)); p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 4, 4); }

        // keyframe diamonds along the bottom edge
        QSet<int> seen;
        p.setPen(QPen(QColor(0, 0, 0, 160), 1)); p.setBrush(QColor("#ffe27a"));
        for (auto it = c.keys.constBegin(); it != c.keys.constEnd(); ++it)
            for (const Keyframe& k : it.value()) {
                const double x = r.left() + k.t * m_pps;
                if (k.t < 0 || k.t > c.dur || seen.contains(int(x))) continue;
                seen.insert(int(x));
                p.drawPolygon(QPolygonF({QPointF(x, r.bottom() - 12), QPointF(x + 5.5, r.bottom() - 6.5), QPointF(x, r.bottom() - 1), QPointF(x - 5.5, r.bottom() - 6.5)}));
            }

        QString tags;
        if (c.speed != 1.0 && !c.freeze) tags += QString("  %1×").arg(c.speed, 0, 'g', 3);
        if (c.muted && !c.freeze) tags += "  muted";
        const QString firstLine = c.text.text.section('\n', 0, 0);
        const QString title = c.kind == KindText ? "Aa  " + firstLine : c.kind != KindMedia ? (c.kind == KindAdjustment ? "Adjustment layer" : (c.effects.isEmpty() ? QString("Effect layer") : effectLabel(c.effects[0]) + " layer")) : (a ? a->name() : "?");
        const QString label = QString("%1%2  %3s%4%5").arg(c.freeze ? "FREEZE  " : "", title).arg(c.dur, 0, 'f', 1).arg(tags, c.mask.on ? "  ▭ mask" : "");
        const bool showChips = c.kind == KindMedia;
        const int chipsW = (c.effects.isEmpty() || !showChips) ? 0 : int(chipRect(r, c.effects.size() - 1, c.effects.size()).right() - chipRect(r, 0, c.effects.size()).left()) + 6;
        {
            const QRectF lr = r.adjusted(7, 0, -5 - chipsW, 0);
            const QString txt = fm.elidedText(label, Qt::ElideRight, int(r.width()) - 12 - chipsW);
            const bool onPicture = a && a->hasVideo && m_overview && m_overview->contains(c.assetId);
            const Qt::Alignment al = onPicture ? (Qt::AlignTop | Qt::AlignLeft) : (Qt::AlignVCenter | Qt::AlignLeft);
            p.setPen(QColor(0, 0, 0, 200)); p.drawText(lr.translated(1, onPicture ? 4 : 1), al, txt);   // shadow keeps it legible over thumbnails
            p.setPen(Qt::white);          p.drawText(lr.translated(0, onPicture ? 3 : 0), al, txt);
        }
        for (int i = 0; i < c.effects.size() && r.width() > 90 && showChips; ++i) {   // small tags: SHAKE / IN / OUT / effect initials
            const Effect& e = c.effects[i];
            const QRectF cr = chipRect(r, i, c.effects.size());
            p.setPen(Qt::NoPen);
            p.setBrush(e.type == "shake" ? QColor("#d9822b") : e.type.startsWith("swoosh") ? QColor("#8a5fd6") : QColor("#3d9a95"));
            p.drawRoundedRect(cr, 3, 3);
            p.setPen(Qt::white);
            QString tag = e.type == "shake" ? "SHAKE" : e.type == "swoosh_in" ? "IN" : e.type == "swoosh_out" ? "OUT" : e.type == "shadow" ? "SHADOW"
                        : e.type == "border" ? "BORDER" : e.type == "blur" ? "BLUR" : e.type == "mirror" ? "MIRROR" : e.type == "chromakey" ? "KEY" : e.type == "blackflash" ? "FLASH" : e.type.toUpper();
            p.drawText(cr, Qt::AlignCenter, tag);
        }
        p.setOpacity(1.0);
    }

    if (!m_fxTarget.isEmpty())
        if (const Clip* t = m_project->clip(m_fxTarget)) {
            p.setBrush(QColor(255, 213, 74, 50)); p.setPen(QPen(QColor("#ffd54a"), 2));
            p.drawRoundedRect(clipRect(*t), 4, 4);
        }
    if (!m_dropAsset.isEmpty() && m_dropRow >= 0) {
        if (const MediaAsset* a = m_project->asset(m_dropAsset)) {
            Clip g; g.track = m_dropRow; g.start = m_dropTime; g.dur = a->isImage ? kStill : a->duration;
            p.setBrush(QColor(255, 255, 255, 50)); p.setPen(QPen(QColor("#ffd54a"), 1, Qt::DashLine));
            p.drawRoundedRect(clipRect(g), 4, 4);
        }
    }
    if (m_layerRow >= 0) {
        Clip g; g.track = m_layerRow; g.start = m_layerTime;
        g.dur = m_project->duration() - m_layerTime > 1.0 ? m_project->duration() - m_layerTime : 5.0;
        g.start = m_project->findFreeStart(m_layerRow, m_layerTime, g.dur);
        p.setBrush(QColor(138, 95, 214, 70)); p.setPen(QPen(QColor("#ffd54a"), 1, Qt::DashLine));
        p.drawRoundedRect(clipRect(g), 4, 4);
    }
    if (m_snapLine >= 0 && m_mode != Mode::None) {
        p.setPen(QPen(QColor("#ffd54a"), 1, Qt::DashLine));
        p.drawLine(QPointF(xFor(m_snapLine), kRulerH), QPointF(xFor(m_snapLine), height()));
    }
    if (m_mode == Mode::Band) {
        p.setBrush(QColor(74, 144, 226, 45)); p.setPen(QPen(QColor("#4a90e2"), 1));
        p.drawRect(m_band);
    }
    p.setClipping(false);

    // ruler
    p.fillRect(QRect(0, 0, width(), kRulerH), QColor("#2a2a2d"));
    static const double steps[] = {0.04, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
    double step = steps[std::size(steps) - 1];
    for (double s : steps) if (s * m_pps >= 70) { step = s; break; }
    p.setPen(QColor("#8d8d93"));
    for (double t = std::floor(m_scroll / step) * step; xFor(t) < width(); t += step) {
        const double x = xFor(t);
        if (x < kHeaderW) continue;
        p.drawLine(QPointF(x, kRulerH - 8), QPointF(x, kRulerH));
        const int whole = int(std::floor(t + 1e-9));
        QString txt = QString("%1:%2").arg(whole / 60).arg(whole % 60, 2, 10, QChar('0'));
        if (step < 1) txt += QString(".%1").arg(int(std::lround((t - whole) * 100)), 2, 10, QChar('0'));
        p.drawText(QPointF(x + 4, kRulerH - 11), txt);
    }
    p.fillRect(QRect(0, 0, kHeaderW - 2, kRulerH), QColor("#2e2e32"));

    const double px = xFor(m_playhead);
    if (px >= kHeaderW - 1) {
        p.setPen(QPen(QColor("#ff5a52"), 1.5));
        p.drawLine(QPointF(px, 0), QPointF(px, height()));
        p.setPen(Qt::NoPen); p.setBrush(QColor("#ff5a52"));
        p.drawPolygon(QPolygonF({QPointF(px - 6, 0), QPointF(px + 6, 0), QPointF(px, 10)}));
    }
}

// ---- mouse ------------------------------------------------------------------------------------

void TimelineWidget::scrubTo(double x) {
    setPlayhead(std::max(0.0, tFor(x)));
    emit playheadMoved(m_playhead);
}

void TimelineWidget::mousePressEvent(QMouseEvent* e) {
    setFocus();
    if (e->button() != Qt::LeftButton) return;
    const QPointF pos = e->position();
    m_pressPos = pos;

    int row = -1;
    const int btn = headerButtonAt(pos, &row);          // hide / lock / mute buttons in the track header
    if (btn >= 0) {
        const Track& t = m_project->tracks[row];
        const bool cur = btn == 0 ? t.hidden : btn == 1 ? t.locked : t.muted;
        m_stack->push(new TrackFlagCommand(m_project, row, btn, !cur));
        return;
    }
    if (pos.y() < kRulerH) { m_mode = Mode::Scrub; scrubTo(pos.x()); return; }
    if (pos.x() < kHeaderW) return;

    {   // a keyframe diamond: drag it sideways to retime it (all properties keyed at that moment move together)
        QString kid; double kt = 0;
        if (keyHit(pos, &kid, &kt)) {
            m_sel = {kid};
            emit selectionChanged();
            m_orig = {*m_project->clip(kid)};
            m_live = m_orig;
            m_keyT0 = kt;
            m_anchorId = kid;
            m_mode = Mode::KeyMove;
            update();
            return;
        }
    }
    const Hit h = hitTest(pos);
    const bool multi = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
    if (h.index < 0) {                                   // empty lane: drag a selection box (a plain click just moves the playhead)
        m_selBase = multi ? m_sel : QSet<QString>();
        m_mode = Mode::Band;
        m_band = QRectF(pos, pos);
        return;
    }
    const Clip c = m_project->clips[h.index];
    bool dragOk = true;
    if (multi) {
        if (m_sel.contains(c.id)) { m_sel.remove(c.id); dragOk = false; } else m_sel.insert(c.id);
    } else if (!m_sel.contains(c.id)) {
        m_sel = {c.id};
    } else if (m_sel.size() > 1) {
        m_collapseTo = c.id;     // keep the group so it can be dragged; collapse on release if it was only a click
    }
    emit selectionChanged();
    update();
    if (!dragOk) return;

    m_anchorId = c.id;
    m_pressTime = tFor(pos.x());
    m_orig.clear();
    if (h.zone == Zone::Body) { for (const Clip& k : m_project->clips) if (m_sel.contains(k.id) && !m_project->tracks[k.track].locked) m_orig << k; }
    else m_orig << c;
    m_live = m_orig;
    m_mode = h.zone == Zone::Left ? Mode::TrimL : h.zone == Zone::Right ? Mode::TrimR : Mode::Move;
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    if (m_mode == Mode::None) {
        int row; const int btn = headerButtonAt(pos, &row);
        const Hit h = hitTest(pos);
        QString kid; double kt;
        const bool onKey = keyHit(pos, &kid, &kt);
        setCursor(onKey ? Qt::SizeHorCursor : btn >= 0 ? Qt::PointingHandCursor : (h.zone == Zone::Left || h.zone == Zone::Right) ? Qt::SizeHorCursor : Qt::ArrowCursor);
        if (onKey) QToolTip::showText(e->globalPosition().toPoint(), "Keyframe: drag to move it in time", this);
        update();    // header button hover
        return;
    }
    if (m_mode == Mode::Scrub) { scrubTo(pos.x()); return; }
    if (m_mode == Mode::Band) {
        m_band = QRectF(m_pressPos, pos).normalized();
        m_sel = m_selBase;                      // (not `a | b`: that operator is broken in Qt 6.9+)
        m_sel.unite(clipsInBand(m_band));
        emit selectionChanged();
        update();
        return;
    }

    if (m_mode == Mode::KeyMove) {
        const Clip& o = m_orig[0];
        const double fps = std::max(1.0, m_project->settings.fps);
        double kt = std::clamp((pos.x() - xFor(o.start)) / m_pps, 0.0, o.dur);
        kt = std::round(kt * fps) / fps;                                       // keyframes sit on whole frames
        const double ph = m_playhead - o.start;
        m_snapLine = -1;
        if (ph >= 0 && ph <= o.dur && std::abs(ph - kt) * m_pps < kSnapPx) { kt = ph; m_snapLine = m_playhead; }   // snap to the playhead
        Clip n = o;
        for (auto it = n.keys.begin(); it != n.keys.end(); ++it) {
            QVector<Keyframe> kept; Keyframe moved; bool found = false;
            for (const Keyframe& k : it.value()) {
                if (std::abs(k.t - m_keyT0) < 1e-4) { moved = {kt, k.v}; found = true; }
                else if (std::abs(k.t - kt) >= 1e-4) kept.push_back(k);        // a key already sitting at the target time is replaced
            }
            if (!found) continue;
            kept.push_back(moved);
            std::sort(kept.begin(), kept.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
            it.value() = kept;
        }
        if (n != m_live[0]) { m_project->replace(m_live, QVector<Clip>{n}); m_live = {n}; }
        QToolTip::showText(e->globalPosition().toPoint(), QString("%1 s").arg(kt, 0, 'f', 2), this);
        update();
        return;
    }

    QSet<QString> selection;
    for (const Clip& c : m_orig) selection.insert(c.id);
    const double t = tFor(pos.x());
    m_snapLine = -1;
    QVector<Clip> cand = m_orig;

    if (m_mode == Mode::Move) {
        double dt = t - m_pressTime;
        double minStart = 1e18;
        for (const Clip& c : m_orig) minStart = std::min(minStart, c.start);
        dt = std::max(dt, -minStart);
        const double thr = kSnapPx / m_pps;
        double best = thr, adj = 0;
        for (const Clip& c : m_orig)
            for (double edge : {c.start + dt, c.end() + dt})
                for (double tg : snapTargets(selection))
                    if (std::abs(tg - edge) < best) { best = std::abs(tg - edge); adj = tg - edge; m_snapLine = tg; }
        dt = std::max(dt + adj, -minStart);

        int tdelta = 0;
        const int row = rowAt(pos.y());
        for (const Clip& c : m_orig) if (c.id == m_anchorId && row >= 0) tdelta = row - c.track;
        for (int i = 0; i < cand.size(); ++i) {
            cand[i].start = std::max(0.0, m_orig[i].start + dt);
            const int nt = m_orig[i].track + tdelta;
            if (nt >= 0 && nt < m_project->tracks.size() && !m_project->tracks[nt].locked &&
                m_project->tracks[nt].kind == m_project->tracks[m_orig[i].track].kind)
                cand[i].track = nt;
        }
    } else {
        const Clip& o = m_orig[0];
        Clip& n = cand[0];
        const MediaAsset* a = m_project->asset(o.assetId);
        const bool still = (a && a->isImage) || o.freeze || o.kind != KindMedia;   // layers can be stretched freely, like stills
        if (m_mode == Mode::TrimL) {
            const double ns = snap(o.start + (t - m_pressTime), selection);
            double dt = ns - o.start;
            dt = std::max(dt, -o.start);
            if (!still) dt = std::max(dt, -o.in / o.speed);   // can't reveal footage before the start of the file
            dt = std::min(dt, o.dur - kMinDur);
            n.start = o.start + dt;
            n.in = still ? o.in : o.in + dt * o.speed;
            n.dur = o.dur - dt;
            for (auto it = n.keys.begin(); it != n.keys.end(); ++it)      // keyframes stay put on the timeline
                for (Keyframe& k : it.value()) k.t -= dt;
        } else {
            const double maxDur = (a && !still) ? (a->duration - o.in) / o.speed : 1e9;
            const double ne = snap(o.end() + (t - m_pressTime), selection);
            n.dur = std::clamp(ne - o.start, kMinDur, std::max(kMinDur, maxDur));
        }
    }
    if (candidateValid(cand, selection)) {
        m_project->replace(m_live, cand);
        m_live = cand;
    }
    update();
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const Mode mode = m_mode;
    m_mode = Mode::None;
    m_snapLine = -1;
    if (mode == Mode::Band) {
        if ((e->position() - m_pressPos).manhattanLength() < 4) {   // it was a click: deselect and move the playhead
            if (!m_selBase.isEmpty()) m_sel = m_selBase; else m_sel.clear();
            emit selectionChanged();
            scrubTo(e->position().x());
        }
        update();
        return;
    }
    if (!m_collapseTo.isEmpty()) {
        if (m_live == m_orig) { m_sel = {m_collapseTo}; emit selectionChanged(); }
        m_collapseTo.clear();
    }
    if ((mode == Mode::Move || mode == Mode::TrimL || mode == Mode::TrimR || mode == Mode::KeyMove) && m_live != m_orig) {
        const QVector<Clip> live = m_live;
        m_project->replace(live, m_orig);   // revert silently, then re-apply as ONE undoable command
        m_stack->push(new ChangeClipsCommand(m_project, m_orig, live, mode == Mode::Move ? "Move clip" : mode == Mode::KeyMove ? "Move keyframe" : "Trim clip"));
    }
    m_orig.clear(); m_live.clear();
    update();
}

void TimelineWidget::wheelEvent(QWheelEvent* e) {
    const int dy = e->angleDelta().y() ? e->angleDelta().y() : e->angleDelta().x();
    if (e->modifiers() & Qt::ControlModifier) {
        const double tAtCursor = tFor(e->position().x());
        m_pps = std::clamp(m_pps * std::pow(1.15, dy / 120.0), 4.0, 2400.0);
        m_scroll = std::max(0.0, tAtCursor - (e->position().x() - kHeaderW) / m_pps);
    } else {
        m_scroll = std::max(0.0, m_scroll - (dy / 120.0) * 80.0 / m_pps);
    }
    update();
}

// ---- right-click on a track header: add / delete tracks ----------------------------------------------------------------

void TimelineWidget::contextMenuEvent(QContextMenuEvent* e) {
    const int row = rowAt(e->pos().y());
    if (row < 0 || e->pos().x() >= kHeaderW) return;
    const Track& tr = m_project->tracks[row];
    QMenu menu;
    auto add = [&](const QString& text, TrackKind kind, int index) {
        menu.addAction(text, [this, kind, index] { if (QUndoCommand* c = makeInsertTrackCommand(*m_project, kind, index)) m_stack->push(c); });
    };
    if (tr.kind == TrackKind::Video) {
        add("Add video track above", TrackKind::Video, row);
        add("Add video track below", TrackKind::Video, row + 1);
        add("Add audio track", TrackKind::Audio, m_project->tracks.size());
    } else {
        add("Add audio track above", TrackKind::Audio, row);
        add("Add audio track below", TrackKind::Audio, row + 1);
    }
    menu.addSeparator();
    menu.addAction(QString("Delete track %1").arg(tr.name), [this, row] {
        int clips = 0;
        for (const Clip& c : m_project->clips) if (c.track == row) ++clips;
        if (clips > 0 && QMessageBox::question(this, "Delete track", QString("Delete track %1 and the %2 clip(s) on it?\n\nYou can undo this with Ctrl+Z.").arg(m_project->tracks[row].name).arg(clips)) != QMessageBox::Yes) return;
        QString err;
        if (QUndoCommand* c = makeRemoveTrackCommand(*m_project, row, true, &err)) m_stack->push(c);
        else QMessageBox::information(this, "Delete track", err);
    });
    menu.exec(e->globalPos());
}

// ---- drag & drop from the media / effects panels -------------------------------------------------------

bool TimelineWidget::dropAllowed(const QString& assetId, int row) const {
    const MediaAsset* a = m_project->asset(assetId);
    if (!a || row < 0 || a->missing || m_project->tracks[row].locked) return false;
    const TrackKind k = m_project->tracks[row].kind;
    return k == TrackKind::Video ? a->hasVideo : (!a->hasVideo && a->hasAudio);
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasFormat(kAssetMime) || e->mimeData()->hasFormat(kEffectMime)) e->acceptProposedAction(); else e->ignore();
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* e) {
    if (e->mimeData()->hasFormat(kEffectMime)) {
        const EffectPreset* pr = findPreset(QString::fromUtf8(e->mimeData()->data(kEffectMime)));
        if (pr && pr->layer) {                                // a separate layer: show where it will land
            const int row = rowAt(e->position().y());
            const bool ok = row >= 0 && m_project->tracks[row].kind == TrackKind::Video && !m_project->tracks[row].locked;
            m_layerRow = ok ? row : -1;
            m_layerTime = std::max(0.0, tFor(e->position().x()));
            m_fxTarget.clear();
            update();
            if (ok) e->acceptProposedAction(); else e->ignore();
            return;
        }
        const Hit h = hitTest(e->position());
        m_layerRow = -1;
        const bool ok = h.index >= 0 && (m_project->clips[h.index].kind == KindMedia || m_project->clips[h.index].kind == KindText) && m_project->tracks[m_project->clips[h.index].track].kind == TrackKind::Video;
        m_fxTarget = ok ? m_project->clips[h.index].id : QString();
        update();
        if (ok) e->acceptProposedAction(); else e->ignore();
        return;
    }
    const QString id = QString::fromUtf8(e->mimeData()->data(kAssetMime));
    const int row = rowAt(e->position().y());
    if (!dropAllowed(id, row)) { m_dropAsset.clear(); update(); e->ignore(); return; }
    m_dropAsset = id; m_dropRow = row;
    const MediaAsset* a = m_project->asset(id);
    const double dur = a->isImage ? kStill : a->duration;
    m_dropTime = m_project->findFreeStart(row, std::max(0.0, tFor(e->position().x())), dur);
    e->acceptProposedAction();
    update();
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*) { m_dropAsset.clear(); m_fxTarget.clear(); m_layerRow = -1; update(); }

void TimelineWidget::dropEvent(QDropEvent* e) {
    if (e->mimeData()->hasFormat(kEffectMime)) {
        const QString target = m_fxTarget;
        m_fxTarget.clear();
        const EffectPreset* pr = findPreset(QString::fromUtf8(e->mimeData()->data(kEffectMime)));
        if (pr && pr->layer) {
            const int row = m_layerRow;
            m_layerRow = -1;
            if (row >= 0)
                if (QUndoCommand* cmd = makeAddLayerCommand(*m_project, pr->effect, row, m_layerTime)) { m_stack->push(cmd); e->acceptProposedAction(); }
            update();
            return;
        }
        if (pr && !target.isEmpty())
            if (QUndoCommand* cmd = makeApplyEffectCommand(*m_project, {target}, pr->effect)) {
                m_stack->push(cmd);
                m_sel = {target};
                emit selectionChanged();
                e->acceptProposedAction();
            }
        update();
        return;
    }
    const QString id = QString::fromUtf8(e->mimeData()->data(kAssetMime));
    const int row = rowAt(e->position().y());
    m_dropAsset.clear();
    if (dropAllowed(id, row))
        if (QUndoCommand* cmd = makeAddClipCommand(*m_project, id, row, tFor(e->position().x()))) { m_stack->push(cmd); e->acceptProposedAction(); }
    update();
}

} // namespace ve
