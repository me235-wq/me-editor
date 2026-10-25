#include "AudioPlayer.h"

#ifdef VE_HAS_AUDIO
#include "media/AudioMixer.h"
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace ve {

// Pull-mode device: the sound card asks for samples, we render them from the mixer (or play a test tone).
// Created and used only on the audio thread. bytesAvailable()/atEnd() are overridden because several audio back-ends
// (PulseAudio / PipeWire) won't read from a device that says it has nothing available.
class MixerDevice : public QIODevice {
public:
    explicit MixerDevice(bool int16) : m_int16(int16) {}
    void set(std::shared_ptr<AudioMixer> m, double pos) { std::lock_guard<std::mutex> lk(m_mx); m_mixer = std::move(m); m_pos = pos; m_tone = 0; }
    void tone(int frames) { std::lock_guard<std::mutex> lk(m_mx); m_mixer.reset(); m_tone = frames; m_toneTotal = frames; m_phase = 0; }
    double position() { std::lock_guard<std::mutex> lk(m_mx); return m_pos; }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return (1 << 20) + QIODevice::bytesAvailable(); }
    bool atEnd() const override { return false; }
protected:
    qint64 readData(char* data, qint64 maxlen) override {
        const int bpf = m_int16 ? 4 : 8;                       // bytes per stereo frame
        const int frames = int(maxlen / bpf);
        if (frames <= 0) return 0;
        m_buf.assign(size_t(frames) * 2, 0.0f);
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (m_tone > 0) {
                for (int i = 0; i < frames && m_tone > 0; ++i, --m_tone) {
                    const double fade = std::min({1.0, double(m_toneTotal - m_tone) / 2400.0, double(m_tone) / 2400.0});
                    const float v = float(0.25 * fade * std::sin(2.0 * M_PI * 440.0 * double(m_phase++) / kMixRate));
                    m_buf[size_t(i) * 2] = m_buf[size_t(i) * 2 + 1] = v;
                }
            } else if (m_mixer) {
                m_mixer->render(m_pos, frames, m_buf.data());
                m_pos += double(frames) / kMixRate;
            }
        }
        if (m_int16) {
            auto* o = reinterpret_cast<int16_t*>(data);
            for (size_t i = 0; i < m_buf.size(); ++i) o[i] = int16_t(std::lround(std::clamp(m_buf[i], -1.0f, 1.0f) * 32767.0f));
        } else {
            auto* o = reinterpret_cast<float*>(data);
            for (size_t i = 0; i < m_buf.size(); ++i) o[i] = std::clamp(m_buf[i], -1.0f, 1.0f);
        }
        return qint64(frames) * bpf;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    bool m_int16;
    std::mutex m_mx;
    std::shared_ptr<AudioMixer> m_mixer;
    double m_pos = 0;
    int m_tone = 0, m_toneTotal = 0;
    qint64 m_phase = 0;
    std::vector<float> m_buf;
};

class AudioPlayer::Worker : public QObject {
public:
    std::function<void(const QString&)> report;

