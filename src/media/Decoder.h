#pragma once
#include "core/Project.h"
#include <QImage>
#include <QSize>
#include <QString>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;

namespace ve {

// Reads container + stream info. Never throws; returns a human-readable error.
bool probeMedia(const QString& path, MediaAsset* out, QString* err);

// Random-access video frame decoder (FFmpeg). Tries hardware decoding first
// (VAAPI/CUDA/VDPAU on Linux, D3D11VA/DXVA2/CUDA on Windows) and silently falls back to software.
// Not thread-safe: use one instance per thread.
class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool open(const QString& path, QString* err = nullptr, bool allowHardware = true);
    void close();
    bool isOpen() const { return m_fmt != nullptr; }
    bool usesHardware() const { return m_hw; }

    // Frames are downscaled (never upscaled) to fit this size; null size = native.
    void setMaxSize(const QSize& s);
    // Returns the frame shown at `t` seconds in the source (RGBA8888, rotation applied).
    QImage frameAt(double t);

private:
    bool openCodec(bool hw);
    void closeCodec();
    bool switchToSoftware();
    void seekTo(double t);
    bool decodeUntil(double target);
    QImage convert(AVFrame* f);
    double frameTime(const AVFrame* f) const;

    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_ctx = nullptr;
    AVFrame *m_frame = nullptr, *m_held = nullptr, *m_sw = nullptr;
    AVPacket* m_pkt = nullptr;
    SwsContext* m_sws = nullptr;
    AVBufferRef* m_hwDev = nullptr;
    int m_hwPix = -1;           // AVPixelFormat of hardware frames
    int m_vs = -1;
    bool m_hw = false, m_image = false, m_draining = false, m_eof = false, m_haveCur = false;
    double m_tb = 0, m_startOff = 0, m_fps = 30, m_dur = 0, m_curTime = 0, m_heldTime = 0;
    int m_rot = 0, m_framesOut = 0;
    QSize m_max;
    QImage m_cur;
    QString m_path;
};

} // namespace ve
