#include "FrameServer.h"
#include "Decoder.h"
#include "core/TextRender.h"
#include <QHash>
#include <QSet>
#include <memory>

namespace ve {

FrameServer::FrameServer(QObject* parent) : QObject(parent) {
    qRegisterMetaType<QVector<RenderLayer>>("QVector<ve::RenderLayer>");
    m_thread = std::thread([this] { loop(); });
}

FrameServer::~FrameServer() {
    { std::lock_guard<std::mutex> lk(m_mx); m_quit = true; }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void FrameServer::request(const QVector<LayerRequest>& layers, const QSize& maxSize, const QSize& canvas) {
    { std::lock_guard<std::mutex> lk(m_mx); m_pending = layers; m_max = maxSize; m_canvas = canvas; m_hasPending = true; }
    m_cv.notify_one();
}

void FrameServer::loop() {
    QHash<QString, std::shared_ptr<Decoder>> decoders;
    QSet<QString> broken;
    for (;;) {
        QVector<LayerRequest> req;
        QSize ms, cv;
        {
            std::unique_lock<std::mutex> lk(m_mx);
            m_cv.wait(lk, [this] { return m_quit || m_hasPending; });
            if (m_quit) return;
            req = std::move(m_pending);
            ms = m_max;
            cv = m_canvas;
            m_hasPending = false;
        }
        QVector<RenderLayer> out;
        QSet<QString> used;
        for (int i = 0; i < req.size(); ++i) {
            if (req[i].isText) {                               // text object: draw it (at preview resolution), nothing to decode
                const QSize full = cv.isValid() ? cv : QSize(1920, 1080);
                QSize img = full;
                if (ms.isValid() && ms.width() > 0 && (ms.width() < img.width() || ms.height() < img.height())) img = img.scaled(ms, Qt::KeepAspectRatio);
                double ex = 1;
                const QImage ti = renderText(req[i].text, full, img, &ex);
                LayerFx tfx = req[i].fx;
                tfx.scale *= ex;                               // the image covers a bigger "virtual canvas" when the text is large
                out.push_back({ti, tfx, req[i].passes, req[i].mask, false});
                continue;
            }
            if (req[i].isPass) { out.push_back({QImage(), req[i].fx, req[i].passes, req[i].mask, true}); continue; }
            const QString key = QString::number(i) + "|" + req[i].path;   // one decoder per layer slot
            used.insert(key);
            if (broken.contains(req[i].path)) { out.push_back({QImage(), req[i].fx, req[i].passes, req[i].mask, false}); continue; }
            std::shared_ptr<Decoder>& d = decoders[key];
            if (!d) {
                d = std::make_shared<Decoder>();
                if (!d->open(req[i].path)) { decoders.remove(key); broken.insert(req[i].path); out.push_back({QImage(), req[i].fx, req[i].passes, req[i].mask, false}); continue; }
            }
            d->setMaxSize(ms);
            out.push_back({d->frameAt(req[i].time), req[i].fx, req[i].passes, req[i].mask, false});
        }
        if (decoders.size() > 8) {   // keep a handful of decoders open, drop the rest
            for (auto it = decoders.begin(); it != decoders.end();)
                it = used.contains(it.key()) ? std::next(it) : decoders.erase(it);
        }
        emit framesReady(out);
    }
}

} // namespace ve
