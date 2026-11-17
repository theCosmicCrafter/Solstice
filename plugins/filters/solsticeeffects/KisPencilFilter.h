/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_PENCIL_FILTER_H
#define KIS_PENCIL_FILTER_H
#include <filter/kis_filter.h>

class KisPencilFilter : public KisFilter
{
public:
    KisPencilFilter();
    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const override;
    KisConfigWidget *
    createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool useForMasks) const override;
    QRect neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;
    QRect changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;
    void processImpl(KisPaintDeviceSP device,
                     const QRect &rect,
                     const KisFilterConfigurationSP config,
                     KoUpdater *progressUpdater) const override;
};
#endif
