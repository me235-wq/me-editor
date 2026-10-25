#pragma once
#include "core/Project.h"
#include <QImage>
#include <QVector>

namespace ve {

// What the timeline draws inside a clip: a filmstrip of thumbnails and the audio waveform.
struct AssetOverview {
    QVector<QImage> thumbs;      // evenly spaced through the file, 54 px high
    double thumbStep = 1;        // seconds between thumbnails
    QVector<quint8> peaks;       // loudest sample per time slice, 0..255 (lifted so quiet audio is still visible)
    double peakStep = 0.02;      // seconds per slice
};

// Slow (decodes the file): run it on a worker thread.
AssetOverview buildOverview(const MediaAsset& a);

} // namespace ve
