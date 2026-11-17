/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisCrystalFilter.h"
#include "KisCrystalConfigWidget.h"
#include "KisCrystalLattice.h"

#include <KisSequentialIteratorProgress.h>
#include <KoChannelInfo.h>
#include <KoColor.h>
#include <KoUpdater.h>
#include <QRandomGenerator>
#include <cstring>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_paint_device.h>
#include <kis_painter.h>
#include <kis_random_accessor_ng.h>
#include <limits>

KisCrystalFilter::KisCrystalFilter()
    : KisFilter(KoID("solstice_crystal", i18n("Crystal")), FiltersCategoryArtisticId, i18n("&Crystal..."))
{
    setColorSpaceIndependence(FULLY_INDEPENDENT);
    setSupportsThreading(true);
    setSupportsAdjustmentLayers(true);
    setSupportsLevelOfDetail(false);
    setSupportsPainting(false);
}

KisFilterConfigurationSP KisCrystalFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    auto config = factoryConfiguration(resourcesInterface);
    config->setProperty("cellSize", 20.0);
    config->setProperty("irregularity", 100.0);
    config->setProperty("tiling", false);
    config->setProperty("seed", QRandomGenerator::global()->bounded(std::numeric_limits<int>::max()));
    // Widget initializes missing tile geometry from the canvas, then saves it.
    return config;
}

KisConfigWidget *KisCrystalFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool) const
{
    return new KisCrystalConfigWidget(parent, device);
}

QRect KisCrystalFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    return KisCrystalLattice(config).dependencyRect(rect);
}

QRect KisCrystalFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}

void KisCrystalFilter::processImpl(KisPaintDeviceSP device,
                                   const QRect &rect,
                                   const KisFilterConfigurationSP config,
                                   KoUpdater *progressUpdater) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(device && config);
    if (rect.isEmpty())
        return;
    KisCrystalLattice lattice(config);
    if (lattice.identity())
        return;
    const auto *cs = device->colorSpace();
    const auto channels = cs->channels();
    const auto flags = config->channelFlags();
    QVector<int> enabled;
    for (int i = 0; i < channels.size(); ++i) {
        if (channels[i]->channelType() == KoChannelInfo::COLOR
            && (flags.isEmpty() || (i < flags.size() && flags.testBit(i))))
            enabled.append(i);
    }
    if (enabled.isEmpty())
        return;
    KisPaintDeviceSP output = new KisPaintDevice(cs);
    output->setX(device->x());
    output->setY(device->y());
    output->setDefaultPixel(device->defaultPixel());
    auto original = device->createRandomConstAccessorNG();
    auto source = device->createRandomConstAccessorNG();
    {
        KisSequentialIteratorProgress it(output, rect, progressUpdater);
        while (it.nextPixel()) {
            if (it.x() == rect.x() && progressUpdater && progressUpdater->interrupted())
                return;
            original->moveTo(it.x(), it.y());
            const auto *pixel = original->oldRawData();
            std::memcpy(it.rawData(), pixel, cs->pixelSize());
            if (!(cs->opacityF(pixel) > 0.0))
                continue;
            const QPoint sample = lattice.sampleAt(it.x(), it.y());
            source->moveTo(sample.x(), sample.y());
            pixel = source->oldRawData();
            // A transparent representative contributes no color: preserve the
            // original instead of introducing its hidden RGB into the shape.
            if (!(cs->opacityF(pixel) > 0.0))
                continue;
            for (int index : enabled) {
                const auto *channel = channels[index];
                std::memcpy(it.rawData() + channel->pos(), pixel + channel->pos(), channel->size());
            }
        }
    }
    original.clear();
    source.clear();
    if (progressUpdater && progressUpdater->interrupted())
        return;
    KisPainter::copyAreaOptimized(rect.topLeft(), output, device, rect);
}
