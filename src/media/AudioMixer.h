#pragma once
#include "core/Project.h"
#include <QHash>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace ve {

constexpr int kMixRate = 48000;   // everything is mixed as 48 kHz stereo float

// Random-access audio reader for one file (FFmpeg decode + resample to 48 kHz stereo float).
class AudioSource {
public:
    AudioSource();
    ~AudioSource();
    bool open(const QString& path);
    // `n` frames starting at source sample `s0` (48 kHz); silence where the file has no audio.
    void read(qint64 s0, int n, float* out);

private:
    void close();
    void seekTo(qint64 s0);
    bool initSwr();
    bool decodeMore();
    qint64 cacheEnd() const { return m_cacheStart + qint64(m_cache.size() / 2); }

    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_ctx = nullptr;
    SwrContext* m_swr = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_pkt = nullptr;
    int m_as = -1;
    double m_tb = 0, m_startOff = 0;
    std::vector<float> m_cache;       // interleaved stereo
    qint64 m_cacheStart = 0;
    bool m_haveStart = false, m_eof = false, m_draining = false;
};

// Mixes every audible clip of a project snapshot: volume (keyframes), fades, mute, speed, track mute/hide, freeze = silent.
// The same code feeds live playback and the exported file, so what you hear is what you export.
class AudioMixer {
public:
    explicit AudioMixer(ProjectData snapshot);
    // Interleaved stereo float. Overwrites `out` (frames * 2 floats) with the mix for [t0, t0 + frames/48000).
    void render(double t0, int frames, float* out);
    // Whole timeline → raw signed 16-bit little-endian stereo file of exactly duration()*48000 frames.
    bool renderToFile(const QString& path, const std::atomic<bool>* cancel, const std::function<void(double)>& progress, QString* err);
    const ProjectData& data() const { return m_d; }

private:
    ProjectData m_d;
    QHash<QString, std::shared_ptr<AudioSource>> m_src;   // by clip id
    QSet<QString> m_bad;
};

double clipGain(const Clip& c, double local);   // volume keyframes × fades

} // namespace ve
