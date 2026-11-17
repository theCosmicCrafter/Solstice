/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_REMOVE_DUST_FILTER_H
#define KIS_REMOVE_DUST_FILTER_H
#include <KisDustDetector.h>
#include <filter/kis_filter.h>
class KisRemoveDustFilter : public KisFilter
{
public:
    KisRemoveDustFilter();
    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const override;
    KisConfigWidget *
    createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool useForMasks) const override;
    bool needsTransparentPixels(const KisFilterConfigurationSP config, const KoColorSpace *cs) const override;
    QRect neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;
    QRect changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;
    void processImpl(KisPaintDeviceSP device,
                     const QRect &rect,
                     const KisFilterConfigurationSP config,
                     KoUpdater *progress) const override;

    /// Persisted values of the "mode" property
    static QStringList modeIds();
    static KisDustDetector::Options options(const KisPropertiesConfigurationSP config);
    static KisDustDetector::Replacement replacement(const KisPropertiesConfigurationSP config);
};
#endif
