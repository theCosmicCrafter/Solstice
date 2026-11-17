/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_FILM_GRAIN_FILTER_H
#define KIS_FILM_GRAIN_FILTER_H

#include <filter/kis_filter.h>

class KisFilmGrainFilter : public KisFilter
{
public:
    KisFilmGrainFilter();

    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const override;
    KisConfigWidget *
    createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool useForMasks) const override;
    void processImpl(KisPaintDeviceSP device,
                     const QRect &rect,
                     const KisFilterConfigurationSP config,
                     KoUpdater *progressUpdater) const override;
};

#endif
