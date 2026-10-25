#include "Effects.h"
#include <algorithm>
#include <cmath>

namespace ve {

static constexpr double kTwoPi = 6.283185307179586;
static constexpr double kSwooshZoom = 0.30;     // extra zoom at the far end of a swoosh
static constexpr double kShutterSwoosh = 0.010; // seconds of motion captured by the blur
static constexpr double kSwooshTravel = 1.25;   // start/end distance in canvas sizes: zoomed layer + blur fully clear the screen
static constexpr double kShutterShake = 0.006;

// ---- registry -------------------------------------------------------------------------------------------------------

static ParamDef P(const char* name, const char* label, double def, double lo, double hi, double step, double mult = 1, int dec = 0, const char* suffix = "") {
    ParamDef d; d.name = name; d.label = label; d.def = def; d.lo = lo; d.hi = hi; d.step = step; d.mult = mult; d.decimals = dec; d.suffix = suffix; return d;
}
static QVector<ParamDef> colourParams(double r, double g, double b) {
    QVector<ParamDef> v;
    for (auto [n, d] : {std::pair<const char*, double>{"r", r}, {"g", g}, {"b", b}}) { ParamDef p = P(n, n, d, 0, 1, 0.01); p.colour = true; v.push_back(p); }
    return v;
}

static const QVector<EffectDef>& registry() {
    static const QVector<EffectDef> v = [] {
        QVector<EffectDef> r;
        { EffectDef d; d.type = "shadow"; d.label = "Drop shadow"; d.strengthLabel = "Opacity"; d.strengthDef = 0.6;
          d.params = {P("ox", "Offset X", 24, -3000, 3000, 1, 1, 0, " px"), P("oy", "Offset Y", 24, -3000, 3000, 1, 1, 0, " px"), P("blur", "Softness", 20, 0, 500, 1, 1, 0, " px")};
          d.params += colourParams(0, 0, 0); r.push_back(d); }
        { EffectDef d; d.type = "border"; d.label = "Border"; d.strengthLabel = "Thickness"; d.strengthDef = 0.3;
          d.params = colourParams(1, 1, 1); r.push_back(d); }
        { EffectDef d; d.type = "blur"; d.label = "Blur"; d.strengthDef = 0.4; r.push_back(d); }
        { EffectDef d; d.type = "chromakey"; d.label = "Chroma key"; d.strengthLabel = "Key amount"; d.strengthDef = 1.0;
          d.params = {P("tol", "Tolerance", 0.15, 0, 1, 0.01, 100, 0, " %"), P("soft", "Edge softness", 0.08, 0, 1, 0.01, 100, 0, " %"), P("spill", "Spill removal", 0.4, 0, 1, 0.01, 100, 0, " %")};
          d.params += colourParams(0, 1, 0); r.push_back(d); }
        { EffectDef d; d.type = "blackflash"; d.label = "Black flash"; d.strengthLabel = "Darkness"; d.strengthDef = 1.0;
          ParamDef pat = P("pattern", "Pattern", 0, 0, 1, 1);
          pat.choices = {"Steady flicker", "Double flash"};
          d.params = {P("rate", "Flashes per second", 4, 0.1, 60, 0.5, 1, 1), P("dark", "Dark part of each flash", 0.4, 0.02, 0.98, 0.01, 100, 0, " %"),
                      P("soft", "Edge softness", 0.15, 0, 1, 0.01, 100, 0, " %"), pat, P("glitch", "Glitch / distortion", 0.3, 0, 1, 0.01, 100, 0, " %"),
                      P("at", "Start at", 0, 0, 3600, 0.1, 1, 1, " s")};
          r.push_back(d); }
        { EffectDef d; d.type = "adjust"; d.label = "Adjustment"; d.strengthLabel = "Amount"; d.strengthDef = 1.0;
          const char* names[][2] = {{"temp", "Temperature"}, {"tint", "Tint"}, {"saturation", "Saturation"}, {"exposure", "Exposure"}, {"contrast", "Contrast"},
                                    {"highlights", "Highlights"}, {"shadows", "Shadows"}, {"sharpen", "Sharpen"}, {"clarity", "Clarity"}, {"vignette", "Vignette"}};
          for (auto& n : names) {
              const bool oneSided = QString(n[0]) == "sharpen" || QString(n[0]) == "vignette" || QString(n[0]) == "clarity";
              d.params.push_back(P(n[0], n[1], 0, oneSided ? 0 : -1, 1, 0.05, 100));
          }
          r.push_back(d); }
        return r;
    }();
    return v;
}

const EffectDef* effectDef(const QString& type) {
    for (const EffectDef& d : registry()) if (d.type == type) return &d;
    return nullptr;
}

double effectParamDefault(const QString& type, const QString& name) {
    if (const EffectDef* d = effectDef(type))
        for (const ParamDef& p : d->params) if (p.name == name) return p.def;
    return 0.0;
}

Effect makeEffect(const QString& type) {
    Effect e; e.type = type;
    if (const EffectDef* d = effectDef(type)) { e.amount = d->strengthDef; for (const ParamDef& p : d->params) e.p[p.name] = p.def; }
    return e;
}

// Black flash: the picture flickers between normal and dark, rhythmically.
//  pattern 0 "steady":  one dark pulse per cycle.   pattern 1 "double": two quick dark pulses, then a pause.
// `dark` is the fraction of the cycle that is dark; `soft` how much of each pulse is spent easing in and out (0 = hard cut).
static double smooth01(double u) { u = std::clamp(u, 0.0, 1.0); return u * u * (3.0 - 2.0 * u); }
static double flashLevel(double local, double at, double rate, double dark, double soft, int pattern) {
    const double t = local - at;
    if (t < 0 || rate <= 0) return 0;
    const double T = 1.0 / rate;
    const double ph = std::fmod(t, T);
    double w = pattern == 1 ? dark * T / 2.0 : dark * T;               // length of one dark pulse
    if (pattern == 1) w = std::min(w, T / 3.0);
    auto pulse = [&](double x) {
        if (x < 0 || x >= w) return 0.0;
        const double r = std::max(1e-4, soft * w * 0.5);
        return std::min(smooth01(x / r), smooth01((w - x) / r));
    };
    return pattern == 1 ? std::max(pulse(ph), pulse(ph - 2.0 * w)) : pulse(ph);
}

QVector<FxPass> resolvePasses(const Clip& c, double local, const ProjectSettings&) {
    QVector<FxPass> out;
    for (const Effect& e : c.effects) {
        const EffectDef* d = effectDef(e.type);
        if (!d) continue;                                   // shake / swoosh: transforms
        FxPass p;
        p.type = e.type;
        p.strength = std::clamp(propValue(c, "fx." + e.type, local), 0.0, 1.0);
        for (const ParamDef& pd : d->params) p.p[pd.name] = propValue(c, "fx." + e.type + "." + pd.name, local);
        if (e.type == "blackflash") {                       // evaluated here: the GPU just darkens (and glitches) by this much
            const double level = flashLevel(local, p.p["at"], p.p["rate"], p.p["dark"], p.p["soft"], int(std::lround(p.p["pattern"])));
            p.p["amount"] = p.strength * level;
            p.p["glitchAmt"] = p.p["glitch"] * level;
            p.p["seed"] = p.p["rate"] > 0 ? std::floor((local - p.p["at"]) * p.p["rate"]) : 0.0;   // each flash tears differently
            p.strength = 1.0;
            if (level <= 1e-4) continue;                    // nothing to draw between flashes
        } else if (p.strength <= 1e-4 && e.type != "adjust") {
            continue;
        }
        out.push_back(p);
    }
    return out;
}

MaskFx resolveMask(const Clip& c, double local) {
    MaskFx m;
    m.on = c.mask.on;
    if (!m.on) return m;
    m.invert = c.mask.invert;
    m.x = propValue(c, "mask.x", local); m.y = propValue(c, "mask.y", local);
    m.w = std::max(0.0, propValue(c, "mask.w", local)); m.h = std::max(0.0, propValue(c, "mask.h", local));
    m.rot = propValue(c, "mask.rot", local); m.feather = std::max(0.0, propValue(c, "mask.feather", local));
    return m;
}

const QVector<EffectPreset>& effectPresets() {
    static const QVector<EffectPreset> v = [] {
        QVector<EffectPreset> p;
        p.push_back({"shake_light", "Light", "Shake", {"shake", 0.25, 0, 0}});
        p.push_back({"shake_medium", "Medium", "Shake", {"shake", 0.55, 0, 0}});
        p.push_back({"shake_heavy", "Heavy", "Shake", {"shake", 0.95, 0, 0}});
        const char* sides[] = {"left", "right", "top", "bottom"};
        for (int d = 0; d < 4; ++d)
            p.push_back({QString("swoosh_in_%1").arg(sides[d]), QString("From %1").arg(sides[d]), "Swoosh in", {"swoosh_in", 0, 0.45, d}});
        for (int d = 0; d < 4; ++d)
            p.push_back({QString("swoosh_out_%1").arg(sides[d]), QString("To %1").arg(sides[d]), "Swoosh out", {"swoosh_out", 0, 0.45, d}});
        // effects that sit on a clip
        p.push_back({"fx_shadow", "Drop shadow", "Clip effects", makeEffect("shadow")});
        p.push_back({"fx_border", "Border", "Clip effects", makeEffect("border")});
        p.push_back({"fx_blur", "Blur", "Clip effects", makeEffect("blur")});
        p.push_back({"fx_chromakey", "Chroma key (green screen)", "Clip effects", makeEffect("chromakey")});
        p.push_back({"fx_blackflash", "Black flash (soft)", "Clip effects", makeEffect("blackflash")});
        { Effect m; m.type = "mirror"; p.push_back({"fx_mirror", "Mirror (flip left ↔ right)", "Clip effects", m}); }
        // the same effects as separate layers: they act on everything below them on the timeline
        p.push_back({"layer_blur", "Blur layer", "Effect layers", makeEffect("blur"), true});
        p.push_back({"layer_blackflash", "Black flash layer", "Effect layers", makeEffect("blackflash"), true});
        p.push_back({"layer_adjust", "Adjustment layer", "Adjustment", makeEffect("adjust"), true});
        return p;
    }();
    return v;
}

const EffectPreset* findPreset(const QString& id) {
    for (const EffectPreset& p : effectPresets())
        if (p.id == id) return &p;
    return nullptr;
}

QString effectLabel(const Effect& e) {
    if (e.type == "mirror") return "Mirror";
    if (const EffectDef* d = effectDef(e.type)) return d->label;
    static const char* sides[] = {"left", "right", "top", "bottom"};
    const char* side = sides[std::clamp(e.direction, 0, 3)];
    if (e.type == "shake") return "Shake";
    if (e.type == "swoosh_in") return QString("Swoosh in · %1").arg(side);
    if (e.type == "swoosh_out") return QString("Swoosh out · %1").arg(side);
    return e.type;
}

namespace {
struct Pose { double x, y, rot; };

// Two incommensurate sine pairs per axis: looks random, but is deterministic (same frame every time).
Pose shakePose(double t, double amp, double maxRot) {
    const double f = 7.0;   // base frequency in Hz
    Pose p;
    p.x = amp * (0.62 * std::sin(kTwoPi * f * t + 0.3) + 0.38 * std::sin(kTwoPi * f * 2.37 * t + 1.9));
    p.y = amp * (0.62 * std::sin(kTwoPi * f * 1.31 * t + 2.2) + 0.38 * std::sin(kTwoPi * f * 2.91 * t + 0.6));
    p.rot = maxRot * std::sin(kTwoPi * f * 1.73 * t + 0.9);
    return p;
}
} // namespace

LayerFx evalEffects(const Clip& c, double local, const ProjectSettings& s) {
    LayerFx fx;
    const double W = s.width, H = s.height;
    for (const Effect& e : c.effects) {
        if (e.type == "mirror") { fx.flipX = !fx.flipX; continue; }
        if (e.type == "shake") {
            const double a = std::clamp(propValue(c, "fx.shake", local), 0.0, 1.0);   // strength may be keyframed
            const double amp = (0.008 + 0.032 * a) * W;     // pixels
            const double maxRot = 1.4 * a;                  // degrees
            const Pose p = shakePose(local, amp, maxRot);
            fx.dx += p.x; fx.dy += p.y; fx.rotation += p.rot;
            // Zoom in just enough that the shaking frame never reveals the canvas edge.
            const double margin = amp + 0.5 * std::hypot(W, H) * (maxRot * M_PI / 180.0);
            fx.scale *= 1.0 + 2.0 * margin / std::min(W, H);
            // Motion blur from the actual velocity (central difference).
            const double dt = 1.0 / 600.0;
            const Pose p1 = shakePose(local + dt, amp, maxRot), p0 = shakePose(local - dt, amp, maxRot);
            fx.blurX += (p1.x - p0.x) / (2 * dt) * kShutterShake;
            fx.blurY += (p1.y - p0.y) / (2 * dt) * kShutterShake;
        } else if (e.type == "swoosh_in" || e.type == "swoosh_out") {
            const bool in = e.type == "swoosh_in";
            const double d = std::max(0.05, std::min(e.duration, c.dur * 0.5));
            double u = in ? local / d : (local - (c.dur - d)) / d;    // 0..1 progress inside the swoosh window
            if (u < 0 || u > 1) {
                if (in ? u > 1 : u < 0) continue;                     // not in the animated part of the clip
                u = std::clamp(u, 0.0, 1.0);                          // before-in / after-out: hold the end pose
            }
            static const double dirx[] = {-1, 1, 0, 0}, diry[] = {0, 0, -1, 1};
            const int dir = std::clamp(e.direction, 0, 3);
            const double dist = (dir < 2 ? W : H) * kSwooshTravel;
            // in: ease-out quart (fast start, soft landing); out: ease-in quart (soft start, fast exit)
            const double p = in ? 1.0 - std::pow(1.0 - u, 4.0) : std::pow(u, 4.0);
            const double away = in ? 1.0 - p : p;                     // 0 = on screen, 1 = fully off screen
            const double dpdu = in ? 4.0 * std::pow(1.0 - u, 3.0) : 4.0 * std::pow(u, 3.0);
            fx.dx += dirx[dir] * dist * away;
            fx.dy += diry[dir] * dist * away;
            fx.scale *= 1.0 + kSwooshZoom * away;
            const double speed = dist * dpdu / d;                     // pixels per second
            fx.blurX += dirx[dir] * speed * kShutterSwoosh;
            fx.blurY += diry[dir] * speed * kShutterSwoosh;
        }
    }
    return fx;
}

LayerFx evalLayer(const Clip& c, double local, const ProjectSettings& s) {
    LayerFx fx = evalEffects(c, local, s);
    fx.dx += propValue(c, "x", local);
    fx.dy += propValue(c, "y", local);
    fx.scale *= std::max(0.0, propValue(c, "scale", local));
    fx.rotation += propValue(c, "rotation", local);
    fx.opacity *= std::clamp(propValue(c, "opacity", local), 0.0, 1.0);
    return fx;
}

} // namespace ve
