#include "Compositor.h"
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <algorithm>
#include <cmath>

namespace ve {

// ---------------------------------------------------------------------------------------------------------
// Shaders. Everything works on premultiplied alpha. Textures are FBO textures (stored bottom-up, v = 0 at the bottom).
// ---------------------------------------------------------------------------------------------------------

static const char* kLayerVs = R"(#version 330 core
uniform vec2 uCorner[4];   // TL, TR, BL, BR in target pixels (y down)
uniform vec2 uSize;
out vec2 vUv;
void main() {
    vec2 uvs[4] = vec2[4](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));
    vec2 p = uCorner[gl_VertexID];
    gl_Position = vec4(p.x / uSize.x * 2.0 - 1.0, 1.0 - p.y / uSize.y * 2.0, 0.0, 1.0);
    vUv = uvs[gl_VertexID];
})";

static const char* kFsVs = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 c[4] = vec2[4](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));
    vec2 q = c[gl_VertexID];
    vUv = q;
    gl_Position = vec4(q * 2.0 - 1.0, 0.0, 1.0);
})";

// Shared by every fragment shader: the rectangle mask and the "mix the effect over the original" helper.
static const char* kPre = R"(#version 330 core
in vec2 vUv; out vec4 o;
uniform sampler2D uTex;      // input of this step
uniform sampler2D uOrig;     // what it looked like before the effect (for mixing)
uniform vec2 uSize;          // target size in pixels
uniform float uStrength;     // 0..1: how much of the effect to apply
uniform vec4 uMaskA;         // centre x, centre y, half width, half height (pixels, y down)
uniform vec4 uMaskB;         // rotation (rad), feather (px), invert, enabled
float maskCov() {
    if (uMaskB.w < 0.5) return 1.0;
    vec2 p = vec2(gl_FragCoord.x, uSize.y - gl_FragCoord.y) - uMaskA.xy;
    float c = cos(uMaskB.x), s = sin(uMaskB.x);
    p = vec2(c * p.x + s * p.y, -s * p.x + c * p.y);
    vec2 q = abs(p) - uMaskA.zw;
    float sd = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
    float cov = clamp(0.5 - sd / max(uMaskB.y, 1.0), 0.0, 1.0);
    return uMaskB.z > 0.5 ? 1.0 - cov : cov;
}
vec4 finish(vec4 fx) { return mix(texture(uOrig, vUv), fx, uStrength * maskCov()); }
)";

static const char* kLayerFs = R"(
uniform float uOpacity;
void main() { vec4 c = texture(uTex, vUv); o = vec4(c.rgb * c.a, c.a) * uOpacity * maskCov(); })";   // straight RGBA in, premultiplied out

static const char* kBlitFs = R"(
void main() { o = texture(uTex, vUv) * maskCov(); })";

static const char* kCopyFs = R"(
void main() { o = texture(uTex, vUv); })";

// Box-filtered line blur along uStep: the motion blur of swooshes and shake.
static const char* kMBlurFs = R"(
uniform vec2 uStep;
void main() {
    const int N = 48;
    vec4 s = vec4(0.0);
    for (int i = 0; i < N; ++i) { float f = (float(i) + 0.5) / float(N) - 0.5; s += texture(uTex, vUv + uStep * f); }
    o = s / float(N);
})";

// Separable gaussian. Pass 1 (uFinal = 0) is raw; pass 2 (uFinal = 1) is mixed over the original through strength + mask.
static const char* kGaussFs = R"(
uniform vec2 uDir; uniform float uRadius; uniform int uFinal;
void main() {
    const int N = 24;
    float sigma = max(uRadius * 0.5, 0.0001);
    vec4 s = vec4(0.0); float ws = 0.0;
    for (int i = -N; i <= N; ++i) {
        float x = float(i) / float(N) * uRadius;
        float w = exp(-0.5 * x * x / (sigma * sigma));
        s += w * texture(uTex, vUv + uDir * x); ws += w;
    }
    vec4 r = s / ws;
    o = (uFinal == 1) ? finish(r) : r;
})";

