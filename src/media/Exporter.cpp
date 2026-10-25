#include "Exporter.h"
#include "AudioMixer.h"
#include "Compositor.h"
#include "Decoder.h"
#include "core/TextRender.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QProcess>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
}

namespace ve {

static QString num(double v) { return QString::number(v, 'f', 6); }

QString Exporter::findFfmpeg() {
#ifdef Q_OS_WIN
    const QString name = "ffmpeg.exe";
#else
    const QString name = "ffmpeg";
#endif
    const QString local = QCoreApplication::applicationDirPath() + "/" + name;
    if (QFileInfo::exists(local)) return local;
    return QStandardPaths::findExecutable("ffmpeg");
}

QVector<EncoderChoice> Exporter::availableEncoders() {
    QVector<EncoderChoice> out;
    const QString ff = findFfmpeg();
    if (ff.isEmpty()) return out;
    QProcess p;
    p.start(ff, {"-hide_banner", "-encoders"});
    if (!p.waitForFinished(8000)) return out;
    const QString txt = QString::fromUtf8(p.readAllStandardOutput());
    auto has = [&](const char* e) { return txt.contains(QString(" %1 ").arg(e)); };
    struct Row { const char* enc; const char* label; };
    const Row rows[] = {
        {"libx264", "H.264 (CPU)"},          {"libx265", "H.265 / HEVC (CPU)"},
        {"h264_nvenc", "H.264 (NVIDIA GPU)"}, {"hevc_nvenc", "H.265 (NVIDIA GPU)"},
        {"h264_amf", "H.264 (AMD GPU)"},      {"hevc_amf", "H.265 (AMD GPU)"},
        {"h264_qsv", "H.264 (Intel GPU)"},    {"hevc_qsv", "H.265 (Intel GPU)"},
    };
    for (const Row& r : rows)
        if (has(r.enc)) out.push_back({r.label, r.enc});
    return out;
}

bool Exporter::gpuAvailable() {
    if (!qEnvironmentVariableIsEmpty("VE_NO_GPU_EXPORT")) return false;
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) return false;
    QOffscreenSurface s;
    s.setFormat(QSurfaceFormat::defaultFormat());
    s.create();
    QOpenGLContext c;
    c.setFormat(QSurfaceFormat::defaultFormat());
    if (!c.create() || !s.isValid() || !c.makeCurrent(&s)) return false;
    const bool ok = c.format().version() >= qMakePair(3, 3);
    c.doneCurrent();
    return ok;
}

static QStringList encoderArgs(const QString& enc, int q) {
    const QString Q = QString::number(q);
    if (enc == "libx264") return {"-c:v", enc, "-preset", "medium", "-crf", Q, "-pix_fmt", "yuv420p"};
    if (enc == "libx265") return {"-c:v", enc, "-preset", "medium", "-crf", Q, "-pix_fmt", "yuv420p", "-tag:v", "hvc1"};
    if (enc.endsWith("_nvenc")) return {"-c:v", enc, "-preset", "p5", "-rc", "vbr", "-cq", Q, "-b:v", "0", "-pix_fmt", "yuv420p"};
    if (enc.endsWith("_amf")) return {"-c:v", enc, "-quality", "quality", "-rc", "cqp", "-qp_i", Q, "-qp_p", Q, "-pix_fmt", "yuv420p"};
    if (enc.endsWith("_qsv")) return {"-c:v", enc, "-global_quality", Q, "-pix_fmt", "nv12"};
    return {"-c:v", enc};
}

// Output side shared by both paths. `-t` hard-caps the file at the timeline length and `-r` pins the frame rate,
// so the container can never end up longer than the project.
static void appendOutputArgs(QStringList& a, const ProjectData& p, const ExportOptions& o) {
    a << "-r" << num(p.settings.fps);
    a += encoderArgs(o.encoder, o.quality);
    a << "-c:a" << "aac" << "-b:a" << QString("%1k").arg(o.audioKbps) << "-ar" << "48000" << "-ac" << "2";
    a << "-t" << num(p.duration());
    a << "-max_muxing_queue_size" << "1024";
    if (o.outPath.endsWith(".mp4", Qt::CaseInsensitive) || o.outPath.endsWith(".mov", Qt::CaseInsensitive)) a << "-movflags" << "+faststart";
    a << o.outPath;
}

