#include "PreviewWidget.h"
#include "media/Compositor.h"
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QMouseEvent>
#include <QPainter>
#include <QVector4D>

namespace ve {

// Fit `inner` into `outer` keeping aspect ratio, centered.
static QRectF fitRect(const QRectF& outer, const QSizeF& inner) {
    if (inner.isEmpty() || outer.isEmpty()) return QRectF();
    const double s = qMin(outer.width() / inner.width(), outer.height() / inner.height());
    const QSizeF sz(inner.width() * s, inner.height() * s);
    return QRectF(outer.center().x() - sz.width() / 2, outer.center().y() - sz.height() / 2, sz.width(), sz.height());
}

// The picture area inside a preview widget: a small margin is left all round so the selection handles stay reachable
// even when the picture fills the preview.
static QRectF canvasIn(const QRectF& widgetRect, const QSizeF& canvas) { return fitRect(widgetRect.adjusted(16, 16, -16, -16), canvas); }

MaskOutline::MaskOutline(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
}

void MaskOutline::paintEvent(QPaintEvent*) {
    if (!m_mask.on || m_canvas.isEmpty()) return;
    const QRectF canvas = canvasIn(QRectF(0, 0, width(), height()), m_canvas);
    const double k = canvas.width() / m_canvas.width();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(canvas.center() + QPointF(m_mask.x * k, m_mask.y * k));
    p.rotate(m_mask.rot);
    const QRectF r(-m_mask.w * k / 2, -m_mask.h * k / 2, m_mask.w * k, m_mask.h * k);
    p.setPen(QPen(QColor(0, 0, 0, 160), 3));
    p.drawRect(r);
    p.setPen(QPen(QColor("#ffd54a"), 1.5, Qt::DashLine));
    p.drawRect(r);
    if (m_mask.feather > 1) { p.setPen(QPen(QColor(255, 213, 74, 110), 1, Qt::DotLine)); const double f = m_mask.feather * k / 2; p.drawRect(r.adjusted(-f, -f, f, f)); p.drawRect(r.adjusted(f, f, -f, -f)); }
    p.setBrush(QColor("#ffd54a")); p.setPen(Qt::NoPen);                                    // corner handles
    for (QPointF pt : {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()}) p.drawRect(QRectF(pt.x() - 3, pt.y() - 3, 6, 6));
}

// ---- direct manipulation overlay ------------------------------------------------------------------------------------

TransformOverlay::TransformOverlay(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_NoSystemBackground);
    setMouseTracking(true);
}

QRectF TransformOverlay::canvasRect() const { return canvasIn(QRectF(0, 0, width(), height()), m_canvas); }
double TransformOverlay::k() const { return canvasRect().width() / std::max(1, m_canvas.width()); }
QPointF TransformOverlay::toProject(const QPointF& w) const { return (w - canvasRect().center()) / k(); }
QPointF TransformOverlay::widgetPoint(double px, double py) const { return canvasRect().center() + QPointF(px, py) * k(); }

QPointF TransformOverlay::toLocal(const QPointF& P) const {
    const QPointF d = P - QPointF(m_box.x, m_box.y);
    const double a = m_box.rot * M_PI / 180.0, ca = std::cos(a), sa = std::sin(a);
    return QPointF(d.x() * ca + d.y() * sa, -d.x() * sa + d.y() * ca);       // inverse of a clockwise rotation (y points down)
}

QPointF TransformOverlay::handlePoint(int idx) const {
    const double hw = m_box.w / 2, hh = m_box.h / 2;
    const QPointF pts[9] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}, {0, -hh}, {hw, 0}, {0, hh}, {-hw, 0}, {0, -hh - 30.0 / k()}};
    const QPointF l = pts[std::clamp(idx, 0, 8)];
    const double a = m_box.rot * M_PI / 180.0, ca = std::cos(a), sa = std::sin(a);
    return widgetPoint(m_box.x + l.x() * ca - l.y() * sa, m_box.y + l.x() * sa + l.y() * ca);
}