static const char* kShadowFs = R"(
uniform vec2 uOff; uniform float uBlur; uniform vec3 uColor; uniform float uOpacity; uniform vec2 uPx;
void main() {
    vec4 src = texture(uTex, vUv);
    float a = 0.0;
    const int N = 32;
    for (int i = 0; i < N; ++i) {
        float r = sqrt((float(i) + 0.5) / float(N)), th = float(i) * 2.39996323;
        a += texture(uTex, vUv - uOff * uPx + vec2(cos(th), sin(th)) * r * uBlur * uPx).a;
    }
    a /= float(N);
    vec4 sh = vec4(uColor * a * uOpacity, a * uOpacity);
    o = finish(src + sh * (1.0 - src.a));
})";

static const char* kBorderFs = R"(
uniform float uThick; uniform vec3 uColor; uniform vec2 uPx;
float alphaAt(vec2 uv) { if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return 0.0; return texture(uTex, uv).a; }
void main() {
    vec4 c = texture(uTex, vUv);
    float mn = 1.0;
    for (int ring = 1; ring <= 2; ++ring)
        for (int i = 0; i < 16; ++i) {
            float th = 6.2831853 * float(i) / 16.0;
            mn = min(mn, alphaAt(vUv + vec2(cos(th), sin(th)) * uThick * 0.5 * float(ring) * uPx));
        }
    float edge = clamp(c.a * (1.0 - mn) * 1.5, 0.0, 1.0);          // inner stroke: where the picture borders emptiness
    o = finish(mix(c, vec4(uColor, 1.0) * c.a, edge));
})";

static const char* kChromaFs = R"(
uniform vec3 uKey; uniform float uTol, uSoft, uSpill;
vec3 ycc(vec3 c) { return vec3(dot(c, vec3(0.299, 0.587, 0.114)), -0.168736 * c.r - 0.331264 * c.g + 0.5 * c.b, 0.5 * c.r - 0.418688 * c.g - 0.081312 * c.b); }
void main() {
    vec4 c = texture(uTex, vUv);
    vec3 rgb = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
    // distance to the key colour: mostly colour, a little brightness (so a dark shade of the key still goes, a different colour never does)
    float d = length((ycc(rgb) - ycc(uKey)) * vec3(0.45, 1.0, 1.0));
    float k = smoothstep(uTol, uTol + max(uSoft, 0.001), d);                       // 0 = the key colour (transparent), 1 = anything else (untouched)
    float lum = dot(rgb, vec3(0.299, 0.587, 0.114));
    float sp = (1.0 - smoothstep(uTol, uTol + uSoft + 0.2, d)) * uSpill * k;        // colour spill on the kept edge pixels
    rgb = mix(rgb, vec3(lum), sp);
    float a = c.a * k;
    o = finish(vec4(rgb * a, a));
})";

static const char* kDimFs = R"(
uniform float uAmount, uGlitch, uSeed;
float hash(float n) { return fract(sin(n * 91.345 + uSeed * 17.13) * 43758.5453); }
void main() {
    vec2 uv = vUv;
    float row = floor(uv.y * uSize.y / 5.0);                                        // horizontal tearing in bands, different on every flash
    uv.x += (hash(row) - 0.5) * uGlitch * 0.07 * step(0.62, hash(row + 3.7));
    float sp = uGlitch * 0.012;                                                     // red / blue split
    vec4 c = vec4(texture(uTex, uv + vec2(sp, 0.0)).r, texture(uTex, uv).g, texture(uTex, uv - vec2(sp, 0.0)).b, texture(uTex, uv).a);
    o = finish(vec4(c.rgb * (1.0 - uAmount), c.a));
})";