    void play(ProjectData d, double t) {
        if (!ensureSink()) return;
        auto mixer = std::make_shared<AudioMixer>(std::move(d));
        std::vector<float> prime(4800 * 2);
        mixer->render(t, 4800, prime.data());                   // open the decoders now, not inside the sound card's callback
        m_dev->set(std::move(mixer), t);
        if (m_sink->state() != QAudio::ActiveState) { m_sink->stop(); m_sink->start(m_dev); }
    }
    void update(ProjectData d) { if (m_dev) m_dev->set(std::make_shared<AudioMixer>(std::move(d)), m_dev->position()); }
    void stop() { if (m_sink) m_sink->stop(); if (m_dev) m_dev->set(nullptr, 0); }
    void beep() {
        if (!ensureSink()) return;
        m_dev->tone(int(kMixRate * 1.2));
        m_sink->stop();
        m_sink->start(m_dev);
        report("Playing a test tone… if you hear nothing, check the system volume and the selected output device.");
        QTimer::singleShot(1800, this, [this] { stop(); });
    }
    ~Worker() override { delete m_sink; delete m_dev; }

private:
    bool ensureSink() {
        if (m_sink) return true;
        const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        if (dev.isNull()) { report("No sound output device is selected on this computer."); return false; }
        QAudioFormat fmt;
        fmt.setSampleRate(kMixRate);
        fmt.setChannelCount(2);
        bool ok = false, int16 = false;
        for (auto sf : {QAudioFormat::Float, QAudioFormat::Int16}) {      // prefer float, fall back to 16-bit
            fmt.setSampleFormat(sf);
            if (dev.isFormatSupported(fmt)) { ok = true; int16 = sf == QAudioFormat::Int16; break; }
        }
        if (!ok) { report(QString("Your sound device “%1” doesn't accept 48 kHz stereo audio.").arg(dev.description())); return false; }
        m_dev = new MixerDevice(int16);
        m_dev->open(QIODevice::ReadOnly);
        m_sink = new QAudioSink(dev, fmt);
        m_sink->setVolume(1.0);
        m_sink->setBufferSize(kMixRate * (int16 ? 4 : 8) / 10);           // ~100 ms
        connect(m_sink, &QAudioSink::stateChanged, this, [this](QAudio::State s) {
            if (s != QAudio::StoppedState || m_sink->error() == QAudio::NoError) return;
            const char* why = m_sink->error() == QAudio::OpenError ? "the device couldn't be opened"
                              : m_sink->error() == QAudio::IOError ? "reading audio failed" : m_sink->error() == QAudio::UnderrunError ? "the audio ran dry" : "a fatal error";
            report(QString("Audio output stopped: %1.").arg(why));
        });
        report(QString("Audio output: %1 (%2)").arg(dev.description(), int16 ? "16-bit" : "32-bit float"));
        return true;
    }
    MixerDevice* m_dev = nullptr;
    QAudioSink* m_sink = nullptr;
};

AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent) {
    m_available = !QMediaDevices::audioOutputs().isEmpty();
    if (!m_available) return;
    m_thread = new QThread(this);
    m_worker = new Worker;
    m_worker->report = [this](const QString& m) { QMetaObject::invokeMethod(this, [this, m] { emit message(m); }, Qt::QueuedConnection); };
    m_worker->moveToThread(m_thread);
    m_thread->start();
}

AudioPlayer::~AudioPlayer() {
    if (m_thread) {
        m_worker->report = nullptr;
        QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->stop(); }, Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(m_worker, [w = m_worker] { delete w; }, Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait();
    }
}

void AudioPlayer::play(const ProjectData& s, double t) {
    if (m_worker) QMetaObject::invokeMethod(m_worker, [w = m_worker, s, t] { w->play(s, t); }, Qt::QueuedConnection);
}
void AudioPlayer::updateProject(const ProjectData& s) {
    if (m_worker) QMetaObject::invokeMethod(m_worker, [w = m_worker, s] { w->update(s); }, Qt::QueuedConnection);
}
void AudioPlayer::stop() {
    if (m_worker) QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->stop(); }, Qt::QueuedConnection);
}
void AudioPlayer::beep() {
    if (m_worker) QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->beep(); }, Qt::QueuedConnection);
    else emit message("Qt Multimedia can't find any sound output on this computer (is PipeWire / PulseAudio running?).");
}

} // namespace ve

#else   // built without Qt Multimedia: playback is silent, everything else (and export) is unaffected

namespace ve {
AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent) {}
AudioPlayer::~AudioPlayer() = default;
void AudioPlayer::play(const ProjectData&, double) {}
void AudioPlayer::updateProject(const ProjectData&) {}
void AudioPlayer::stop() {}
void AudioPlayer::beep() { emit message("This build has no audio support. Install qt6-multimedia-dev and rebuild."); }
} // namespace ve

#endif
