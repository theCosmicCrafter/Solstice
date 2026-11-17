/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_FILM_FILTER_H
#define KIS_FILM_FILTER_H

#include <filter/kis_filter.h>

class KisFilmFilter : public KisFilter
{
public:
    enum Preset {
        Custom = 0,
        Vintage = 1,
        Modern = 2,
        Warm = 3
    };
    enum Tone {
        None = 0,
        Fade = 1,
        Sepia = 2,
        WarmTone = 3,
        Cool = 4
    };

    KisFilmFilter();
    // Presets are expanded only on explicit selection, never while rendering
    // or loading saved parameters. Seed and document geometry are retained.
    static void applyPreset(KisPropertiesConfigurationSP config, int preset);
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
