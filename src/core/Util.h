#pragma once
#include <QString>
#include <cmath>

namespace ve {
// hh:mm:ss:ff timecode
inline QString formatTimecode(double t, double fps) {
    if (t < 0) t = 0;
    const int f = qMax(1, int(std::lround(fps > 0 ? fps : 30.0)));
    qint64 whole = qint64(std::floor(t));
    int ff = int(std::lround((t - double(whole)) * f));
    if (ff >= f) { ff = 0; ++whole; }
    return QString("%1:%2:%3:%4")
        .arg(whole / 3600, 2, 10, QChar('0'))
        .arg((whole / 60) % 60, 2, 10, QChar('0'))
        .arg(whole % 60, 2, 10, QChar('0'))
        .arg(ff, 2, 10, QChar('0'));
}
} // namespace ve

namespace ve {
// Canvas size for an aspect ratio, where `shortSide` is the smaller dimension (1080 → "1080p"). Both sides even.
inline void sizeForRatio(int rw, int rh, int shortSide, int* w, int* h) {
    rw = qMax(1, rw); rh = qMax(1, rh);
    double W, H;
    if (rw >= rh) { H = shortSide; W = shortSide * double(rw) / rh; }
    else          { W = shortSide; H = shortSide * double(rh) / rw; }
    *w = qMax(2, int(std::lround(W / 2.0)) * 2);
    *h = qMax(2, int(std::lround(H / 2.0)) * 2);
}
} // namespace ve
