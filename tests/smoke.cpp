#include "ui/MainWindow.h"
#include "ui/PreviewWidget.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QLabel>
#include <QListWidget>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QEventLoop>
#include <QMimeData>
#include <QMouseEvent>
#include <QProcess>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
using namespace ve;
struct Peek : MainWindow { using MainWindow::dropEvent; };   // lets the test call the protected handler directly
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL  %s (line %d)\n", #c, __LINE__); ++fails; } else std::printf("  ok    %s\n", #c); } while (0)
static void wait(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }
static void mouse(QWidget* w, QEvent::Type t, QPointF p) {
    QMouseEvent e(t, p, w->mapToGlobal(p), Qt::LeftButton, t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}
static void click(QWidget* w, QPointF p) { mouse(w, QEvent::MouseButtonPress, p); mouse(w, QEvent::MouseButtonRelease, p); }
static void drag(QWidget* w, QPointF a, QPointF b) { mouse(w, QEvent::MouseButtonPress, a); mouse(w, QEvent::MouseMove, (a + b) / 2); mouse(w, QEvent::MouseMove, b); mouse(w, QEvent::MouseButtonRelease, b); }
int main(int argc, char** argv) {
    { QSurfaceFormat f; f.setVersion(3, 3); f.setProfile(QSurfaceFormat::CoreProfile); QSurfaceFormat::setDefaultFormat(f); }
    setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", dir.filePath("xdg").toUtf8());
    // a folder tree like a user would have: Shoot/{clips/a.mp4, clips/b.mp4, music/song.mp3, loop.gif}
    QDir().mkpath(dir.filePath("Shoot/clips")); QDir().mkpath(dir.filePath("Shoot/music"));
    auto ff = [&](QStringList a, const QString& out) { QStringList all = {"-y", "-loglevel", "error"}; all += a; all << dir.filePath(out); QProcess::execute("ffmpeg", all); };
    ff({"-f","lavfi","-i","testsrc2=d=6:s=1280x720:rate=30","-f","lavfi","-i","sine=d=6","-c:v","libx264","-pix_fmt","yuv420p","-c:a","aac"}, "Shoot/clips/a.mp4");
    ff({"-f","lavfi","-i","testsrc=d=4:s=640x360:rate=30","-c:v","libx264","-pix_fmt","yuv420p"}, "Shoot/clips/b.mp4");
    ff({"-f","lavfi","-i","sine=d=5:f=330","-c:a","libmp3lame"}, "Shoot/music/song.mp3");
    ff({"-f","lavfi","-i","testsrc=d=2:s=160x90:r=10"}, "Shoot/loop.gif");

    MainWindow w(true);
    w.resize(1440, 880); w.show(); wait(400);
    auto* tl = w.findChild<QWidget*>(QString(), Qt::FindChildrenRecursively);   // placeholder
    for (auto* c : w.findChildren<QWidget*>()) if (QString(c->metaObject()->className()) == "ve::TimelineWidget") tl = c;
    auto* tree = w.findChild<QTreeWidget*>();

    std::printf("[import a whole folder by dropping it on the window]\n");
    { QMimeData md; md.setUrls({QUrl::fromLocalFile(dir.filePath("Shoot"))});
      QDropEvent ev(QPointF(300, 300), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier); static_cast<Peek&>(w).dropEvent(&ev); }
    wait(2500);
    std::printf("        folders=%d assets=%d dirExists=%d\n", int(w.project().folders.size()), int(w.project().assets.size()), int(QFileInfo(dir.filePath("Shoot")).isDir()));
    CHECK(w.project().folders.size() == 3);                  // Shoot, clips, music
    CHECK(w.project().assets.size() == 4);                   // a.mp4 b.mp4 song.mp3 loop.gif
    QString idA, idB, idSong, idGif;
    for (const MediaAsset& a : w.project().assets) { if (a.name() == "a.mp4") idA = a.id; if (a.name() == "b.mp4") idB = a.id; if (a.name() == "song.mp3") idSong = a.id; if (a.name() == "loop.gif") idGif = a.id; }
    bool nested = false; for (const Folder& f : w.project().folders) if (f.name == "clips") for (const Folder& p : w.project().folders) nested = nested || (p.id == f.parentId && p.name == "Shoot");
    CHECK(nested && w.project().asset(idA)->folderId == w.project().asset(idB)->folderId && w.project().asset(idSong)->folderId != w.project().asset(idA)->folderId);

    std::printf("[add clips (double-click in the media panel)]\n");
    std::function<void(QTreeWidgetItem*)> dbl = [&](QTreeWidgetItem* it) {
        for (int i = 0; i < it->childCount(); ++i) dbl(it->child(i));
        const QString id = it->data(0, Qt::UserRole).toString();
        if (it->data(0, Qt::UserRole + 1).toString() == "asset" && (id == idA || id == idSong)) { emit tree->itemDoubleClicked(it, 0); }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) dbl(tree->topLevelItem(i));
    wait(300);
    CHECK(w.project().clips.size() == 2);
    w.seekTo(7.0);
    { std::function<void(QTreeWidgetItem*)> f2 = [&](QTreeWidgetItem* it) { for (int i = 0; i < it->childCount(); ++i) f2(it->child(i)); if (it->data(0, Qt::UserRole).toString() == idB) emit tree->itemDoubleClicked(it, 0); };
      for (int i = 0; i < tree->topLevelItemCount(); ++i) f2(tree->topLevelItem(i)); }
    wait(300);
    CHECK(w.project().clips.size() == 3);

    std::printf("[track buttons: hide / lock / mute]\n");
    const double rowY = 26 + 2 * 48 + 23;                      // V1 (third row: V3, V2, V1, A1, A2)
    click(tl, QPointF(51, rowY));  CHECK(w.project().tracks[2].hidden);
    click(tl, QPointF(51, rowY));  CHECK(!w.project().tracks[2].hidden);
    click(tl, QPointF(103, rowY)); CHECK(w.project().tracks[2].muted);
    click(tl, QPointF(103, rowY)); CHECK(!w.project().tracks[2].muted);
    click(tl, QPointF(77, rowY));  CHECK(w.project().tracks[2].locked);
    w.seekTo(1.0); wait(200);
    click(tl, QPointF(400, rowY)); wait(100);                                  // clicking a locked clip must not select it
    for (auto* a : w.actions()) if (a->text() == "Delete") a->trigger();
    CHECK(w.project().clips.size() == 3);                                       // …and Delete does nothing to it
    click(tl, QPointF(77, rowY));  CHECK(!w.project().tracks[2].locked);

    std::printf("[drag-box selection]\n");
    auto labelHas = [&](const QString& needle) { for (auto* l : w.findChildren<QLabel*>()) if (l->isVisible() && l->text().contains(needle)) return true; return false; };
    wait(100);
    w.seekTo(0.0);
    drag(tl, QPointF(1000, 40), QPointF(200, 150)); wait(150);                  // starts on the empty V2 lane, sweeps across both V1 clips
    CHECK(labelHas("2 clips selected"));
    w.grab().save("/tmp/ui_band.png");
    click(tl, QPointF(300, rowY)); wait(150);                                   // plain click on one clip of the group → just that clip
    CHECK(labelHas("a.mp4") && !labelHas("clips selected"));
    drag(tl, QPointF(650, 40), QPointF(250, 150)); wait(150);                   // a smaller box that only touches clip a
    CHECK(labelHas("a.mp4") && !labelHas("clips selected"));
    {   // Ctrl+A selects everything
        for (auto* a : w.actions()) if (a->text() == "Select all") a->trigger();
        wait(150); CHECK(labelHas("3 clips selected"));
    }

    std::printf("[select one clip, edit it, keyframe it]\n");
    click(tl, QPointF(400, rowY)); wait(150);                                   // select a.mp4 on V1
    auto spins = w.findChildren<QDoubleSpinBox*>();
    std::vector<QAbstractButton*> diamonds;
    for (auto* b : w.findChildren<QAbstractButton*>()) if (b->width() == 24 && b->height() == 24) diamonds.push_back(b);
    CHECK(spins.size() >= 8 && diamonds.size() >= 6);                           // x y size rotation opacity volume (+ speed, fades)
    for (auto* d : diamonds) std::printf("        diamond visible=%d enabled=%d\n", int(d->isVisible()), int(d->isEnabled()));
    const QString idClip = w.project().clips[0].track == 2 ? w.project().clips[0].id : w.project().clips[1].id;
    // opacity row: find the spin whose suffix is " %" and range 0..100 that is the 5th property row → use diamond index 4
    w.seekTo(0.0); wait(100);
    diamonds[4]->click(); wait(100);                                            // key on opacity at t=0
    { QStringList k; for (auto it = w.project().clip(idClip)->keys.begin(); it != w.project().clip(idClip)->keys.end(); ++it) k << it.key(); std::printf("        keys now: [%s]  diamonds=%d enabled4=%d\n", qPrintable(k.join(",")), int(diamonds.size()), int(diamonds[4]->isEnabled())); }
    CHECK(w.project().clip(idClip)->keys.contains("opacity"));
    // set opacity to 0 at t=0 via the spin next to that diamond
    auto spinFor = [&](QAbstractButton* d) { QDoubleSpinBox* best = nullptr; for (auto* s : spins) if (s->geometry().center().y() > 0 && s->parentWidget() == d->parentWidget()) best = s; return best; };
    QDoubleSpinBox* opac = spinFor(diamonds[4]);
    opac->setValue(0); wait(100);
    w.seekTo(2.0); wait(100);
    opac->setValue(100); wait(100);                                             // auto-key: editing an animated property creates a key at the playhead
    const Clip* c = w.project().clip(idClip);
    CHECK(c->keys["opacity"].size() == 2);
    CHECK(std::abs(propValue(*c, "opacity", 1.0) - 0.5) < 0.02 && std::abs(propValue(*c, "opacity", 0.0)) < 0.01);
    w.seekTo(1.0); wait(500);
    w.grab().save("/tmp/ui_keys.png");

    std::printf("[drag a keyframe diamond in the timeline]\n");
    {
        const double clipBottom = 26 + 2 * 48 + 2 + 42;                                     // bottom edge of a V1 clip
        drag(tl, QPointF(120 + 2.0 * 80, clipBottom - 6), QPointF(120 + 3.0 * 80, clipBottom - 6)); wait(200);   // grab the key at 2 s, pull it to 3 s
        const Clip* kc = w.project().clip(idClip);
        const auto ks = kc->keys["opacity"];
        CHECK(ks.size() == 2 && std::abs(ks[0].t) < 1e-6 && std::abs(ks[1].t - 3.0) < 0.04);   // the key moved, the other one stayed
        CHECK(std::abs(kc->start) < 1e-9);                                                       // …and the clip itself did not move
        CHECK(std::abs(propValue(*kc, "opacity", 1.5) - 0.5) < 0.05);                            // the fade now takes 3 s instead of 2 s
        for (auto* a : w.actions()) if (a->text().startsWith("Undo")) { a->trigger(); break; }
        wait(100);
        CHECK(std::abs(w.project().clip(idClip)->keys["opacity"][1].t - 2.0) < 1e-6);            // one undo puts it back
    }

    std::printf("[speed from the inspector]\n");
    const double durBefore = w.project().clip(idClip)->dur;
    QDoubleSpinBox* speed = nullptr; for (auto* s : spins) if (s->suffix() == " ×") speed = s;
    CHECK(speed != nullptr);
    if (speed) { speed->setValue(2.0); wait(150); }
    CHECK(std::abs(w.project().clip(idClip)->dur - durBefore / 2) < 0.05);

    std::printf("[effect layer, mask, clip effect]\n");
    auto presetItem = [&](const QString& text) -> QListWidgetItem* {
        for (auto* lw : w.findChildren<QListWidget*>()) for (int i = 0; i < lw->count(); ++i) if (lw->item(i)->text().trimmed() == text) return lw->item(i);
        return nullptr;
    };
    QListWidgetItem* blurLayer = presetItem("Blur layer");
    CHECK(blurLayer != nullptr);
    size_t before = w.project().clips.size();
    w.seekTo(1.0); wait(100);
    if (blurLayer) { auto* lw = blurLayer->listWidget(); emit lw->itemDoubleClicked(blurLayer); }
    wait(250);
    CHECK(w.project().clips.size() == before + 1);
    QString layerId; for (const Clip& c : w.project().clips) if (c.kind == KindEffectLayer) layerId = c.id;
    CHECK(!layerId.isEmpty() && w.project().clip(layerId)->track == 0);                       // landed on the top track
    click(tl, QPointF(120 + 2.5 * 80, 26 + 23)); wait(200);                                   // select it (top row)
    CHECK(labelHas("Effect layer") && labelHas("Where it applies"));
    QCheckBox* maskBox = nullptr; for (auto* cb : w.findChildren<QCheckBox*>()) if (cb->isVisible() && cb->text().startsWith("Enable rectangle")) maskBox = cb;
    CHECK(maskBox != nullptr);
    if (maskBox) { maskBox->setChecked(true); wait(250); }
    CHECK(w.project().clip(layerId)->mask.on && w.project().clip(layerId)->mask.w > 0);
    w.seekTo(2.0); wait(700);
    w.grab().save("/tmp/ui_layer.png");
    // a clip effect from the same list: select a V1 clip and double-click "Border"
    click(tl, QPointF(120 + 0.5 * 80, rowY)); wait(200);
    if (QListWidgetItem* br = presetItem("Border")) { emit br->listWidget()->itemDoubleClicked(br); wait(250); }
    bool hasBorder = false; for (const Clip& c : w.project().clips) for (const Effect& e : c.effects) hasBorder = hasBorder || e.type == "border";
    CHECK(hasBorder);
    CHECK(labelHas("Border"));

    std::printf("[direct manipulation in the preview]\n");
    {
        auto* ov = w.previewOverlay();
        CHECK(ov != nullptr);
        auto clipNow = [&]() { return w.project().clip(idClip); };
        auto undo = [&]() { for (auto* a : w.actions()) if (a->text().startsWith("Undo")) { a->trigger(); break; } wait(120); };
        auto visibleSpinPx = [&]() { for (auto* sp : w.findChildren<QDoubleSpinBox*>()) if (sp->isVisible() && sp->suffix() == " px") return sp; return static_cast<QDoubleSpinBox*>(nullptr); };
        click(tl, QPointF(120 + 0.5 * 80, rowY)); wait(200);
        w.seekTo(0.5); wait(300);
        CHECK(ov->box().on && std::abs(clipNow()->x) < 1e-6 && std::abs(clipNow()->scale - 1.0) < 1e-6 && std::abs(clipNow()->rotation) < 1e-6);
        const double k = ov->canvasRect().width() / 1920.0;

        // move: drag inside the box
        QPointF c0 = ov->widgetPoint(ov->box().x, ov->box().y);
        drag(ov, c0, c0 + QPointF(60, 30)); wait(250);
        CHECK(std::abs(clipNow()->x - 60 / k) < 2.0 && std::abs(clipNow()->y - 30 / k) < 2.0);
        CHECK(std::abs(ov->box().x - clipNow()->x) < 1e-6);                                      // the box follows the object
        QDoubleSpinBox* px = visibleSpinPx();
        CHECK(px != nullptr && std::abs(px->value() - clipNow()->x) < 0.6);                      // the inspector shows the new Position X
        undo();
        CHECK(std::abs(clipNow()->x) < 1e-6 && std::abs(clipNow()->y) < 1e-6);                   // the whole drag was ONE undo step

        // inspector → preview: typing a value moves the box
        if (px = visibleSpinPx()) { px->setValue(100); wait(250); }
        CHECK(std::abs(ov->box().x - 100) < 1e-6);
        undo();

        // resize: drag the top-right corner outwards by half the distance from the centre
        QPointF cen = ov->widgetPoint(ov->box().x, ov->box().y), hnd = ov->handlePoint(1);
        drag(ov, hnd, cen + (hnd - cen) * 1.5); wait(250);
        CHECK(std::abs(clipNow()->scale - 1.5) < 0.03 && std::abs(ov->box().w - 1920 * 1.5) < 60);
        undo();
        CHECK(std::abs(clipNow()->scale - 1.0) < 1e-6);

        // rotate: drag the round handle 30° round the centre
        cen = ov->widgetPoint(ov->box().x, ov->box().y);
        QPointF rh = ov->handlePoint(8), v = rh - cen;
        const double a30 = 30.0 * M_PI / 180.0;
        drag(ov, rh, cen + QPointF(v.x() * std::cos(a30) - v.y() * std::sin(a30), v.x() * std::sin(a30) + v.y() * std::cos(a30))); wait(250);
        CHECK(std::abs(clipNow()->rotation - 30.0) < 2.0 && std::abs(ov->box().rot - clipNow()->rotation) < 1e-6);
        undo();
        CHECK(std::abs(clipNow()->rotation) < 1e-6);

        // keyframes: with Position X animated, a drag creates keys at the playhead and leaves the base value alone
        w.seekTo(0.2); wait(250);
        std::vector<QAbstractButton*> dia;
        for (auto* b : w.findChildren<QAbstractButton*>()) if (b->isVisible() && b->width() == 24 && b->height() == 24) dia.push_back(b);
        std::sort(dia.begin(), dia.end(), [&](QAbstractButton* a, QAbstractButton* b) { return a->mapTo(&w, QPoint(0, 0)).y() < b->mapTo(&w, QPoint(0, 0)).y(); });
        CHECK(!dia.empty());
        if (!dia.empty()) { dia[0]->click(); wait(200); }                                         // ◇ next to Position X
        CHECK(clipNow()->keys.contains("x"));
        c0 = ov->widgetPoint(ov->box().x, ov->box().y);
        drag(ov, c0, c0 + QPointF(50, 0)); wait(250);
        CHECK(clipNow()->keys["x"].size() == 1 && std::abs(clipNow()->keys["x"][0].v - 50 / k) < 3.0 && std::abs(clipNow()->x) < 1e-6);
        w.seekTo(1.0); wait(250);
        c0 = ov->widgetPoint(ov->box().x, ov->box().y);
        drag(ov, c0, c0 + QPointF(50, 0)); wait(250);
        CHECK(clipNow()->keys["x"].size() == 2 && std::abs(clipNow()->keys["x"].back().v - 100 / k) < 4.0 && std::abs(clipNow()->x) < 1e-6);   // a second key; the base is untouched

        // clicking a picture in the preview selects its clip; clicking an empty lane deselects
        click(tl, QPointF(900, 26 + 3 * 48 + 23)); wait(200);
        CHECK(!ov->box().on);
        click(ov, ov->widgetPoint(0, 0)); wait(250);
        CHECK(ov->box().on);
        w.grab().save("/tmp/ui_direct.png");
    }

    std::printf("[Batch 3: tracks, copy / paste, text]\n");
    {
        auto act = [&](const QString& text) { for (auto* a : w.actions()) if (a->text() == text || (text == "Undo" && a->text().startsWith("Undo"))) { a->trigger(); wait(150); return true; } return false; };
        // tracks
        const int nt = int(w.project().tracks.size());
        act("Add video track");
        CHECK(int(w.project().tracks.size()) == nt + 1 && w.project().tracks[0].kind == TrackKind::Video && w.project().tracks[0].name == "V4");
        act("Add audio track");
        CHECK(int(w.project().tracks.size()) == nt + 2 && w.project().tracks.last().kind == TrackKind::Audio);
        act("Undo"); act("Undo");
        CHECK(int(w.project().tracks.size()) == nt);

        // copy / paste / duplicate: select the V1 clip with the border, copy, paste at 30 s
        const size_t n0 = w.project().clips.size();
        click(tl, QPointF(120 + 0.5 * 80, rowY)); wait(200);
        act("Copy");
        w.seekTo(30.0); wait(150);
        act("Paste");
        CHECK(w.project().clips.size() == n0 + 1);
        Clip pasted; for (const Clip& c : w.project().clips) if (c.start > 29.9 && c.start < 30.1) pasted = c;
        CHECK(!pasted.id.isEmpty() && pasted.track == 2 && pasted.id != idClip && !pasted.effects.isEmpty() && std::abs(pasted.dur - 3.0) < 0.05);   // same track, effects and length come along
        act("Duplicate");                                                                      // the pasted clip is selected: duplicate lands right after it
        CHECK(w.project().clips.size() == n0 + 2);
        act("Undo"); act("Undo");
        CHECK(w.project().clips.size() == n0);

        // text: add, type, restyle
        w.seekTo(1.0); wait(150);
        act("Add text"); wait(300);
        QString tid; for (const Clip& c : w.project().clips) if (c.kind == KindText) tid = c.id;
        CHECK(!tid.isEmpty() && std::abs(w.project().clip(tid)->dur - 5.0) < 1e-6 && w.project().clip(tid)->text.text == "Text");
        auto* ov = w.previewOverlay();
        CHECK(ov->box().on && ov->box().w > 100);                                              // the new text is selected and has a frame on the preview
        QPlainTextEdit* te = nullptr; for (auto* p : w.findChildren<QPlainTextEdit*>()) if (p->isVisible()) te = p;
        CHECK(te != nullptr);
        if (te) { te->setPlainText("Hello\nCapCut-style"); wait(250); }
        CHECK(w.project().clip(tid)->text.text == "Hello\nCapCut-style");
        const double boxW0 = ov->box().w, boxH0 = ov->box().h;
        std::vector<QDoubleSpinBox*> pxSpins;
        for (auto* sp : w.findChildren<QDoubleSpinBox*>()) if (sp->isVisible() && sp->suffix() == " px") pxSpins.push_back(sp);
        CHECK(pxSpins.size() >= 3);
        if (pxSpins.size() >= 3) {
            pxSpins[0]->setValue(200); wait(250);                                              // Font size
            CHECK(std::abs(w.project().clip(tid)->text.size - 200) < 1e-6 && ov->box().w > boxW0 * 1.4 && ov->box().h > boxH0 * 1.4);   // the frame follows the text
            pxSpins[1]->setValue(8); wait(250);                                                // Outline width
            CHECK(std::abs(w.project().clip(tid)->text.strokeW - 8) < 1e-6);
        }
        for (auto* cb : w.findChildren<QCheckBox*>()) if (cb->isVisible() && cb->text() == "Bold") { cb->setChecked(true); wait(200); }
        for (auto* cb : w.findChildren<QCheckBox*>()) if (cb->isVisible() && cb->text() == "Underline") { cb->setChecked(true); wait(200); }
        CHECK(w.project().clip(tid)->text.bold && w.project().clip(tid)->text.underline);
        // drag the text in the preview like any picture
        const double k = ov->canvasRect().width() / 1920.0;
        QPointF c0 = ov->widgetPoint(ov->box().x, ov->box().y);
        drag(ov, c0, c0 + QPointF(40, 20)); wait(250);
        CHECK(std::abs(w.project().clip(tid)->x - 40 / k) < 3.0 && std::abs(w.project().clip(tid)->y - 20 / k) < 3.0);
        // an effect on text
        if (QListWidgetItem* bi = presetItem("Border")) { emit bi->listWidget()->itemDoubleClicked(bi); wait(250); }
        bool textFx = false; for (const Effect& e : w.project().clip(tid)->effects) textFx = textFx || e.type == "border";
        CHECK(textFx);
        w.seekTo(1.5); wait(700);
        w.grab().save("/tmp/ui_text.png");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "UI SMOKE PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
