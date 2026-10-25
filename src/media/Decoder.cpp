#include "Decoder.h"
#include <QFileInfo>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(disable : 4996)
#endif

namespace ve {

static bool isImageFormat(const AVFormatContext* f) {
    const char* n = f->iformat ? f->iformat->name : "";
    return std::strstr(n, "_pipe") || !std::strcmp(n, "image2") || !std::strcmp(n, "ico");
}

static int streamRotation(AVStream* st) {
    const int32_t* m = nullptr;
#if LIBAVFORMAT_VERSION_MAJOR >= 62
    const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
    if (sd) m = reinterpret_cast<const int32_t*>(sd->data);
#else
    m = reinterpret_cast<const int32_t*>(av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, nullptr));
#endif
    if (!m) return 0;
    const double theta = av_display_rotation_get(m);   // counter-clockwise degrees
    if (std::isnan(theta)) return 0;
    int r = int(std::lround(-theta)) % 360;
    if (r < 0) r += 360;
    return (r / 90) * 90;
}

bool probeMedia(const QString& path, MediaAsset* out, QString* err) {
    AVFormatContext* fmt = nullptr;
    const QByteArray u = path.toUtf8();
    if (avformat_open_input(&fmt, u.constData(), nullptr, nullptr) < 0) {
        if (err) *err = "Couldn't open this file.";
        return false;
    }
    struct Guard { AVFormatContext** f; ~Guard() { avformat_close_input(f); } } guard{&fmt};
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        if (err) *err = "Couldn't read this file's streams. It may be corrupted.";
        return false;
    }
    const int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    MediaAsset a;
    a.path = path;
    a.hasAudio = ai >= 0;
    if (vi >= 0) {
        AVStream* st = fmt->streams[vi];
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {   // ignore cover art in audio files
            a.hasVideo = true;
            a.width = st->codecpar->width;
            a.height = st->codecpar->height;
            a.rotation = streamRotation(st);
            if (a.rotation == 90 || a.rotation == 270) std::swap(a.width, a.height);
            const AVRational fr = av_guess_frame_rate(fmt, st, nullptr);
            a.fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0;
            a.isImage = isImageFormat(fmt);
        }
    }
    if (!a.hasVideo && !a.hasAudio) {
        if (err) *err = "No video or audio found in this file.";
        return false;
    }
    if (!a.isImage) {
        if (fmt->duration > 0) a.duration = double(fmt->duration) / AV_TIME_BASE;
        if (a.duration <= 0) {   // GIFs and some streams don't store a length: measure it by reading the packets
            AVPacket* pkt = av_packet_alloc();
            double end = 0;
            while (av_read_frame(fmt, pkt) >= 0) {
                const AVStream* st = fmt->streams[pkt->stream_index];
                if (pkt->pts != AV_NOPTS_VALUE)
                    end = std::max(end, (double(pkt->pts) + double(std::max<int64_t>(pkt->duration, 0))) * av_q2d(st->time_base));
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
            a.duration = end;
        }
        if (a.duration <= 0) {
            if (err) *err = "Couldn't determine this file's length.";
            return false;
        }
    }
    *out = a;
    return true;
}

// ---------------------------------------------------------------------------------------------

static AVPixelFormat pickHwFormat(AVCodecContext* c, const AVPixelFormat* fmts) {
    const AVPixelFormat want = AVPixelFormat(reinterpret_cast<intptr_t>(c->opaque));
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == want) return *p;
    return AV_PIX_FMT_NONE;   // can't use hardware → decoder open/decoding fails → we fall back to software
}

static QVector<AVHWDeviceType> hwCandidates() {
#if defined(_WIN32)
    return {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2, AV_HWDEVICE_TYPE_CUDA};
#elif defined(__linux__)
    return {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_VDPAU};
#else
    return {};
#endif
}

Decoder::Decoder() {
    m_frame = av_frame_alloc();
    m_held = av_frame_alloc();
    m_sw = av_frame_alloc();
    m_pkt = av_packet_alloc();
}

Decoder::~Decoder() {
    close();
    av_frame_free(&m_frame);
    av_frame_free(&m_held);
    av_frame_free(&m_sw);
    av_packet_free(&m_pkt);
}

void Decoder::setMaxSize(const QSize& s) {
    if (s == m_max) return;
    m_max = s;
    m_haveCur = false;   // cached frame has the wrong size now
}

