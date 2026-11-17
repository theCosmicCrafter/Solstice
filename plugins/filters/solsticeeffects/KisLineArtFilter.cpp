/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisLineArtFilter.h"
#include "KisCpuPlaneBlur.h"
#include "KisEffectColorSampler.h"
#include "KisLineArtConfigWidget.h"
#include <KoColor.h>
#include <KoUpdater.h>
#include <cmath>
#include <cstring>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_painter.h>
#include <kis_sequential_iterator.h>
namespace
{
double density(KisFilterConfigurationSP config)
{
    const double value = config->getDouble("density", 100);
    return qBound(0.0, std::isfinite(value) ? value : 100.0, 200.0);
}
int radius(KisFilterConfigurationSP config)
{
    return qBound(8, config->getInt("paperRadius", 32), 256);
}
bool cancelled(KoUpdater *progress, int percent)
{
    if (!progress)
        return false;
    progress->setProgress(percent);
    return progress->interrupted();
}
double smooth(double x)
{
    x = qBound(0.0, x, 1.0);
    return x * x * (3 - 2 * x);
}
} // namespace
KisLineArtFilter::KisLineArtFilter()
    : KisFilter(KoID("solstice_line_art", i18n("Line Art Extraction")),
                FiltersCategoryOtherId,
                i18n("&Line Art Extraction..."))
{
    setColorSpaceIndependence(FULLY_INDEPENDENT);
    setSupportsThreading(true);
    setSupportsAdjustmentLayers(true);
    setSupportsPainting(false);
    setSupportsLevelOfDetail(false);
}
KisFilterConfigurationSP KisLineArtFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    auto config = factoryConfiguration(resourcesInterface);
    config->setProperty("density", 100.0);
    config->setProperty("paperRadius", 32);
    config->setProperty("lineColor", "black");
    return config;
}
KisConfigWidget *KisLineArtFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool) const
{
    return new KisLineArtConfigWidget(parent, device);
}
bool KisLineArtFilter::needsTransparentPixels(const KisFilterConfigurationSP, const KoColorSpace *) const
{
    return false;
}
QRect KisLineArtFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    const int r = radius(config), halo = density(config) > 0 ? r + SolsticeEffects::softBlurHalo(r / 4.0) : 0;
    return rect.isEmpty() ? rect : rect.adjusted(-halo, -halo, halo, halo);
}
QRect KisLineArtFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}
void KisLineArtFilter::processImpl(KisPaintDeviceSP device,
                                   const QRect &rect,
                                   const KisFilterConfigurationSP config,
                                   KoUpdater *progress) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(device && config);
    if (rect.isEmpty() || cancelled(progress, 0))
        return;
    const auto *cs = device->colorSpace();
    const auto channels = cs->channels();
    const auto flags = config->channelFlags();
    QVector<int> enabled;
    int gray = -1;
    for (int i = 0; i < channels.size(); ++i) {
        if (cs->colorModelId() == GrayAColorModelID && channels[i]->channelType() == KoChannelInfo::COLOR)
            gray = i;
        if ((channels[i]->channelType() == KoChannelInfo::COLOR || channels[i]->channelType() == KoChannelInfo::ALPHA)
            && (flags.isEmpty() || (i < flags.size() && flags.testBit(i))))
            enabled.append(i);
    }
    if (enabled.isEmpty())
        return;
    const double gain = density(config) / 100;
    const int r = radius(config);
    const double sigma = r / 4.0;
    const bool keepColor = config->getString("lineColor", "black") == "original";
    SolsticeEffects::ColorSampler color(device);
    QVector<float> values(gray >= 0 ? cs->channelCount() : color.working->channelCount());
    const auto brightness = [&](const quint8 *p) {
        double light;
        if (gray >= 0) {
            cs->normalisedChannelsValue(p, values);
            light = values[gray];
        } else {
            color.decode(p, values);
            if (!std::isfinite(values[color.red]) || !std::isfinite(values[color.green])
                || !std::isfinite(values[color.blue]))
                return -1.0;
            light = .2126 * values[color.red] + .7152 * values[color.green] + .0722 * values[color.blue];
        }
        return std::isfinite(light) ? qBound(0.0, light, 1.0) : -1.0;
    };
    SolsticeEffects::FloatPlane paper(rect), support(rect);
    if (gain > 0) {
        const int blurHalo = SolsticeEffects::softBlurHalo(sigma);
        const QRect middle = rect.adjusted(-blurHalo, -blurHalo, blurHalo, blurHalo);
        SolsticeEffects::FloatPlane input(neededRect(rect, config, 0)), maximum(middle), valid(middle);
        for (int y = input.rect.top(); y <= input.rect.bottom(); ++y) {
            if (cancelled(progress, 25 * (y - input.rect.y()) / input.rect.height()))
                return;
            for (int x = input.rect.left(); x <= input.rect.right(); ++x) {
                color.source->moveTo(x, y);
                const auto *p = color.source->oldRawData();
                const double a = cs->opacityF(p);
                // Empty/invalid pixels are missing observations, not white paper.
                input.at(x, y) = a > 0 && a <= 1 && std::isfinite(a) ? float(brightness(p)) : -1.f;
            }
        }
        if (cancelled(progress, 25) || !SolsticeEffects::maximumFilter(input, maximum, r, progress))
            return;
        input.values.clear();
        input.values.shrink_to_fit();
        for (size_t i = 0; i < maximum.values.size(); ++i) {
            valid.values[i] = maximum.values[i] >= 0 ? 1.f : 0.f;
            maximum.values[i] = qMax(0.f, maximum.values[i]);
        }
        if (cancelled(progress, 45) || !SolsticeEffects::softBlur(maximum, paper, sigma, progress))
            return;
        maximum.values.clear();
        maximum.values.shrink_to_fit();
        if (cancelled(progress, 60) || !SolsticeEffects::softBlur(valid, support, sigma, progress))
            return;
    }
    // Native black, using a private converter for non-RGB/Gray spaces.
    QByteArray black(cs->pixelSize(), '\0');
    if (!keepColor && cs->colorModelId() != RGBAColorModelID && gray < 0) {
        KoColor rgbBlack(Qt::black, KoColorSpaceRegistry::instance()->rgb8());
        std::unique_ptr<KoColorConversionTransformation> converter(
            rgbBlack.colorSpace()->createColorConverter(cs,
                                                        KoColorConversionTransformation::internalRenderingIntent(),
                                                        KoColorConversionTransformation::internalConversionFlags()));
        if (!converter)
            return;
        converter->transform(rgbBlack.data(), reinterpret_cast<quint8 *>(black.data()), 1);
    }
    KisPaintDeviceSP output = new KisPaintDevice(cs);
    output->setX(device->x());
    output->setY(device->y());
    output->setDefaultPixel(device->defaultPixel());
    QByteArray encoded(cs->pixelSize(), '\0');
    const double floor = .06 - .02 * gain;
    {
        KisSequentialIterator it(output, rect);
        while (it.nextPixel()) {
            if (it.x() == rect.x() && cancelled(progress, 75 + 24 * (it.y() - rect.y()) / rect.height()))
                return;
            color.source->moveTo(it.x(), it.y());
            const auto *p = color.source->oldRawData();
            std::memcpy(it.rawData(), p, cs->pixelSize());
            const double alpha = cs->opacityF(p);
            if (!(alpha > 0) || alpha > 1 || !std::isfinite(alpha))
                continue;
            const double light = brightness(p);
            if (light < 0)
                continue;
            double ink = 0;
            if (gain > 0) {
                const double count = support.at(it.x(), it.y());
                const double estimate = count > 1e-6 ? qBound(0.0, double(paper.at(it.x(), it.y())) / count, 1.0) : 1.0;
                // A dark neighborhood cannot identify the paper. Blend toward
                // white instead of dividing black/black and erasing broad ink.
                const double reliable = smooth((estimate - .35) / .30);
                const double denominator = qMax(.35, 1 - reliable + reliable * estimate);
                const double dark = 1 - qBound(0.0, light / denominator, 1.0);
                ink = qMin(1.0, gain * dark * smooth((dark - floor) / floor));
            }
            std::memcpy(encoded.data(),
                        keepColor ? p : reinterpret_cast<const quint8 *>(black.constData()),
                        cs->pixelSize());
            cs->setOpacity(reinterpret_cast<quint8 *>(encoded.data()), alpha * ink, 1);
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
