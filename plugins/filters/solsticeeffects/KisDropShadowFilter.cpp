/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisDropShadowFilter.h"
#include "KisCpuPlaneBlur.h"
#include "KisDropShadowConfigWidget.h"
#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpaceRegistry.h>
#include <KoUpdater.h>
#include <cmath>
#include <cstring>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_painter.h>
#include <kis_random_accessor_ng.h>
#include <kis_sequential_iterator.h>
#include <limits>
#include <memory>
namespace
{
double control(KisFilterConfigurationSP config, const QString &key, double fallback, double maximum = 100)
{
    const double value = config->getDouble(key, fallback);
    return qBound(0.0, std::isfinite(value) ? value : fallback, maximum);
}
struct Settings {
    double dx, dy, sigma, opacity;
    explicit Settings(KisFilterConfigurationSP config)
    {
        const double distance = control(config, "distance", 20, 1000);
        double angle = config->getDouble("angle", 45);
        if (!std::isfinite(angle))
            angle = 45;
        angle = std::fmod(angle, 360) * M_PI / 180;
        dx = distance * std::cos(angle);
        dy = distance * std::sin(angle);
        // Cardinal integer offsets must not gain a fractional footprint from trig noise.
        if (std::abs(dx - std::round(dx)) < 1e-10)
            dx = std::round(dx);
        if (std::abs(dy - std::round(dy)) < 1e-10)
            dy = std::round(dy);
        const double blur = control(config, "blur", 20);
        sigma = .0064 * blur * blur;
        opacity = control(config, "opacity", 50) / 100;
    }
};
QRect translatedSupport(const QRect &rect, double dx, double dy, int halo)
{
    return rect.adjusted(int(std::floor(dx)) - halo,
                         int(std::floor(dy)) - halo,
                         int(std::ceil(dx)) + halo,
                         int(std::ceil(dy)) + halo);
}
bool cancelled(KoUpdater *progress, int percent)
{
    if (!progress)
        return false;
    progress->setProgress(percent);
    return progress->interrupted();
}
} // namespace
KisDropShadowFilter::KisDropShadowFilter()
    : KisFilter(KoID("solstice_drop_shadow", i18n("Drop Shadow")), FiltersCategoryArtisticId, i18n("&Drop Shadow..."))
{
    setColorSpaceIndependence(FULLY_INDEPENDENT);
    setSupportsThreading(true);
    setSupportsAdjustmentLayers(true);
    setSupportsPainting(false);
    setSupportsLevelOfDetail(false);
}
KisFilterConfigurationSP KisDropShadowFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    auto config = factoryConfiguration(resourcesInterface);
    config->setProperty("blur", 20.0);
    config->setProperty("angle", 45.0);
    config->setProperty("distance", 20.0);
    config->setProperty("opacity", 50.0);
    config->setProperty("shadowColor",
                        QVariant::fromValue(KoColor(Qt::black, KoColorSpaceRegistry::instance()->rgb8())));
    return config;
}
KisConfigWidget *
KisDropShadowFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool) const
{
    return new KisDropShadowConfigWidget(parent, device);
}
bool KisDropShadowFilter::needsTransparentPixels(const KisFilterConfigurationSP, const KoColorSpace *) const
{
    return true;
}
QRect KisDropShadowFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    const Settings s(config);
    return rect.isEmpty() || s.opacity == 0
        ? rect
        : rect.united(translatedSupport(rect, -s.dx, -s.dy, SolsticeEffects::softBlurHalo(s.sigma)));
}
QRect KisDropShadowFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    const Settings s(config);
    return rect.isEmpty() || s.opacity == 0
        ? rect
        : rect.united(translatedSupport(rect, s.dx, s.dy, SolsticeEffects::softBlurHalo(s.sigma)));
}
void KisDropShadowFilter::processImpl(KisPaintDeviceSP device,
                                      const QRect &rect,
                                      const KisFilterConfigurationSP config,
                                      KoUpdater *progress) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(device && config);
    const Settings s(config);
    if (rect.isEmpty() || s.opacity == 0 || cancelled(progress, 0))
        return;
    const auto *cs = device->colorSpace();
    const auto channels = cs->channels();
    const auto flags = config->channelFlags();
    QVector<int> enabled, colors;
    for (int i = 0; i < channels.size(); ++i) {
        if (channels[i]->channelType() == KoChannelInfo::COLOR)
            colors.append(i);
        if ((channels[i]->channelType() == KoChannelInfo::COLOR || channels[i]->channelType() == KoChannelInfo::ALPHA)
            && (flags.isEmpty() || (i < flags.size() && flags.testBit(i))))
            enabled.append(i);
    }
    if (enabled.isEmpty())
        return;
    KoColor shadow = config->getColor("shadowColor", KoColor(Qt::black, cs));
    QByteArray shadowPixel(cs->pixelSize(), '\0');
    // Own the converter per patch; avoid the shared conversion cache on worker threads.
    std::unique_ptr<KoColorConversionTransformation> converter(
        shadow.colorSpace()->createColorConverter(cs,
                                                  KoColorConversionTransformation::internalRenderingIntent(),
                                                  KoColorConversionTransformation::internalConversionFlags()));
    if (!converter)
        return;
    converter->transform(shadow.data(), reinterpret_cast<quint8 *>(shadowPixel.data()), 1);
    QVector<float> tint(cs->channelCount()), values(cs->channelCount());
    cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(shadowPixel.constData()), tint);
    for (int i : colors)
        if (!std::isfinite(tint[i]))
            return;
    const int halo = SolsticeEffects::softBlurHalo(s.sigma);
    const QRect maskRect = rect.adjusted(-halo, -halo, halo, halo);
    SolsticeEffects::FloatPlane mask(maskRect), blurred(rect);
    auto source = device->createRandomConstAccessorNG();
    // Translation fractions are constant for every integer document coordinate.
    const int ox = int(std::floor(-s.dx)), oy = int(std::floor(-s.dy));
    const double fx = -s.dx - ox, fy = -s.dy - oy;
    for (int y = maskRect.top(); y <= maskRect.bottom(); ++y) {
        if (cancelled(progress, 40 * (y - maskRect.y()) / maskRect.height()))
            return;
        for (int x = maskRect.left(); x <= maskRect.right(); ++x) {
            double sum = 0;
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const double weight = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                    if (weight == 0)
                        continue;
                    source->moveTo(x + ox + i, y + oy + j);
                    const double a = cs->opacityF(source->oldRawData());
                    if (std::isfinite(a))
                        sum += qBound(0.0, a, 1.0) * weight;
                }
            mask.at(x, y) = float(sum);
        }
    }
    if (cancelled(progress, 40) || !SolsticeEffects::softBlur(mask, blurred, s.sigma, progress)
        || cancelled(progress, 65))
        return;
    KisPaintDeviceSP output = new KisPaintDevice(cs);
    output->setX(device->x());
    output->setY(device->y());
    output->setDefaultPixel(device->defaultPixel());
    QByteArray encoded(cs->pixelSize(), '\0');
    const double maximum =
        cs->colorDepthId() == Float16BitsColorDepthID ? 65504.0 : double(std::numeric_limits<float>::max());
    {
        KisSequentialIterator it(output, rect);
        while (it.nextPixel()) {
            if (it.x() == rect.x() && cancelled(progress, 65 + 34 * (it.y() - rect.y()) / rect.height()))
                return;
            source->moveTo(it.x(), it.y());
            const auto *p = source->oldRawData();
            std::memcpy(it.rawData(), p, cs->pixelSize());
            const double a = cs->opacityF(p);
            if (!std::isfinite(a) || a < 0 || a >= 1)
                continue;
            const double behind = qBound(0.0, double(blurred.at(it.x(), it.y())), 1.0) * s.opacity * (1 - a);
            if (!(behind > 0))
                continue;
            const double resultAlpha = a + behind;
            cs->normalisedChannelsValue(p, values);
            bool valid = true;
            for (int i : colors) {
                const double result = ((a > 0 ? double(values[i]) * a : 0) + double(tint[i]) * behind) / resultAlpha;
                valid &= std::isfinite(result) && std::abs(result) <= maximum;
                values[i] = float(result);
            }
            if (!valid)
                continue;
            cs->fromNormalisedChannelsValue(reinterpret_cast<quint8 *>(encoded.data()), values);
            cs->setOpacity(reinterpret_cast<quint8 *>(encoded.data()), resultAlpha, 1);
            for (int i : enabled) {
                const auto *ch = channels[i];
                std::memcpy(it.rawData() + ch->pos(), encoded.constData() + ch->pos(), ch->size());
            }
        }
    }
    source.clear();
    if (cancelled(progress, 99))
        return;
    KisPainter::copyAreaOptimized(rect.topLeft(), output, device, rect);
    if (progress)
        progress->setProgress(100);
}
