#pragma once
#include "Project.h"
#include <QImage>
#include <QSizeF>

namespace ve {

// Size of a text object's frame (the text plus its outline) in project pixels, before the clip's own scale.
QSizeF textBounds(const TextStyle& s);

// Rasterises the text into a transparent image with the canvas' aspect ratio (text centred), so the compositor
// can treat it like any other picture: position / size / rotation / effects all come from the clip.
// `imageSize` may be smaller than `canvas` (preview quality); the text is scaled to match. Thread-safe.
//
// Text wider (or taller) than the canvas is NOT cut off: the image then covers a "virtual canvas" that is `*expand` times bigger,
// and the caller must multiply the layer's scale by `*expand` so the text keeps its real size on screen.
QImage renderText(const TextStyle& s, const QSize& canvas, const QSize& imageSize, double* expand = nullptr);

} // namespace ve
