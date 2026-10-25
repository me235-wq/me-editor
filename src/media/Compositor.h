#pragma once
#include "core/Effects.h"
#include <QOpenGLFunctions>
#include <QSize>
#include <QVector>
#include <functional>

class QOpenGLShaderProgram;
class QOpenGLTexture;
class QOpenGLVertexArrayObject;
class QOpenGLFramebufferObject;

namespace ve {

// The one renderer. The live preview and the exporter both call render(), so what you see is what you export.
//
// Per picture layer: fit into the canvas → move / zoom / rotate → motion blur → its own effects (shadow, border, blur, mirror,
//   chroma key, black flash) → rectangle mask → alpha-composite over what is below.
// Per effect / adjustment layer: the effects run on the whole composite built so far, optionally limited to a mask.
// All GL calls need a current context.
class Compositor : protected QOpenGLFunctions {
public:
    Compositor();
    ~Compositor();
    bool init();        // call once with the context current
    void cleanup();     // call with the context current, before it is destroyed

    // Draws the project canvas (project coordinates `projectSize`) into `target`, which may be any size
    // (smaller = faster preview). Leaves `target` bound.
    void render(const QVector<RenderLayer>& layers, const QSize& projectSize, QOpenGLFramebufferObject* target);

private:
    using Setup = std::function<void(QOpenGLShaderProgram*)>;
    QOpenGLFramebufferObject* fboAt(int idx, QOpenGLFramebufferObject* target) { return idx < 0 ? target : m_buf[idx]; }
    static int freeIdx(int a, int b) { for (int i = 0; i < 3; ++i) if (i != a && i != b) return i; return 0; }
    void bindTex(unsigned unit, unsigned tex);
    void setMask(QOpenGLShaderProgram* pr, const MaskFx* m, double k, const QSize& S);
    void drawLayer(QOpenGLTexture* tex, const float corners[8], float opacity, const QSize& S, const MaskFx* mask, double k);
    void pass(QOpenGLShaderProgram* pr, QOpenGLFramebufferObject* dst, unsigned in, unsigned orig, double strength, const MaskFx* mask, double k, const Setup& setup);
    int runPass(const FxPass& f, int cur, const MaskFx* mask, double k, QOpenGLFramebufferObject* target);
    QOpenGLShaderProgram* build(const char* vs, const QString& fs);

    QOpenGLShaderProgram *m_layer = nullptr, *m_blit = nullptr, *m_copy = nullptr, *m_mblur = nullptr, *m_gauss = nullptr, *m_shadow = nullptr,
                         *m_border = nullptr, *m_chroma = nullptr, *m_dim = nullptr, *m_adjust = nullptr;
    QOpenGLVertexArrayObject* m_vao = nullptr;
    QOpenGLFramebufferObject* m_buf[3] = {nullptr, nullptr, nullptr};
    QVector<QOpenGLTexture*> m_tex;
    bool m_ok = true;
};

} // namespace ve
