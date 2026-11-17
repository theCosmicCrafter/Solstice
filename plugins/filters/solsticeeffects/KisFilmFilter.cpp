/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisFilmFilter.h"
#include "KisCpuEffectKernels.h"
#include "KisEffectColorSampler.h"
#include "KisFilmConfigWidget.h"

#include <KisSequentialIteratorProgress.h>
#include <KoColor.h>
#include <KoProgressProxy.h>
#include <KoUpdater.h>
#include <QRandomGenerator>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_painter.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
// Worker-local progress mapping, without QObject/timers or nested event loops.
// Cancellation always reads the original updater, including inside kernels.
class StageProgress : public KoProgressProxy
{
public:
    StageProgress(KoUpdater *updater, int begin, int span)
        : m_updater(updater)
        , m_begin(begin)
        , m_span(span)
    {
    }
    int maximum() const override
    {
        return m_max;
    }
    void setRange(int minimum, int maximum) override
    {
        m_min = minimum;
        m_max = maximum;
    }
    void setFormat(const QString &) override
    {
    }
    void setValue(int value) override
    {
        if (m_updater) {
            const double fraction = m_max > m_min ? double(value - m_min) / (m_max - m_min) : 1.0;
            m_updater->setProgress(m_begin + int(m_span * qBound(0.0, fraction, 1.0)));
        }
    }

private:
    KoUpdater *m_updater;
    int m_begin, m_span;
    int m_min = 0, m_max = 100;
};

double
setting(const KisFilterConfigurationSP config, const char *name, double fallback, double low = 0.0, double high = 100.0)
{
    const double value = config->getDouble(name, fallback);
    return qBound(low, std::isfinite(value) ? value : fallback, high);
}

KisFilterConfigurationSP aberrationSettings(const KisFilterConfigurationSP film)
{
    // A local configuration, not a registry filter invocation or another undo
    // stroke. The shared kernel only uses these explicit properties.
    auto config = film->clone();
    config->setChannelFlags(QBitArray());
    config->setProperty("mode", 0);
    config->setProperty("strength", setting(film, "aberrationStrength", 1.5));
    config->setProperty("angle", setting(film, "aberrationAngle", 90.0, -180.0, 180.0));
    return config;
}

void applyTone(KisPaintDeviceSP device,
               const QRect &rect,
               const KisFilterConfigurationSP config,
               KoUpdater *progress,
               KoProgressProxy *reporter)
{
    const int type = config->getInt("toneType", KisFilmFilter::Sepia);
    const double tone = type >= KisFilmFilter::Fade && type <= KisFilmFilter::Cool
        ? setting(config, "toneStrength", 60.0) / 100.0
        : 0.0;
    const double leak = setting(config, "lightLeakStrength", 35.0) / 100.0;
    if (tone == 0.0 && leak == 0.0) {
        return;
    }
    const double cx = setting(config, "lightLeakX", 1000.0, -1e9, 1e9);
    const double cy = setting(config, "lightLeakY", 500.0, -1e9, 1e9);
    const double radius = setting(config, "lightLeakRadius", 750.0, 1.0, 1e9);
    SolsticeEffects::ColorSampler codec(device);
    KIS_SAFE_ASSERT_RECOVER_RETURN(codec.red >= 0 && codec.green >= 0 && codec.blue >= 0);
    const std::array<int, 3> rgb = {codec.red, codec.green, codec.blue};
    const std::array<double, 3> leakTint = {1.0, 0.38, 0.08};
    const auto *cs = device->colorSpace();
    const auto channels = cs->channels();
    QVector<float> values(codec.working->channelCount());
    QByteArray converted(cs->pixelSize(), '\0');
    KisSequentialIteratorProgress it(device, rect, reporter);
    while (it.nextPixel()) {
        if (it.x() == rect.x() && progress && progress->interrupted())
            return;
        if (!(cs->opacityF(it.rawDataConst()) > 0.0))
            continue;
        codec.decode(it.rawDataConst(), values);
        if (!std::isfinite(values[rgb[0]]) || !std::isfinite(values[rgb[1]]) || !std::isfinite(values[rgb[2]]))
            continue;
        const double luma = 0.2126 * values[rgb[0]] + 0.7152 * values[rgb[1]] + 0.0722 * values[rgb[2]];
        std::array<double, 3> target = {values[rgb[0]], values[rgb[1]], values[rgb[2]]};
        if (tone > 0.0) {
            switch (type) {
            case KisFilmFilter::Fade:
                for (double &v : target)
                    v = 0.08 + 0.86 * v;
                break;
            case KisFilmFilter::Sepia:
                target = {1.12 * luma + 0.04, luma + 0.025, 0.78 * luma + 0.01};
                break;
            case KisFilmFilter::WarmTone:
                target = {1.06 * target[0] + 0.025, target[1] + 0.01, 0.90 * target[2]};
                break;
            case KisFilmFilter::Cool:
                target = {0.93 * target[0], target[1] + 0.01, 1.08 * target[2] + 0.02};
                break;
            }
        }
        const double dx = (it.x() - cx) / radius;
        const double dy = (it.y() - cy) / radius;
        const double exposure = leak > 0.0 ? leak * std::exp(-2.0 * (dx * dx + dy * dy)) : 0.0;
        for (int c = 0; c < 3; ++c) {
            const double original = values[rgb[c]];
            double result = original + tone * (target[c] - original);
            // Screen-like colored exposure, with no clipping of float HDR.
            result += qMax(0.0, 1.0 - result) * exposure * leakTint[c];
            values[rgb[c]] = float(result);
        }
        codec.encode(values, reinterpret_cast<quint8 *>(converted.data()));
        for (const auto *channel : channels) {
            if (channel->channelType() == KoChannelInfo::COLOR) {
                std::memcpy(it.rawData() + channel->pos(), converted.constData() + channel->pos(), channel->size());
            }
        }
    }
}
} // namespace

