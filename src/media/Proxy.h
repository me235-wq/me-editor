#pragma once
#include "core/Project.h"
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QSet>

namespace ve {

// Lightweight copies of heavy videos (720p H.264, short keyframe spacing) used ONLY for the live preview.
// Export always reads the originals. Proxies are made on demand, in the background, and kept between sessions.
class ProxyManager : public QObject {
    Q_OBJECT
public:
    explicit ProxyManager(const QString& folder, QObject* parent = nullptr);
    static bool worthProxying(const MediaAsset& a);   // a video bigger than about 1440p
    QString existing(const MediaAsset& a) const;      // path of a finished proxy, or ""
    bool busy(const MediaAsset& a) const { return m_running.contains(a.id); }
    bool request(const MediaAsset& a);                // starts building one; false if not needed / already there / no FFmpeg
    QString proxyPathFor(const MediaAsset& a) const;

signals:
    void ready(const QString& assetId, const QString& proxyPath);
    void failed(const QString& assetId, const QString& reason);

private:
    QString m_dir;
    QSet<QString> m_running;
};

} // namespace ve
