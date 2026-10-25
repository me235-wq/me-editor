#include "AudioMixer.h"
#include <QFile>
#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace ve {

// ---------------------------------------------------------------------------------------------------------
// AudioSource
// ---------------------------------------------------------------------------------------------------------

AudioSource::AudioSource() {
    m_frame = av_frame_alloc();
    m_pkt = av_packet_alloc();
}

AudioSource::~AudioSource() {
    close();
    av_frame_free(&m_frame);
    av_packet_free(&m_pkt);
}

void AudioSource::close() {
    if (m_swr) swr_free(&m_swr);
    if (m_ctx) avcodec_free_context(&m_ctx);
    if (m_fmt) avformat_close_input(&m_fmt);
}

bool AudioSource::initSwr() {
    if (m_swr) swr_free(&m_swr);
    AVChannelLayout out, in;
    av_channel_layout_default(&out, 2);
    if (m_ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&in, std::max(1, m_ctx->ch_layout.nb_channels));
    else av_channel_layout_copy(&in, &m_ctx->ch_layout);
    const int r = swr_alloc_set_opts2(&m_swr, &out, AV_SAMPLE_FMT_FLT, kMixRate, &in, m_ctx->sample_fmt, m_ctx->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&out);
    av_channel_layout_uninit(&in);
    if (r < 0 || !m_swr || swr_init(m_swr) < 0) { if (m_swr) swr_free(&m_swr); return false; }
    return true;
}

bool AudioSource::open(const QString& path) {
    close();
    const QByteArray u = path.toUtf8();
    if (avformat_open_input(&m_fmt, u.constData(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) { close(); return false; }
    m_as = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (m_as < 0) { close(); return false; }
    AVStream* st = m_fmt->streams[m_as];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) { close(); return false; }
    m_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_ctx, st->codecpar);
    m_ctx->pkt_timebase = st->time_base;
    if (avcodec_open2(m_ctx, codec, nullptr) < 0) { close(); return false; }
    m_tb = av_q2d(st->time_base);
    m_startOff = st->start_time != AV_NOPTS_VALUE ? double(st->start_time) * m_tb : 0.0;
    if (!initSwr()) { close(); return false; }
    m_haveStart = false; m_eof = false; m_draining = false;
    m_cache.clear();
    return true;
}

void AudioSource::seekTo(qint64 s0) {
    const double t = std::max(0.0, double(s0) / kMixRate - 0.05);
    av_seek_frame(m_fmt, m_as, int64_t((t + m_startOff) / m_tb), AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_ctx);
    initSwr();
    m_cache.clear();
    m_haveStart = false; m_eof = false; m_draining = false;
}

bool AudioSource::decodeMore() {
    for (;;) {
        const int r = avcodec_receive_frame(m_ctx, m_frame);
        if (r == 0) {
            if (!m_haveStart) {
                int64_t ts = m_frame->best_effort_timestamp;
                const double t = ts == AV_NOPTS_VALUE ? 0.0 : double(ts) * m_tb - m_startOff;
                m_cacheStart = qint64(std::llround(t * kMixRate));
                m_haveStart = true;
                m_cache.clear();
            }
            const int maxOut = swr_get_out_samples(m_swr, m_frame->nb_samples);
            const size_t old = m_cache.size();
            m_cache.resize(old + size_t(maxOut) * 2);
            uint8_t* o[1] = {reinterpret_cast<uint8_t*>(m_cache.data() + old)};
            int got = swr_convert(m_swr, o, maxOut, const_cast<const uint8_t**>(m_frame->extended_data), m_frame->nb_samples);
            if (got < 0) got = 0;
            m_cache.resize(old + size_t(got) * 2);
            av_frame_unref(m_frame);
            return true;
        }
        if (r == AVERROR(EAGAIN)) {
            if (m_draining) { m_eof = true; return false; }
            if (av_read_frame(m_fmt, m_pkt) < 0) {
                avcodec_send_packet(m_ctx, nullptr);
                m_draining = true;
                continue;
            }
            if (m_pkt->stream_index == m_as) avcodec_send_packet(m_ctx, m_pkt);
            av_packet_unref(m_pkt);
            continue;
        }
        m_eof = true;   // EOF or a real decode error: treat the rest as silence
        return false;
    }
}

