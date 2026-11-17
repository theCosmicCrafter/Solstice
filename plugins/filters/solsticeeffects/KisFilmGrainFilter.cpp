/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisFilmGrainFilter.h"
#include "KisCpuEffectKernels.h"
#include "KisFilmGrainConfigWidget.h"

#include <KisRandomGenerator2D.h>
#include <KisSequentialIteratorProgress.h>
#include <KoChannelInfo.h>
#include <KoUpdater.h>
#include <QRandomGenerator>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_paint_device.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

KisFilmGrainFilter::KisFilmGrainFilter()
    : KisFilter(KoID("solstice_film_grain", i18n("Film Grain")), FiltersCategoryOtherId, i18n("Film &Grain..."))
{
    setColorSpaceIndependence(FULLY_INDEPENDENT);
    setSupportsThreading(true);
    setSupportsAdjustmentLayers(true);
    // Grain is defined at full-resolution pixel coordinates. Do not invent
    // a different grain field in a reduced-resolution preview.
    setSupportsLevelOfDetail(false);
    setSupportsPainting(false);
}

KisFilterConfigurationSP KisFilmGrainFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    auto config = factoryConfiguration(resourcesInterface);
    config->setProperty("strength", 30.0);
    config->setProperty("monochrome", false);
    config->setProperty("seed", QRandomGenerator::global()->bounded(std::numeric_limits<int>::max()));
    return config;
}

KisConfigWidget *KisFilmGrainFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP, bool) const
{
    return new KisFilmGrainConfigWidget(parent);
}

void KisFilmGrainFilter::processImpl(KisPaintDeviceSP device,
                                     const QRect &rect,
                                     const KisFilterConfigurationSP config,
                                     KoUpdater *progressUpdater) const
{
    SolsticeEffects::applyGrain(device, rect, config, progressUpdater);
}

void SolsticeEffects::applyGrain(KisPaintDeviceSP device,
                                 const QRect &rect,
                                 const KisFilterConfigurationSP config,
                                 KoUpdater *progressUpdater,
                                 KoProgressProxy *reporter)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(device && config);
    const double strength = config->getDouble("strength", 30.0);
    if (!std::isfinite(strength) || strength <= 0.0 || rect.isEmpty()) {
        return;
    }
    const double amplitude = qMin(strength, 100.0) / 200.0;
    const bool monochrome = config->getBool("monochrome", false);
    const quint64 seed = static_cast<quint32>(qMax(0, config->getInt("seed", 0)));
    const KoColorSpace *cs = device->colorSpace();
    const auto channels = cs->channels();
    const QBitArray flags = config->channelFlags();

    QVector<int> enabled;
    std::vector<std::unique_ptr<KisRandomGenerator2D>> random;
    for (int i = 0; i < channels.size(); ++i) {
        if (channels[i]->channelType() == KoChannelInfo::COLOR
            && (flags.isEmpty() || (i < flags.size() && flags.testBit(i)))) {
            enabled.append(i);
            // Use display order, not byte order: RGB float and BGRA integer
            // devices should use the same color-channel random fields.
            const quint64 salt = monochrome ? 0 : quint64(channels[i]->displayPosition() + 1) * 0x9e3779b97f4a7c15ULL;
            random.emplace_back(new KisRandomGenerator2D(seed ^ salt));
        }
    }
    if (enabled.isEmpty()) {
        return;
    }

    QVector<float> values(cs->channelCount());
    QByteArray converted(cs->pixelSize(), '\0');
    KisSequentialIteratorProgress it(device, rect, reporter ? reporter : progressUpdater);
    while (it.nextPixel()) {
        if (it.x() == rect.x() && progressUpdater && progressUpdater->interrupted()) {
            return;
        }
        if (cs->opacityF(it.rawDataConst()) == 0.0) {
            continue;
        }
        cs->normalisedChannelsValue(it.rawDataConst(), values);
        const double commonNoise = monochrome ? 2.0 * random.front()->doubleRandomAt(it.x(), it.y()) - 1.0 : 0.0;
        for (int n = 0; n < enabled.size(); ++n) {
            const int index = enabled[n];
            if (!std::isfinite(values[index])) {
                continue;
            }
            const double noise = monochrome ? commonNoise : 2.0 * random[n]->doubleRandomAt(it.x(), it.y()) - 1.0;
            values[index] += float(amplitude * noise);
        }
        // Integer conversion bounds values to its representable range. Float
        // channels retain negative and HDR values in the document's profile.
        cs->fromNormalisedChannelsValue(reinterpret_cast<quint8 *>(converted.data()), values);
        for (int index : enabled) {
            const auto *channel = channels[index];
            // Copy only enabled color channels, leaving alpha and disabled
            // channels bit-identical, even if their float data contains NaNs.
            std::memcpy(it.rawData() + channel->pos(), converted.constData() + channel->pos(), channel->size());
        }
    }
}
