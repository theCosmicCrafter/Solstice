/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_LENS_BLUR_HIGHLIGHTS_H
#define KIS_LENS_BLUR_HIGHLIGHTS_H

#include <QRect>
#include <kis_convolution_painter.h>
#include <kis_types.h>
class KoUpdater;

namespace KisLensBlurHighlights
{
double boundedPercent(const KisFilterConfigurationSP config, const char *key);
void apply(KisPaintDeviceSP device,
           const QRect &rect,
           const QRect &sourceRect,
           const KisFilterConfigurationSP config,
           KisConvolutionKernelSP kernel,
           KoUpdater *progress,
           KisConvolutionPainter::EnginePreference engine = KisConvolutionPainter::NONE);
} // namespace KisLensBlurHighlights
#endif
