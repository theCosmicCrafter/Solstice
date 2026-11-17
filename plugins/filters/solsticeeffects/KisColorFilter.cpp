/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisColorFilter.h"
#include "KisColorFilterConfigWidget.h"
#include "KisEffectColorSampler.h"
#include "KisSharpnessFilter.h"
#include <KoColorSpaceTraits.h>
#include <KoUpdater.h>
#include <array>
#include <cmath>
#include <cstring>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_painter.h>
#include <kis_sequential_iterator.h>
namespace
{
using RGB = std::array<double, 3>;
double value(KisPropertiesConfigurationSP config,
             const QString &key,
             double low = -100,
             double high = 100,
             double fallback = 0)
{
    const double v = config->getDouble(key, fallback);
    return qBound(low, std::isfinite(v) ? v : fallback, high);
}
double light(const RGB &c)
{
    return .2126 * c[0] + .7152 * c[1] + .0722 * c[2];
}
bool finite(const RGB &c)
{
    return std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]);
}
bool cancelled(KoUpdater *progress, int percent)
{
    if (!progress)
        return false;
    progress->setProgress(percent);
    return progress->interrupted();
}
struct Grade {
    double brightness, contrast, cosine, sine, saturation, curve, mono, sepia;
    RGB gains, shadows, highlights;
    bool neutral;
    Grade(KisPropertiesConfigurationSP config, const QString &prefix, bool gray)
    {
        const auto get = [&](const QString &key) {
            return value(config, prefix + key);
        };
        const double b = get("Brightness"), c = get("Contrast");
        const double h = gray ? 0 : value(config, prefix + "Hue", -180, 180);
        const double s = gray ? 0 : get("Saturation"), t = gray ? 0 : get("Temperature"), tint = gray ? 0 : get("Tint");
        brightness = .5 * b / 100;
        contrast = std::exp2(c / 100);
        cosine = std::cos(h * M_PI / 180);
        sine = std::sin(h * M_PI / 180) / std::sqrt(3.0);
        saturation = 1 + s / 100;
        curve = gray ? 0 : get("Curve") / 100;
        mono = gray ? 0 : value(config, prefix + "Monochrome", 0, 100) / 100;
        sepia = gray ? 0 : value(config, prefix + "Sepia", 0, 100) / 100;
        gains = {std::exp2(t * .004 + tint * .0015), std::exp2(-tint * .003), std::exp2(-t * .004 + tint * .0015)};
        neutral = b == 0 && c == 0 && h == 0 && s == 0 && t == 0 && tint == 0 && curve == 0 && mono == 0 && sepia == 0;
        const QStringList suffixes = {"R", "G", "B"};
        for (int i = 0; i < 3; ++i) {
            shadows[i] = gray ? 0 : get("Shadow" + suffixes[i]) / 100;
            highlights[i] = gray ? 0 : get("Highlight" + suffixes[i]) / 100;
            neutral &= shadows[i] == 0 && highlights[i] == 0;
        }
    }
    RGB apply(RGB c) const
    {
        if (neutral)
            return c;
        for (double &v : c)
            v = .5 + (v + brightness - .5) * contrast;
        const double mean = (c[0] + c[1] + c[2]) / 3;
        RGB rotated = {c[0] * cosine + mean * (1 - cosine) + (c[2] - c[1]) * sine,
                       c[1] * cosine + mean * (1 - cosine) + (c[0] - c[2]) * sine,
                       c[2] * cosine + mean * (1 - cosine) + (c[1] - c[0]) * sine};
        const double l = light(rotated);
        for (int i = 0; i < 3; ++i) {
            c[i] = (l + saturation * (rotated[i] - l)) * gains[i];
            // Linear continuation outside SDR avoids clipping negative/HDR values.
            if (c[i] > 0 && c[i] < 1)
                c[i] += curve * (c[i] - .5) * c[i] * (1 - c[i]);
        }
        const double tone = light(c), w = qBound(0.0, tone, 1.0);
        const RGB sepiaScale = {1.12, .99, .78};
        for (int i = 0; i < 3; ++i) {
            c[i] += shadows[i] * (1 - w) * (1 - w) + highlights[i] * w * w;
            c[i] += mono * (tone - c[i]);
            c[i] += sepia * (tone * sepiaScale[i] - c[i]);
        }
        return c;
    }
};
} // namespace
KisColorFilter::KisColorFilter()
    : KisFilter(KoID("solstice_color_filter", i18n("Color Filter")), FiltersCategoryAdjustId, i18n("&Color Filter..."))
{
    setColorSpaceIndependence(FULLY_INDEPENDENT);
    setSupportsThreading(true);
    setSupportsAdjustmentLayers(true);
    setSupportsPainting(false);
}
QStringList KisColorFilter::presetNames()
{
    return {i18n("None"),
            i18n("Vivid"),
            i18n("Warm"),
            i18n("Cool"),
            i18n("Lemonade"),
            i18n("Cobalt Blue"),
            i18n("Aquamarine"),
            i18n("Aurora"),
            i18n("Sepia"),
            i18n("Monotone")};
}
void KisColorFilter::applyPreset(KisPropertiesConfigurationSP config, int preset)
{
    preset = qBound(0, preset, 9);
    for (const QString &key : {"Brightness",
                               "Contrast",
                               "Hue",
                               "Saturation",
                               "Temperature",
                               "Tint",
                               "Curve",
                               "Monochrome",
                               "Sepia",
                               "ShadowR",
                               "ShadowG",
                               "ShadowB",
                               "HighlightR",
                               "HighlightG",
                               "HighlightB"})
        config->setProperty("look" + key, 0.0);
    config->setProperty("preset", preset);
    config->setProperty("lookEnabled", preset != 0);
    const auto set = [&](const QString &key, double v) {
        config->setProperty("look" + key, v);
    };
    switch (preset) {
    case 1:
        set("Contrast", 12);
        set("Saturation", 25);
        set("Curve", 20);
        break;
    case 2:
        set("Temperature", 25);
        set("Tint", 3);
        set("Contrast", 3);
        break;
    case 3:
        set("Temperature", -25);
        set("Tint", -3);
        set("Saturation", 5);
        break;
    case 4:
        set("Temperature", 35);
        set("Tint", -20);
        set("Brightness", 5);
        set("HighlightR", 3);
        break;
    case 5:
        set("Temperature", -45);
        set("Tint", 10);
        set("ShadowR", -4);
        set("HighlightB", 8);
        break;
    case 6:
        set("Temperature", -20);
        set("Tint", -30);
        set("Saturation", 8);
        break;
    case 7:
        set("Temperature", -8);
        set("Tint", -15);
        set("ShadowR", 6);
        set("ShadowB", 10);
        set("HighlightG", 6);
        break;
    case 8:
        set("Sepia", 100);
        break;
    case 9:
        set("Monochrome", 100);
        set("Contrast", 8);
        break;
    default:
        break;
    }
}
KisFilterConfigurationSP KisColorFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    auto config = factoryConfiguration(resourcesInterface);
    applyPreset(config, 0);
    config->setProperty("amount", 100.0);
    for (const QString &key : {"Brightness", "Contrast", "Hue", "Saturation", "Temperature", "Tint", "Sharpness"})
        config->setProperty("adjust" + key, 0.0);
    return config;
}
KisConfigWidget *
KisColorFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool useForMasks) const
{
    return new KisColorFilterConfigWidget(parent, device, useForMasks);
}
bool KisColorFilter::supportsLevelOfDetail(const KisFilterConfigurationSP config, int) const
{
    return value(config, "adjustSharpness", 0, 100) == 0;
}
QRect KisColorFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    return rect.isEmpty() || supportsLevelOfDetail(config, 0) ? rect : rect.adjusted(-3, -3, 3, 3);
}
QRect KisColorFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}
void KisColorFilter::processImpl(KisPaintDeviceSP device,
                                 const QRect &rect,
                                 const KisFilterConfigurationSP config,
                                 KoUpdater *progress) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(device && config);
    if (rect.isEmpty() || cancelled(progress, 0))
        return;
    const auto *cs = device->colorSpace();
    const bool gray = cs->colorModelId() == GrayAColorModelID;
    const Grade look(config, "look", false), adjust(config, "adjust", gray);
    const double amount =
        gray || !config->getBool("lookEnabled", false) ? 0 : value(config, "amount", 0, 100, 100) / 100;
    const double sharpness = value(config, "adjustSharpness", 0, 100);
    if ((amount == 0 || look.neutral) && adjust.neutral && sharpness == 0)
        return;
    const auto channels = cs->channels();
    const auto flags = config->channelFlags();
    QVector<int> enabled;
    int grayChannel = -1;
    for (int i = 0; i < channels.size(); ++i) {
        if (channels[i]->channelType() != KoChannelInfo::COLOR)
            continue;
        if (gray)
            grayChannel = i;
        if (flags.isEmpty() || (i < flags.size() && flags.testBit(i)))
            enabled.append(i);
    }
    if (enabled.isEmpty())
        return;
    SolsticeEffects::ColorSampler color(device);
    const auto *floatSpace = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", color.working->profile());
    KIS_SAFE_ASSERT_RECOVER_RETURN(floatSpace);
    KisPaintDeviceSP staged = new KisPaintDevice(floatSpace);
    QVector<float> values(gray ? cs->channelCount() : color.working->channelCount());
    const auto decode = [&](const quint8 *p) {
        if (gray) {
            cs->normalisedChannelsValue(p, values);
            return RGB{values[grayChannel], values[grayChannel], values[grayChannel]};
        }
        color.decode(p, values);
        return RGB{values[color.red], values[color.green], values[color.blue]};
    };
    const QRect inputRect = neededRect(rect, config, 0);
    {
        KisSequentialIterator it(staged, inputRect);
        while (it.nextPixel()) {
            if (it.x() == inputRect.x() && cancelled(progress, 30 * (it.y() - inputRect.y()) / inputRect.height()))
                return;
            color.source->moveTo(it.x(), it.y());
            const auto *p = color.source->oldRawData();
            const double alpha = cs->opacityF(p);
            if (!(alpha > 0) || !std::isfinite(alpha))
                continue;
            RGB c = decode(p);
            if (!finite(c))
                continue;
            if (amount > 0) {
                const auto looked = look.apply(c);
                for (int i = 0; i < 3; ++i)
                    c[i] += amount * (looked[i] - c[i]);
            }
            c = adjust.apply(c);
            auto *out = reinterpret_cast<KoRgbF32Traits::Pixel *>(it.rawData());
            out->red = float(c[0]);
            out->green = float(c[1]);
            out->blue = float(c[2]);
            out->alpha = float(alpha);
        }
    }
    if (cancelled(progress, 30))
        return;
    if (sharpness > 0) {
        auto sharpConfig = config->clone();
        sharpConfig->setProperty("strength", sharpness);
        sharpConfig->setChannelFlags(QBitArray());
        KisSharpnessFilter::applyCpu(staged, rect, sharpConfig, progress, 30, 55);
    }
    if (cancelled(progress, 85))
        return;
    KisPaintDeviceSP output = new KisPaintDevice(cs);
    output->setX(device->x());
    output->setY(device->y());
    output->setDefaultPixel(device->defaultPixel());
    auto processed = staged->createRandomConstAccessorNG();
    QByteArray encoded(cs->pixelSize(), '\0');
    {
        KisSequentialIterator it(output, rect);
        while (it.nextPixel()) {
            if (it.x() == rect.x() && cancelled(progress, 85 + 14 * (it.y() - rect.y()) / rect.height()))
                return;
            color.source->moveTo(it.x(), it.y());
            const auto *p = color.source->oldRawData();
            std::memcpy(it.rawData(), p, cs->pixelSize());
            if (!(cs->opacityF(p) > 0) || !finite(decode(p)))
                continue;
            processed->moveTo(it.x(), it.y());
            const auto *out = reinterpret_cast<const KoRgbF32Traits::Pixel *>(processed->rawDataConst());
            if (!(out->alpha > 0) || !finite(RGB{out->red, out->green, out->blue}))
                continue;
            if (gray) {
                values[grayChannel] = out->red;
                cs->fromNormalisedChannelsValue(reinterpret_cast<quint8 *>(encoded.data()), values);
            } else {
                values[color.red] = out->red;
                values[color.green] = out->green;
                values[color.blue] = out->blue;
                color.encode(values, reinterpret_cast<quint8 *>(encoded.data()));
            }
            for (int index : enabled) {
                const auto *ch = channels[index];
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