static const char* kAdjustFs = R"(
uniform float uTemp, uTint, uSat, uExp, uCon, uHi, uSh, uSharp, uClar, uVig;
uniform vec2 uPx;
vec3 tap(vec2 d) { return texture(uTex, vUv + d * uPx).rgb; }
void main() {
    vec4 c = texture(uTex, vUv);
    vec3 rgb = c.a > 0.0 ? c.rgb / c.a : c.rgb;
    float l0 = dot(rgb, vec3(0.299, 0.587, 0.114));
    vec3 detail = vec3(0.0);
    if (uSharp > 0.0) {                                                              // sharpen: unsharp mask on the 4 neighbours
        vec3 bl = (tap(vec2(-1, 0)) + tap(vec2(1, 0)) + tap(vec2(0, -1)) + tap(vec2(0, 1))) * 0.25;
        detail += (rgb - bl) * uSharp * 1.5;
    }
    if (uClar > 0.0) {                                                               // clarity: local contrast at a wider radius, mid-tones only
        vec3 bl = vec3(0.0);
        for (int i = 0; i < 12; ++i) { float th = 6.2831853 * float(i) / 12.0; bl += tap(vec2(cos(th), sin(th)) * 7.0); }
        bl /= 12.0;
        float w = 1.0 - pow(abs(l0 * 2.0 - 1.0), 2.0);
        detail += (rgb - bl) * uClar * 1.4 * w;
    }
    rgb *= exp2(uExp * 1.5);                                                         // exposure (stops)
    rgb.r *= 1.0 + 0.25 * uTemp; rgb.b *= 1.0 - 0.25 * uTemp;                        // temperature: warm / cool
    rgb.g *= 1.0 - 0.2 * uTint; rgb.r *= 1.0 + 0.07 * uTint; rgb.b *= 1.0 + 0.07 * uTint;   // tint: green / magenta
    rgb = (rgb - 0.5) * (1.0 + uCon) + 0.5;                                          // contrast
    float l = dot(rgb, vec3(0.299, 0.587, 0.114));
    rgb += uHi * 0.3 * smoothstep(0.4, 1.0, l);                                      // highlights
    rgb += uSh * 0.3 * (1.0 - smoothstep(0.0, 0.6, l));                              // shadows
    l = dot(rgb, vec3(0.299, 0.587, 0.114));
    rgb = mix(vec3(l), rgb, 1.0 + uSat);                                             // saturation
    rgb += detail;
    float r = length(vUv - 0.5) * 1.4142;
    rgb *= 1.0 - uVig * smoothstep(0.3, 1.0, r);                                     // vignette
    rgb = clamp(rgb, 0.0, 1.0);
    o = finish(vec4(rgb * c.a, c.a));
})";

// ---------------------------------------------------------------------------------------------------------

Compositor::Compositor() = default;
Compositor::~Compositor() = default;

QOpenGLShaderProgram* Compositor::build(const char* vs, const QString& fs) {
    auto* p = new QOpenGLShaderProgram;
    const bool ok = p->addShaderFromSourceCode(QOpenGLShader::Vertex, vs) && p->addShaderFromSourceCode(QOpenGLShader::Fragment, fs) && p->link();
    if (!ok) { qWarning("Shader failed: %s", qPrintable(p->log())); m_ok = false; }
    return p;
}

bool Compositor::init() {
    initializeOpenGLFunctions();
    const QString pre = kPre;
    m_layer = build(kLayerVs, pre + kLayerFs);
    m_blit = build(kFsVs, pre + kBlitFs);
    m_copy = build(kFsVs, pre + kCopyFs);
    m_mblur = build(kFsVs, pre + kMBlurFs);
    m_gauss = build(kFsVs, pre + kGaussFs);
    m_shadow = build(kFsVs, pre + kShadowFs);
    m_border = build(kFsVs, pre + kBorderFs);
    m_chroma = build(kFsVs, pre + kChromaFs);
    m_dim = build(kFsVs, pre + kDimFs);
    m_adjust = build(kFsVs, pre + kAdjustFs);
    m_vao = new QOpenGLVertexArrayObject;
    m_vao->create();
    return m_ok;
}