void Decoder::closeCodec() {
    if (m_ctx) avcodec_free_context(&m_ctx);
    if (m_hwDev) av_buffer_unref(&m_hwDev);
    m_hw = false;
    m_hwPix = -1;
}

void Decoder::close() {
    closeCodec();
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_fmt) avformat_close_input(&m_fmt);
    av_frame_unref(m_frame);
    av_frame_unref(m_held);
    av_frame_unref(m_sw);
    m_haveCur = false;
    m_cur = QImage();
}

bool Decoder::openCodec(bool hw) {
    AVStream* st = m_fmt->streams[m_vs];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return false;
    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx) return false;
    avcodec_parameters_to_context(m_ctx, st->codecpar);
    m_ctx->pkt_timebase = st->time_base;

    if (hw) {
        bool ok = false;
        for (AVHWDeviceType type : hwCandidates()) {
            AVPixelFormat pix = AV_PIX_FMT_NONE;
            for (int i = 0;; ++i) {
                const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
                if (!cfg) break;
                if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) { pix = cfg->pix_fmt; break; }
            }
            if (pix == AV_PIX_FMT_NONE) continue;
            if (av_hwdevice_ctx_create(&m_hwDev, type, nullptr, nullptr, 0) < 0) { m_hwDev = nullptr; continue; }
            m_ctx->hw_device_ctx = av_buffer_ref(m_hwDev);
            m_ctx->get_format = pickHwFormat;
            m_ctx->opaque = reinterpret_cast<void*>(intptr_t(pix));
            m_hwPix = pix;
            ok = true;
            break;
        }
        if (!ok) { avcodec_free_context(&m_ctx); return false; }
        m_hw = true;
    } else {
        m_ctx->thread_count = 0;   // auto
        m_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    if (avcodec_open2(m_ctx, codec, nullptr) < 0) {
        closeCodec();
        return false;
    }
    m_draining = false;
    m_framesOut = 0;
    return true;
}

bool Decoder::switchToSoftware() {
    closeCodec();
    if (!openCodec(false)) return false;
    seekTo(m_haveCur ? m_curTime : 0);
    return true;
}

bool Decoder::open(const QString& path, QString* err, bool allowHardware) {
    close();
    m_path = path;
    const QByteArray u = path.toUtf8();
    if (avformat_open_input(&m_fmt, u.constData(), nullptr, nullptr) < 0) {
        if (err) *err = "Couldn't open this file.";
        return false;
    }
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        if (err) *err = "Couldn't read this file's streams.";
        close();
        return false;
    }
    m_vs = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_vs < 0) {
        if (err) *err = "This file has no video.";
        close();
        return false;
    }
    AVStream* st = m_fmt->streams[m_vs];
    m_tb = av_q2d(st->time_base);
    m_startOff = st->start_time != AV_NOPTS_VALUE ? double(st->start_time) * m_tb : 0.0;
    const AVRational fr = av_guess_frame_rate(m_fmt, st, nullptr);
    m_fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 30.0;
    m_dur = m_fmt->duration > 0 ? double(m_fmt->duration) / AV_TIME_BASE : 0;
    m_rot = streamRotation(st);
    m_image = isImageFormat(m_fmt);

    const bool tryHw = allowHardware && !m_image && st->codecpar->width >= 640;
    if (!(tryHw && openCodec(true)) && !openCodec(false)) {
        if (err) *err = "Couldn't decode this video. Try converting it to H.264 or check whether the file is corrupted.";
        close();
        return false;
    }
    return true;
}

double Decoder::frameTime(const AVFrame* f) const {
    int64_t ts = f->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE) ts = f->pts;
    if (ts == AV_NOPTS_VALUE) return m_curTime + 1.0 / m_fps;
    return double(ts) * m_tb - m_startOff;
}