void AudioSource::read(qint64 s0, int n, float* out) {
    std::fill(out, out + size_t(n) * 2, 0.0f);
    if (!m_ctx || n <= 0) return;
    if (s0 < 0) {                                   // before the start of the file: leading silence
        const int skip = int(std::min<qint64>(n, -s0));
        out += size_t(skip) * 2; n -= skip; s0 += skip;
        if (n <= 0) return;
    }
    if (m_haveStart && m_eof && s0 >= cacheEnd()) return;     // past the end of the audio
    if (!m_haveStart || s0 < m_cacheStart || s0 > cacheEnd() + kMixRate) seekTo(s0);
    const qint64 need = s0 + n;
    while (!m_eof && cacheEnd() < need) decodeMore();

    const qint64 frames = qint64(m_cache.size() / 2);
    for (int i = 0; i < n; ++i) {
        const qint64 idx = s0 + i - m_cacheStart;
        if (idx >= 0 && idx < frames) { out[2 * i] = m_cache[size_t(idx) * 2]; out[2 * i + 1] = m_cache[size_t(idx) * 2 + 1]; }
    }
    const qint64 keepFrom = s0 - 4;                 // forget what we've already played, keep memory bounded
    if (keepFrom - m_cacheStart > 4 * kMixRate) {
        const qint64 drop = std::min(keepFrom - m_cacheStart, frames);
        m_cache.erase(m_cache.begin(), m_cache.begin() + size_t(drop) * 2);
        m_cacheStart += drop;
    }
}

// ---------------------------------------------------------------------------------------------------------
// AudioMixer
// ---------------------------------------------------------------------------------------------------------

double clipGain(const Clip& c, double local) {
    double g = std::max(0.0, propValue(c, "volume", local));
    if (c.fadeIn > 0) g *= std::clamp(local / c.fadeIn, 0.0, 1.0);
    if (c.fadeOut > 0) g *= std::clamp((c.dur - local) / c.fadeOut, 0.0, 1.0);
    return g;
}

AudioMixer::AudioMixer(ProjectData snapshot) : m_d(std::move(snapshot)) {}

void AudioMixer::render(double t0, int frames, float* out) {
    std::fill(out, out + size_t(frames) * 2, 0.0f);
    const double sr = kMixRate, t1 = t0 + frames / sr;
    for (const Clip& c : m_d.clips) {
        if (c.freeze || c.muted || c.end() <= t0 || c.start >= t1) continue;
        const Track& tr = m_d.tracks[c.track];
        if (tr.hidden || tr.muted) continue;
        const MediaAsset* a = m_d.asset(c.assetId);
        if (!a || !a->hasAudio || a->missing || a->isImage || m_bad.contains(c.id)) continue;

        const int f0 = std::max(0, int(std::ceil((c.start - t0) * sr - 1e-9)));
        const int f1 = std::min(frames, int(std::ceil((c.end() - t0) * sr - 1e-9)));
        if (f1 <= f0) continue;

        std::shared_ptr<AudioSource>& src = m_src[c.id];
        if (!src) {
            src = std::make_shared<AudioSource>();
            if (!src->open(a->path)) { m_bad.insert(c.id); m_src.remove(c.id); continue; }
        }
        auto srcPos = [&](int f) { return (c.in + (t0 + f / sr - c.start) * c.speed) * sr; };   // source sample index (fractional)
        const qint64 smin = qint64(std::floor(srcPos(f0)));
        const qint64 smax = qint64(std::floor(srcPos(f1 - 1))) + 2;
        std::vector<float> tmp(size_t(smax - smin + 1) * 2);
        src->read(smin, int(smax - smin + 1), tmp.data());

        constexpr int kBlock = 64;   // gain is evaluated every 64 samples and ramped linearly in between
        for (int b = f0; b < f1; b += kBlock) {
            const int be = std::min(b + kBlock, f1);
            const double g0 = clipGain(c, t0 + b / sr - c.start), g1 = clipGain(c, t0 + be / sr - c.start);
            for (int f = b; f < be; ++f) {
                const double g = g0 + (g1 - g0) * double(f - b) / double(be - b);
                const double pos = srcPos(f);
                const qint64 i = qint64(std::floor(pos));
                const float fr = float(pos - double(i));
                const size_t k = size_t(i - smin) * 2;
                out[2 * f] += float(g) * (tmp[k] * (1 - fr) + tmp[k + 2] * fr);
                out[2 * f + 1] += float(g) * (tmp[k + 1] * (1 - fr) + tmp[k + 3] * fr);
            }
        }
    }
}

bool AudioMixer::renderToFile(const QString& path, const std::atomic<bool>* cancel, const std::function<void(double)>& progress, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { if (err) *err = "Couldn't write a temporary audio file."; return false; }
    const qint64 total = qint64(std::llround(m_d.duration() * kMixRate));
    constexpr int kChunk = kMixRate;
    std::vector<float> buf(size_t(kChunk) * 2);
    std::vector<int16_t> pcm(size_t(kChunk) * 2);
    for (qint64 pos = 0; pos < total; pos += kChunk) {
        if (cancel && cancel->load()) return false;
        const int n = int(std::min<qint64>(kChunk, total - pos));
        render(double(pos) / kMixRate, n, buf.data());
        for (int i = 0; i < n * 2; ++i) pcm[size_t(i)] = int16_t(std::lround(std::clamp(buf[size_t(i)], -1.0f, 1.0f) * 32767.0f));
        if (f.write(reinterpret_cast<const char*>(pcm.data()), qint64(n) * 4) != qint64(n) * 4) { if (err) *err = "Disk full while writing audio."; return false; }
        if (progress) progress(double(pos) / double(total));
    }
    return true;
}

} // namespace ve
