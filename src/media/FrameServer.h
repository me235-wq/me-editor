#pragma once
#include "core/Effects.h"
#include <QImage>
#include <QObject>
#include <QSize>
#include <QVector>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ve {

struct LayerRequest {
    QString path;
    double time = 0;   // seconds inside the source
    LayerFx fx;        // effect transform evaluated for this exact timeline instant
    QVector<FxPass> passes;
    MaskFx mask;
    bool isPass = false;   // effect / adjustment layer: nothing to decode
    TextStyle text;
    bool isText = false;   // text object: rendered from `text`
};

// Decodes preview frames on a worker thread so the UI never blocks.
// Only the newest request is processed (older ones are dropped), which keeps scrubbing responsive.
class FrameServer : public QObject {
    Q_OBJECT
public:
    explicit FrameServer(QObject* parent = nullptr);
    ~FrameServer() override;
    void request(const QVector<LayerRequest>& layers, const QSize& maxSize, const QSize& canvas = QSize());

signals:
    void framesReady(const QVector<ve::RenderLayer>& layers);   // one per requested layer (null image = failed)

private:
    void loop();
    std::thread m_thread;
    std::mutex m_mx;
    std::condition_variable m_cv;
    QVector<LayerRequest> m_pending;
    QSize m_max, m_canvas;
    bool m_hasPending = false, m_quit = false;
};

} // namespace ve
