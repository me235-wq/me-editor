#include "Proxy.h"
#include "Exporter.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>

namespace ve {

ProxyManager::ProxyManager(const QString& folder, QObject* parent) : QObject(parent), m_dir(folder) { QDir().mkpath(folder); }

bool ProxyManager::worthProxying(const MediaAsset& a) {
    return a.hasVideo && !a.isImage && !a.missing && std::max(a.width, a.height) >= 2560 && std::min(a.width, a.height) >= 1440;
}

QString ProxyManager::proxyPathFor(const MediaAsset& a) const {
    const QFileInfo fi(a.path);   // keyed by path + size + modification time, so an edited file gets a fresh proxy
    const QByteArray key = (a.path + "|" + QString::number(fi.size()) + "|" + QString::number(fi.lastModified().toSecsSinceEpoch())).toUtf8();
    return m_dir + "/" + QCryptographicHash::hash(key, QCryptographicHash::Md5).toHex() + ".mp4";
}

QString ProxyManager::existing(const MediaAsset& a) const {
    const QString p = proxyPathFor(a);
    return QFileInfo(p).size() > 0 ? p : QString();
}

bool ProxyManager::request(const MediaAsset& a) {
    if (!worthProxying(a) || busy(a) || !existing(a).isEmpty()) return false;
    const QString ff = Exporter::findFfmpeg();
    if (ff.isEmpty()) { emit failed(a.id, "FFmpeg wasn't found."); return false; }
    const QString out = proxyPathFor(a), tmp = out + ".part.mp4";
    auto* p = new QProcess(this);
    m_running.insert(a.id);
    const QString id = a.id;
    connect(p, &QProcess::finished, this, [=](int code, QProcess::ExitStatus st) {
        m_running.remove(id);
        const QString err = QString::fromUtf8(p->readAllStandardError()).trimmed();
        p->deleteLater();
        if (st == QProcess::NormalExit && code == 0 && QFileInfo(tmp).size() > 0 && QFile::rename(tmp, out)) emit ready(id, out);
        else { QFile::remove(tmp); emit failed(id, err.isEmpty() ? "FFmpeg stopped." : err.split('\n').last()); }
    });
    connect(p, &QProcess::errorOccurred, this, [=](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        m_running.remove(id);
        p->deleteLater();
        emit failed(id, "FFmpeg couldn't be started.");
    });
    // video only (sound comes from the original through the mixer); autorotate bakes phone rotation in; short GOP = fast seeking
    p->start(ff, {"-y", "-hide_banner", "-loglevel", "error", "-i", a.path, "-an", "-vf", "scale=-2:720", "-c:v", "libx264", "-preset", "veryfast",
                  "-crf", "24", "-g", "12", "-pix_fmt", "yuv420p", "-f", "mp4", tmp});
    return true;
}

} // namespace ve
