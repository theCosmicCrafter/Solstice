/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_SHARPNESS_FILTER_H
#define KIS_SHARPNESS_FILTER_H
#include <filter/kis_filter.h>
class KisSharpnessFilter : public KisFilter
{
public:
    KisSharpnessFilter();
    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const override;
    KisConfigWidget *
    createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool useForMasks) const override;
    QRect neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;
    QRect changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;
    void processImpl(KisPaintDeviceSP device,
                     const QRect &rect,
                     const KisFilterConfigurationSP config,
                     KoUpdater *progress) const override;
    // Also used by Color Filter on its private floating-point intermediate.
    static void applyCpu(KisPaintDeviceSP device,
                         const QRect &rect,
                         const KisFilterConfigurationSP config,
                         KoUpdater *progress,
                         int progressStart = 0,
                         int progressSpan = 100);
};
#endif
