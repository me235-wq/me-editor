#pragma once
#include "core/Project.h"
#include <QObject>
#include <QOffscreenSurface>
#include <QTemporaryDir>
#include <atomic>
#include <memory>
#include <thread>

namespace ve {

struct EncoderChoice {
    QString label;     // shown in the UI
    QString encoder;   // FFmpeg encoder name
};

struct ExportOptions {
    QString outPath;
    QString encoder = "libx264";
    int quality = 20;       // CRF/CQ value; lower = better
    int audioKbps = 192;
    bool allowGpu = true;   // false forces the plain FFmpeg path (no transform / effects / keyframes)
};

// Result of checking a finished file's timing.
struct DurationCheck {
    bool ok = false;
    double headerSeconds = 0, videoEnd = 0, audioEnd = 0;
    QString detail;
};

// Renders the timeline without ever touching the originals.
//  1. Audio is mixed by AudioMixer (same code as playback) into a temporary file of the exact length.
//  2. GPU path (default): each video frame goes through the same Compositor as the preview and is piped to FFmpeg.
//     Filter path (no OpenGL): a pure FFmpeg filter graph (cuts, layers, speed, freeze; no transform/effects).
//  3. The output is capped at the timeline length, then its duration is verified and repaired if a player would misread it.
class Exporter : public QObject {
    Q_OBJECT
public:
    explicit Exporter(QObject* parent = nullptr);
    ~Exporter() override;

    static QString findFfmpeg();                         // next to the app first, then PATH
    static QVector<EncoderChoice> availableEncoders();   // CPU encoders + any hardware encoders FFmpeg lists
    static bool gpuAvailable();                          // can we create an OpenGL 3.3 context? (needs a QGuiApplication)

    // Pure functions (unit-testable). `audioPath` is the raw s16le stereo 48 kHz file produced by the mixer.
    static bool buildFilterCommand(const ProjectData& p, const ExportOptions& o, const QString& scriptPath, const QString& audioPath,
                                   QStringList* args, QString* graph, QString* err);
    static bool buildGpuCommand(const ProjectData& p, const ExportOptions& o, const QString& audioPath, QStringList* args, QString* err);

    // Reads every packet and compares timing with `expected` seconds.
    static DurationCheck checkDuration(const QString& path, double expected);
    // Stream-copy remux with the length capped and the header rewritten. Returns true if the file was replaced.
    static bool repairByRemux(const QString& ffmpeg, const QString& path, double expected);

    bool start(const ProjectData& p, const ExportOptions& o, QString* err);
    void cancel() { m_cancel = true; }
    bool usedGpu() const { return m_gpu; }
    bool repaired() const { return m_repaired; }

signals:
    void progress(double fraction);
    void finished(bool ok, const QString& message);

private:
    void job(ProjectData data, ExportOptions o, QString ffmpeg, bool gpu);
    QString runGpu(const ProjectData& data, const QStringList& args, const QString& ffmpeg);
    QString runFilter(const ProjectData& data, const QStringList& args, const QString& ffmpeg);

    QTemporaryDir m_tmp;
    std::unique_ptr<QOffscreenSurface> m_surface;
    std::thread m_thread;
    std::atomic<bool> m_cancel{false};
    bool m_gpu = false, m_repaired = false;
};

} // namespace ve
