#include "TextRender.h"
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace ve {

static QMutex g_mx;   // fonts and QPainter text are not guaranteed thread-safe: preview and export threads take turns

namespace {
struct Layout {
    QPainterPath path;     // centred on the origin
    QSizeF size;
};

Layout layoutText(const TextStyle& s) {
    QFont f(s.font);
    f.setPixelSize(std::max(1, int(std::lround(s.size))));
    f.setBold(s.bold);
    f.setItalic(s.italic);
    const QFontMetricsF fm(f);
    const QStringList lines = s.text.split('\n');
    const double lineH = fm.lineSpacing();
    QVector<double> widths;
    double maxW = 0;
    for (const QString& l : lines) { const double w = fm.horizontalAdvance(l.isEmpty() ? QString(" ") : l); widths.push_back(w); maxW = std::max(maxW, w); }
    Layout L;
    for (int i = 0; i < lines.size(); ++i) {
        const double x = (maxW - widths[i]) / 2.0, baseline = i * lineH + fm.ascent();
        if (lines[i].isEmpty()) continue;
        L.path.addText(QPointF(x, baseline), f, lines[i]);
        if (s.underline) L.path.addRect(QRectF(x, baseline + fm.underlinePos(), widths[i], std::max(1.0, fm.lineWidth())));
    }
    const QRectF rect(0, 0, maxW, lines.size() * lineH);
    L.path.translate(-rect.center());
    const double pad = std::max(0.0, s.strokeW);
    L.size = QSizeF(std::max(1.0, rect.width() + 2 * pad), std::max(1.0, rect.height() + 2 * pad));
    return L;
}
} // namespace

QSizeF textBounds(const TextStyle& s) {
    QMutexLocker lk(&g_mx);
    return layoutText(s).size;
}

QImage renderText(const TextStyle& s, const QSize& canvas, const QSize& imageSize, double* expand) {
    QMutexLocker lk(&g_mx);
    static QHash<QString, QPair<QImage, double>> cache;
    const QString key = QString("%1|%2|%3|%4%5%6|%7,%8,%9|%10|%11,%12,%13|%14x%15|%16x%17")
                            .arg(s.text, s.font).arg(s.size).arg(s.bold).arg(s.italic).arg(s.underline).arg(s.r).arg(s.g).arg(s.b)
                            .arg(s.strokeW).arg(s.sr).arg(s.sg).arg(s.sb).arg(canvas.width()).arg(canvas.height()).arg(imageSize.width()).arg(imageSize.height());
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) { if (expand) *expand = it->second; return it->first; }

    const Layout L = layoutText(s);
    // The image covers a "virtual canvas" `e` times the real one, so text bigger than the canvas is never clipped.
    const double e = std::max({1.0, (L.size.width() + 8) / std::max(1, canvas.width()), (L.size.height() + 8) / std::max(1, canvas.height())});
    double q = imageSize.width() / double(std::max(1, canvas.width()));              // image pixels per project pixel
    q = std::min({q, 4096.0 / (canvas.width() * e), 4096.0 / (canvas.height() * e)});   // keep the image a sane size
    const QSize px(std::max(1, int(std::lround(canvas.width() * e * q))), std::max(1, int(std::lround(canvas.height() * e * q))));

    QImage im(px, QImage::Format_RGBA8888_Premultiplied);
    im.fill(Qt::transparent);
    QPainter p(&im);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.scale(q, q);
    p.translate(canvas.width() * e / 2.0, canvas.height() * e / 2.0);
    if (s.strokeW > 0) {                                  // outline first (twice the width, so `strokeW` shows outside the fill)
        p.setPen(QPen(QColor::fromRgbF(float(s.sr), float(s.sg), float(s.sb)), s.strokeW * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(L.path);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor::fromRgbF(float(s.r), float(s.g), float(s.b)));
    p.drawPath(L.path);
    p.end();
    const QImage out = im.convertToFormat(QImage::Format_RGBA8888);   // straight alpha, as the compositor expects
    if (cache.size() > 24) cache.clear();
    cache.insert(key, {out, e});
    if (expand) *expand = e;
    return out;
}

} // namespace ve