static QStringList audioInputArgs(const QString& audioPath) {
    return {"-f", "s16le", "-ar", "48000", "-ac", "2", "-i", audioPath};
}

static bool checkMedia(const ProjectData& p, QString* err) {
    if (p.clips.isEmpty() || p.duration() <= 0) { if (err) *err = "The timeline is empty. Add some clips first."; return false; }
    for (const Clip& c : p.clips) {
        if (c.kind != KindMedia) continue;                 // effect / adjustment layers have no file
        const MediaAsset* as = p.asset(c.assetId);
        if (!as) { if (err) *err = "A clip refers to media that is no longer in the project."; return false; }
        if (as->missing || !QFileInfo::exists(as->path)) {
            if (err) *err = "Media is missing: " + as->path + "\nMove the file back to this location and try again.";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Command builders
// ---------------------------------------------------------------------------------------------------------

bool Exporter::buildFilterCommand(const ProjectData& p, const ExportOptions& o, const QString& scriptPath, const QString& audioPath,
                                  QStringList* args, QString* graph, QString* err) {
    if (!checkMedia(p, err)) return false;
    const double D = p.duration();
    const int W = p.settings.width, H = p.settings.height;
    const double fps = p.settings.fps;

    QVector<Clip> order;   // video tracks bottom→top: higher tracks composite over lower ones; hidden tracks are skipped
    for (int tr = p.tracks.size() - 1; tr >= 0; --tr)
        if (p.tracks[tr].kind == TrackKind::Video && !p.tracks[tr].hidden)
            for (const Clip& c : p.clipsOnTrack(tr))
                if (c.kind == KindMedia && p.asset(c.assetId)->hasVideo) order.push_back(c);

    QStringList a = {"-y", "-hide_banner", "-loglevel", "error", "-progress", "pipe:1", "-nostats"};
    QString g;
    QString base = "base0";
    g += QString("color=c=black:s=%1x%2:r=%3:d=%4[%5];\n").arg(W).arg(H).arg(num(fps)).arg(num(D)).arg(base);
    int baseN = 0;
    for (int i = 0; i < order.size(); ++i) {
        const Clip& c = order[i];
        const MediaAsset* as = p.asset(c.assetId);
        if (as->isImage) a << "-loop" << "1" << "-framerate" << num(fps) << "-t" << num(c.dur);
        else if (c.freeze) a << "-ss" << num(c.in) << "-t" << num(std::max(0.5, 2.0 / fps));
        else a << "-ss" << num(c.in) << "-t" << num(c.dur * c.speed);
        a << "-i" << as->path;
        const QString pre = c.freeze
            ? QString("trim=end_frame=1,setpts=PTS-STARTPTS,tpad=stop_mode=clone:stop_duration=%1,trim=duration=%1,").arg(num(c.dur))
            : QString("setpts=(PTS-STARTPTS)/%1,").arg(num(c.speed));
        g += QString("[%1:v]%6fps=%2,scale=%3:%4:force_original_aspect_ratio=decrease,setsar=1,setpts=PTS+%5/TB[v%1];\n")
                 .arg(i).arg(num(fps)).arg(W).arg(H).arg(num(c.start)).arg(pre);
        const QString next = QString("base%1").arg(++baseN);
        g += QString("[%1][v%2]overlay=(main_w-overlay_w)/2:(main_h-overlay_h)/2:eof_action=pass:format=auto[%3];\n").arg(base).arg(i).arg(next);
        base = next;
    }
    g += QString("[%1]format=yuv420p[vout]").arg(base);
    const int audioInput = order.size();
    a += audioInputArgs(audioPath);

    QFile f(scriptPath);   // a script file avoids Windows' command-line length limit
    if (!f.open(QIODevice::WriteOnly)) { if (err) *err = "Couldn't write a temporary file for export."; return false; }
    f.write(g.toUtf8());
    f.close();
    a << "-filter_complex_script" << scriptPath << "-map" << "[vout]" << "-map" << QString("%1:a").arg(audioInput);
    appendOutputArgs(a, p, o);
    *args = a;
    if (graph) *graph = g;
    return true;
}

bool Exporter::buildGpuCommand(const ProjectData& p, const ExportOptions& o, const QString& audioPath, QStringList* args, QString* err) {
    if (!checkMedia(p, err)) return false;
    QStringList a = {"-y", "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pixel_format", "rgba",
                     "-video_size", QString("%1x%2").arg(p.settings.width).arg(p.settings.height), "-framerate", num(p.settings.fps), "-i", "-"};
    a += audioInputArgs(audioPath);
    a << "-vf" << "vflip,format=yuv420p" << "-map" << "0:v" << "-map" << "1:a";   // GL reads rows bottom-up
    appendOutputArgs(a, p, o);
    *args = a;
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Verify / repair
// ---------------------------------------------------------------------------------------------------------


// ---- MP4/MOV header boxes, read the way media players read them (libav recomputes durations and would hide a bad header) ----
namespace {
quint64 be(const QByteArray& b, int off, int n) { quint64 v = 0; for (int i = 0; i < n; ++i) v = (v << 8) | quint8(b[off + i]); return v; }

struct Mp4Times { bool valid = false; double mvhd = 0, maxTrack = 0; };

void walkBoxes(const QByteArray& d, int begin, int end, double* mvTimescale, Mp4Times* t, int depth) {
    int p = begin;
    while (p + 8 <= end) {
        quint64 size = be(d, p, 4);
        const QByteArray type = d.mid(p + 4, 4);
        int hdr = 8;
        if (size == 1 && p + 16 <= end) { size = be(d, p + 8, 8); hdr = 16; }
        if (size == 0) size = quint64(end - p);
        if (size < quint64(hdr) || p + int(size) > end) break;
        const int body = p + hdr, bend = p + int(size);
        if (type == "moov" || type == "trak" || type == "mdia") { if (depth < 4) walkBoxes(d, body, bend, mvTimescale, t, depth + 1); }
        else if (type == "mvhd" && body + 4 <= bend) {
            const int v = quint8(d[body]);
            const quint64 ts = be(d, body + (v ? 20 : 12), 4), du = be(d, body + (v ? 24 : 16), v ? 8 : 4);
            if (ts) { *mvTimescale = double(ts); t->mvhd = double(du) / double(ts); t->valid = true; }
        } else if (type == "tkhd" && body + 4 <= bend && *mvTimescale > 0) {
            const int v = quint8(d[body]);
            const quint64 du = be(d, body + (v ? 28 : 20), v ? 8 : 4);
            t->maxTrack = std::max(t->maxTrack, double(du) / *mvTimescale);
        } else if (type == "mdhd" && body + 4 <= bend) {
            const int v = quint8(d[body]);
            const quint64 ts = be(d, body + (v ? 20 : 12), 4), du = be(d, body + (v ? 24 : 16), v ? 8 : 4);
            if (ts) t->maxTrack = std::max(t->maxTrack, double(du) / double(ts));
        }
        p = bend;
    }
}

Mp4Times readMp4Times(const QString& path) {
    Mp4Times t;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return t;
    qint64 pos = 0;
    while (pos + 8 <= f.size()) {                       // top-level boxes: find moov without loading the media data
        f.seek(pos);
        const QByteArray h = f.read(16);
        if (h.size() < 8) break;
        quint64 size = be(h, 0, 4);
        if (size == 1 && h.size() >= 16) size = be(h, 8, 8);
        if (size == 0) size = quint64(f.size() - pos);
        if (size < 8) break;
        if (h.mid(4, 4) == "moov" && size < (1ull << 30)) {
            f.seek(pos);
            const QByteArray moov = f.read(qint64(size));
            double ts = 0;
            walkBoxes(moov, 0, moov.size(), &ts, &t, 0);
            return t;
        }
        pos += qint64(size);
    }
    return t;
}
} // namespace

DurationCheck Exporter::checkDuration(const QString& path, double expected) {
    DurationCheck r;
    AVFormatContext* fmt = nullptr;
    const QByteArray u = path.toUtf8();
    if (avformat_open_input(&fmt, u.constData(), nullptr, nullptr) < 0) { r.detail = "The exported file can't be opened."; return r; }
    struct Guard { AVFormatContext** f; ~Guard() { avformat_close_input(f); } } guard{&fmt};
    avformat_find_stream_info(fmt, nullptr);
    std::vector<double> end(fmt->nb_streams, 0.0), first(fmt->nb_streams, 1e18);
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt, pkt) >= 0) {
        const AVStream* st = fmt->streams[pkt->stream_index];
        if (pkt->pts != AV_NOPTS_VALUE) {
            const double tb = av_q2d(st->time_base);
            const double t0 = double(pkt->pts) * tb;
            const double t1 = t0 + (pkt->duration > 0 ? double(pkt->duration) * tb : 0.0);
            end[pkt->stream_index] = std::max(end[pkt->stream_index], t1);
            first[pkt->stream_index] = std::min(first[pkt->stream_index], t0);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    r.headerSeconds = fmt->duration > 0 ? double(fmt->duration) / AV_TIME_BASE : 0.0;
    const double tol = 0.3 + expected * 0.01;
    bool streamsOk = true;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const auto type = fmt->streams[i]->codecpar->codec_type;
        if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) continue;
        (type == AVMEDIA_TYPE_VIDEO ? r.videoEnd : r.audioEnd) = end[i];
        if (std::abs(end[i] - expected) > tol || first[i] > 0.5) streamsOk = false;
    }
    bool headerOk = r.headerSeconds > 0 && std::abs(r.headerSeconds - expected) <= tol;
    const Mp4Times mp4 = readMp4Times(path);       // what VLC / Windows / phones actually read
    if (mp4.valid) {
        r.headerSeconds = std::max({r.headerSeconds, mp4.mvhd, mp4.maxTrack});
        if (std::abs(mp4.mvhd - expected) > tol || std::abs(mp4.maxTrack - expected) > tol + 0.5) headerOk = false;
    }
    r.ok = streamsOk && headerOk;
    r.detail = QString("expected %1 s; header says %2 s; video ends at %3 s; audio ends at %4 s")
                   .arg(expected, 0, 'f', 2).arg(r.headerSeconds, 0, 'f', 2).arg(r.videoEnd, 0, 'f', 2).arg(r.audioEnd, 0, 'f', 2);
    return r;
}

bool Exporter::repairByRemux(const QString& ffmpeg, const QString& path, double expected) {
    const QFileInfo fi(path);
    const QString fixed = fi.absolutePath() + "/" + fi.completeBaseName() + ".fixed." + fi.suffix();
    QProcess p;
    p.start(ffmpeg, {"-y", "-hide_banner", "-loglevel", "error", "-i", path, "-map", "0", "-c", "copy", "-t", num(expected), "-movflags", "+faststart", fixed});
    if (!p.waitForFinished(120000) || p.exitCode() != 0 || !QFileInfo::exists(fixed)) { QFile::remove(fixed); return false; }
    QFile::remove(path);
    return QFile::rename(fixed, path);
}

// ---------------------------------------------------------------------------------------------------------
// Job
// ---------------------------------------------------------------------------------------------------------

QString Exporter::runGpu(const ProjectData& data, const QStringList& args, const QString& ffmpeg) {
    const int W = data.settings.width, H = data.settings.height;
    const double fps = data.settings.fps;
    QOpenGLContext ctx;
    ctx.setFormat(QSurfaceFormat::defaultFormat());
    if (!ctx.create() || !ctx.makeCurrent(m_surface.get())) return "Couldn't start the GPU renderer (OpenGL 3.3 is required).";
    QString problem;
    {
        Compositor comp;
        if (!comp.init()) { ctx.doneCurrent(); return "Couldn't compile the GPU shaders."; }
        auto fbo = std::make_unique<QOpenGLFramebufferObject>(QSize(W, H));
        QOpenGLFunctions* gl = ctx.functions();
        QProcess proc;
        proc.start(ffmpeg, args);
        if (!proc.waitForStarted(15000)) problem = "FFmpeg couldn't be started. Make sure ffmpeg is installed.";

        QHash<QString, std::shared_ptr<Decoder>> decoders;
        const int total = std::max(1, int(std::ceil(data.duration() * fps - 1e-9)));
        QByteArray buf(W * H * 4, 0);
        for (int i = 0; i < total && problem.isEmpty(); ++i) {
            if (m_cancel) { problem = "Export cancelled."; break; }
            const double t = i / fps;
            QVector<RenderLayer> layers;
            const QVector<ActiveLayer> active = data.videoLayersAt(t + 1e-6);
            for (int j = 0; j < active.size(); ++j) {
                if (active[j].isText) {
                    double ex = 1;
                    const QImage ti = renderText(active[j].text, QSize(W, H), QSize(W, H), &ex);
                    LayerFx tfx = active[j].fx;
                    tfx.scale *= ex;
                    layers.push_back({ti, tfx, active[j].passes, active[j].mask, false});
                    continue;
                }
                if (active[j].isPass) { layers.push_back({QImage(), active[j].fx, active[j].passes, active[j].mask, true}); continue; }
                const QString key = QString::number(j) + "|" + active[j].path;
                std::shared_ptr<Decoder>& d = decoders[key];
                if (!d) {
                    d = std::make_shared<Decoder>();
                    if (!d->open(active[j].path)) { problem = "Couldn't decode " + QFileInfo(active[j].path).fileName() + ". Try converting it to H.264."; break; }
                }
                layers.push_back({d->frameAt(active[j].sourceTime), active[j].fx, active[j].passes, active[j].mask, false});
            }
            if (!problem.isEmpty()) break;
            comp.render(layers, QSize(W, H), fbo.get());
            gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
            gl->glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
            if (proc.state() != QProcess::Running) { problem = "FFmpeg stopped unexpectedly."; break; }
            proc.write(buf);
            while (proc.bytesToWrite() > 0)
                if (!proc.waitForBytesWritten(15000)) { problem = "FFmpeg stopped accepting video frames."; break; }
            emit progress(0.12 + 0.83 * double(i + 1) / total);
        }
        if (problem.isEmpty()) {
            proc.closeWriteChannel();
            proc.waitForFinished(-1);
            if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
                problem = "FFmpeg said: " + QString::fromUtf8(proc.readAllStandardError()).trimmed().split('\n').last();
        } else if (proc.state() != QProcess::NotRunning) {
            proc.kill();
            proc.waitForFinished(5000);
        }
        comp.cleanup();
        fbo.reset();   // must be destroyed while the context is current
    }
    ctx.doneCurrent();
    return problem;
}

QString Exporter::runFilter(const ProjectData& data, const QStringList& args, const QString& ffmpeg) {
    QProcess proc;
    proc.start(ffmpeg, args);
    if (!proc.waitForStarted(15000)) return "FFmpeg couldn't be started. Make sure ffmpeg is installed.";
    QString buf;
    const double total = std::max(0.001, data.duration());
    auto pump = [&] {
        buf += QString::fromUtf8(proc.readAllStandardOutput());
        int nl;
        while ((nl = buf.indexOf('\n')) >= 0) {
            const QString line = buf.left(nl).trimmed();
            buf.remove(0, nl + 1);
            if (line.startsWith("out_time_us=")) {
                bool ok = false;
                const double us = line.mid(12).toDouble(&ok);
                if (ok) emit progress(0.12 + 0.83 * std::clamp(us / 1e6 / total, 0.0, 1.0));
            }
        }
    };
    while (proc.state() != QProcess::NotRunning) {
        if (m_cancel) { proc.kill(); proc.waitForFinished(5000); return "Export cancelled."; }
        proc.waitForReadyRead(200);
        pump();
    }
    pump();
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return "FFmpeg said: " + QString::fromUtf8(proc.readAllStandardError()).trimmed().split('\n').last();
    return {};
}

void Exporter::job(ProjectData data, ExportOptions o, QString ffmpeg, bool gpu) {
    const double D = data.duration();
    auto fail = [this](const QString& m) {
        emit finished(false, m == "Export cancelled." ? m : "Couldn't export the video.\n\n" + m +
                                 "\n\nTry a different encoder (for example H.264 CPU), or check that every media file still exists.");
    };
    // 1. audio, rendered to a finished file of exactly the right length
    const QString audioPath = m_tmp.filePath("audio.s16");
    {
        AudioMixer mixer(data);
        QString err;
        if (!mixer.renderToFile(audioPath, &m_cancel, [this](double f) { emit progress(f * 0.12); }, &err)) {
            fail(m_cancel ? "Export cancelled." : err);
            return;
        }
    }
    // 2. video
    QStringList args;
    QString err;
    const bool built = gpu ? buildGpuCommand(data, o, audioPath, &args, &err)
                           : buildFilterCommand(data, o, m_tmp.filePath("graph.txt"), audioPath, &args, nullptr, &err);
    if (!built) { fail(err); return; }
    const QString problem = gpu ? runGpu(data, args, ffmpeg) : runFilter(data, args, ffmpeg);
    if (!problem.isEmpty()) { fail(problem); return; }

    // 3. never hand over a file whose duration a media player could misread
    DurationCheck chk = checkDuration(o.outPath, D);
    if (!chk.ok) {
        const bool fixed = repairByRemux(ffmpeg, o.outPath, D);
        chk = checkDuration(o.outPath, D);
        m_repaired = fixed && chk.ok;
        if (!chk.ok) { fail("The exported file has an invalid duration (" + chk.detail + ") and couldn't be repaired automatically."); return; }
    }
    emit progress(1.0);
    emit finished(true, m_repaired ? "Export finished. (The file's duration header was wrong and has been repaired.)" : "Export finished.");
}

// ---------------------------------------------------------------------------------------------------------

Exporter::Exporter(QObject* parent) : QObject(parent) {}

Exporter::~Exporter() {
    m_cancel = true;
    if (m_thread.joinable()) m_thread.join();
}

bool Exporter::start(const ProjectData& p, const ExportOptions& o, QString* err) {
    const QString ff = findFfmpeg();
    if (ff.isEmpty()) {
        if (err) *err = "FFmpeg wasn't found. Install it (or place ffmpeg next to this app) to export.";
        return false;
    }
    if (!m_tmp.isValid()) { if (err) *err = "Couldn't create a temporary folder."; return false; }
    if (!checkMedia(p, err)) return false;
    if (m_thread.joinable()) m_thread.join();
    m_cancel = false;
    m_repaired = false;
    m_gpu = o.allowGpu && gpuAvailable();
    if (m_gpu) {
        m_surface = std::make_unique<QOffscreenSurface>();   // must be created on the GUI thread
        m_surface->setFormat(QSurfaceFormat::defaultFormat());
        m_surface->create();
    }
    const bool gpu = m_gpu;
    m_thread = std::thread([this, p, o, ff, gpu] { job(p, o, ff, gpu); });
    return true;
}

} // namespace ve
