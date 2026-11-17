/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_CPU_EFFECT_KERNELS_H
#define KIS_CPU_EFFECT_KERNELS_H

#include <QRect>
#include <kis_types.h>

class KoUpdater;
class KoProgressProxy;

namespace SolsticeEffects
{
// Plugin-local CPU kernels. No dialogs, registry lookups or undo commands.
// Grain reads current pixels; resampling reads the source transaction's old
// pixels. Multi-stage callers must use their own non-transactional scratch
// device so later stages see the preceding stage's output.
void applyGrain(KisPaintDeviceSP device,
                const QRect &rect,
                const KisFilterConfigurationSP config,
                KoUpdater *progress,
                KoProgressProxy *reporter = nullptr);
void applyChromaticAberration(KisPaintDeviceSP device,
                              const QRect &rect,
                              const KisFilterConfigurationSP config,
                              KoUpdater *progress,
                              KoProgressProxy *reporter = nullptr);
QRect chromaticNeededRect(const QRect &rect, const KisFilterConfigurationSP config);
} // namespace SolsticeEffects

#endif