int TransformOverlay::handleAt(const QPointF& P) const {
    const QPointF l = toLocal(P);
    const double hw = m_box.w / 2, hh = m_box.h / 2, tol = 10.0 / k();
    const QPointF pts[9] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}, {0, -hh}, {hw, 0}, {0, hh}, {-hw, 0}, {0, -hh - 30.0 / k()}};
    int best = -1; double bd = tol;
    const int order[9] = {8, 0, 1, 2, 3, 4, 5, 6, 7};      // rotation handle first, then corners, then edges (nearest wins)
    for (int i : order) {
        const double d = std::hypot(l.x() - pts[i].x(), l.y() - pts[i].y());
        if (d <= bd) { bd = d; best = i; if (d < tol * 0.01) break; }
    }
    return best;
}

TransformOverlay::Mode TransformOverlay::hit(const QPointF& P, int* handle) const {
    if (!m_box.on) return Mode::None;
    const int h = handleAt(P);
    if (handle) *handle = h;
    if (h == 8) return Mode::Rotate;
    if (h >= 0) return Mode::Scale;
    const QPointF l = toLocal(P);
    return (std::abs(l.x()) <= m_box.w / 2 && std::abs(l.y()) <= m_box.h / 2) ? Mode::Move : Mode::None;
}

void TransformOverlay::set(const TransformBox& b, const QSize& canvas) {
    m_box = b;
    m_canvas = canvas;
    if (!b.on) { m_mode = Mode::None; unsetCursor(); }
    update();
}

void TransformOverlay::updateCursor(const QPointF& pos) {
    int h = -1;
    const Mode m = hit(toProject(pos), &h);
    if (m == Mode::Move) setCursor(Qt::SizeAllCursor);
    else if (m == Mode::Rotate) setCursor(Qt::PointingHandCursor);
    else if (m == Mode::Scale) setCursor(h == 0 || h == 2 ? Qt::SizeFDiagCursor : h == 1 || h == 3 ? Qt::SizeBDiagCursor : (h == 4 || h == 6) ? Qt::SizeVerCursor : Qt::SizeHorCursor);
    else unsetCursor();
}

void TransformOverlay::mousePressEvent(QMouseEvent* e) {
    const QPointF P = toProject(e->position());
    int h = -1;
    const Mode m = e->button() == Qt::LeftButton ? hit(P, &h) : Mode::None;
    if (m == Mode::None) {                                // not on the box: maybe select another picture, and let the preview see the click too
        if (e->button() == Qt::LeftButton && canvasRect().contains(e->position()) && onClick) onClick(P);
        e->ignore();
        return;
    }
    m_mode = m; m_box0 = m_box; m_p0 = P;
    if (onDrag) onDrag(Begin, TransformDrag());
}

void TransformOverlay::mouseMoveEvent(QMouseEvent* e) {
    if (m_mode == Mode::None) { updateCursor(e->position()); e->ignore(); return; }
    const QPointF P = toProject(e->position());
    const QPointF c0(m_box0.x, m_box0.y);
    TransformDrag d;
    d.x = m_box0.x; d.y = m_box0.y; d.rot = m_box0.rot;
    m_guideX = m_guideY = false;
    if (m_mode == Mode::Move) {
        QPointF n = c0 + (P - m_p0);
        const double thr = 8.0 / k();                                                // gentle snap to the canvas centre lines
        if (std::abs(n.x()) < thr) { n.setX(0); m_guideX = true; }
        if (std::abs(n.y()) < thr) { n.setY(0); m_guideY = true; }
        d.x = std::round(n.x() * 10) / 10; d.y = std::round(n.y() * 10) / 10; d.moved = true;
    } else if (m_mode == Mode::Scale) {
        const double r0 = std::hypot(m_p0.x() - c0.x(), m_p0.y() - c0.y());
        const double r1 = std::hypot(P.x() - c0.x(), P.y() - c0.y());
        d.scaleFactor = r0 > 1e-6 ? std::clamp(r1 / r0, 0.01, 100.0) : 1.0; d.scaled = true;
    } else {
        const double a0 = std::atan2(m_p0.y() - c0.y(), m_p0.x() - c0.x()), a1 = std::atan2(P.y() - c0.y(), P.x() - c0.x());
        double deg = m_box0.rot + (a1 - a0) * 180.0 / M_PI;
        const double step = 15.0, nearest = std::round(deg / step) * step;
        if ((e->modifiers() & Qt::ShiftModifier) || std::abs(deg - nearest) < 2.5) deg = nearest;   // snaps to 15° steps
        d.rot = std::round(deg * 10) / 10; d.rotated = true;
    }
    if (onDrag) onDrag(Change, d);
    update();
}

