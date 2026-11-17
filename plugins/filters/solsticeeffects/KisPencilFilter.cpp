/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisPencilFilter.h"
#include "KisEffectColorSampler.h"
#include "KisPencilConfigWidget.h"
#include <KisRandomGenerator2D.h>
#include <KoColor.h>
#include <KoUpdater.h>
#include <QRandomGenerator>
#include <cmath>
#include <cstring>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_painter.h>
#include <kis_sequential_iterator.h>
#include <limits>
#include <vector>

namespace
{
double bounded(const KisFilterConfigurationSP config, const char *key, double fallback, double low, double high)
{
    const double value = config->getDouble(key, fallback);
    return qBound(low, std::isfinite(value) ? value : fallback, high);
}
double outlineStrength(const KisFilterConfigurationSP config)
{
    return config->getBool("outlines", true) ? bounded(config, "outlineStrength", 60, 0, 100) / 100.0 : 0.0;
}
double smoothstep(double low, double high, double value)
{
    const double t = qBound(0.0, (value - low) / (high - low), 1.0);
    return t * t * (3.0 - 2.0 * t);
}
bool report(KoUpdater *progress, int value)
{
    if (!progress)
        return false;
    progress->setProgress(value);
    return progress->interrupted();
}
double luminance(const QVector<float> &values, const SolsticeEffects::ColorSampler &color)
{
    return .2126 * values[color.red] + .7152 * values[color.green] + .0722 * values[color.blue];
}
struct Field {
    float light = 0, coverage = 0;
};
} // namespace

KisPencilFilter::KisPencilFilter()
    : KisFilter(KoID("solstice_pencil", i18n("Pencil")), FiltersCategoryArtisticId, i18n("&Pencil..."))
{
    setColorSpaceIndependence(FULLY_INDEPENDENT);
    setSupportsThreading(true);
    setSupportsAdjustmentLayers(true);
    setSupportsLevelOfDetail(false);
    setSupportsPainting(false);
}

KisFilterConfigurationSP KisPencilFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    auto config = factoryConfiguration(resourcesInterface);
    config->setProperty("outlines", true);
    config->setProperty("outlineStrength", 60.0);
    config->setProperty("hatching", true);
    config->setProperty("hatchStrength", 65.0);
    config->setProperty("hatchSize", 4.0);
    config->setProperty("roughness", 50.0);
    config->setProperty("angle", 45.0);
    config->setProperty("grayscale", false);
    config->setProperty("seed", QRandomGenerator::global()->bounded(std::numeric_limits<int>::max()));
    return config;
}

KisConfigWidget *KisPencilFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool) const
{
    return new KisPencilConfigWidget(parent, device);
}

QRect KisPencilFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    return outlineStrength(config) > 0.0 ? rect.adjusted(-2, -2, 2, 2) : rect;
}
QRect KisPencilFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}

