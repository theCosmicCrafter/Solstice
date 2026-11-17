/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisRemoveDustFilter.h"
#include "KisRemoveDustConfigWidget.h"
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoUpdater.h>
#include <cmath>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
namespace
{
const QString transparencyId = QStringLiteral("transparency");
const QString whiteId = QStringLiteral("white");
const QString gapsSurroundingId = QStringLiteral("gaps_surrounding");
const QString gapsColorId = QStringLiteral("gaps_color");
QString modeId(const KisPropertiesConfigurationSP config)
{
    const QString id = config->getString("mode", transparencyId);
    return KisRemoveDustFilter::modeIds().contains(id) ? id : transparencyId;
}
} // namespace
KisRemoveDustFilter::KisRemoveDustFilter()
    : KisFilter(KoID("solstice_remove_dust", i18n("Remove Dust")), FiltersCategoryEnhanceId, i18n("&Remove Dust..."))
{
    setColorSpaceIndependence(FULLY_INDEPENDENT);
    setSupportsThreading(true);
    setSupportsAdjustmentLayers(true);
    setSupportsPainting(false);
    setSupportsLevelOfDetail(false);
}
QStringList KisRemoveDustFilter::modeIds()
{
    return {transparencyId, whiteId, gapsSurroundingId, gapsColorId};
}
KisDustDetector::Options KisRemoveDustFilter::options(const KisPropertiesConfigurationSP config)
{
    KisDustDetector::Options options;
    const QString id = modeId(config);
    options.mode = id == whiteId ? KisDustDetector::DustOnWhite
        : id == transparencyId   ? KisDustDetector::DustOnTransparency
                                 : KisDustDetector::TransparentGaps;
    options.dustSize =
        qBound(KisDustDetector::minimumDustSize, config->getInt("dustSize", 4), KisDustDetector::maximumDustSize);
    const double tolerance = config->getDouble("tolerance", KisDustDetector::defaultTolerancePercent);
    options.tolerance = qBound(0.0, std::isfinite(tolerance) ? tolerance : 0.0, 100.0) / 100;
    return options;
}
KisDustDetector::Replacement KisRemoveDustFilter::replacement(const KisPropertiesConfigurationSP config)
{
    return modeId(config) == gapsColorId ? KisDustDetector::FixedColor
                                         : KisDustDetector::defaultReplacement(options(config).mode);
}
KisFilterConfigurationSP KisRemoveDustFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    auto config = factoryConfiguration(resourcesInterface);
    config->setProperty("mode", transparencyId);
    config->setProperty("dustSize", 4);
    config->setProperty("tolerance", KisDustDetector::defaultTolerancePercent);
    config->setProperty("color", QVariant::fromValue(KoColor(Qt::black, KoColorSpaceRegistry::instance()->rgb8())));
    return config;
}
KisConfigWidget *
KisRemoveDustFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool) const
{
    return new KisRemoveDustConfigWidget(parent, device);
}
bool KisRemoveDustFilter::needsTransparentPixels(const KisFilterConfigurationSP config, const KoColorSpace *) const
{
    // Only the gap modes paint where the layer is transparent.
    return options(config).mode == KisDustDetector::TransparentGaps;
}
QRect KisRemoveDustFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    return KisDustDetector::neededRect(rect, options(config).dustSize);
}
QRect KisRemoveDustFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}
void KisRemoveDustFilter::processImpl(KisPaintDeviceSP device,
                                      const QRect &rect,
                                      const KisFilterConfigurationSP config,
                                      KoUpdater *progress) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(device && config);
    if (rect.isEmpty())
        return;
    const auto components = KisDustDetector::detect(device, rect, options(config), progress);
    if (progress && progress->interrupted())
        return;
    const KoColor color = config->getColor("color", KoColor(Qt::black, device->colorSpace()));
    KisDustDetector::apply(device, components, replacement(config), color, rect);
    if (progress)
        progress->setProgress(100);
}