KisFilmFilter::KisFilmFilter()
    : KisFilter(KoID("solstice_film", i18n("Film")), FiltersCategoryArtisticId, i18n("&Film..."))
{
    setSupportsThreading(true);
    setSupportsAdjustmentLayers(true);
    setSupportsLevelOfDetail(false);
    setSupportsPainting(false);
}

void KisFilmFilter::applyPreset(KisPropertiesConfigurationSP config, int preset)
{
    if (preset < Vintage || preset > Warm)
        return;
    config->setProperty("preset", preset);
    config->setProperty("toneType", preset == Vintage ? Sepia : preset == Modern ? Cool : WarmTone);
    config->setProperty("toneStrength", preset == Vintage ? 60.0 : preset == Modern ? 25.0 : 70.0);
    config->setProperty("aberrationStrength", preset == Vintage ? 1.5 : preset == Modern ? 0.75 : 0.0);
    config->setProperty("aberrationAngle", 90.0);
    config->setProperty("grainStrength", preset == Vintage ? 20.0 : preset == Modern ? 8.0 : 0.0);
    config->setProperty("monochrome", true);
    config->setProperty("lightLeakStrength", preset == Vintage ? 35.0 : preset == Modern ? 8.0 : 10.0);
}

KisFilterConfigurationSP KisFilmFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    auto config = factoryConfiguration(resourcesInterface);
    applyPreset(config, Vintage);
    config->setProperty("seed", QRandomGenerator::global()->bounded(std::numeric_limits<int>::max()));
    // Geometry is initialized by the widget from the canvas and then saved.
    return config;
}

KisConfigWidget *KisFilmFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool) const
{
    return new KisFilmConfigWidget(parent, device);
}

QRect KisFilmFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    return SolsticeEffects::chromaticNeededRect(rect, aberrationSettings(config));
}

QRect KisFilmFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}

void KisFilmFilter::processImpl(KisPaintDeviceSP device,
                                const QRect &rect,
                                const KisFilterConfigurationSP config,
                                KoUpdater *progressUpdater) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(device && config);
    if (rect.isEmpty() || (progressUpdater && progressUpdater->interrupted()))
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
    const auto aberration = aberrationSettings(config);
    const double grain = setting(config, "grainStrength", 20.0);
    const int type = config->getInt("toneType", Sepia);
    const bool tonal = (type >= Fade && type <= Cool && setting(config, "toneStrength", 60.0) > 0.0)
        || setting(config, "lightLeakStrength", 35.0) > 0.0;
    if (aberration->getDouble("strength") == 0.0 && grain == 0.0 && !tonal)
        return;

    StageProgress aberrationProgress(progressUpdater, 0, 50);
    StageProgress toneProgress(progressUpdater, 50, 25);
    StageProgress grainProgress(progressUpdater, 75, 20);
    StageProgress copyProgress(progressUpdater, 95, 5);
    const auto cancelled = [&]() {
        return progressUpdater && progressUpdater->interrupted();
    };

    // Snapshot only this patch's source halo. All reads from the original
    // transaction finish before committing; other jobs cannot pollute inputs.
    KisPaintDeviceSP scratch = new KisPaintDevice(cs);
    scratch->setX(device->x());
    scratch->setY(device->y());
    scratch->setDefaultPixel(device->defaultPixel());
    const QRect input = SolsticeEffects::chromaticNeededRect(rect, aberration);
    KisPainter::copyAreaOptimizedOldData(input.topLeft(), device, scratch, input);
    if (cancelled())
        return;
    SolsticeEffects::applyChromaticAberration(scratch, rect, aberration, progressUpdater, &aberrationProgress);
    aberrationProgress.setValue(aberrationProgress.maximum());
    if (cancelled())
        return;
    applyTone(scratch, rect, config, progressUpdater, &toneProgress);
    toneProgress.setValue(toneProgress.maximum());
    if (cancelled())
        return;
    auto grainConfig = config->clone();
    grainConfig->setChannelFlags(QBitArray());
    grainConfig->setProperty("strength", grain);
    grainConfig->setProperty("monochrome", config->getBool("monochrome", true));
    SolsticeEffects::applyGrain(scratch, rect, grainConfig, progressUpdater, &grainProgress);
    grainProgress.setValue(grainProgress.maximum());
    if (cancelled())
        return;

    auto result = scratch->createRandomConstAccessorNG();
    KisSequentialIteratorProgress it(device, rect, &copyProgress);
    while (it.nextPixel()) {
        if (it.x() == rect.x() && cancelled())
            return;
        // Copy only final enabled channels. All stages see the complete color,
        // even if a channel is disabled for the final application.
        if (!(cs->opacityF(it.rawDataConst()) > 0.0))
            continue;
        result->moveTo(it.x(), it.y());
        for (int index : enabled) {
            const auto *channel = channels[index];
            std::memcpy(it.rawData() + channel->pos(), result->rawDataConst() + channel->pos(), channel->size());
        }
    }
}
