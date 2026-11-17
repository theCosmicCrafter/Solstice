/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisGlowFilter.h"
#include "KisCpuPlaneBlur.h"
#include "KisEffectColorSampler.h"
#include "KisGlowConfigWidget.h"
#include <KoUpdater.h>
#include <array>
#include <cmath>
#include <cstring>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_painter.h>
#include <kis_sequential_iterator.h>
#include <limits>
namespace
{
double control(KisFilterConfigurationSP config, const QString &key, double fallback)
{
    const double value = config->getDouble(key, fallback);
    return qBound(0.0, std::isfinite(value) ? value : fallback, 100.0);
}
double sigma(KisFilterConfigurationSP config)
{
    const double range = control(config, "blurRange", 30);
    return .0064 * range * range;
}
bool cancelled(KoUpdater *progress, int percent)
{
    if (!progress)
        return false;
    progress->setProgress(percent);
    return progress->interrupted();
}
} // namespace
KisGlowFilter::KisGlowFilter()
    : KisFilter(KoID("solstice_glow", i18n("Glow")), FiltersCategoryArtisticId, i18n("&Glow..."))
{
    setColorSpaceIndependence(FULLY_INDEPENDENT);
    setSupportsThreading(true);
    setSupportsAdjustmentLayers(true);
    setSupportsPainting(false);
    // Preserve the full-resolution bright-pass and fine highlights in preview.
    setSupportsLevelOfDetail(false);
}
KisFilterConfigurationSP KisGlowFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    auto config = factoryConfiguration(resourcesInterface);
    config->setProperty("strength", 50.0);
    config->setProperty("effectArea", 50.0);
    config->setProperty("blurRange", 30.0);
    return config;
}
KisConfigWidget *KisGlowFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool) const
{
    return new KisGlowConfigWidget(parent, device);
}
QRect KisGlowFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    const int halo = control(config, "strength", 50) > 0 ? SolsticeEffects::softBlurHalo(sigma(config)) : 0;
    return rect.isEmpty() ? rect : rect.adjusted(-halo, -halo, halo, halo);
}
QRect KisGlowFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}
void KisGlowFilter::processImpl(KisPaintDeviceSP device,
                                const QRect &rect,
                                const KisFilterConfigurationSP config,
                                KoUpdater *progress) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(device && config);
    const double gain = control(config, "strength", 50) / 50;
    if (rect.isEmpty() || gain == 0 || cancelled(progress, 0))
        return;
    const auto *cs = device->colorSpace();
    const double maximum =
        cs->colorDepthId() == Float16BitsColorDepthID ? 65504.0 : double(std::numeric_limits<float>::max());
    const auto channels = cs->channels();
    const auto flags = config->channelFlags();
    QVector<int> enabled;
    int gray = -1;
    for (int i = 0; i < channels.size(); ++i) {
        if (channels[i]->channelType() != KoChannelInfo::COLOR)
            continue;
        if (cs->colorModelId() == GrayAColorModelID)
            gray = i;
        if (flags.isEmpty() || (i < flags.size() && flags.testBit(i)))
            enabled.append(i);
    }
    if (enabled.isEmpty())
        return;
    SolsticeEffects::ColorSampler color(device);
    QVector<float> values(gray >= 0 ? cs->channelCount() : color.working->channelCount());
    const auto decode = [&](const quint8 *p) {
        if (gray >= 0) {
            cs->normalisedChannelsValue(p, values);
            return std::array<double, 3>{values[gray], values[gray], values[gray]};
        }
        color.decode(p, values);
        return std::array<double, 3>{values[color.red], values[color.green], values[color.blue]};
    };
    const QRect inputRect = neededRect(rect, config, 0);
    const int count = gray >= 0 ? 1 : 3;
    std::vector<SolsticeEffects::FloatPlane> emission, blurred;
    emission.reserve(count);
    blurred.reserve(count);
    for (int i = 0; i < count; ++i) {
        emission.emplace_back(inputRect);
        blurred.emplace_back(rect);
    }
    const double threshold = 1 - control(config, "effectArea", 50) / 100;
    for (int y = inputRect.top(); y <= inputRect.bottom(); ++y) {
        if (cancelled(progress, 25 * (y - inputRect.y()) / inputRect.height()))
            return;
        for (int x = inputRect.left(); x <= inputRect.right(); ++x) {
            color.source->moveTo(x, y);
            const auto *p = color.source->oldRawData();
            const double alpha = cs->opacityF(p);
            if (!(alpha > 0) || !std::isfinite(alpha))
                continue;
            auto rgb = decode(p);
            if (!std::isfinite(rgb[0]) || !std::isfinite(rgb[1]) || !std::isfinite(rgb[2]))
                continue;
            for (double &v : rgb)
                v = qMax(0.0, v);
            const double light = gray >= 0 ? rgb[0] : .2126 * rgb[0] + .7152 * rgb[1] + .0722 * rgb[2];
            const double t = qBound(0.0, (light - threshold + .1) / .2, 1.0);
            const double weight = t * t * (3 - 2 * t) * qMin(1.0, alpha);
            for (int i = 0; i < count; ++i)
                emission[i].at(x, y) = float(rgb[i] * weight);
        }
    }
    for (int i = 0; i < count; ++i) {
        if (cancelled(progress, 25 + 50 * i / count))
            return;
        if (!SolsticeEffects::softBlur(emission[i], blurred[i], sigma(config), progress))
            return;
        emission[i].values.clear();
        emission[i].values.shrink_to_fit();
    }
    if (cancelled(progress, 75))
        return;
    KisPaintDeviceSP output = new KisPaintDevice(cs);
    output->setX(device->x());
    output->setY(device->y());
    output->setDefaultPixel(device->defaultPixel());
    QByteArray encoded(cs->pixelSize(), '\0');
    {
        KisSequentialIterator it(output, rect);
        while (it.nextPixel()) {
            if (it.x() == rect.x() && cancelled(progress, 75 + 24 * (it.y() - rect.y()) / rect.height()))
                return;
            color.source->moveTo(it.x(), it.y());
            const auto *p = color.source->oldRawData();
            std::memcpy(it.rawData(), p, cs->pixelSize());
            const double alpha = cs->opacityF(p);
            if (!(alpha > 0) || !std::isfinite(alpha))
                continue;
            auto rgb = decode(p);
            if (!std::isfinite(rgb[0]) || !std::isfinite(rgb[1]) || !std::isfinite(rgb[2]))
                continue;
            bool changed = false, valid = true;
            for (int i = 0; i < count; ++i) {
                const double glow = qMax(0.0, double(blurred[i].at(it.x(), it.y()))) * gain;
                // Screen in SDR, continuous additive excess above SDR. No
                // clipping of HDR or negative input, nor alpha normalization.
                const double result = rgb[i] + glow - qBound(0.0, rgb[i], 1.0) * qMin(1.0, glow);
                changed |= result != rgb[i];
                rgb[i] = result;
                valid &= std::isfinite(result) && std::abs(result) <= maximum;
            }
            if (!changed || !valid)
                continue;
            if (gray >= 0) {
                values[gray] = float(rgb[0]);
                cs->fromNormalisedChannelsValue(reinterpret_cast<quint8 *>(encoded.data()), values);
            } else {
                values[color.red] = float(rgb[0]);
                values[color.green] = float(rgb[1]);
                values[color.blue] = float(rgb[2]);
                color.encode(values, reinterpret_cast<quint8 *>(encoded.data()));
            }
            for (int i : enabled) {
                const auto *ch = channels[i];
                std::memcpy(it.rawData() + ch->pos(), encoded.constData() + ch->pos(), ch->size());
            }
        }
    }
    color.source.clear();
    if (cancelled(progress, 99))
        return;
    KisPainter::copyAreaOptimized(rect.topLeft(), output, device, rect);
    if (progress)
        progress->setProgress(100);
}