void KisPencilFilter::processImpl(KisPaintDeviceSP device,
                                  const QRect &rect,
                                  const KisFilterConfigurationSP config,
                                  KoUpdater *progress) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(device && config);
    const double outlines = outlineStrength(config);
    const double hatching =
        config->getBool("hatching", true) ? bounded(config, "hatchStrength", 65, 0, 100) / 100.0 : 0.0;
    if (rect.isEmpty() || (outlines == 0.0 && hatching == 0.0))
        return;
    const auto *cs = device->colorSpace();
    const auto channels = cs->channels();
    const auto flags = config->channelFlags();
    QVector<int> enabled;
    for (int i = 0; i < channels.size(); ++i)
        if (channels[i]->channelType() == KoChannelInfo::COLOR
            && (flags.isEmpty() || (i < flags.size() && flags.testBit(i))))
            enabled.append(i);
    if (enabled.isEmpty())
        return;

    SolsticeEffects::ColorSampler color(device);
    const QRect smoothRect = rect.adjusted(-1, -1, 1, 1);
    std::vector<Field> smoothed;
    if (outlines > 0.0) {
        const QRect inputRect = rect.adjusted(-2, -2, 2, 2);
        std::vector<Field> input(size_t(inputRect.width()) * inputRect.height());
        // Explicit CPU binomial smoothing, normalized by source coverage.
        // No Gaussian helper that could auto-dispatch to the GPU is called.
        for (int y = 0; y < inputRect.height(); ++y) {
            if (report(progress, 25 * y / inputRect.height()))
                return;
            for (int x = 0; x < inputRect.width(); ++x) {
                color.source->moveTo(inputRect.x() + x, inputRect.y() + y);
                const auto *pixel = color.source->oldRawData();
                const double alpha = cs->opacityF(pixel);
                if (!(alpha > 0.0) || !std::isfinite(alpha))
                    continue;
                color.decode(pixel, color.values);
                const double light = luminance(color.values, color);
                if (!std::isfinite(light))
                    continue;
                auto &sample = input[size_t(y) * inputRect.width() + x];
                sample.coverage = float(qMin(1.0, alpha));
                sample.light = float(qBound(0.0, light, 1.0) * sample.coverage);
            }
        }
        smoothed.resize(size_t(smoothRect.width()) * smoothRect.height());
        constexpr int weights[] = {1, 2, 1};
        for (int y = 0; y < smoothRect.height(); ++y) {
            if (report(progress, 25 + 20 * y / smoothRect.height()))
                return;
            for (int x = 0; x < smoothRect.width(); ++x) {
                double light = 0, coverage = 0;
                for (int dy = 0; dy < 3; ++dy) {
                    for (int dx = 0; dx < 3; ++dx) {
                        const auto &sample = input[size_t(y + dy) * inputRect.width() + x + dx];
                        const int weight = weights[dx] * weights[dy];
                        light += sample.light * weight;
                        coverage += sample.coverage * weight;
                    }
                }
                smoothed[size_t(y) * smoothRect.width() + x] = {float(coverage > 0 ? light / coverage : 1.0),
                                                                float(coverage)};
            }
        }
    }

    const double spacing = bounded(config, "hatchSize", 4, 1, 32);
    const double roughness = bounded(config, "roughness", 50, 0, 100) / 100.0;
    const double angle = std::fmod(bounded(config, "angle", 45, 0, 180), 180.0) * M_PI / 180.0;
    const double cosAngle = std::cos(angle), sinAngle = std::sin(angle);
    const double segmentLength = spacing * (12.0 - 8.0 * roughness);
    const bool grayscale = config->getBool("grayscale", false);
    KisRandomGenerator2D strokes(quint32(qMax(0, config->getInt("seed", 0))));
    KisRandomGenerator2D grain(quint32(qMax(0, config->getInt("seed", 0))) ^ 0x9e3779b97f4a7c15ULL);
    KisPaintDeviceSP output = new KisPaintDevice(cs);
    output->setX(device->x());
    output->setY(device->y());
    output->setDefaultPixel(device->defaultPixel());
    QByteArray encoded(cs->pixelSize(), '\0');
    {
        KisSequentialIterator it(output, rect);
        while (it.nextPixel()) {
            if (it.x() == rect.x()
                && report(progress,
                          (outlines > 0 ? 45 : 0) + (outlines > 0 ? 54 : 99) * (it.y() - rect.y()) / rect.height()))
                return;
            color.source->moveTo(it.x(), it.y());
            const auto *original = color.source->oldRawData();
            std::memcpy(it.rawData(), original, cs->pixelSize());
            if (!(cs->opacityF(original) > 0.0))
                continue;
            color.decode(original, color.values);
            const double originalLight = luminance(color.values, color);
            if (!std::isfinite(originalLight))
                continue;
            double outlineInk = 0, hatchInk = 0;
            if (outlines > 0.0) {
                const int x = it.x() - smoothRect.x(), y = it.y() - smoothRect.y();
                const double center = smoothed[size_t(y) * smoothRect.width() + x].light;
                const auto sample = [&](int dx, int dy) {
                    const auto &p = smoothed[size_t(y + dy) * smoothRect.width() + x + dx];
                    return p.coverage > 0.0f ? double(p.light) : center;
                };
                const double gx = (sample(1, -1) + 2 * sample(1, 0) + sample(1, 1) - sample(-1, -1) - 2 * sample(-1, 0)
                                   - sample(-1, 1))
                    / 8.0;
                const double gy = (sample(-1, 1) + 2 * sample(0, 1) + sample(1, 1) - sample(-1, -1) - 2 * sample(0, -1)
                                   - sample(1, -1))
                    / 8.0;
                outlineInk = .85 * outlines * qMin(1.0, 3.0 * std::hypot(gx, gy));
            }
            if (hatching > 0.0) {
                const double darkness = 1.0 - qBound(0.0, originalLight, 1.0);
                const double u = (it.x() + .5) * cosAngle + (it.y() + .5) * sinAngle;
                const double v = -(it.x() + .5) * sinAngle + (it.y() + .5) * cosAngle;
                const qint64 line = qint64(std::floor(v / spacing));
                const qint64 segment = qint64(std::floor(u / segmentLength));
                const double phase = smoothstep(0.0, 1.0, u / segmentLength - segment);
                const double width = qMin(2.0, .35 + .12 * spacing * (.25 + .75 * darkness));
                double coverage = 0;
                for (qint64 row = line - 1; row <= line + 1; ++row) {
                    double noise = .5;
                    if (roughness > 0.0)
                        noise = (1.0 - phase) * strokes.doubleRandomAt(segment, row)
                            + phase * strokes.doubleRandomAt(segment + 1, row);
                    const double center = (row + .5 + roughness * .45 * (noise - .5)) * spacing;
                    const double stroke = 1.0 - smoothstep(qMax(0.0, width - .5), width + .5, std::abs(v - center));
                    const double broken =
                        1.0 - roughness + roughness * smoothstep(.12, .45, noise) * (.45 + .55 * noise);
                    coverage += stroke * broken;
                }
                const double paper = 1.0 + .15 * roughness * (2.0 * grain.doubleRandomAt(it.x(), it.y()) - 1.0);
                hatchInk = .8 * hatching * std::pow(darkness, .8) * qBound(0.0, coverage * paper, 1.0);
            }
            // Independent outline/hatch masks combine only here. Avoid storing
            // full intermediate RGB images or changing source alpha.
            const double ink = (1.0 - outlineInk) * (1.0 - hatchInk);
            for (int index : {color.red, color.green, color.blue}) {
                double value = grayscale ? originalLight : color.values[index];
                value += qMax(0.0, 1.0 - value) * .32 * hatching;
                color.values[index] = float(value * ink);
            }
            color.encode(color.values, reinterpret_cast<quint8 *>(encoded.data()));
            for (int index : enabled) {
                const auto *channel = channels[index];
                std::memcpy(it.rawData() + channel->pos(), encoded.constData() + channel->pos(), channel->size());
            }
        }
    }
    color.source.clear();
    if (progress && progress->interrupted())
        return;
    KisPainter::copyAreaOptimized(rect.topLeft(), output, device, rect);
    if (progress)
        progress->setProgress(100);
}