void Compositor::cleanup() {
    qDeleteAll(m_tex); m_tex.clear();
    for (auto& b : m_buf) { delete b; b = nullptr; }
    for (QOpenGLShaderProgram** p : {&m_layer, &m_blit, &m_copy, &m_mblur, &m_gauss, &m_shadow, &m_border, &m_chroma, &m_dim, &m_adjust}) { delete *p; *p = nullptr; }
    delete m_vao; m_vao = nullptr;
}

void Compositor::bindTex(unsigned unit, unsigned tex) {
    glActiveTexture(unit);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void Compositor::setMask(QOpenGLShaderProgram* pr, const MaskFx* m, double k, const QSize& S) {
    if (m && m->on) {
        pr->setUniformValue("uMaskA", QVector4D(float(S.width() / 2.0 + m->x * k), float(S.height() / 2.0 + m->y * k), float(m->w / 2 * k), float(m->h / 2 * k)));
        pr->setUniformValue("uMaskB", QVector4D(float(m->rot * M_PI / 180.0), float(std::max(0.0, m->feather * k)), m->invert ? 1.f : 0.f, 1.f));
    } else {
        pr->setUniformValue("uMaskB", QVector4D(0, 0, 0, 0));
    }
}

void Compositor::drawLayer(QOpenGLTexture* tex, const float c[8], float opacity, const QSize& S, const MaskFx* mask, double k) {
    m_layer->bind();
    const QVector2D pts[4] = {{c[0], c[1]}, {c[2], c[3]}, {c[4], c[5]}, {c[6], c[7]}};
    m_layer->setUniformValueArray("uCorner", pts, 4);
    m_layer->setUniformValue("uSize", QVector2D(float(S.width()), float(S.height())));
    m_layer->setUniformValue("uOpacity", opacity);
    setMask(m_layer, mask, k, S);
    glActiveTexture(GL_TEXTURE0);
    tex->bind();
    m_layer->setUniformValue("uTex", 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    tex->release();
}

// One full-screen step: `in` (and `orig` for mixing) → dst.
void Compositor::pass(QOpenGLShaderProgram* pr, QOpenGLFramebufferObject* dst, unsigned in, unsigned orig, double strength, const MaskFx* mask, double k, const Setup& setup) {
    const QSize S = dst->size();
    dst->bind();
    glViewport(0, 0, S.width(), S.height());
    glDisable(GL_BLEND);
    pr->bind();
    bindTex(GL_TEXTURE0, in);
    bindTex(GL_TEXTURE1, orig);
    pr->setUniformValue("uTex", 0);
    pr->setUniformValue("uOrig", 1);
    pr->setUniformValue("uSize", QVector2D(float(S.width()), float(S.height())));
    pr->setUniformValue("uStrength", float(strength));
    pr->setUniformValue("uPx", QVector2D(1.0f / S.width(), 1.0f / S.height()));
    setMask(pr, mask, k, S);
    if (setup) setup(pr);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glActiveTexture(GL_TEXTURE0);
}

// Runs one effect on buffer `cur` (-1 = the target itself). Returns the buffer that now holds the result.
int Compositor::runPass(const FxPass& f, int cur, const MaskFx* mask, double k, QOpenGLFramebufferObject* target) {
    const QSize S = target->size();
    const unsigned in = fboAt(cur, target)->texture();
    auto P = [&](const char* n) { return float(f.p.value(n)); };

    if (f.type == "blur") {
        const double radius = f.strength * 100.0 * k;
        if (radius < 0.5) return cur;
        const int mid = freeIdx(cur, -2), dst = freeIdx(cur, mid);
        pass(m_gauss, m_buf[mid], in, in, 1.0, nullptr, k, [&](QOpenGLShaderProgram* p) {
            p->setUniformValue("uDir", QVector2D(1.0f / S.width(), 0)); p->setUniformValue("uRadius", float(radius)); p->setUniformValue("uFinal", 0); });
        pass(m_gauss, m_buf[dst], m_buf[mid]->texture(), in, 1.0, mask, k, [&](QOpenGLShaderProgram* p) {
            p->setUniformValue("uDir", QVector2D(0, 1.0f / S.height())); p->setUniformValue("uRadius", float(radius)); p->setUniformValue("uFinal", 1); });
        return dst;
    }
    const int dst = freeIdx(cur, -2);
    QOpenGLFramebufferObject* d = m_buf[dst];
    if (f.type == "shadow") {
        pass(m_shadow, d, in, in, 1.0, mask, k, [&](QOpenGLShaderProgram* p) {
            p->setUniformValue("uOff", QVector2D(float(P("ox") * k), float(-P("oy") * k)));      // y flips: textures are bottom-up
            p->setUniformValue("uBlur", float(P("blur") * k));
            p->setUniformValue("uColor", QVector3D(P("r"), P("g"), P("b")));
            p->setUniformValue("uOpacity", float(f.strength)); });
    } else if (f.type == "border") {
        pass(m_border, d, in, in, 1.0, mask, k, [&](QOpenGLShaderProgram* p) {
            p->setUniformValue("uThick", float(f.strength * 40.0 * k));
            p->setUniformValue("uColor", QVector3D(P("r"), P("g"), P("b"))); });
    } else if (f.type == "chromakey") {
        pass(m_chroma, d, in, in, f.strength, mask, k, [&](QOpenGLShaderProgram* p) {
            p->setUniformValue("uKey", QVector3D(P("r"), P("g"), P("b")));
            p->setUniformValue("uTol", P("tol")); p->setUniformValue("uSoft", P("soft")); p->setUniformValue("uSpill", P("spill")); });
    } else if (f.type == "blackflash") {
        pass(m_dim, d, in, in, 1.0, mask, k, [&](QOpenGLShaderProgram* p) { p->setUniformValue("uAmount", P("amount")); p->setUniformValue("uGlitch", P("glitchAmt")); p->setUniformValue("uSeed", P("seed")); });
    } else if (f.type == "adjust") {
        pass(m_adjust, d, in, in, f.strength, mask, k, [&](QOpenGLShaderProgram* p) {
            p->setUniformValue("uTemp", P("temp")); p->setUniformValue("uTint", P("tint")); p->setUniformValue("uSat", P("saturation"));
            p->setUniformValue("uExp", P("exposure")); p->setUniformValue("uCon", P("contrast")); p->setUniformValue("uHi", P("highlights"));
            p->setUniformValue("uSh", P("shadows")); p->setUniformValue("uSharp", P("sharpen")); p->setUniformValue("uClar", P("clarity"));
            p->setUniformValue("uVig", P("vignette")); });
    } else {
        return cur;
    }
    return dst;
}

void Compositor::render(const QVector<RenderLayer>& layers, const QSize& projectSize, QOpenGLFramebufferObject* target) {
    const QSize S = target->size();
    const double k = S.width() / double(std::max(1, projectSize.width()));   // project px → target px
    for (auto& b : m_buf)
        if (!b || b->size() != S) { delete b; b = new QOpenGLFramebufferObject(S); }

    // 1. upload frames (textures are reused while the size stays the same)
    while (m_tex.size() > layers.size()) delete m_tex.takeLast();
    while (m_tex.size() < layers.size()) m_tex.push_back(nullptr);
    for (int i = 0; i < layers.size(); ++i) {
        const QImage& img = layers[i].image;
        if (img.isNull()) continue;
        QOpenGLTexture*& t = m_tex[i];
        if (!t || t->width() != img.width() || t->height() != img.height()) {
            delete t;
            t = new QOpenGLTexture(QOpenGLTexture::Target2D);
            t->setFormat(QOpenGLTexture::RGBA8_UNorm);
            t->setSize(img.width(), img.height());
            t->setMipLevels(1);
            t->allocateStorage();
            t->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
            t->setWrapMode(QOpenGLTexture::ClampToEdge);
        }
        t->setData(0, QOpenGLTexture::RGBA, QOpenGLTexture::UInt8, img.constBits());
    }

    // 2. composite bottom → top
    target->bind();
    glViewport(0, 0, S.width(), S.height());
    glDisable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    m_vao->bind();

    for (int i = 0; i < layers.size(); ++i) {
        const RenderLayer& L = layers[i];

        if (L.pass) {                                  // effect / adjustment layer: act on everything composed so far
            int cur = -1;
            for (const FxPass& f : L.passes) cur = runPass(f, cur, &L.mask, k, target);
            if (cur >= 0) pass(m_copy, target, m_buf[cur]->texture(), m_buf[cur]->texture(), 1.0, nullptr, k, nullptr);
            continue;
        }
        if (L.image.isNull() || !m_tex[i]) continue;
        const LayerFx& fx = L.fx;

        // fit ("contain") into the canvas, then apply scale / rotation / translation about the layer centre
        const double s = std::min(S.width() / double(L.image.width()), S.height() / double(L.image.height()));
        const double w = L.image.width() * s * fx.scale, h = L.image.height() * s * fx.scale;
        const double cx = S.width() / 2.0 + fx.dx * k, cy = S.height() / 2.0 + fx.dy * k;
        const double a = fx.rotation * M_PI / 180.0, ca = std::cos(a), sa = std::sin(a);
        auto P = [&](double x, double y, float* out) { out[0] = float(cx + x * ca - y * sa); out[1] = float(cy + x * sa + y * ca); };
        float corners[8];
        const double fl = fx.flipX ? -1.0 : 1.0;      // mirror effect: swap left and right of the picture (about its own centre)
        P(-fl * w / 2, -h / 2, corners); P(fl * w / 2, -h / 2, corners + 2); P(-fl * w / 2, h / 2, corners + 4); P(fl * w / 2, h / 2, corners + 6);
        const double bx = fx.blurX * k, by = fx.blurY * k;
        const bool motion = std::hypot(bx, by) >= 0.75;

        if (!motion && L.passes.isEmpty()) {           // fast path: straight onto the canvas
            target->bind();
            glViewport(0, 0, S.width(), S.height());
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // premultiplied alpha
            drawLayer(m_tex[i], corners, float(fx.opacity), S, &L.mask, k);
            continue;
        }
        // the picture on its own transparent canvas, so effects only touch the picture
        int cur = 0;
        m_buf[0]->bind();
        glViewport(0, 0, S.width(), S.height());
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        drawLayer(m_tex[i], corners, float(fx.opacity), S, nullptr, k);
        if (motion) {
            const int dst = freeIdx(cur, -2);
            const unsigned t = m_buf[cur]->texture();
            pass(m_mblur, m_buf[dst], t, t, 1.0, nullptr, k, [&](QOpenGLShaderProgram* p) {
                p->setUniformValue("uStep", QVector2D(float(bx / S.width()), float(-by / S.height()))); });   // texture v points up
            cur = dst;
        }
        for (const FxPass& f : L.passes) cur = runPass(f, cur, nullptr, k, target);   // the clip's mask is applied once, below
        target->bind();
        glViewport(0, 0, S.width(), S.height());
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        m_blit->bind();
        bindTex(GL_TEXTURE0, m_buf[cur]->texture());
        m_blit->setUniformValue("uTex", 0);
        m_blit->setUniformValue("uSize", QVector2D(float(S.width()), float(S.height())));
        setMask(m_blit, &L.mask, k, S);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    m_vao->release();
    glDisable(GL_BLEND);
}

} // namespace ve
