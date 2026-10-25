#pragma once
#include "core/Effects.h"
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QSize>
#include <QVector>
#include <QWidget>
#include <functional>
#include <memory>

class QOpenGLShaderProgram;
class QOpenGLVertexArrayObject;
class QOpenGLFramebufferObject;

namespace ve {

class Compositor;

// The selected picture's frame on the canvas (project pixels; x/y = centre offset from the canvas centre, rot in degrees clockwise).
struct TransformBox {
    bool on = false;
    double x = 0, y = 0, w = 0, h = 0, rot = 0;
};
// What a drag on the box wants: absolute position / rotation, or a scale factor relative to when the drag began.
struct TransformDrag {
    double x = 0, y = 0, rot = 0, scaleFactor = 1;
    bool moved = false, scaled = false, rotated = false;
};

// CapCut-style direct manipulation: a thin bounding box with corner / edge handles and a rotation handle over the preview.
// Drag inside to move, drag a corner or edge handle to scale (about the centre), drag the round handle to rotate.
// Clicks that miss the box are reported (to select another picture) and passed on to the preview.
class TransformOverlay : public QWidget {
public:
    enum Phase { Begin, Change, End };
    explicit TransformOverlay(QWidget* parent);
    void set(const TransformBox& b, const QSize& canvas);
    const TransformBox& box() const { return m_box; }
    QRectF canvasRect() const;
    QPointF widgetPoint(double px, double py) const;      // project coordinates -> widget coordinates
    QPointF handlePoint(int idx) const;                    // widget coords: 0-3 corners (TL TR BR BL), 4-7 edges (T R B L), 8 rotation
    std::function<void(Phase, const TransformDrag&)> onDrag;
    std::function<void(const QPointF&)> onClick;           // project coordinates (origin = canvas centre)
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
private:
    enum class Mode { None, Move, Scale, Rotate };
    double k() const;
    QPointF toProject(const QPointF& w) const;
    QPointF toLocal(const QPointF& projectPt) const;       // into the box's own (unrotated) frame, centre = origin
    int handleAt(const QPointF& projectPt) const;          // 0-8 as above, or -1
    Mode hit(const QPointF& projectPt, int* handle) const;
    void updateCursor(const QPointF& widgetPos);
    TransformBox m_box;
    QSize m_canvas{1920, 1080};
    Mode m_mode = Mode::None;
    TransformBox m_box0;
    QPointF m_p0;
    bool m_guideX = false, m_guideY = false;
};

// Common interface so the main window doesn't care whether the GPU or CPU path is in use.
class PreviewSurface {
public:
    virtual ~PreviewSurface() = default;
    virtual void setCanvasSize(const QSize& s) = 0;                  // project resolution (defines aspect ratio)
    virtual void setRenderScale(double s) = 0;                       // preview quality: 1, 0.5, 0.25
    virtual void setLayers(const QVector<RenderLayer>& layers) = 0;  // bottom → top
    virtual QWidget* widget() = 0;
    virtual QString name() const = 0;
    virtual void setMaskOutline(const MaskFx& m) = 0;                // yellow outline of the selected clip's mask (m.on = false hides it)
    // Eyedropper: the next click on the picture calls back with the colour under the cursor.
    virtual void setPickMode(bool on, std::function<void(const QColor&)> cb = nullptr) = 0;
    virtual TransformOverlay* transformOverlay() = 0;
};

// Transparent child widget that draws the mask rectangle over the preview.
class MaskOutline : public QWidget {
public:
    explicit MaskOutline(QWidget* parent);
    void set(const MaskFx& m, const QSize& canvas) { m_mask = m; m_canvas = canvas; update(); }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    MaskFx m_mask;
    QSize m_canvas{1920, 1080};
};

// GPU preview: the shared Compositor renders the frame (effects included) into an FBO, which is shown letterboxed.
class GlPreview : public QOpenGLWidget, public PreviewSurface, protected QOpenGLFunctions {
public:
    explicit GlPreview(QWidget* parent = nullptr);
    ~GlPreview() override;
    void setCanvasSize(const QSize& s) override;
    void setRenderScale(double s) override;
    void setLayers(const QVector<RenderLayer>& layers) override;
    QWidget* widget() override { return this; }
    QString name() const override { return "OpenGL"; }
    void setMaskOutline(const MaskFx& m) override { m_maskFx = m; if (m_outline) m_outline->set(m, m_canvas); }
    void setPickMode(bool on, std::function<void(const QColor&)> cb = nullptr) override { m_pick = on; m_pickCb = std::move(cb); setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor); }
    TransformOverlay* transformOverlay() override { return m_xform; }
    QSize minimumSizeHint() const override { return {320, 180}; }

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    QSize m_canvas{1920, 1080};
    double m_scale = 1.0;
    QVector<RenderLayer> m_layers;
    bool m_dirty = true;
    MaskFx m_maskFx;
    MaskOutline* m_outline = nullptr;
    TransformOverlay* m_xform = nullptr;
    bool m_pick = false;
    std::function<void(const QColor&)> m_pickCb;
    std::unique_ptr<Compositor> m_comp;
    std::unique_ptr<QOpenGLFramebufferObject> m_fbo;
    QOpenGLShaderProgram* m_blit = nullptr;
    QOpenGLVertexArrayObject* m_vao = nullptr;
};

// CPU fallback (used when OpenGL 3.3 isn't available, or VE_SOFTWARE_PREVIEW=1).
// Shows move / zoom / rotate (so shake and swoosh motion are visible) but not the motion blur.
class SoftwarePreview : public QWidget, public PreviewSurface {
public:
    explicit SoftwarePreview(QWidget* parent = nullptr);
    void setCanvasSize(const QSize& s) override;
    void setRenderScale(double) override {}
    void setLayers(const QVector<RenderLayer>& layers) override;
    QWidget* widget() override { return this; }
    QString name() const override { return "Software (transform only: no effects)"; }
    void setMaskOutline(const MaskFx& m) override { if (m_outline) m_outline->set(m, m_canvas); }
    void setPickMode(bool on, std::function<void(const QColor&)> cb = nullptr) override { m_pick = on; m_pickCb = std::move(cb); setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor); }
    TransformOverlay* transformOverlay() override { return m_xform; }
    QSize minimumSizeHint() const override { return {320, 180}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    QSize m_canvas{1920, 1080};
    QVector<RenderLayer> m_layers;
    MaskOutline* m_outline = nullptr;
    TransformOverlay* m_xform = nullptr;
    bool m_pick = false;
    std::function<void(const QColor&)> m_pickCb;
};

} // namespace ve
