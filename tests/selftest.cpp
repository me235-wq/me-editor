// Headless end-to-end test of the engine: model + undo/redo + save/load + decode + export.
#include "core/Commands.h"
#include "core/Util.h"
#include "media/AudioMixer.h"
#include "core/TextRender.h"
#include "media/Compositor.h"
#include "media/Overview.h"
#include "media/Proxy.h"
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include "media/Decoder.h"
#include "media/Exporter.h"
#include "core/Effects.h"
#include <QGuiApplication>
#include <QSurfaceFormat>
#include <QFileInfo>
#include <QDir>
#include <QEventLoop>
#include <QProcess>
#include <QTimer>
#include <QPainter>
#include <QFile>
#include <QTemporaryDir>
#include <QUndoStack>
#include <cstdio>

using namespace ve;
static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__); ++g_fail; } else std::printf("  ok    %s\n", #cond); } while (0)


static double meanLuma(const QImage& im) {
    const QImage q = im.convertToFormat(QImage::Format_RGBA8888);
    double s = 0; const uchar* b = q.constBits(); const qsizetype n = qsizetype(q.width()) * q.height();
    for (qsizetype i = 0; i < n; ++i) s += 0.299 * b[i * 4] + 0.587 * b[i * 4 + 1] + 0.114 * b[i * 4 + 2];
    return s / double(n);
}
static double meanAbsDiff(const QImage& a0, const QImage& b0) {
    const QImage a = a0.convertToFormat(QImage::Format_RGBA8888);
    const QImage b = (b0.size() == a.size() ? b0 : b0.scaled(a.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)).convertToFormat(QImage::Format_RGBA8888);
    double s = 0; const uchar *pa = a.constBits(), *pb = b.constBits(); const qsizetype n = qsizetype(a.width()) * a.height();
    for (qsizetype i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) s += std::abs(int(pa[i * 4 + c]) - int(pb[i * 4 + c]));
    return s / double(n * 3);
}
static bool runExport(Exporter& ex, const ProjectData& p, const ExportOptions& o, QString* msg) {
    bool done = false, ok = false; QString err; QEventLoop loop;
    QObject::connect(&ex, &Exporter::finished, &ex, [&](bool k, const QString& m) { ok = k; *msg = m; done = true; loop.quit(); });
    if (!ex.start(p, o, &err)) { *msg = err; return false; }
    if (!done) loop.exec();
    return ok;
}
static QImage frameOf(const QString& path, double t) { Decoder d; QString e; return d.open(path, &e, false) ? d.frameAt(t) : QImage(); }

static bool near(double a, double b, double tol = 0.15) { return std::abs(a - b) <= tol; }

int main(int argc, char** argv) {
    {
        QSurfaceFormat f; f.setVersion(3, 3); f.setProfile(QSurfaceFormat::CoreProfile);   // same as the real app
        QSurfaceFormat::setDefaultFormat(f);
    }
    QGuiApplication app(argc, argv);
    QTemporaryDir dir;
    const QString src = dir.filePath("src.mp4");

    // 1. Make a 6 s test clip (video + audio) with FFmpeg's generators.
    std::printf("[setup] generating test media\n");
    QProcess gen;
    gen.start("ffmpeg", {"-y", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=duration=6:size=640x360:rate=30",
                         "-f", "lavfi", "-i", "sine=frequency=440:duration=6", "-c:v", "libx264", "-g", "30",
                         "-pix_fmt", "yuv420p", "-c:a", "aac", src});
    gen.waitForFinished(60000);
    CHECK(QFileInfo::exists(src));

    std::printf("[probe]\n");
    MediaAsset a;
    QString err;
    CHECK(probeMedia(src, &a, &err));
    CHECK(near(a.duration, 6.0, 0.2));
    CHECK(a.width == 640 && a.height == 360);
    CHECK(a.hasVideo && a.hasAudio && !a.isImage);
    a.id = newId();

    std::printf("[decode]\n");
    Decoder d;
    CHECK(d.open(src, &err));
    QImage f0 = d.frameAt(0.0);
    QImage f3 = d.frameAt(3.0);          // random-access seek
    QImage f3b = d.frameAt(3.0 + 1.0 / 30);  // forward step, no seek
    QImage f1 = d.frameAt(1.0);          // backwards seek
    CHECK(!f0.isNull() && f0.size() == QSize(640, 360));
    CHECK(!f3.isNull() && !f1.isNull() && !f3b.isNull());
    CHECK(f0 != f3 && f1 != f3 && f3 != f3b);
    d.setMaxSize(QSize(320, 180));
    CHECK(d.frameAt(2.0).width() <= 320);
    std::printf("        hardware decode in use: %s\n", d.usesHardware() ? "yes" : "no (software)");

    std::printf("[model / undo]\n");
    Project p;
    QUndoStack stack;
    p.addAsset(a);
    stack.push(makeAddClipCommand(p, a.id, 1, 0));            // V1 0..6
    CHECK(p.clips.size() == 1 && near(p.duration(), 6.0, 0.2));
    const QString first = p.clips[0].id;
    stack.push(makeSplitCommand(p, {first}, 2.0));
    CHECK(p.clips.size() == 2);
    Clip right; for (const Clip& c : p.clips) if (c.start > 1.0) right = c;
    CHECK(near(right.in, 2.0, 1e-6) && near(right.start, 2.0, 1e-6));
    stack.push(makeDeleteCommand(p, {first}, true));          // ripple delete the left part
    CHECK(p.clips.size() == 1 && near(p.clips[0].start, 0.0, 1e-6));
    stack.undo(); CHECK(p.clips.size() == 2 && near(p.clips[0].start + p.clips[1].start, 2.0, 1e-6));
    stack.undo(); CHECK(p.clips.size() == 1 && near(p.clips[0].dur, a.duration, 1e-6));
    stack.redo(); stack.redo(); CHECK(p.clips.size() == 1 && near(p.clips[0].in, 2.0, 1e-6));
    stack.push(makeAddClipCommand(p, a.id, 0, 1.0));          // overlay on V2 at 1 s
    CHECK(p.clips.size() == 2);
    CHECK(makeAddClipCommand(p, a.id, 3, 0) == nullptr);      // video file refused on an audio track (A1)
    CHECK(p.videoLayersAt(2.0).size() == 2 && p.videoLayersAt(0.5).size() == 1);

    std::printf("[save / load]\n");
    CHECK(p.save(dir.filePath("t.vproj"), &err));
    Project q;
    CHECK(q.load(dir.filePath("t.vproj"), &err));
    CHECK(q.clips.size() == p.clips.size() && q.assets.size() == 1 && q.settings.width == 1920);
    bool same = true;
    for (const Clip& c : p.clips) { const Clip* o = q.clip(c.id); same = same && o && *o == c; }
    CHECK(same);

    std::printf("[export]\n");
    ExportOptions o; o.outPath = dir.filePath("out.mp4"); o.quality = 28;
    p.settings.width = 1280; p.settings.height = 720; p.settings.fps = 30;
    Exporter ex;
    bool done = false, ok = false; QString msg;
    QEventLoop loop;
    QObject::connect(&ex, &Exporter::finished, &ex, [&](bool k, const QString& m) { ok = k; msg = m; done = true; loop.quit(); });
    CHECK(ex.start(p, o, &err));
    if (!done) loop.exec();
    if (!ok) std::printf("        %s\n", qPrintable(msg));
    CHECK(ok);
    MediaAsset outA;
    CHECK(probeMedia(o.outPath, &outA, &err));
    CHECK(near(outA.duration, p.duration(), 0.2));
    CHECK(outA.width == 1280 && outA.height == 720 && outA.hasAudio);


    std::printf("[rotation + still image]\n");
    const QString rot = dir.filePath("rot.mp4"), png = dir.filePath("pic.png");
    QProcess::execute("ffmpeg", {"-y", "-loglevel", "error", "-display_rotation", "90", "-i", src, "-c", "copy", rot});
    QProcess::execute("ffmpeg", {"-y", "-loglevel", "error", "-f", "lavfi", "-i", "color=c=red:s=200x200", "-frames:v", "1", png});
    MediaAsset ra, pa;
    CHECK(probeMedia(rot, &ra, &err));
    CHECK(ra.rotation != 0 && ra.width == 360 && ra.height == 640);   // portrait after rotation metadata
    Decoder rd; CHECK(rd.open(rot, &err));
    CHECK(rd.frameAt(1.0).size() == QSize(360, 640));
    CHECK(probeMedia(png, &pa, &err) && pa.isImage && !pa.hasAudio);
    ra.id = newId(); pa.id = newId();
    p.addAsset(ra); p.addAsset(pa);
    stack.push(makeAddClipCommand(p, pa.id, 0, 4.0));                  // image on V2 near the end
    stack.push(makeAddClipCommand(p, ra.id, 1, 5.0));                  // rotated clip on V1 after the first
    ExportOptions o2; o2.outPath = dir.filePath("out2.mp4"); o2.quality = 30;
    Exporter ex2; bool d2 = false, ok2 = false; QString m2; QEventLoop l2;
    QObject::connect(&ex2, &Exporter::finished, &ex2, [&](bool k, const QString& m) { ok2 = k; m2 = m; d2 = true; l2.quit(); });
    CHECK(ex2.start(p, o2, &err));
    if (!d2) l2.exec();
    if (!ok2) std::printf("        %s\n", qPrintable(m2));
    CHECK(ok2);
    MediaAsset o2a; CHECK(probeMedia(o2.outPath, &o2a, &err) && near(o2a.duration, p.duration(), 0.25));

    std::printf("[effect math]\n");
    {
        ProjectSettings ps; ps.width = 1920; ps.height = 1080;
        Clip c; c.dur = 3.0;
        c.effects = {{"swoosh_in", 0, 0.45, 0}};                       // from the left
        LayerFx s0 = evalEffects(c, 0.0, ps), s1 = evalEffects(c, 0.45, ps), s2 = evalEffects(c, 1.5, ps);
        CHECK(near(s0.dx, -1920 * 1.25, 1e-6) && near(s0.scale, 1.30, 1e-6));  // starts fully off-screen, zoomed
        CHECK(s0.blurX < -100);                                          // fast → strong horizontal blur
        CHECK(near(s1.dx, 0, 1e-6) && near(s1.scale, 1.0, 1e-6) && std::abs(s1.blurX) < 1e-6);
        CHECK(near(s2.dx, 0, 1e-9) && near(s2.scale, 1.0, 1e-9) && s2.blurX == 0);   // untouched after the swoosh
        c.effects = {{"swoosh_out", 0, 0.45, 1}};                      // to the right
        CHECK(near(evalEffects(c, 3.0 - 0.45, ps).dx, 0, 1e-6) && near(evalEffects(c, 3.0, ps).dx, 1920 * 1.25, 1e-6));
        CHECK(evalEffects(c, 1.0, ps).dx == 0);
        c.effects = {{"swoosh_in", 0, 0.45, 2}};                       // from the top → vertical
        CHECK(near(evalEffects(c, 0.0, ps).dy, -1080 * 1.25, 1e-6) && evalEffects(c, 0.0, ps).dx == 0);
        c.effects = {{"shake", 0.95, 0, 0}};
        double maxx = 0; bool det = true;
        for (double t = 0; t < 3; t += 1.0 / 60) { LayerFx f = evalEffects(c, t, ps); maxx = std::max(maxx, std::abs(f.dx)); det = det && evalEffects(c, t, ps).dx == f.dx; }
        CHECK(det && maxx > 30 && maxx <= (0.008 + 0.032 * 0.95) * 1920 * 1.0001);
        CHECK(evalEffects(c, 0.5, ps).scale > 1.1);                    // zoomed enough to hide the frame edge
    }

    std::printf("[effects + freeze commands]\n");
    Project fp; QUndoStack fs;
    fp.addAsset(a);
    fs.push(makeAddClipCommand(fp, a.id, 1, 0));                       // 0..6
    fs.push(makeAddClipCommand(fp, a.id, 1, 6));                       // 6..12
    QString firstId; for (const Clip& c : fp.clips) if (c.start < 1) firstId = c.id;
    Effect si{"swoosh_in", 0, 0.45, 0}, so{"swoosh_out", 0, 0.45, 1}, sh{"shake", 0.5, 0, 0};
    fs.push(makeApplyEffectCommand(fp, {firstId}, si));
    fs.push(makeApplyEffectCommand(fp, {firstId}, so));
    fs.push(makeApplyEffectCommand(fp, {firstId}, sh));
    CHECK(fp.clip(firstId)->effects.size() == 3);
    Effect sh2{"shake", 0.9, 0, 0};
    fs.push(makeApplyEffectCommand(fp, {firstId}, sh2));               // same type replaces
    CHECK(fp.clip(firstId)->effects.size() == 3);
    fs.undo(); CHECK(near(fp.clip(firstId)->effects.last().amount, 0.5, 1e-9));
    fs.push(makeFreezeCommand(fp, {firstId}, 2.0, 1.5));
    CHECK(fp.clips.size() == 4);
    int nfreeze = 0; Clip fz, rt, lf, nx;
    for (const Clip& c : fp.clips) {
        if (c.freeze) { ++nfreeze; fz = c; }
        else if (near(c.start, 0, 1e-9)) lf = c;
        else if (near(c.start, 3.5, 1e-6)) rt = c;
        else nx = c;
    }
    CHECK(nfreeze == 1 && near(fz.start, 2.0, 1e-6) && near(fz.dur, 1.5, 1e-6) && near(fz.in, 2.0, 1e-6) && fz.effects.isEmpty());
    CHECK(near(lf.dur, 2.0, 1e-6) && near(rt.in, 2.0, 1e-6) && near(rt.dur, 4.0, 1e-6));
    CHECK(near(nx.start, 7.5, 1e-6));                                  // next clip rippled right by the hold
    auto has = [](const Clip& c, const char* t) { for (const Effect& e : c.effects) if (e.type == t) return true; return false; };
    CHECK(has(lf, "swoosh_in") && !has(lf, "swoosh_out") && has(rt, "swoosh_out") && !has(rt, "swoosh_in"));   // each end keeps its own swoosh
    CHECK(fp.save(dir.filePath("fx.vproj"), &err));
    Project fq; CHECK(fq.load(dir.filePath("fx.vproj"), &err));
    bool same2 = fq.clips.size() == fp.clips.size();
    for (const Clip& c : fp.clips) { const Clip* o2 = fq.clip(c.id); same2 = same2 && o2 && *o2 == c; }
    CHECK(same2);
    fs.undo(); CHECK(fp.clips.size() == 2 && near(fp.duration(), 12.0, 0.2));

    std::printf("[filter-path export with freeze frame (no GPU)]\n");
    {
        Project p4; QUndoStack s4; p4.addAsset(a);
        p4.settings.width = 640; p4.settings.height = 360;
        s4.push(makeAddClipCommand(p4, a.id, 1, 0));
        s4.push(makeFreezeCommand(p4, {p4.clips[0].id}, 2.0, 1.0));
        ExportOptions o4; o4.outPath = dir.filePath("freeze_cpu.mp4"); o4.quality = 24; o4.allowGpu = false;
        Exporter e4; QString m4;
        CHECK(runExport(e4, p4, o4, &m4));
        if (m4 != "Export finished.") std::printf("        %s\n", qPrintable(m4));
        MediaAsset oa; CHECK(probeMedia(o4.outPath, &oa, &err) && near(oa.duration, 7.0, 0.25));
        const double frozen = meanAbsDiff(frameOf(o4.outPath, 2.3), frameOf(o4.outPath, 2.9));
        const double moving = meanAbsDiff(frameOf(o4.outPath, 0.9), frameOf(o4.outPath, 1.5));
        std::printf("        frozen diff %.2f   moving diff %.2f\n", frozen, moving);
        CHECK(frozen < 1.5 && moving > frozen * 3);
    }

    std::printf("[GPU compositor + export]\n");
    if (!Exporter::gpuAvailable()) {
        std::printf("  SKIP  no OpenGL 3.3 here (run under a desktop session or xvfb-run to include this section)\n");
    } else {
        Project p5; QUndoStack s5; p5.addAsset(a);
        p5.settings.width = 1280; p5.settings.height = 720; p5.settings.fps = 30;
        s5.push(makeAddClipCommand(p5, a.id, 1, 0));
        s5.push(makeAddClipCommand(p5, a.id, 1, 6));                   // a second clip, later
        const QString id0 = p5.clips[0].id;
        s5.push(makeApplyEffectCommand(p5, {id0}, {"swoosh_in", 0, 0.45, 0}));
        s5.push(makeFreezeCommand(p5, {id0}, 2.0, 1.0));               // 0..2 | freeze 2..3 | 3..7 | next clip 7..13
        Clip second; for (const Clip& c : p5.clips) if (!c.freeze && c.start > 6.5) second = c;
        s5.push(makeApplyEffectCommand(p5, {second.id}, {"shake", 0.95, 0, 0}));
        ExportOptions o5; o5.outPath = dir.filePath("gpu.mp4"); o5.quality = 22;
        Exporter e5; QString m5;
        CHECK(runExport(e5, p5, o5, &m5));
        if (!e5.usedGpu()) std::printf("        (expected the GPU path)\n");
        CHECK(e5.usedGpu());
        if (m5 != "Export finished.") std::printf("        %s\n", qPrintable(m5));
        MediaAsset ga; CHECK(probeMedia(o5.outPath, &ga, &err) && near(ga.duration, p5.duration(), 0.25) && ga.width == 1280 && ga.hasAudio);

        const double lumaStart = meanLuma(frameOf(o5.outPath, 0.0));   // swoosh start: layer is entirely off-screen
        const double lumaMid = meanLuma(frameOf(o5.outPath, 0.15));    // mid-swoosh: partly on screen
        const double lumaDone = meanLuma(frameOf(o5.outPath, 1.0));
        std::printf("        luma: start %.1f  mid-swoosh %.1f  settled %.1f\n", lumaStart, lumaMid, lumaDone);
        CHECK(lumaStart < 8 && lumaMid > lumaStart + 5 && lumaDone > 40);

        // orientation + colour: after the swoosh the frame must equal the source, scaled (catches flips / channel swaps)
        const double ref = meanAbsDiff(frameOf(o5.outPath, 1.0), frameOf(src, 1.0));
        std::printf("        exported frame vs source: mean diff %.2f\n", ref);
        CHECK(ref < 10);

        const double frozen = meanAbsDiff(frameOf(o5.outPath, 2.2), frameOf(o5.outPath, 2.8));
        const double moving = meanAbsDiff(frameOf(o5.outPath, 0.9), frameOf(o5.outPath, 1.5));
        std::printf("        freeze: frozen diff %.2f   moving diff %.2f\n", frozen, moving);
        CHECK(frozen < 1.5 && moving > frozen * 3);

        const double shaken = meanAbsDiff(frameOf(o5.outPath, 8.0), frameOf(src, 1.0));   // 7+1.0 s → source 1.0 s
        std::printf("        shake: exported vs unshaken source %.2f\n", shaken);
        CHECK(shaken > 12);                                                              // clearly displaced / zoomed
    }

    // ======================================================================== Batch 1 ========================================================================
    std::printf("[keyframes]\n");
    {
        Clip c; c.id = "k"; c.dur = 4; c.opacity = 0.8;
        CHECK(near(propValue(c, "opacity", 1.0), 0.8, 1e-9));                         // no keys: base value
        setKeyAt(c, "opacity", 0.0, 0.0, 0.01); setKeyAt(c, "opacity", 2.0, 1.0, 0.01);
        CHECK(near(propValue(c, "opacity", 1.0), 0.5, 1e-9) && near(propValue(c, "opacity", 3.0), 1.0, 1e-9) && near(propValue(c, "opacity", -1), 0.0, 1e-9));
        CHECK(hasKeyAt(c, "opacity", 2.005, 0.01) && !hasKeyAt(c, "opacity", 1.0, 0.01));
        setKeyAt(c, "opacity", 2.004, 0.6, 0.01);                                      // same spot: replaces, doesn't duplicate
        CHECK(c.keys["opacity"].size() == 2 && near(propValue(c, "opacity", 2.0), 0.6, 1e-9));
        removeKeyAt(c, "opacity", 0.0, 0.01); removeKeyAt(c, "opacity", 2.0, 0.01);
        CHECK(c.keys.isEmpty() && near(c.opacity, 0.6, 1e-9));                         // last key removed: value stays, no jump
        c.effects = {{"shake", 0.2, 0, 0}};
        setKeyAt(c, "fx.shake", 0, 0.1, 0.01); setKeyAt(c, "fx.shake", 2, 0.9, 0.01);
        ProjectSettings ps;
        LayerFx lo = evalLayer(c, 0.0, ps), hi = evalLayer(c, 2.0, ps);
        CHECK(hi.scale > lo.scale);                                                    // stronger shake zooms more → the strength itself is animated
        Clip d; d.dur = 4; d.x = 100; d.scale = 0.5; d.rotation = 30; d.opacity = 0.5;
        LayerFx f = evalLayer(d, 1.0, ps);
        CHECK(near(f.dx, 100, 1e-9) && near(f.scale, 0.5, 1e-9) && near(f.rotation, 30, 1e-9) && near(f.opacity, 0.5, 1e-9));
    }

    std::printf("[speed + split + track flags]\n");
    Project sp; QUndoStack ss; sp.addAsset(a);
    ss.push(makeAddClipCommand(sp, a.id, 1, 0));                      // 0..6
    const QString sid = sp.clips[0].id;
    QString serr;
    ss.push(makeSetSpeedCommand(sp, sid, 2.0, &serr));
    CHECK(near(sp.clips[0].dur, 3.0, 0.2) && near(sp.clips[0].speed, 2.0, 1e-9));             // twice as fast → half as long
    CHECK(near(sp.videoLayersAt(1.0)[0].sourceTime, 2.0, 1e-6));                               // timeline 1 s → source 2 s
    ss.push(makeSplitCommand(sp, {sid}, 1.0));
    Clip rsp; for (const Clip& c : sp.clips) if (c.start > 0.5) rsp = c;
    CHECK(near(rsp.in, 2.0, 1e-6) && near(rsp.dur, sp.clips[0].dur + sp.clips[1].dur - 1.0, 1e-6) && near(rsp.speed, 2.0, 1e-9));
    ss.undo(); ss.undo();
    CHECK(near(sp.clips[0].speed, 1.0, 1e-9) && near(sp.clips[0].dur, a.duration, 1e-6));
    ss.push(makeAddClipCommand(sp, a.id, 1, 6));                      // a neighbour right after it
    CHECK(makeSetSpeedCommand(sp, sid, 0.5, &serr) == nullptr && !serr.isEmpty());            // would overlap the neighbour: refused with a reason
    ss.push(new TrackFlagCommand(&sp, 1, 0, true));                   // hide V1
    CHECK(sp.videoLayersAt(1.0).isEmpty());
    ss.undo(); CHECK(sp.videoLayersAt(1.0).size() == 1);
    ss.push(new TrackFlagCommand(&sp, 1, 1, true));                   // lock V1
    CHECK(makeSplitCommand(sp, {sid}, 2.0) == nullptr && makeDeleteCommand(sp, {sid}, false) == nullptr && makeFreezeCommand(sp, {sid}, 2.0, 1.0) == nullptr);
    CHECK(makeAddClipCommand(sp, a.id, 1, 20) == nullptr);
    { Clip c = sp.clips[0]; c.opacity = 0.3; CHECK(makeSetClipCommand(sp, c, "x") == nullptr); }
    ss.undo();
    { Clip c = *sp.clip(sid); c.opacity = 0.3; ss.push(makeSetClipCommand(sp, c, "o", "k1")); c.opacity = 0.2; ss.push(makeSetClipCommand(sp, c, "o", "k1")); }
    const int depth = ss.index(); ss.undo();
    CHECK(near(sp.clip(sid)->opacity, 1.0, 1e-9) && ss.index() == depth - 1);                  // two drags of the same property = one undo step

    std::printf("[media library]\n");
    {
        Project lp; QUndoStack ls; lp.addAsset(a);
        MediaAsset b2 = a; b2.id = newId(); b2.path = src; lp.addAsset(b2);
        QString f1, f2;
        ls.push(makeNewFolderCommand(lp, "Raw", "", &f1));
        ls.push(makeNewFolderCommand(lp, "Day 1", f1, &f2));
        ls.push(makeMoveToFolderCommand(lp, {a.id}, {}, f2));
        CHECK(lp.asset(a.id)->folderId == f2 && lp.folders.size() == 2);
        CHECK(makeMoveToFolderCommand(lp, {}, {f1}, f2) == nullptr);                          // can't move a folder into its own child
        ls.push(makeRenameFolderCommand(lp, f2, "Day one"));
        ls.push(makeAddClipCommand(lp, a.id, 1, 0));
        CHECK(lp.clips.size() == 1);
        ls.push(makeRemoveMediaCommand(lp, {}, {f1}));                                         // delete the folder: its files and their clips go too
        CHECK(lp.folders.isEmpty() && !lp.assets.contains(a.id) && lp.assets.contains(b2.id) && lp.clips.isEmpty());
        ls.undo();
        CHECK(lp.folders.size() == 2 && lp.assets.contains(a.id) && lp.clips.size() == 1 && lp.asset(a.id)->folderId == f2);
        CHECK(lp.save(dir.filePath("lib.vproj"), &err));
        Project lq; CHECK(lq.load(dir.filePath("lib.vproj"), &err));
        CHECK(lq.folders.size() == 2 && lq.asset(a.id)->folderId == f2 && lq.folders[1].parentId == f1);
    }

    std::printf("[canvas aspect ratio]\n");
    {
        int w, h;
        sizeForRatio(16, 9, 1080, &w, &h); CHECK(w == 1920 && h == 1080);
        sizeForRatio(9, 16, 1080, &w, &h); CHECK(w == 1080 && h == 1920);                      // Shorts
        sizeForRatio(1, 1, 1080, &w, &h);  CHECK(w == 1080 && h == 1080);
        sizeForRatio(21, 9, 1080, &w, &h); CHECK(w == 2520 && h == 1080);
        sizeForRatio(3, 2, 721, &w, &h);   CHECK(w % 2 == 0 && h % 2 == 0);                    // custom: always even (encoders need it)
    }

    std::printf("[audio mixer]\n");
    {
        Project mp; QUndoStack ms; mp.addAsset(a);
        ms.push(makeAddClipCommand(mp, a.id, 1, 1.0));                  // video-with-audio clip on V1, starts at 1 s, 6 s long
        std::vector<float> buf(48000 * 2);
        auto level = [&](AudioMixer& m, double t, int frames = 4800) {
            m.render(t, frames, buf.data()); double s = 0;
            for (int i = 0; i < frames * 2; ++i) s += double(buf[size_t(i)]) * buf[size_t(i)];
            return std::sqrt(s / (frames * 2));
        };
        AudioMixer base(mp);
        const double L = level(base, 3.0);
        std::printf("        reference level %.4f\n", L);
        CHECK(L > 0.05 && level(base, 0.5) < 1e-6 && level(base, 8.0) < 1e-6);                 // silent before and after the clip
        auto variant = [&](auto edit) { Project q; q.settings = mp.settings; q.tracks = mp.tracks; q.assets = mp.assets; q.clips = mp.clips; edit(q); return AudioMixer(q); };
        { AudioMixer m = variant([](Project& q) { q.clips[0].volume = 0.5; }); CHECK(near(level(m, 3.0) / L, 0.5, 0.06)); }
        { AudioMixer m = variant([](Project& q) { q.clips[0].muted = true; }); CHECK(level(m, 3.0) < 1e-6); }
        { AudioMixer m = variant([](Project& q) { q.tracks[1].muted = true; }); CHECK(level(m, 3.0) < 1e-6); }
        { AudioMixer m = variant([](Project& q) { q.tracks[1].hidden = true; }); CHECK(level(m, 3.0) < 1e-6); }
        { AudioMixer m = variant([](Project& q) { q.clips[0].fadeIn = 1.0; q.clips[0].fadeOut = 1.0; });
          CHECK(level(m, 1.05, 2400) < L * 0.2 && near(level(m, 4.0) / L, 1.0, 0.08) && level(m, 6.9, 2400) < L * 0.2); }
        { AudioMixer m = variant([](Project& q) { setKeyAt(q.clips[0], "volume", 0.0, 0.0, 0.01); setKeyAt(q.clips[0], "volume", 2.0, 1.0, 0.01); });
          const double mid = level(m, 2.0, 2400);                                              // clip-local 1.0 s: halfway through the ramp
          CHECK(near(mid / L, 0.5, 0.12) && near(level(m, 4.0) / L, 1.0, 0.08)); }
        {   // speed 2: pitch doubles and the clip is half as long; verify by counting zero crossings
            AudioMixer m = variant([](Project& q) { Clip c = q.clips[0]; c.speed = 2.0; c.dur = c.dur / 2; q.replace({q.clips[0]}, {c}); });
            auto crossings = [&](AudioMixer& mm, double t) { mm.render(t, 24000, buf.data()); int n = 0; for (int i = 1; i < 24000; ++i) if ((buf[size_t(2 * i - 2)] < 0) != (buf[size_t(2 * i)] < 0)) ++n; return n; };
            const double ratio = double(crossings(m, 1.5)) / double(crossings(base, 3.0));
            std::printf("        speed 2 → zero-crossing ratio %.2f\n", ratio);
            CHECK(near(ratio, 2.0, 0.15) && level(m, 4.2) < 1e-6);                             // clip now ends at 4 s
        }
        {   // freeze frame: silent while frozen, audio resumes at the right place
            Project q; q.settings = mp.settings; q.tracks = mp.tracks; q.assets = mp.assets; q.clips = mp.clips;
            // (use a real Project for the command)
            Project fz; QUndoStack fzs; fz.addAsset(a); fzs.push(makeAddClipCommand(fz, a.id, 1, 0));
            fzs.push(makeFreezeCommand(fz, {fz.clips[0].id}, 2.0, 1.0));                        // 0..2 | freeze 2..3 | 3..7
            AudioMixer m(fz);
            const double before = level(m, 1.0), during = level(m, 2.5), after = level(m, 3.5);
            CHECK(before > 0.05 && during < 1e-6 && after > 0.05);
            // sync: the right half starts at source time 2.0 and the video layer there asks for exactly that frame
            CHECK(near(fz.videoLayersAt(3.0)[0].sourceTime, 2.0, 1e-6) && near(fz.videoLayersAt(2.5)[0].sourceTime, 2.0, 1e-6));
        }
    }

    std::printf("[import formats]\n");
    {
        struct F { const char* name; QStringList args; bool video, audio, image; };
        const F files[] = {
            {"t.mkv",  {"-f","lavfi","-i","testsrc=d=2:s=320x180:r=25","-f","lavfi","-i","sine=d=2","-c:v","libx264","-c:a","aac"}, true, true, false},
            {"t.avi",  {"-f","lavfi","-i","testsrc=d=2:s=320x180:r=25","-c:v","mpeg4"}, true, false, false},
            {"t.webm", {"-f","lavfi","-i","testsrc=d=2:s=320x180:r=25","-c:v","libvpx"}, true, false, false},
            {"t.gif",  {"-f","lavfi","-i","testsrc=d=2:s=160x90:r=10"}, true, false, false},
            {"t.mp3",  {"-f","lavfi","-i","sine=d=2","-c:a","libmp3lame"}, false, true, false},
            {"t.wav",  {"-f","lavfi","-i","sine=d=2"}, false, true, false},
            {"t.flac", {"-f","lavfi","-i","sine=d=2"}, false, true, false},
            {"t.ogg",  {"-f","lavfi","-i","sine=d=2","-c:a","libvorbis"}, false, true, false},
            {"t.webp", {"-f","lavfi","-i","testsrc=d=1:s=160x90:r=1","-frames:v","1"}, true, false, true},
            {"t.png",  {"-f","lavfi","-i","testsrc=d=1:s=160x90:r=1","-frames:v","1"}, true, false, true},
            {"t.jpg",  {"-f","lavfi","-i","testsrc=d=1:s=160x90:r=1","-frames:v","1"}, true, false, true},
        };
        for (const F& f : files) {
            const QString path = dir.filePath(f.name);
            QStringList a2 = {"-y", "-loglevel", "error"}; a2 += f.args; a2 << path;
            QProcess::execute("ffmpeg", a2);
            if (!QFileInfo::exists(path)) { std::printf("  SKIP  %s (this FFmpeg build can't create it)\n", f.name); continue; }
            MediaAsset m; QString e2;
            const bool ok = probeMedia(path, &m, &e2);
            if (!ok) std::printf("        %s: %s\n", f.name, qPrintable(e2));
            const bool match = ok && m.hasVideo == f.video && m.hasAudio == f.audio && m.isImage == f.image && (f.image || near(m.duration, 2.0, 0.4));
            std::printf("  %s  %-7s video=%d audio=%d image=%d dur=%.2f\n", match ? "ok  " : "FAIL", f.name, ok && m.hasVideo, ok && m.hasAudio, ok && m.isImage, ok ? m.duration : 0.0);
            if (!match) ++g_fail;
            if (ok && m.hasVideo) { Decoder dd; CHECK(dd.open(path, &e2, false) && !dd.frameAt(f.image ? 0 : 0.5).isNull()); }
            if (ok && m.hasAudio && !m.hasVideo) { AudioSource as2; std::vector<float> ab(4800 * 2); CHECK(as2.open(path)); as2.read(48000, 4800, ab.data()); double r = 0; for (float v : ab) r += double(v) * v; CHECK(r > 1.0); }
        }
    }

    std::printf("[export: audio, repair, GIF]\n");
    {
        Project p6; QUndoStack s6; p6.addAsset(a);
        MediaAsset g; QString e3; probeMedia(dir.filePath("t.gif"), &g, &e3); g.id = newId(); p6.addAsset(g);
        p6.settings.width = 640; p6.settings.height = 360;
        s6.push(makeAddClipCommand(p6, a.id, 1, 0));
        { Clip c = p6.clips[0]; c.dur = 3.0; c.in = 1.0; p6.replace({c}, {c}); }                  // a 3 second cut
        s6.push(makeAddClipCommand(p6, g.id, 0, 0));                                              // 2 s GIF overlay on V2
        ExportOptions o6; o6.outPath = dir.filePath("audio3.mp4"); o6.quality = 28; o6.allowGpu = false;
        Exporter e6; QString m6;
        CHECK(runExport(e6, p6, o6, &m6));
        if (m6 != "Export finished.") std::printf("        %s\n", qPrintable(m6));
        DurationCheck dc = Exporter::checkDuration(o6.outPath, 3.0);
        std::printf("        %s\n", qPrintable(dc.detail));
        CHECK(dc.ok && near(dc.videoEnd, 3.0, 0.1) && near(dc.audioEnd, 3.0, 0.1));
        { AudioSource as3; std::vector<float> ab(4800 * 2); CHECK(as3.open(o6.outPath)); as3.read(48000, 4800, ab.data()); double r = 0; for (float v : ab) r += double(v) * v; CHECK(std::sqrt(r / ab.size()) > 0.04); }
        // muted track → the file still has an audio stream, but it is silent
        p6.tracks[1].muted = true;
        ExportOptions o7 = o6; o7.outPath = dir.filePath("muted.mp4");
        Exporter e7; CHECK(runExport(e7, p6, o7, &m6));
        { AudioSource as4; std::vector<float> ab(4800 * 2); CHECK(as4.open(o7.outPath)); as4.read(48000, 4800, ab.data()); double r = 0; for (float v : ab) r += double(v) * v; CHECK(std::sqrt(r / ab.size()) < 0.003); }

        // the "thousands of hours" symptom: break the header on purpose, make sure we detect and repair it
        QFile bf(o6.outPath); CHECK(bf.open(QIODevice::ReadOnly)); QByteArray bytes = bf.readAll(); bf.close();
        const int mv = bytes.indexOf("mvhd");
        CHECK(mv > 0 && bytes[mv + 4] == 0);
        const int off = mv + 20;                                    // mvhd v0: duration field
        bytes[off] = char(0xFF); bytes[off + 1] = char(0xFF); bytes[off + 2] = char(0xFF); bytes[off + 3] = char(0x00);
        const QString broken = dir.filePath("broken.mp4");
        QFile wf(broken); CHECK(wf.open(QIODevice::WriteOnly)); wf.write(bytes); wf.close();
        const DurationCheck bad = Exporter::checkDuration(broken, 3.0);
        std::printf("        broken file: header says %.0f s (%.0f hours)\n", bad.headerSeconds, bad.headerSeconds / 3600);
        CHECK(!bad.ok && bad.headerSeconds > 3600);
        CHECK(Exporter::repairByRemux("ffmpeg", broken, 3.0));
        const DurationCheck fixed = Exporter::checkDuration(broken, 3.0);
        std::printf("        after repair: %s\n", qPrintable(fixed.detail));
        CHECK(fixed.ok);
    }

    std::printf("[GPU: transform, opacity, keyframes]\n");
    if (Exporter::gpuAvailable()) {
        Project p9; QUndoStack s9; p9.addAsset(a);
        p9.settings.width = 640; p9.settings.height = 360; p9.settings.fps = 30;
        s9.push(makeAddClipCommand(p9, a.id, 1, 0));
        Clip c = p9.clips[0]; c.dur = 4; c.in = 0;
        c.scale = 0.5;
        setKeyAt(c, "opacity", 0.0, 0.0, 0.01); setKeyAt(c, "opacity", 2.0, 1.0, 0.01);
        p9.replace({p9.clips[0]}, {c});
        ExportOptions o9; o9.outPath = dir.filePath("xform.mp4"); o9.quality = 24;
        Exporter e9; QString m9; CHECK(runExport(e9, p9, o9, &m9) && e9.usedGpu());
        const QImage fr0 = frameOf(o9.outPath, 0.1), fr1 = frameOf(o9.outPath, 1.0), fr3 = frameOf(o9.outPath, 3.0);
        const double l0 = meanLuma(fr0), l1 = meanLuma(fr1), l3 = meanLuma(fr3);
        std::printf("        luma at 0.1 s: %.1f   1.0 s (half-way ramp): %.1f   3.0 s: %.1f\n", l0, l1, l3);
        CHECK(l0 < 12 && l1 > l0 + 5 && l3 > l1 + 5);                                             // fade-in by opacity keyframes
        const QRgb corner = fr3.convertToFormat(QImage::Format_RGB32).pixel(4, 4);
        const QRgb centre = fr3.convertToFormat(QImage::Format_RGB32).pixel(fr3.width() / 2, fr3.height() / 2);
        CHECK(qGray(corner) < 20 && qGray(centre) > 20);                                           // scale 0.5: black border around a smaller picture
    } else std::printf("  SKIP  no OpenGL here\n");

    // ======================================================================== Batch 2 ========================================================================
    std::printf("[effects model: registry, resolution, keyframes, layers, mask]\n");
    {
        CHECK(effectDef("blur") && effectDef("adjust") && effectDef("chromakey") && !effectDef("shake"));
        Effect b = makeEffect("blur");
        CHECK(near(b.amount, 0.4, 1e-9));
        Clip c; c.id = "c"; c.dur = 5; c.effects = {makeEffect("shadow"), makeEffect("blackflash")};
        ProjectSettings ps;
        QVector<FxPass> v = resolvePasses(c, 1.0, ps);
        CHECK(v.size() == 1 && v[0].type == "shadow" && near(v[0].p["ox"], 24, 1e-9));              // black flash is between flashes at 1.0 s → omitted
        // soft black flash: eases 0 → peak → 0 over its length
        c.effects = {makeEffect("blackflash")};
        auto amount = [&](double t) { QVector<FxPass> q = resolvePasses(c, t, ps); return q.isEmpty() ? 0.0 : q[0].p["amount"]; };
        // rhythmic flicker: 2 flashes per second, dark for half of each cycle, soft edges
        c.effects[0].p["rate"] = 2.0; c.effects[0].p["dark"] = 0.5; c.effects[0].p["soft"] = 0.2;
        CHECK(amount(0.0) < 1e-3 && near(amount(0.125), 1.0, 1e-6) && amount(0.02) > 0.0 && amount(0.02) < 0.9 && amount(0.3) == 0.0 && near(amount(0.625), 1.0, 1e-6));
        // double flash: two dark pulses, then a pause (hard edges here)
        c.effects[0].p["pattern"] = 1; c.effects[0].p["rate"] = 1.0; c.effects[0].p["dark"] = 0.6; c.effects[0].p["soft"] = 0.0;
        CHECK(near(amount(0.15), 1.0, 1e-6) && amount(0.45) == 0.0 && near(amount(0.75), 1.0, 1e-6) && amount(0.95) == 0.0);
        c.effects[0].p["glitch"] = 0.5;                                                           // the glitch rides along with each flash
        CHECK(near(resolvePasses(c, 0.15, ps)[0].p["glitchAmt"], 0.5, 1e-6) && resolvePasses(c, 0.15, ps)[0].p["seed"] == 0.0 && resolvePasses(c, 1.15, ps)[0].p["seed"] == 1.0);
        // strength and parameters are keyframable ("fx.<type>" and "fx.<type>.<param>")
        c.effects = {makeEffect("blur")};
        setKeyAt(c, "fx.blur", 0, 0.0, 0.01); setKeyAt(c, "fx.blur", 2, 1.0, 0.01);
        CHECK(near(resolvePasses(c, 1.0, ps)[0].strength, 0.5, 1e-9));
        c.effects = {makeEffect("adjust")}; c.keys.clear();
        setKeyAt(c, "fx.adjust.exposure", 0, 0.0, 0.01); setKeyAt(c, "fx.adjust.exposure", 4, 0.8, 0.01);
        CHECK(near(resolvePasses(c, 2.0, ps)[0].p["exposure"], 0.4, 1e-9));
        // mask: off → nothing; on → keyframed geometry
        CHECK(!resolveMask(c, 0).on);
        c.mask = {true, false, 10, 20, 100, 60, 0, 8};
        setKeyAt(c, "mask.x", 0, 0, 0.01); setKeyAt(c, "mask.x", 4, 200, 0.01); setKeyAt(c, "mask.feather", 0, 0, 0.01); setKeyAt(c, "mask.feather", 4, 40, 0.01);
        const MaskFx m = resolveMask(c, 2.0);
        CHECK(m.on && near(m.x, 100, 1e-9) && near(m.y, 20, 1e-9) && near(m.feather, 20, 1e-9) && near(m.w, 100, 1e-9));
        // layers: an effect layer has no picture, spans the timeline, and is found at the right times
        Project lp; QUndoStack ls; lp.addAsset(a);
        CHECK(lp.tracks.size() == 5 && lp.tracks[0].name == "V3");                                  // a new top video track for layers
        ls.push(makeAddClipCommand(lp, a.id, 2, 0));                                                // V1: 6 s
        ls.push(makeAddLayerCommand(lp, makeEffect("blur"), 0, 1.0));                               // on V3, from 1 s
        CHECK(lp.clips.size() == 2);
        Clip layer; for (const Clip& k : lp.clips) if (k.kind != KindMedia) layer = k;
        CHECK(layer.kind == KindEffectLayer && layer.assetId.isEmpty() && near(layer.start, 1.0, 1e-9) && near(layer.dur, 5.0, 0.3));
        QVector<ActiveLayer> al = lp.videoLayersAt(2.0), al0 = lp.videoLayersAt(0.5);
        CHECK(al.size() == 2 && al[0].path == src && !al[0].isPass && al[1].isPass && al[1].passes.size() == 1 && al0.size() == 1);   // bottom first; layer on top
        ls.push(makeAddLayerCommand(lp, makeEffect("adjust"), 1, 0.0));
        int adj = 0; for (const Clip& k : lp.clips) if (k.kind == KindAdjustment) ++adj;
        CHECK(adj == 1 && !lp.usesEffects() == false);
        CHECK(lp.save(dir.filePath("b2.vproj"), &err));
        Project lq; CHECK(lq.load(dir.filePath("b2.vproj"), &err));
        bool same3 = lq.clips.size() == lp.clips.size();
        for (const Clip& k : lp.clips) { const Clip* o2 = lq.clip(k.id); same3 = same3 && o2 && *o2 == k; }
        CHECK(same3);
        // applying a clip effect twice replaces it and starts fresh (old keyframes dropped)
        Clip& media = lp.clips[0].kind == KindMedia ? lp.clips[0] : lp.clips[1];
        { Clip m2 = media; m2.effects = {makeEffect("blur")}; setKeyAt(m2, "fx.blur", 0, 0.9, 0.01); lp.replace({media}, {m2}); }
        ls.push(makeApplyEffectCommand(lp, {media.id}, makeEffect("blur")));
        CHECK(lp.clip(media.id)->effects.size() == 1 && !lp.clip(media.id)->keys.contains("fx.blur"));
        ls.push(makeSetEffectsCommand(lp, media.id, {}, "x"));
        CHECK(lp.clip(media.id)->effects.isEmpty());
        // layers are not media: they don't break export checks or the audio mixer
        AudioMixer lm(lp); std::vector<float> lbuf(4800 * 2); lm.render(2.0, 4800, lbuf.data());
        CHECK(true);
    }

    std::printf("[GPU compositor: masks and effects, pixel by pixel]\n");
    if (!Exporter::gpuAvailable()) {
        std::printf("  SKIP  no OpenGL here (run under a desktop session or xvfb-run)\n");
    } else {
        QOffscreenSurface surf; surf.setFormat(QSurfaceFormat::defaultFormat()); surf.create();
        QOpenGLContext gctx; gctx.setFormat(QSurfaceFormat::defaultFormat()); gctx.create(); gctx.makeCurrent(&surf);
        {
            Compositor comp; CHECK(comp.init());
            const QSize cs(320, 180);
            QOpenGLFramebufferObject fbo(cs);
            auto render = [&](const QVector<RenderLayer>& layers) { comp.render(layers, cs, &fbo); return fbo.toImage().convertToFormat(QImage::Format_RGB32); };
            auto solid = [&](QColor c) { QImage im(320, 180, QImage::Format_RGBA8888); im.fill(c); return im; };
            auto pic = [&](QImage im) { RenderLayer l; l.image = im; return l; };
            auto pass = [&](const QString& type, double strength, QMap<QString, double> p, MaskFx m = MaskFx()) {
                RenderLayer l; l.pass = true; l.mask = m; FxPass f; f.type = type; f.strength = strength; f.p = p; l.passes = {f}; return l; };
            auto fxLayer = [&](QImage im, const QString& type, double strength, QMap<QString, double> p) { RenderLayer l = pic(im); FxPass f; f.type = type; f.strength = strength; f.p = p; l.passes = {f}; return l; };
            auto at = [&](const QImage& im, int x, int y) { return QColor(im.pixel(x, y)); };
            auto luma = [&](QColor c) { return 0.299 * c.red() + 0.587 * c.green() + 0.114 * c.blue(); };
            auto checker = [&] { QImage im(320, 180, QImage::Format_RGBA8888); for (int y = 0; y < 180; ++y) for (int x = 0; x < 320; ++x) im.setPixelColor(x, y, ((x / 8 + y / 8) & 1) ? Qt::white : Qt::black); return im; };
            auto variance = [&](const QImage& im, QRect r) { double s = 0, s2 = 0; int n = 0; for (int y = r.top(); y < r.bottom(); ++y) for (int x = r.left(); x < r.right(); ++x) { const double l = luma(at(im, x, y)); s += l; s2 += l * l; ++n; } const double m = s / n; return s2 / n - m * m; };

            const QImage red = render({pic(solid(Qt::red))});
            CHECK(at(red, 160, 90) == QColor(255, 0, 0) && at(red, 2, 2) == QColor(255, 0, 0));    // plain layer passes through untouched

            { MaskFx m{true, false, 0, 0, 100, 60, 0, 0};                                          // --- rectangle mask
              RenderLayer l = pic(solid(Qt::red)); l.mask = m;
              QImage a1 = render({l}); CHECK(at(a1, 160, 90).red() > 250 && luma(at(a1, 20, 20)) < 3);                       // inside red, outside black
              l.mask.invert = true; QImage a2 = render({l}); CHECK(luma(at(a2, 160, 90)) < 3 && at(a2, 20, 20).red() > 250);
              l.mask.invert = false; l.mask.feather = 40; QImage a3 = render({l});                                            // feather: soft edge
              const int edge = at(a3, 210, 90).red();                                                                          // exactly on the right edge (x = 160 + 50)
              CHECK(edge > 80 && edge < 180);
              l.mask.feather = 0; l.mask.x = 100; QImage a4 = render({l});                                                     // moved right
              CHECK(at(a4, 260, 90).red() > 250 && luma(at(a4, 160, 90)) < 3);
              l.mask.x = 0; l.mask.w = 100; l.mask.h = 20; l.mask.rot = 90; QImage a5 = render({l});                           // rotated 90°: a tall thin bar
              CHECK(at(a5, 160, 130).red() > 250 && luma(at(a5, 200, 90)) < 3); }

            { const QImage plain = render({pic(checker())});                                       // --- blur (effect layer)
              QImage b1 = render({pic(checker()), pass("blur", 0.3, {})});
              CHECK(variance(b1, QRect(40, 40, 240, 100)) < variance(plain, QRect(40, 40, 240, 100)) * 0.25);
              MaskFx m{true, false, -80, 0, 100, 120, 0, 0};                                        // only the left part
              QImage b2 = render({pic(checker()), pass("blur", 0.3, {}, m)});
              CHECK(variance(b2, QRect(40, 40, 70, 100)) < 3000 && at(b2, 250, 90) == at(plain, 250, 90) && at(b2, 270, 20) == at(plain, 270, 20));   // blurred inside the mask, untouched outside
              QImage b3 = render({pic(checker()), pass("blur", 0.0, {})}); CHECK(at(b3, 100, 50) == at(plain, 100, 50)); }   // strength 0 = no effect

            { QImage lr(320, 180, QImage::Format_RGBA8888); for (int y = 0; y < 180; ++y) for (int x = 0; x < 320; ++x) lr.setPixelColor(x, y, x < 160 ? Qt::red : Qt::blue);
              QImage tb(320, 180, QImage::Format_RGBA8888); for (int y = 0; y < 180; ++y) for (int x = 0; x < 320; ++x) tb.setPixelColor(x, y, y < 90 ? Qt::green : Qt::yellow);   // --- mirror = flip left <-> right
              RenderLayer fl = pic(lr); fl.fx.flipX = true;
              QImage m = render({fl});
              CHECK(at(m, 70, 90).blue() > 250 && at(m, 250, 90).red() > 250);                                                 // what was on the right is now on the left, and vice versa
              CHECK(at(render({pic(lr)}), 70, 90).red() > 250);                                                                 // without the effect: unchanged
              RenderLayer ft = pic(tb); ft.fx.flipX = true; QImage mt = render({ft});
              CHECK(at(mt, 160, 30).green() > 250 && at(mt, 160, 150).red() > 250);                                             // top and bottom stay where they were
              RenderLayer small = pic(lr); small.fx.scale = 0.5; small.fx.flipX = true; QImage ms = render({small});          // flips about the picture's own centre, not the canvas
              CHECK(at(ms, 100, 90).blue() > 250 && at(ms, 220, 90).red() > 250 && luma(at(ms, 40, 90)) < 3); }

            { QImage gs = solid(Qt::green); { QPainter p(&gs); p.fillRect(QRect(120, 60, 80, 60), Qt::red); }     // --- chroma key over a blue background
              QMap<QString, double> ck = {{"r", 0}, {"g", 1}, {"b", 0}, {"tol", 0.2}, {"soft", 0.1}, {"spill", 0.4}};
              QImage k1 = render({pic(solid(Qt::blue)), fxLayer(gs, "chromakey", 1.0, ck)});
              CHECK(at(k1, 20, 20).blue() > 250 && at(k1, 20, 20).green() < 10);                                 // green is gone: the blue shows through
              CHECK(at(k1, 160, 90).red() > 250 && at(k1, 160, 90).blue() < 10);                                 // the red object stays
              QImage k2 = render({pic(solid(Qt::blue)), fxLayer(gs, "chromakey", 0.0, ck)}); CHECK(at(k2, 20, 20).green() > 250);   // strength 0 keeps the green
              // several colours in one picture: only the picked colour (blue) disappears; red, grey, white and a dark blue-ish grey all stay
              QImage mc(320, 180, QImage::Format_RGBA8888);
              for (int y = 0; y < 180; ++y) for (int x = 0; x < 320; ++x) mc.setPixelColor(x, y, x < 80 ? QColor(Qt::red) : x < 160 ? QColor(Qt::blue) : x < 240 ? QColor(128, 128, 128) : QColor(230, 230, 230));
              QMap<QString, double> bk = {{"r", 0}, {"g", 0}, {"b", 1}, {"tol", 0.15}, {"soft", 0.08}, {"spill", 0}};
              QImage k3 = render({pic(solid(Qt::black)), fxLayer(mc, "chromakey", 1.0, bk)});
              CHECK(luma(at(k3, 120, 90)) < 3);                                                                                // blue → removed (shows the black backdrop)
              CHECK(at(k3, 40, 90).red() > 250 && at(k3, 200, 90).red() == 128 && at(k3, 280, 90).red() == 230);               // everything else untouched
              QImage k4 = render({pic(solid(Qt::white)), fxLayer(mc, "chromakey", 1.0, bk)}); CHECK(at(k4, 120, 90).red() > 250 && at(k4, 120, 90).blue() > 250 && at(k4, 120, 90).green() > 250); }

            { QImage d1 = render({pic(solid(QColor(200, 200, 200))), pass("blackflash", 1.0, {{"amount", 0.5}})});      // --- black flash
              CHECK(std::abs(at(d1, 50, 50).red() - 100) < 4);
              QImage d2 = render({fxLayer(solid(QColor(200, 200, 200)), "blackflash", 1.0, {{"amount", 1.0}})}); CHECK(luma(at(d2, 50, 50)) < 3);   // as a clip effect
              QImage d3 = render({pic(solid(QColor(200, 200, 200))), pass("blackflash", 1.0, {{"amount", 0.0}})}); CHECK(at(d3, 50, 50).red() == 200); }

            { RenderLayer sq = pic(solid(Qt::white)); sq.fx.scale = 0.3;                                 // --- drop shadow (a small white square on grey)
              QImage plainSq = render({pic(solid(QColor(128, 128, 128))), sq});
              FxPass f; f.type = "shadow"; f.strength = 1.0; f.p = {{"ox", 20}, {"oy", 20}, {"blur", 0}, {"r", 0}, {"g", 0}, {"b", 0}};
              RenderLayer sh = sq; sh.passes = {f};
              QImage s1 = render({pic(solid(QColor(128, 128, 128))), sh});
              CHECK(at(plainSq, 200, 130).red() == 128 && at(s1, 200, 130).red() < 40);                                  // shadow lands down-right of the square
              CHECK(at(s1, 160, 90).red() > 250 && at(s1, 40, 40).red() == 128 && at(s1, 120, 50).red() == 128); }       // square and far background untouched

            { RenderLayer rl = pic(solid(Qt::red)); rl.fx.scale = 0.5;                                    // --- border (inner stroke of the picture)
              FxPass f; f.type = "border"; f.strength = 0.3; f.p = {{"r", 1}, {"g", 1}, {"b", 1}};
              rl.passes = {f};
              QImage b = render({rl});
              CHECK(at(b, 84, 90).green() > 200 && at(b, 160, 90).green() < 30 && luma(at(b, 40, 90)) < 3); }          // white rim just inside the edge, red centre, black outside

            { QImage g = render({pic(solid(QColor(100, 100, 100))), pass("adjust", 1.0, {{"exposure", 0.5}})});        // --- adjustments
              CHECK(at(g, 100, 100).red() > 150);
              QImage s = render({pic(solid(Qt::red)), pass("adjust", 1.0, {{"saturation", -1.0}})});
              CHECK(std::abs(at(s, 100, 100).red() - at(s, 100, 100).green()) < 4 && std::abs(at(s, 100, 100).red() - at(s, 100, 100).blue()) < 4);
              QImage w = render({pic(solid(QColor(128, 128, 128))), pass("adjust", 1.0, {{"temp", 1.0}})});
              CHECK(at(w, 50, 50).red() > 150 && at(w, 50, 50).blue() < 110);
              QImage con = render({pic(solid(QColor(180, 180, 180))), pass("adjust", 1.0, {{"contrast", 0.5}})}); CHECK(at(con, 50, 50).red() > 200);
              QImage v = render({pic(solid(QColor(200, 200, 200))), pass("adjust", 1.0, {{"vignette", 1.0}})});
              CHECK(at(v, 160, 90).red() > 190 && at(v, 3, 3).red() < 60);                                            // dark corners, bright centre
              QImage hl = render({pic(solid(QColor(230, 230, 230))), pass("adjust", 1.0, {{"highlights", -1.0}})}); CHECK(at(hl, 50, 50).red() < 215);
              QImage sdw = render({pic(solid(QColor(40, 40, 40))), pass("adjust", 1.0, {{"shadows", 1.0}})}); CHECK(at(sdw, 50, 50).red() > 60);
              QImage sharp = render({pic(checker()), pass("adjust", 1.0, {{"sharpen", 1.0}, {"clarity", 0.5}})}); CHECK(variance(sharp, QRect(40, 40, 240, 100)) >= variance(render({pic(checker())}), QRect(40, 40, 240, 100)) * 0.95);
              MaskFx half{true, false, -80, 0, 160, 180, 0, 0};                                                        // adjustment limited by a mask
              QImage mk = render({pic(solid(QColor(100, 100, 100))), pass("adjust", 1.0, {{"exposure", 0.5}}, half)});
              CHECK(at(mk, 40, 90).red() > 150 && at(mk, 280, 90).red() == 100);
              QImage amt = render({pic(solid(QColor(100, 100, 100))), pass("adjust", 0.0, {{"exposure", 0.5}})}); CHECK(at(amt, 40, 90).red() == 100); }   // amount 0 = nothing

            { QImage st = render({pic(solid(Qt::green)), pass("adjust", 1.0, {{"saturation", -1.0}}), [&] { RenderLayer t = pic(solid(Qt::red)); t.fx.scale = 0.3; return t; }()});   // --- layer order
              CHECK(std::abs(at(st, 20, 20).red() - at(st, 20, 20).green()) < 6 && at(st, 160, 90).red() > 250 && at(st, 160, 90).green() < 10); }          // effect layer hits what is below it, not what is above
        }
        gctx.doneCurrent();
    }

    std::printf("[GPU export with an adjustment layer + effect layer]\n");
    if (Exporter::gpuAvailable()) {
        auto exportWith = [&](bool withLayer) {
            Project p; QUndoStack s; p.addAsset(a);
            p.settings.width = 640; p.settings.height = 360; p.settings.fps = 30;
            s.push(makeAddClipCommand(p, a.id, 2, 0));
            { Clip c = p.clips[0]; c.dur = 3; p.replace({c}, {c}); }
            if (withLayer) {
                Effect e = makeEffect("adjust"); e.p["exposure"] = 0.8; e.p["saturation"] = -1.0;
                s.push(makeAddLayerCommand(p, e, 0, 0));
                Effect b = makeEffect("blackflash"); b.p["rate"] = 0.5; b.p["dark"] = 0.5; b.p["soft"] = 0.2; b.p["glitch"] = 0; s.push(makeAddLayerCommand(p, b, 1, 0));   // dark from 0 to 1 s of every 2 s
            }
            ExportOptions o; o.outPath = dir.filePath(withLayer ? "layers.mp4" : "nolayers.mp4"); o.quality = 24;
            Exporter ex; QString m; const bool ok = runExport(ex, p, o, &m);
            if (!ok) std::printf("        %s\n", qPrintable(m));
            return std::make_pair(ok, o.outPath);
        };
        auto plain = exportWith(false), layered = exportWith(true);
        CHECK(plain.first && layered.first);
        const QImage fp = frameOf(plain.second, 1.5).convertToFormat(QImage::Format_RGB32), fl = frameOf(layered.second, 1.5).convertToFormat(QImage::Format_RGB32);
        const QImage fpeak = frameOf(layered.second, 0.5).convertToFormat(QImage::Format_RGB32);
        // saturation -1 → grey pixels in the layered export (testsrc is very colourful), and the flash is darkest at its peak (0.5 s of a 1 s flash)
        auto chroma = [](const QImage& im) { double s = 0; for (int y = 0; y < im.height(); y += 4) for (int x = 0; x < im.width(); x += 4) { QColor c(im.pixel(x, y)); s += std::abs(c.red() - c.green()) + std::abs(c.green() - c.blue()); } return s; };
        std::printf("        colourfulness plain %.0f  layered %.0f   luma at flash peak %.1f vs settled %.1f\n", chroma(fp), chroma(fl), meanLuma(fpeak), meanLuma(fl));
        CHECK(chroma(fl) < chroma(fp) * 0.15 && meanLuma(fpeak) < 25 && meanLuma(fl) > meanLuma(fpeak) + 20);
    } else std::printf("  SKIP  no OpenGL here\n");

    // ======================================================================== Batch 3 ========================================================================
    std::printf("[tracks: add, remove, renumber]\n");
    {
        Project tp; QUndoStack ts; tp.addAsset(a);
        ts.push(makeAddClipCommand(tp, a.id, 2, 0));                                          // on V1
        const QString cid = tp.clips[0].id;
        ts.push(makeInsertTrackCommand(tp, TrackKind::Video, 0));                              // a new top video track
        CHECK(tp.tracks.size() == 6 && tp.tracks[0].name == "V4" && tp.tracks[3].name == "V1" && tp.tracks[4].name == "A1");
        CHECK(tp.clip(cid)->track == 3 && tp.tracks[tp.clip(cid)->track].name == "V1");        // the clip stays on "its" track
        ts.undo();
        CHECK(tp.tracks.size() == 5 && tp.clip(cid)->track == 2);
        ts.push(makeInsertTrackCommand(tp, TrackKind::Video, 99));                             // asking for a video track below the audio ones is clamped
        CHECK(tp.tracks.last().kind == TrackKind::Audio && tp.tracks[3].kind == TrackKind::Video && tp.tracks[4].kind == TrackKind::Audio);
        ts.undo();
        ts.push(makeInsertTrackCommand(tp, TrackKind::Audio, 99));
        CHECK(tp.tracks.last().name == "A3" && tp.tracks.size() == 6);
        QString terr;
        CHECK(makeRemoveTrackCommand(tp, 2, false, &terr) == nullptr && !terr.isEmpty());      // V1 has a clip: needs the "also remove clips" go-ahead
        ts.push(makeRemoveTrackCommand(tp, 2, true, &terr));
        CHECK(tp.clips.isEmpty() && tp.tracks.size() == 5 && tp.tracks[1].name == "V1" && tp.tracks[2].name == "A1");   // the track is gone and the rest renumbered
        ts.undo();
        CHECK(tp.clips.size() == 1 && tp.clip(cid)->track == 2 && tp.tracks.size() == 6);      // undo brings the track and its clip back
        Project one; one.tracks = {{"V1", TrackKind::Video}, {"A1", TrackKind::Audio}};
        CHECK(makeRemoveTrackCommand(one, 0, true, &terr) == nullptr && makeRemoveTrackCommand(one, 1, true, &terr) == nullptr);   // the last track of a kind stays
        for (int i = 0; i < 20; ++i) if (QUndoCommand* c = makeInsertTrackCommand(one, TrackKind::Video, 0)) { c->redo(); delete c; }
        int nv = 0; for (const Track& t : one.tracks) if (t.kind == TrackKind::Video) ++nv;
        CHECK(nv == kMaxTracksPerKind);
        one.tracks[1].locked = true; CHECK(makeRemoveTrackCommand(one, 1, true, &terr) == nullptr);
    }

    std::printf("[copy / paste]\n");
    {
        Project pp; QUndoStack ps; pp.addAsset(a);
        ps.push(makeAddClipCommand(pp, a.id, 2, 0));                                          // A: V1 0..6
        ps.push(makeAddClipCommand(pp, a.id, 2, 7));                                          // B: V1 7..13
        { Clip c = pp.clips[0]; c.opacity = 0.5; setKeyAt(c, "opacity", 1.0, 0.2, 0.01); c.effects = {makeEffect("blur")}; pp.replace({pp.clips[0]}, {c}); }
        const QStringList both{pp.clips[0].id, pp.clips[1].id};
        const QVector<Clip> clip = copyClips(pp, both);
        CHECK(clip.size() == 2 && clip[0].start <= clip[1].start);
        QStringList made;
        ps.push(makePasteCommand(pp, clip, 20.0, &made));
        CHECK(pp.clips.size() == 4 && made.size() == 2);
        const Clip *n0 = pp.clip(made[0]), *n1 = pp.clip(made[1]);
        CHECK(n0 && n1 && near(n0->start, 20.0, 1e-9) && near(n1->start, 27.0, 1e-9) && n0->track == 2 && n0->id != clip[0].id);       // same spacing, same track, new ids
        CHECK(n0->effects.size() == 1 && n0->keys.contains("opacity") && near(n0->opacity, 0.5, 1e-9));                              // effects and keyframes come along
        ps.undo(); CHECK(pp.clips.size() == 2);
        made.clear();
        ps.push(makePasteCommand(pp, clip, 2.0, &made));                                      // pasting on top of existing clips: slides to the first free spot
        bool clean = pp.clips.size() == 4;
        for (const Clip& c : pp.clips) clean = clean && !pp.overlaps(c);
        CHECK(clean && pp.clip(made[0])->start >= 13.0 - 1e-6);
        ps.undo();
        pp.tracks[2].locked = true;
        CHECK(makePasteCommand(pp, clip, 30.0) == nullptr);                                   // locked track: nothing is pasted onto it
        pp.tracks[2].locked = false;
        Clip t; t.kind = KindText; t.track = 2; t.dur = 3; t.text.text = "hi";                // text and layers paste too
        CHECK(makePasteCommand(pp, {t}, 40.0) != nullptr);
        Clip ac; ac.assetId = pp.assets.firstKey(); ac.track = 3; ac.dur = 2;                 // a video file can't land on an audio track
        CHECK(makePasteCommand(pp, {ac}, 40.0) == nullptr);
    }

    std::printf("[text: rendering, bounds, through the compositor]\n");
    {
        TextStyle ts; ts.text = "Hello"; ts.size = 100;
        const QSizeF b1 = textBounds(ts);
        ts.size = 200; const QSizeF b2 = textBounds(ts);
        CHECK(b2.width() > b1.width() * 1.7 && b2.height() > b1.height() * 1.7);
        ts.size = 100; ts.text = "Hello\nWorld, again"; const QSizeF b3 = textBounds(ts);
        CHECK(b3.height() > b1.height() * 1.8 && b3.width() > b1.width());                    // two lines are taller, the longer line is wider
        ts.text = "Hello"; ts.strokeW = 10; const QSizeF b4 = textBounds(ts);
        CHECK(near(b4.width(), b1.width() + 20, 0.5) && near(b4.height(), b1.height() + 20, 0.5));
        ts.strokeW = 0;
        const QSize canvas(640, 360);
        auto lit = [&](const QImage& im, QRect* box) { int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; long n = 0; for (int y = 0; y < im.height(); ++y) for (int x = 0; x < im.width(); ++x) if (qAlpha(im.pixel(x, y)) > 128) { ++n; x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); } if (box) *box = QRect(QPoint(x0, y0), QPoint(x1, y1)); return n; };
        ts.size = 60;
        QImage im = renderText(ts, canvas, canvas); QRect bx;
        const long n0 = lit(im, &bx);
        CHECK(im.size() == canvas && n0 > 200 && std::abs(bx.center().x() - 320) < 6 && std::abs(bx.center().y() - 180) < 12);          // centred on the canvas
        CHECK(qAlpha(im.pixel(2, 2)) == 0);
        { TextStyle u = ts; u.underline = true; CHECK(lit(renderText(u, canvas, canvas), nullptr) > n0); }                                // underline adds ink
        { TextStyle so = ts; so.strokeW = 6; so.sr = 1; so.sg = 0; so.sb = 0; QImage si = renderText(so, canvas, canvas);
          long reds = 0; for (int y = 0; y < si.height(); ++y) for (int x = 0; x < si.width(); ++x) { QColor c(si.pixel(x, y)); if (c.alpha() > 200 && c.red() > 200 && c.green() < 60) ++reds; }
          CHECK(reds > 100 && lit(si, nullptr) > n0); }                                                                                    // red outline around the white letters
        { QImage half = renderText(ts, canvas, QSize(320, 180)); QRect hb; lit(half, &hb); CHECK(half.size() == QSize(320, 180) && std::abs(hb.width() * 2 - bx.width()) < 6); }   // preview resolution keeps the proportions
        Project xp; QUndoStack xs;
        xs.push(makeAddTextCommand(xp, 1, 0, ts));
        CHECK(xp.clips.size() == 1 && xp.clips[0].kind == KindText && near(xp.clips[0].dur, 5.0, 1e-9));
        QVector<ActiveLayer> al = xp.videoLayersAt(1.0);
        CHECK(al.size() == 1 && al[0].isText && al[0].text.text == "Hello" && al[0].path.isEmpty());
        CHECK(xp.save(dir.filePath("txt.vproj"), &err));
        Project xq; CHECK(xq.load(dir.filePath("txt.vproj"), &err) && xq.clips.size() == 1 && xq.clips[0] == xp.clips[0]);
        xs.push(makeApplyEffectCommand(xp, {xp.clips[0].id}, makeEffect("shadow")));           // effects can be put on text
        CHECK(xp.clips[0].effects.size() == 1 && xp.videoLayersAt(1.0)[0].passes.size() == 1);
        if (Exporter::gpuAvailable()) {   // text goes through the same GPU pipeline: moves, scales and rotates like a picture
            QOffscreenSurface surf; surf.setFormat(QSurfaceFormat::defaultFormat()); surf.create();
            QOpenGLContext gctx; gctx.setFormat(QSurfaceFormat::defaultFormat()); gctx.create(); gctx.makeCurrent(&surf);
            {
                Compositor comp; comp.init(); QOpenGLFramebufferObject fbo(canvas);
                auto shot = [&](LayerFx fx) { RenderLayer l; l.image = renderText(ts, canvas, canvas); l.fx = fx; comp.render({l}, canvas, &fbo); return fbo.toImage(); };
                auto bright = [&](const QImage& i2, QRect* box) { int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; for (int y = 0; y < i2.height(); ++y) for (int x = 0; x < i2.width(); ++x) if (qRed(i2.pixel(x, y)) > 128) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); } *box = QRect(QPoint(x0, y0), QPoint(x1, y1)); };   // opaque black backdrop: look for the white text
                QRect base, moved, big, rot;
                bright(shot(LayerFx()), &base);
                LayerFx m; m.dx = 100; m.dy = -40; bright(shot(m), &moved);
                LayerFx sc; sc.scale = 2.0; bright(shot(sc), &big);
                LayerFx ro; ro.rotation = 90; bright(shot(ro), &rot);
                CHECK(std::abs(moved.center().x() - (base.center().x() + 100)) < 4 && std::abs(moved.center().y() - (base.center().y() - 40)) < 4);
                CHECK(std::abs(big.width() - base.width() * 2) < 8 && std::abs(big.height() - base.height() * 2) < 8);
                CHECK(std::abs(rot.width() - base.height()) < 8 && std::abs(rot.height() - base.width()) < 8);                          // 90° swaps width and height
                // text wider than the canvas must not be cut off, even when it is made smaller afterwards (the reported bug)
                TextStyle lt; lt.text = "OTHER VS THIS ONE"; lt.size = 100;
                const QSizeF lb = textBounds(lt);
                double ex = 1; const QImage li = renderText(lt, canvas, canvas, &ex);
                CHECK(lb.width() > canvas.width() * 1.3 && ex > 1.3 && li.size().width() > canvas.width());       // it really is wider than the canvas
                RenderLayer wl; wl.image = li; wl.fx.scale = 0.5 * ex;                                            // the user shrinks it to 50 %
                comp.render({wl}, canvas, &fbo); QRect wb; bright(fbo.toImage(), &wb);
                std::printf("        wide text at 50%%: %d px wide on screen (expected about %.0f), left edge %d, right edge %d\n", wb.width(), lb.width() * 0.5, wb.left(), wb.right());
                CHECK(std::abs(wb.width() - lb.width() * 0.5) < 12 && wb.left() > 4 && wb.right() < canvas.width() - 5);   // the whole line is there, nothing clipped at either side
            }
            gctx.doneCurrent();
        }
    }

    std::printf("[timeline overview: waveform + thumbnails]\n");
    {
        MediaAsset sa; QString e5; probeMedia(src, &sa, &e5);
        const AssetOverview ov = buildOverview(sa);
        std::printf("        %d thumbnails (%.2f s apart), %d waveform slices\n", int(ov.thumbs.size()), ov.thumbStep, int(ov.peaks.size()));
        CHECK(ov.thumbs.size() == 6 && ov.thumbs[0].height() == 54 && !ov.thumbs[3].isNull());
        CHECK(std::abs(int(ov.peaks.size()) - 300) <= 3);                                       // 6 s at 20 ms per slice
        long sum = 0; int lo = 255; for (int i = 10; i < 290; ++i) { sum += ov.peaks[i]; lo = std::min<int>(lo, ov.peaks[i]); }
        CHECK(sum / 280 > 40 && lo > 10);                                                      // a steady tone is visible all the way through
        MediaAsset au; probeMedia(dir.filePath("t.mp3"), &au, &e5);
        const AssetOverview oa = buildOverview(au);
        CHECK(oa.thumbs.isEmpty() && oa.peaks.size() > 90);
        MediaAsset pic; probeMedia(dir.filePath("t.png"), &pic, &e5);
        const AssetOverview op = buildOverview(pic);
        CHECK(op.thumbs.size() == 1 && op.peaks.isEmpty());
        QProcess::execute("ffmpeg", {"-y", "-loglevel", "error", "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-t", "2", dir.filePath("quiet.wav")});
        MediaAsset qa; probeMedia(dir.filePath("quiet.wav"), &qa, &e5);
        const AssetOverview oq = buildOverview(qa); int mx = 0; for (quint8 v : oq.peaks) mx = std::max<int>(mx, v);
        CHECK(!oq.peaks.isEmpty() && mx < 4);                                                  // silence stays a flat line
    }

    std::printf("[automatic proxies]\n");
    {
        QProcess::execute("ffmpeg", {"-y", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=d=2:s=2560x1440:r=15", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", dir.filePath("big.mp4")});
        MediaAsset big, small; QString e6;
        CHECK(probeMedia(dir.filePath("big.mp4"), &big, &e6) && probeMedia(src, &small, &e6));
        big.id = newId();
        CHECK(ProxyManager::worthProxying(big) && !ProxyManager::worthProxying(small));        // only genuinely heavy video gets one
        QTemporaryDir pdir; ProxyManager pm(pdir.path());
        CHECK(pm.existing(big).isEmpty() && !pm.request(small));
        bool ready = false; QString ppath; QEventLoop pl;
        QObject::connect(&pm, &ProxyManager::ready, &pm, [&](const QString&, const QString& path) { ready = true; ppath = path; pl.quit(); });
        QObject::connect(&pm, &ProxyManager::failed, &pm, [&](const QString&, const QString& why) { std::printf("        proxy failed: %s\n", qPrintable(why)); pl.quit(); });
        QTimer::singleShot(90000, &pl, &QEventLoop::quit);
        CHECK(pm.request(big) && pm.busy(big) && !pm.request(big));                            // a second request while it is being made does nothing
        pl.exec();
        CHECK(ready && QFileInfo::exists(ppath) && pm.existing(big) == ppath);
        MediaAsset pa; probeMedia(ppath, &pa, &e6);
        std::printf("        proxy: %d x %d, %.2f s (original %d x %d, %.2f s)\n", pa.width, pa.height, pa.duration, big.width, big.height, big.duration);
        CHECK(pa.height == 720 && pa.width == 1280 && near(pa.duration, big.duration, 0.2) && !pa.hasAudio);
        CHECK(!pm.request(big));                                                                // already there: reused, not rebuilt
        const double diff = meanAbsDiff(frameOf(ppath, 1.0), frameOf(dir.filePath("big.mp4"), 1.0));   // same picture, just smaller
        std::printf("        proxy vs original frame: mean diff %.2f\n", diff);
        CHECK(diff < 14);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