void TransformOverlay::mouseReleaseEvent(QMouseEvent* e) {
    if (m_mode == Mode::None) { e->ignore(); return; }
    m_mode = Mode::None;
    m_guideX = m_guideY = false;
    if (onDrag) onDrag(End, TransformDrag());
    update();
    updateCursor(e->position());
}

void TransformOverlay::paintEvent(QPaintEvent*) {
    if (!m_box.on) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double kk = k();
    if (m_guideX || m_guideY) {                                                      // centre guides, only while snapped
        const QRectF cr = canvasRect();
        p.setPen(QPen(QColor(255, 255, 255, 150), 1, Qt::DashLine));
        if (m_guideX) p.drawLine(QPointF(cr.center().x(), cr.top()), QPointF(cr.center().x(), cr.bottom()));
        if (m_guideY) p.drawLine(QPointF(cr.left(), cr.center().y()), QPointF(cr.right(), cr.center().y()));
    }
    p.translate(widgetPoint(m_box.x, m_box.y));
    p.rotate(m_box.rot);
    const double hw = m_box.w * kk / 2, hh = m_box.h * kk / 2;
    const QRectF r(-hw, -hh, 2 * hw, 2 * hh);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 120), 3)); p.drawRect(r);                          // dark underline keeps it readable on bright pictures
    p.setPen(QPen(Qt::white, 1.3)); p.drawRect(r);
    const QPointF rh(0, -hh - 30);
    p.setPen(QPen(QColor(0, 0, 0, 120), 3)); p.drawLine(QPointF(0, -hh), rh);
    p.setPen(QPen(Qt::white, 1.2)); p.drawLine(QPointF(0, -hh), rh);
    auto square = [&](QPointF c, double s) { p.setPen(QPen(QColor(0, 0, 0, 170), 1)); p.setBrush(Qt::white); p.drawRect(QRectF(c.x() - s / 2, c.y() - s / 2, s, s)); };
    for (QPointF c : {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()}) square(c, 9);
    for (QPointF c : {QPointF(0, -hh), QPointF(hw, 0), QPointF(0, hh), QPointF(-hw, 0)}) square(c, 6);
    p.setPen(QPen(QColor(0, 0, 0, 170), 1)); p.setBrush(Qt::white); p.drawEllipse(rh, 5.5, 5.5);
}

// ---- GPU preview ----------------------------------------------------------------------------------------------------

GlPreview::GlPreview(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(320, 180);
    m_outline = new MaskOutline(this);
    m_outline->setGeometry(rect());
    m_xform = new TransformOverlay(this);
    m_xform->setGeometry(rect());
}

void GlPreview::resizeEvent(QResizeEvent* e) {
    QOpenGLWidget::resizeEvent(e);
    if (m_outline) m_outline->setGeometry(rect());
    if (m_xform) m_xform->setGeometry(rect());
}

GlPreview::~GlPreview() {
    makeCurrent();
    if (m_comp) m_comp->cleanup();
    m_comp.reset();
    m_fbo.reset();
    delete m_blit;
    delete m_vao;
    doneCurrent();
}

void GlPreview::setCanvasSize(const QSize& s) { m_canvas = s; m_dirty = true; if (m_outline) m_outline->set(m_maskFx, s); update(); }
void GlPreview::setRenderScale(double s) { if (s != m_scale) { m_scale = s; m_dirty = true; update(); } }
void GlPreview::setLayers(const QVector<RenderLayer>& layers) { m_layers = layers; m_dirty = true; update(); }

void GlPreview::initializeGL() {
    initializeOpenGLFunctions();
    m_comp = std::make_unique<Compositor>();
    m_comp->init();
    m_blit = new QOpenGLShaderProgram;
    m_blit->addShaderFromSourceCode(QOpenGLShader::Vertex, R"(#version 330 core
uniform vec4 uRect;            // x0,y0,x1,y1 in clip space
out vec2 vUv;
void main() {
    vec2 c[4] = vec2[4](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));
    vec2 q = c[gl_VertexID];
    vUv = q;                   // the FBO texture is stored bottom-up, like clip space
    gl_Position = vec4(mix(uRect.xy, uRect.zw, q), 0.0, 1.0);
})");
    m_blit->addShaderFromSourceCode(QOpenGLShader::Fragment, R"(#version 330 core
in vec2 vUv; out vec4 o; uniform sampler2D uTex;
void main() { o = texture(uTex, vUv); })");
    m_blit->link();
    m_vao = new QOpenGLVertexArrayObject;
    m_vao->create();
}

