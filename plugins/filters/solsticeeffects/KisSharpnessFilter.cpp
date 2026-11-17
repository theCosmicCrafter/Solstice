/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisSharpnessFilter.h"
#include "KisCpuPlaneBlur.h"
#include "KisEffectColorSampler.h"
#include "KisSharpnessConfigWidget.h"
#include <KoUpdater.h>
#include <cmath>
#include <cstring>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_painter.h>
#include <kis_sequential_iterator.h>
namespace
{
constexpr double sigma = 1.0;
double strength(const KisFilterConfigurationSP config)
{
    const double value = config->getDouble("strength", 50);
    return qBound(0.0, std::isfinite(value) ? value : 50.0, 100.0) / 100.0;
}
bool cancelled(KoUpdater *progress, int percent)
{
    if (!progress)
        return false;
    progress->setProgress(percent);
    return progress->interrupted();
}
} // namespace
KisSharpnessFilter::KisSharpnessFilter()
    : KisFilter(KoID("solstice_sharpness", i18n("Sharpness")), FiltersCategoryEnhanceId, i18n("&Sharpness..."))
{
    setColorSpaceIndependence(FULLY_INDEPENDENT);
    setSupportsThreading(true);
    setSupportsAdjustmentLayers(true);
    setSupportsLevelOfDetail(false);
    setSupportsPainting(false);
}
KisFilterConfigurationSP KisSharpnessFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    auto config = factoryConfiguration(resourcesInterface);
    config->setProperty("strength", 50.0);
    return config;
}
KisConfigWidget *
KisSharpnessFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool) const
{
    return new KisSharpnessConfigWidget(parent, device);
}
QRect KisSharpnessFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    const int halo = strength(config) > 0 ? qMax(1, SolsticeEffects::gaussianHalo(sigma)) : 0;
    return rect.isEmpty() ? rect : rect.adjusted(-halo, -halo, halo, halo);
}
QRect KisSharpnessFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}
void KisSharpnessFilter::processImpl(KisPaintDeviceSP device,
                                     const QRect &rect,
                                     const KisFilterConfigurationSP config,
                                     KoUpdater *progress) const
{
    applyCpu(device, rect, config, progress);
}
void KisSharpnessFilter::applyCpu(KisPaintDeviceSP device,
                                  const QRect &rect,
                                  const KisFilterConfigurationSP config,
                                  KoUpdater *progress,
                                  int progressStart,
                                  int progressSpan)
{
    const auto report = [&](int percent) {
        return cancelled(progress, progressStart + progressSpan * percent / 100);
    };
    KIS_SAFE_ASSERT_RECOVER_RETURN(device && config);
    if (rect.isEmpty() || strength(config) == 0 || report(0))
        return;
    const double gain = 2.0 * strength(config);
    const auto *cs = device->colorSpace();
    const auto channels = cs->channels();
    const auto flags = config->channelFlags();
    QVector<int> enabled;
    int grayChannel = -1;
    for (int i = 0; i < channels.size(); ++i) {
        if (channels[i]->channelType() != KoChannelInfo::COLOR)
            continue;
        if (cs->colorModelId() == GrayAColorModelID)
            grayChannel = i;
        if (flags.isEmpty() || (i < flags.size() && flags.testBit(i)))
            enabled.append(i);
    }
    if (enabled.isEmpty())
        return;
    SolsticeEffects::ColorSampler color(device);
    QVector<float> values(grayChannel >= 0 ? cs->channelCount() : color.working->channelCount());
    const auto decode = [&](const quint8 *p) {
        if (grayChannel >= 0)
            cs->normalisedChannelsValue(p, values);
        else
            color.decode(p, values);
    };
    const auto light = [&] {
        return grayChannel >= 0 ? double(values[grayChannel])
                                : .2126 * values[color.red] + .7152 * values[color.green] + .0722 * values[color.blue];
    };
    const int halo = qMax(1, SolsticeEffects::gaussianHalo(sigma));
    const QRect inputRect = rect.adjusted(-halo, -halo, halo, halo);
    SolsticeEffects::FloatPlane input(inputRect), coverage(inputRect), lights(inputRect);
    for (int y = inputRect.top(); y <= inputRect.bottom(); ++y) {
        if (report(30 * (y - inputRect.y()) / inputRect.height()))
            return;
        for (int x = inputRect.left(); x <= inputRect.right(); ++x) {
            color.source->moveTo(x, y);
            const auto *p = color.source->oldRawData();
            const double alpha = cs->opacityF(p);
            if (!(alpha > 0) || !std::isfinite(alpha))
                continue;
            decode(p);
            const double l = light();
            if (!std::isfinite(l))
                continue;
            const float a = float(qMin(1.0, alpha));
            lights.at(x, y) = float(l);
            coverage.at(x, y) = a;
            input.at(x, y) = float(l * a);
        }
    }
    SolsticeEffects::FloatPlane blurred(rect), blurredAlpha(rect);
    if (!SolsticeEffects::gaussianBlur(input, blurred, sigma, progress) || report(45))
        return;
    if (!SolsticeEffects::gaussianBlur(coverage, blurredAlpha, sigma, progress) || report(60))
        return;
    KisPaintDeviceSP output = new KisPaintDevice(cs);
    output->setX(device->x());
    output->setY(device->y());
    output->setDefaultPixel(device->defaultPixel());
    QByteArray encoded(cs->pixelSize(), '\0');
    {
        KisSequentialIterator it(output, rect);
        while (it.nextPixel()) {
            if (it.x() == rect.x() && report(60 + 39 * (it.y() - rect.y()) / rect.height()))
                return;
            color.source->moveTo(it.x(), it.y());
            const auto *original = color.source->oldRawData();
            std::memcpy(it.rawData(), original, cs->pixelSize());
            if (!(coverage.at(it.x(), it.y()) > 0))
                continue;
            decode(original);
            const double center = light();
            const double a = blurredAlpha.at(it.x(), it.y());
            if (!(a > 0))
                continue;
            double delta = gain * (center - blurred.at(it.x(), it.y()) / a);
            // Preserve flat fields exactly despite float-plane round-off.
            if (std::abs(delta) <= 1e-6 * qMax(1.0, std::abs(center)))
                continue;
            double low = center, high = center;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = it.x() + dx, y = it.y() + dy;
                    if (coverage.at(x, y) > 0) {
                        low = qMin(low, double(lights.at(x, y)));
                        high = qMax(high, double(lights.at(x, y)));
                    }
                }
            const double limit = delta > 0 ? high - center : center - low;
            if (!(limit > 0))
                continue;
            delta = limit * std::tanh(delta / limit);
            if (grayChannel >= 0) {
                values[grayChannel] += float(delta);
                cs->fromNormalisedChannelsValue(reinterpret_cast<quint8 *>(encoded.data()), values);
            } else {
                for (int index : {color.red, color.green, color.blue})
                    values[index] += float(delta);
                color.encode(values, reinterpret_cast<quint8 *>(encoded.data()));
            }
            for (int index : enabled) {
                const auto *channel = channels[index];
                std::memcpy(it.rawData() + channel->pos(), encoded.constData() + channel->pos(), channel->size());
            }
        }
    }
    color.source.clear();
    if (report(99))
        return;
    KisPainter::copyAreaOptimized(rect.topLeft(), output, device, rect);
    if (progress)
        progress->setProgress(progressStart + progressSpan);
}
