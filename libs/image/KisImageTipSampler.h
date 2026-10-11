/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISIMAGETIPSAMPLER_H
#define KISIMAGETIPSAMPLER_H

#include <QImage>
#include <QSharedPointer>
#include <QSize>
#include <QVector>

#include "kritaimage_export.h"

/**
 * GPU engine (Solstice, "Image tip dabs"): a transformed brush tip image
 * (KisQImagePyramid::createImage()), described so that the CPU and the GPU
 * brush compute the same pixels. Row y of the dab is drawn from rows[y].x to
 * rows[y].x + length - 1 with the 16.16 source position (fx, fy) at its
 * first pixel, advancing by (fdx, fdy) per pixel; other pixels stay
 * transparent (0). Every drawn pixel is the bilinear sample of the bordered
 * ARGB32 pyramid level in 16 bits per premultiplied channel, stored as
 * ARGB32. See KisImageTipSampler.cpp for the derivation from Qt and
 * docs/agent/gpu-engine.md, "Image tip dabs".
 */
struct KRITAIMAGE_EXPORT KisImageTipGeometry {
    struct Row {
        int x = 0;
        int length = 0;
        int fx = 0;
        int fy = 0;
    };
    QImage source; // the bordered ARGB32 pyramid level (implicitly shared)
    /// source as QRgba64::fromArgb32().premultiplied(), row-major
    QSharedPointer<const QVector<quint64>> premultiplied;
    QSize size; // the dab size
    bool identity = false; // the level's interior, untransformed
    /// Every visible texel has red == green == blue (isGray()), so one
    /// color channel is unpremultiplied for all three (the same result).
    bool gray = false;
    int fdx = 0;
    int fdy = 0;
    QVector<Row> rows; // one per dab row
};

namespace KisImageTipSampler
{
/// One pixel of render() (dab coordinates), ARGB32.
KRITAIMAGE_EXPORT quint32 pixel(const KisImageTipGeometry &geometry, int x, int y);
/// The ARGB32 dab image of @p geometry.
KRITAIMAGE_EXPORT QImage render(const KisImageTipGeometry &geometry);
/// The premultiplied QRgba64 copy of a bordered ARGB32 level.
KRITAIMAGE_EXPORT QSharedPointer<const QVector<quint64>> premultiply(const QImage &level);
/// Whether every texel of an ARGB32 level with a nonzero alpha has
/// red == green == blue (KisImageTipGeometry::gray).
KRITAIMAGE_EXPORT bool isGray(const QImage &level);
/// Whether render() uses its AVX2 kernels (same results, 4 pixels at a
/// time); true when the CPU supports AVX2.
KRITAIMAGE_EXPORT bool avx2Enabled();
/// Tests: compare the AVX2 and SSE2 kernels. Ignored without AVX2.
KRITAIMAGE_EXPORT void setAvx2EnabledForTesting(bool enabled);
} // namespace KisImageTipSampler

#endif