QImage Decoder::convert(AVFrame* f) {
    AVFrame* src = f;
    if (m_hw && f->format == m_hwPix) {
        av_frame_unref(m_sw);
        if (av_hwframe_transfer_data(m_sw, f, 0) < 0) return {};
        av_frame_copy_props(m_sw, f);
        src = m_sw;
    }
    const int sw = src->width, sh = src->height;
    if (sw <= 0 || sh <= 0) return {};
    const bool swap = (m_rot == 90 || m_rot == 270);
    const double dispW = swap ? sh : sw, dispH = swap ? sw : sh;
    double scale = 1.0;
    if (m_max.isValid() && m_max.width() > 0 && m_max.height() > 0)
        scale = std::min(1.0, std::min(m_max.width() / dispW, m_max.height() / dispH));
    int dw = sw, dh = sh;
    if (scale < 1.0) {
        dw = std::max(2, int(sw * scale) & ~1);
        dh = std::max(2, int(sh * scale) & ~1);
    }
    m_sws = sws_getCachedContext(m_sws, sw, sh, AVPixelFormat(src->format), dw, dh, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws) return {};

    int cs = SWS_CS_ITU601;
    switch (src->colorspace) {
        case AVCOL_SPC_BT709: cs = SWS_CS_ITU709; break;
        case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL: cs = SWS_CS_BT2020; break;
        case AVCOL_SPC_SMPTE170M: case AVCOL_SPC_BT470BG: cs = SWS_CS_ITU601; break;
        default: if (sh >= 720) cs = SWS_CS_ITU709; break;   // HD material without tags is almost always Rec.709
    }
    const int srcRange = src->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    sws_setColorspaceDetails(m_sws, sws_getCoefficients(cs), srcRange, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);

    QImage out(dw, dh, QImage::Format_RGBA8888);
    uint8_t* dst[4] = {out.bits(), nullptr, nullptr, nullptr};
    int ls[4] = {int(out.bytesPerLine()), 0, 0, 0};
    sws_scale(m_sws, src->data, src->linesize, 0, sh, dst, ls);
    if (m_rot) out = out.transformed(QTransform().rotate(m_rot));
    return out;
}

void Decoder::seekTo(double t) {
    const int64_t ts = int64_t((t + m_startOff) / m_tb);
    av_seek_frame(m_fmt, m_vs, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_ctx);
    m_draining = false;
    m_eof = false;
    m_haveCur = false;
    av_frame_unref(m_held);
}

// Decode forward until we reach the first frame at/after `target`.
bool Decoder::decodeUntil(double target) {
    av_frame_unref(m_held);
    bool haveHeld = false;
    for (;;) {
        const int r = avcodec_receive_frame(m_ctx, m_frame);
        if (r == 0) {
            ++m_framesOut;
            const double ft = frameTime(m_frame);
            if (m_image || ft >= target) {
                QImage img = convert(m_frame);
                av_frame_unref(m_frame);
                if (img.isNull()) return false;
                m_cur = img; m_curTime = ft; m_haveCur = true;
                return true;
            }
            av_frame_unref(m_held);            // too early: remember it in case the stream ends first
            av_frame_move_ref(m_held, m_frame);
            m_heldTime = ft;
            haveHeld = true;
            continue;
        }
        if (r == AVERROR(EAGAIN)) {
            if (m_draining) return false;
            if (av_read_frame(m_fmt, m_pkt) < 0) {
                avcodec_send_packet(m_ctx, nullptr);   // flush
                m_draining = true;
                continue;
            }
            if (m_pkt->stream_index == m_vs) {
                const int s = avcodec_send_packet(m_ctx, m_pkt);
                av_packet_unref(m_pkt);
                if (s < 0 && s != AVERROR(EAGAIN)) return false;
            } else {
                av_packet_unref(m_pkt);
            }
            continue;
        }
        if (r == AVERROR_EOF) {
            m_eof = true;
            if (haveHeld) {
                QImage img = convert(m_held);
                if (!img.isNull()) { m_cur = img; m_curTime = m_heldTime; m_haveCur = true; }
            }
            return m_haveCur;
        }
        return false;   // real decode error
    }
}

QImage Decoder::frameAt(double t) {
    if (!m_fmt || !m_ctx) return {};
    if (m_image) {
        if (!m_haveCur) decodeUntil(-1e9);
        return m_cur;
    }
    if (m_dur > 0) t = std::min(t, m_dur);
    t = std::max(0.0, t);
    const double half = 0.5 / (m_fps > 0 ? m_fps : 30.0);

    if (m_haveCur && m_curTime >= t - half && m_curTime < t + half) return m_cur;   // already showing it
    if (m_haveCur && m_eof && t >= m_curTime) return m_cur;                         // past the last frame

    const bool forward = m_haveCur && t > m_curTime && (t - m_curTime) < 2.0 && !m_eof;
    if (!forward) seekTo(t);
    bool ok = decodeUntil(t - half);
    if (!ok && m_hw && m_framesOut == 0) {   // hardware path didn't work on this machine/codec → software
        if (switchToSoftware()) ok = decodeUntil(t - half);
    }
    return ok || m_haveCur ? m_cur : QImage();
}

} // namespace ve
