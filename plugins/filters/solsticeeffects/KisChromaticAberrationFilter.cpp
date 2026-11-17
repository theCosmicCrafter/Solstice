/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisChromaticAberrationFilter.h"
#include "KisChromaticAberrationConfigWidget.h"
#include "KisCpuEffectKernels.h"
#include "KisEffectColorSampler.h"

#include <KisSequentialIteratorProgress.h>
#include <KoChannelInfo.h>
#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpaceRegistry.h>
#include <KoUpdater.h>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_paint_device.h>
#include <kis_painter.h>
#include <kis_random_accessor_ng.h>

#include <cmath>
#include <cstring>
#include <memory>

namespace
{
double finiteValue(const KisFilterConfigurationSP config, const char *key, double fallback)
{
    const double value = config->getDouble(key, fallback);
    return std::isfinite(value) ? value : fallback;
}

double strength(const KisFilterConfigurationSP config)
{
    return qBound(0.0, finiteValue(config, "strength", 3.0), 100.0);
}

} // namespace

KisChromaticAberrationFilter::KisChromaticAberrationFilter()
    : KisFilter(KoID("solstice_chromatic_aberration", i18n("Chromatic Aberration")),
                FiltersCategoryArtisticId,
                i18n("Chromatic &Aberration..."))
{
    setSupportsThreading(true);
    setSupportsAdjustmentLayers(true);
    setSupportsLevelOfDetail(false);
    setSupportsPainting(false);
}

KisFilterConfigurationSP
KisChromaticAberrationFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    auto config = factoryConfiguration(resourcesInterface);
    config->setProperty("mode", 0);
    config->setProperty("strength", 3.0);
    config->setProperty("angle", 0.0);
    // The widget initializes missing radial geometry from the canvas, then
    // saves it explicitly. Programmatic configurations use (0,0), radius 1000.
    return config;
}

KisConfigWidget *
KisChromaticAberrationFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool) const
{
    return new KisChromaticAberrationConfigWidget(parent, device);
}

QRect KisChromaticAberrationFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    return SolsticeEffects::chromaticNeededRect(rect, config);
}

QRect SolsticeEffects::chromaticNeededRect(const QRect &rect, const KisFilterConfigurationSP config)
{
    const double amount = strength(config);
    const int halo = amount > 0.0 ? int(std::ceil(amount)) + 1 : 0;
    return rect.adjusted(-halo, -halo, halo, halo);
}

QRect KisChromaticAberrationFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    // Both mappings have bounded displacement, so this also bounds the
    // inverse dependency for changes under a filter layer or mask.
    return neededRect(rect, config, lod);
}

void KisChromaticAberrationFilter::processImpl(KisPaintDeviceSP device,
                                               const QRect &rect,
                                               const KisFilterConfigurationSP config,
                                               KoUpdater *progressUpdater) const
{
    SolsticeEffects::applyChromaticAberration(device, rect, config, progressUpdater);
}

void SolsticeEffects::applyChromaticAberration(KisPaintDeviceSP device,
                                               const QRect &rect,
                                               const KisFilterConfigurationSP config,
                                               KoUpdater *progressUpdater,
                                               KoProgressProxy *reporter)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(device && config);
    const double amount = strength(config);
    if (rect.isEmpty() || amount == 0.0)
        return;

    ColorSampler sampler(device);
    KIS_SAFE_ASSERT_RECOVER_RETURN(sampler.red >= 0 && sampler.blue >= 0);
    const auto *cs = device->colorSpace();
    const auto channels = cs->channels();
    const QBitArray flags = config->channelFlags();
    QVector<int> enabled;
    for (int i = 0; i < channels.size(); ++i) {
        if (channels[i]->channelType() == KoChannelInfo::COLOR
            && (sampler.toRgb || i == sampler.red || i == sampler.blue)
            && (flags.isEmpty() || (i < flags.size() && flags.testBit(i)))) {
            enabled.append(i);
        }
    }
    if (enabled.isEmpty())
        return;

    const bool radial = config->getInt("mode", 0) == 1;
    const double angle = std::remainder(finiteValue(config, "angle", 0.0), 360.0) * std::acos(-1.0) / 180.0;
    const double centerX = qBound(-1e9, finiteValue(config, "centerX", 0.0), 1e9);
    const double centerY = qBound(-1e9, finiteValue(config, "centerY", 0.0), 1e9);
    const double radius = qBound(1.0, finiteValue(config, "radius", 1000.0), 1e9);
    const QPointF translation(amount * std::cos(angle), amount * std::sin(angle));

    // A separate patch output also makes a single in-place API call safe
    // without a transaction. Multiple patches share the framework's source
    // transaction, and all source reads must use oldRawData().
    KisPaintDeviceSP output = new KisPaintDevice(cs);
    output->setX(device->x());
    output->setY(device->y());
    output->setDefaultPixel(device->defaultPixel());
    auto original = device->createRandomConstAccessorNG();
    QVector<float> values(sampler.working->channelCount());
    QByteArray converted(cs->pixelSize(), '\0');
    {
        KisSequentialIteratorProgress it(output, rect, reporter ? reporter : progressUpdater);
        while (it.nextPixel()) {
            if (it.x() == rect.x() && progressUpdater && progressUpdater->interrupted())
                return;
            original->moveTo(it.x(), it.y());
            const quint8 *pixel = original->oldRawData();
            std::memcpy(it.rawData(), pixel, cs->pixelSize());
            if (!(cs->opacityF(pixel) > 0.0))
                continue;
            QPointF offset = translation;
            if (radial) {
                const double x = it.x() - centerX;
                const double y = it.y() - centerY;
                const double scale = amount / qMax(radius, std::hypot(x, y));
                offset = QPointF(x * scale, y * scale);
            }
            if (offset.isNull())
                continue;
            sampler.decode(pixel, values);
            values[sampler.red] =
                sampler.sample(it.x() - offset.x(), it.y() - offset.y(), sampler.red, values[sampler.red]);
            values[sampler.blue] =
                sampler.sample(it.x() + offset.x(), it.y() + offset.y(), sampler.blue, values[sampler.blue]);
            sampler.encode(values, reinterpret_cast<quint8 *>(converted.data()));
            for (int index : enabled) {
                const auto *channel = channels[index];
                std::memcpy(it.rawData() + channel->pos(), converted.constData() + channel->pos(), channel->size());
            }
        }
    }
    original.clear();
    sampler.source.clear();
    if (progressUpdater && progressUpdater->interrupted())
        return;
    KisPainter::copyAreaOptimized(rect.topLeft(), output, device, rect);
}
