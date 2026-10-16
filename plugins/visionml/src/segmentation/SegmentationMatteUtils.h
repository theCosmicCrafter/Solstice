/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SEGMENTATION_MATTE_UTILS_H_
#define SEGMENTATION_MATTE_UTILS_H_

#include <QRect>
#include <QtGlobal>
#include <algorithm>

namespace VisionMLMatte
{

/**
 * Modulate soft matte with rasterized polygon mask coverage and source image alpha.
 * Formula: out = (matte * mask * alpha + 127*255) / (255*255)
 * Uses 32-bit unsigned integer arithmetic to avoid overflow and provides proper rounding.
 */
inline quint8 modulateMatteAlpha(quint8 matte, quint8 mask, quint8 alpha)
{
    const quint32 num = static_cast<quint32>(matte) * static_cast<quint32>(mask) * static_cast<quint32>(alpha) + (127u * 255u);
    const quint32 denom = 255u * 255u;
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

/**
 * Clamps region so that width and height do not exceed maxSize (e.g. 3072 px),
 * clamped symmetrically around the center where possible, and bounded within bounds.
 * Returns true if the region was clamped in either dimension.
 */
inline bool clampRegionToMax(QRect &region, const QRect &bounds, int maxSize)
{
    bool clamped = false;
    if (region.width() > maxSize) {
        clamped = true;
        const int centerX = region.center().x();
        int left = centerX - maxSize / 2;
        if (left < bounds.left()) {
            left = bounds.left();
        }
        if (left + maxSize > bounds.x() + bounds.width()) {
            left = std::max(bounds.left(), bounds.x() + bounds.width() - maxSize);
        }
        const int width = std::min(maxSize, bounds.width());
        region.setRect(left, region.y(), width, region.height());
    }
    if (region.height() > maxSize) {
        clamped = true;
        const int centerY = region.center().y();
        int top = centerY - maxSize / 2;
        if (top < bounds.top()) {
            top = bounds.top();
        }
        if (top + maxSize > bounds.y() + bounds.height()) {
            top = std::max(bounds.top(), bounds.y() + bounds.height() - maxSize);
        }
        const int height = std::min(maxSize, bounds.height());
        region.setRect(region.x(), top, region.width(), height);
    }
    return clamped;
}

} // namespace VisionMLMatte

#endif // SEGMENTATION_MATTE_UTILS_H_
