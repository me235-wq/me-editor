#include "Overview.h"
#include "AudioMixer.h"
#include "Decoder.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace ve {

AssetOverview buildOverview(const MediaAsset& a) {
    AssetOverview o;
    if (a.missing) return o;
    if (a.hasVideo) {
        Decoder d;
        if (d.open(a.path, nullptr, false)) {
            d.setMaxSize(QSize(256, 144));
            const int n = a.isImage ? 1 : std::clamp(int(std::ceil(a.duration / 2.0)), 6, 90);
            o.thumbStep = a.isImage || a.duration <= 0 ? 1.0 : a.duration / n;
            for (int i = 0; i < n; ++i) {
                const QImage f = d.frameAt(a.isImage ? 0.0 : (i + 0.5) * o.thumbStep);
                o.thumbs.push_back(f.isNull() ? (o.thumbs.isEmpty() ? QImage() : o.thumbs.last()) : f.scaledToHeight(54, Qt::SmoothTransformation));
            }
        }
    }
    if (a.hasAudio && !a.isImage) {
        AudioSource src;
        if (src.open(a.path)) {
            const int bin = int(std::lround(o.peakStep * kMixRate));          // 960 samples
            const int chunk = bin * 50;                                       // read one second at a time
            std::vector<float> buf(size_t(chunk) * 2);
            const qint64 total = qint64(std::ceil(a.duration * kMixRate));
            for (qint64 pos = 0; pos < total; pos += chunk) {
                src.read(pos, chunk, buf.data());
                for (int b = 0; b < 50; ++b) {
                    float peak = 0;
                    for (int i = b * bin; i < (b + 1) * bin; ++i) peak = std::max({peak, std::abs(buf[size_t(i) * 2]), std::abs(buf[size_t(i) * 2 + 1])});
                    o.peaks.push_back(quint8(std::lround(std::pow(std::min(1.0f, peak), 0.6f) * 255.0f)));
                }
            }
            o.peaks.resize(int(std::min<qint64>(o.peaks.size(), total / bin + 1)));
        }
    }
    return o;
}

} // namespace ve
