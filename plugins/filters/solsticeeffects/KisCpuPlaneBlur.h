/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_CPU_PLANE_BLUR_H
#define KIS_CPU_PLANE_BLUR_H
#include <QRect>
#include <vector>
class KoUpdater;
namespace SolsticeEffects
{
struct FloatPlane {
    QRect rect;
    std::vector<float> values;
    explicit FloatPlane(const QRect &bounds);
    float &at(int x, int y);
    float at(int x, int y) const;
};
// Scalar CPU convolution only. No implicit alpha weighting/unpremultiplication.
// Callers provide the complete halo; output is unchanged on cancellation.
int gaussianHalo(double sigma);
bool gaussianBlur(const FloatPlane &source, FloatPlane &output, double sigma, KoUpdater *progress = nullptr);
// Gaussian through sigma 2; three variance-matched boxes beyond that.
// Includes the numerical rebase footprint needed for bit-identical patches.
int softBlurHalo(double sigma);
bool softBlur(const FloatPlane &source, FloatPlane &output, double sigma, KoUpdater *progress = nullptr);
// Square grayscale dilation in O(pixels), using prefix/suffix block maxima.
// Finite inputs only; caller supplies radius pixels on every side.
bool maximumFilter(const FloatPlane &source, FloatPlane &output, int radius, KoUpdater *progress = nullptr);
} // namespace SolsticeEffects
#endif
