#pragma once
#include "Project.h"
#include <QImage>
#include <QVector>

namespace ve {

// A decoded frame plus the effect transform to draw it with. This is what the GPU compositor consumes.
struct RenderLayer {
    QImage image;     // RGBA8888 (null for effect / adjustment layers)
    LayerFx fx;
    QVector<FxPass> passes;   // effects: on this picture, or (pass == true) on everything beneath
    MaskFx mask;
    bool pass = false;
};

// Evaluate all of a clip's effects at `local` seconds after the clip's start.
// Pure function of (clip, time, project size) so preview and export always agree.
LayerFx evalEffects(const Clip& c, double local, const ProjectSettings& s);

// Full per-frame transform of a clip: its own position/size/rotation/opacity (keyframed if animated) plus its effects.
LayerFx evalLayer(const Clip& c, double local, const ProjectSettings& s);

struct EffectPreset {
    QString id;      // stable key (used for drag & drop)
    QString label;   // shown in the Effects panel
    QString group;
    Effect effect;
    bool layer = false;     // true: dropping it on the timeline creates a separate effect / adjustment layer
};

// ---- effect registry: what each effect is called and which values it has -------------------------------------------
struct ParamDef {
    QString name, label;
    double def = 0, lo = 0, hi = 1, step = 0.05;
    double mult = 1;            // shown value = stored value * mult
    int decimals = 0;
    QString suffix;
    QStringList choices;        // non-empty: shown as a drop-down (value = index)
    bool colour = false;        // r/g/b of a colour picker (not animatable, no row of its own)
};
struct EffectDef {
    QString type, label, strengthLabel = "Strength";
    double strengthDef = 1;
    QVector<ParamDef> params;
};
const EffectDef* effectDef(const QString& type);      // nullptr for shake / swoosh (handled as transforms)
double effectParamDefault(const QString& type, const QString& name);
Effect makeEffect(const QString& type);                // with default strength and parameters
// Everything a clip's effects ask the GPU to do at `local` seconds (keyframes resolved). Excludes shake / swoosh.
QVector<FxPass> resolvePasses(const Clip& c, double local, const ProjectSettings& s);
MaskFx resolveMask(const Clip& c, double local);
const QVector<EffectPreset>& effectPresets();
const EffectPreset* findPreset(const QString& id);
QString effectLabel(const Effect& e);

} // namespace ve
