/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SEGMENTATION_MATTE_UTILS_H_
#define SEGMENTATION_MATTE_UTILS_H_

#include <QRect>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace VisionMLMatte
{

/**
 * Modulate soft matte with rasterized polygon mask coverage and source image alpha.
 * Formula: out = round(matte * mask * alpha / (255*255))
 * Uses 32-bit unsigned integer arithmetic to avoid overflow and provides proper rounding.
 */
inline quint8 modulateMatteAlpha(quint8 matte, quint8 mask, quint8 alpha)
{
    const quint32 denom = 255u * 255u;
    const quint32 num =
        static_cast<quint32>(matte) * static_cast<quint32>(mask) * static_cast<quint32>(alpha) + denom / 2;
    return static_cast<quint8>(num / denom);
}

/**
 * Expands region so that width and height are at least minSize (e.g. 64 px),
 * expanding symmetrically around the center where possible, and clamped to bounds.
 * If bounds itself is smaller than minSize in either dimension, this function does nothing.
 */
inline void expandRegionToMin(QRect &region, const QRect &bounds, int minSize)
{
    if (bounds.width() < minSize || bounds.height() < minSize) {
        return;
    }
    if (region.width() < minSize) {
        const int delta = minSize - region.width();
        int left = region.left() - delta / 2;
        if (left < bounds.left()) {
            left = bounds.left();
        }
        if (left + minSize > bounds.x() + bounds.width()) {
            left = bounds.x() + bounds.width() - minSize;
        }
        region.setRect(left, region.y(), minSize, region.height());
    }
    if (region.height() < minSize) {
        const int delta = minSize - region.height();
        int top = region.top() - delta / 2;
        if (top < bounds.top()) {
            top = bounds.top();
        }
        if (top + minSize > bounds.y() + bounds.height()) {
            top = bounds.y() + bounds.height() - minSize;
        }
        region.setRect(region.x(), top, region.width(), minSize);
    }
}

/// Inference crop budget, ~9.4 MP (3072 x 3072): caps memory and the
/// single-threaded matte loops. Area-based so that thin regions
/// (e.g. 20000 x 64) are not needlessly downscaled.
constexpr qint64 kMaxInferencePixels = 3072LL * 3072LL;

/// Smallest dimension the model accepts after downscaling.
constexpr int kMinInferenceDim = 64;

/**
 * Downscale factor in (0,1] fitting `size` within maxPixels while keeping
 * both edges at or above minDim where possible. Returns 1.0 when no
 * downscale is needed. The matte produced at the scaled size is upsampled
 * back to full region size for compositing.
 */
inline double downscaleFactor(const QSize &size, qint64 maxPixels, int minDim)
{
    const qint64 px = static_cast<qint64>(size.width()) * size.height();
    if (px <= maxPixels || px <= 0) {
        return 1.0;
    }
    double scale = std::sqrt(static_cast<double>(maxPixels) / px);
    const int shortest = std::min(size.width(), size.height());
    if (shortest > 0) {
        scale = std::max(scale, static_cast<double>(minDim) / shortest);
    }
    return std::min(scale, 1.0);
}

/**
 * Target size for `downscaleFactor` output; each edge is at least 1 px.
 */
inline QSize scaledSizeFor(const QSize &size, double scale)
{
    return {std::max(1, qRound(size.width() * scale)), std::max(1, qRound(size.height() * scale))};
}

} // namespace VisionMLMatte

#endif // SEGMENTATION_MATTE_UTILS_H_