void GlPreview::paintGL() {
    if (m_outline && m_outline->size() != size()) m_outline->setGeometry(rect());
    if (!m_comp || m_canvas.isEmpty()) return;
    const QSize fs(qMax(2, int(m_canvas.width() * m_scale)), qMax(2, int(m_canvas.height() * m_scale)));
    if (!m_fbo || m_fbo->size() != fs) { m_fbo = std::make_unique<QOpenGLFramebufferObject>(fs); m_dirty = true; }
    if (m_dirty) { m_comp->render(m_layers, m_canvas, m_fbo.get()); m_dirty = false; }

    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    const double dpr = devicePixelRatioF();
    glViewport(0, 0, int(width() * dpr), int(height() * dpr));
    glClearColor(0.105f, 0.105f, 0.115f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const QRectF r = QRectF(0, 0, width(), height());
    const QRectF canvas = canvasIn(r, m_canvas);
    const double w = width(), h = height();
    m_blit->bind();
    m_vao->bind();
    m_blit->setUniformValue("uRect", QVector4D(float(canvas.left() / w * 2 - 1), float(1 - canvas.bottom() / h * 2),
                                               float(canvas.right() / w * 2 - 1), float(1 - canvas.top() / h * 2)));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_fbo->texture());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_blit->setUniformValue("uTex", 0);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao->release();
    m_blit->release();
}

void GlPreview::mousePressEvent(QMouseEvent* e) {
    if (!m_pick) { QOpenGLWidget::mousePressEvent(e); return; }
    const QRectF canvas = canvasIn(QRectF(0, 0, width(), height()), m_canvas);
    if (!canvas.contains(e->position())) return;                      // clicks outside the picture are ignored
    const QImage img = grabFramebuffer();
    const double dpr = devicePixelRatioF();
    const QColor col = img.pixelColor(qBound(0, int(e->position().x() * dpr), img.width() - 1), qBound(0, int(e->position().y() * dpr), img.height() - 1));
    m_pick = false;
    setCursor(Qt::ArrowCursor);
    if (m_pickCb) m_pickCb(col);
}

// ------------------------------------------------------------------------------------------

SoftwarePreview::SoftwarePreview(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 180);
    m_outline = new MaskOutline(this);
    m_xform = new TransformOverlay(this);
}

void SoftwarePreview::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (m_outline) m_outline->setGeometry(rect());
    if (m_xform) m_xform->setGeometry(rect());
}
void SoftwarePreview::setCanvasSize(const QSize& s) { m_canvas = s; update(); }
void SoftwarePreview::setLayers(const QVector<RenderLayer>& layers) { m_layers = layers; update(); }

void SoftwarePreview::mousePressEvent(QMouseEvent* e) {
    if (!m_pick) { QWidget::mousePressEvent(e); return; }
    if (!canvasIn(rect(), m_canvas).contains(e->position())) return;
    const QImage img = grab().toImage();
    const QColor col = img.pixelColor(qBound(0, int(e->position().x()), img.width() - 1), qBound(0, int(e->position().y()), img.height() - 1));
    m_pick = false;
    setCursor(Qt::ArrowCursor);
    if (m_pickCb) m_pickCb(col);
}

void SoftwarePreview::paintEvent(QPaintEvent*) {
    if (m_outline && m_outline->size() != size()) m_outline->setGeometry(rect());
    QPainter p(this);
    p.fillRect(rect(), QColor(27, 27, 29));
    const QRectF canvas = canvasIn(rect(), m_canvas);
    p.fillRect(canvas, Qt::black);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setClipRect(canvas);
    const double k = canvas.width() / qMax(1, m_canvas.width());
    for (const RenderLayer& L : m_layers) {
        if (L.image.isNull()) continue;
        const QRectF fit = fitRect(QRectF(QPointF(0, 0), QSizeF(canvas.size())), L.image.size());
        p.save();
        p.translate(canvas.center() + QPointF(L.fx.dx * k, L.fx.dy * k));
        p.rotate(L.fx.rotation);
        p.scale(L.fx.flipX ? -L.fx.scale : L.fx.scale, L.fx.scale);
        p.drawImage(QRectF(-fit.width() / 2, -fit.height() / 2, fit.width(), fit.height()), L.image);
        p.restore();
    }
}

} // namespace ve
