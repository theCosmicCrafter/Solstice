/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <simpletest.h>

#include <QElapsedTimer>
#include <QFloat16>
#include <QObject>

#include <KisGlobalResourcesInterface.h>
#include <KisGpuContext.h>
#include <KisGpuLayerCompositor.h>
#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>

#include "KisImageResolutionProxy.h"
#include "filter/kis_filter.h"
#include "filter/kis_filter_configuration.h"
#include "filter/kis_filter_registry.h"
#include "gpu/KisGpuMergeBatch.h"
#include "gpu/KisGpuProjectionCompositor.h"
#include "gpu/KisGpuTileBackend.h"
#include "kis_adjustment_layer.h"
#include "kis_default_bounds.h"
#include "kis_filter_mask.h"
#include "kis_group_layer.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"
#include "kis_painter.h"
#include "kis_pixel_selection.h"
#include "kis_selection.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

/**
 * Opt-in measurement of what filter layers cost while a layer below them is
 * painted (KRITA_FILTER_BENCHMARK=1). Prints one line per scenario: the
 * update time of each dab (setDirty() of the painted layer until the image
 * and the GPU are idle), the GPU tiles read back to the CPU during those
 * updates, and the time of a refresh of the whole image.
 */
class KisFilterLayerBenchmark : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void benchmarkPaintedLayer();
};

namespace
{
enum class Mode {
    Rgba8,
    F16Cpu,
    F16Gpu,
    F32Cpu,
    F32Gpu
};

const char *modeName(Mode mode)
{
    switch (mode) {
    case Mode::Rgba8:
        return "RGBA8";
    case Mode::F16Cpu:
        return "F16-CPU";
    case Mode::F16Gpu:
        return "F16-GPU";
    case Mode::F32Cpu:
        return "F32-CPU";
    case Mode::F32Gpu:
        return "F32-GPU";
    }
    return "?";
}

const KoColorSpace *colorSpaceFor(Mode mode)
{
    if (mode == Mode::Rgba8) {
        return KoColorSpaceRegistry::instance()->rgb8();
    }
    const bool f16 = mode == Mode::F16Cpu || mode == Mode::F16Gpu;
    return KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(),
                                                        f16 ? Float16BitsColorDepthID.id()
                                                            : Float32BitsColorDepthID.id(),
                                                        QString());
}

void fillRandom(KisPaintDeviceSP device, const QRect &rect, quint32 seed)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const size_t count = size_t(rect.width()) * rect.height() * 4;
    if (device->pixelSize() == 4) {
        std::vector<quint8> bytes(count);
        for (quint8 &value : bytes) {
            value = quint8(unit(random) * 255.0f);
        }
        device->writeBytes(bytes.data(), rect);
        return;
    }
    std::vector<float> floats(count);
    for (float &value : floats) {
        value = unit(random);
    }
    if (device->pixelSize() == 8) {
        std::vector<qfloat16> halfs(count);
        qFloatToFloat16(halfs.data(), floats.data(), qsizetype(count));
        device->writeBytes(reinterpret_cast<const quint8 *>(halfs.data()), rect);
    } else {
        device->writeBytes(reinterpret_cast<const quint8 *>(floats.data()), rect);
    }
}

std::vector<float> readNormalized(KisPaintDeviceSP device, const QRect &rect)
{
    const size_t count = size_t(rect.width()) * rect.height() * 4;
    std::vector<float> result(count);
    if (device->pixelSize() == 4) {
        std::vector<quint8> bytes(count);
        device->readBytes(bytes.data(), rect);
        for (size_t i = 0; i < count; ++i) {
            result[i] = bytes[i] / 255.0f;
        }
    } else if (device->pixelSize() == 8) {
        std::vector<qfloat16> halfs(count);
        device->readBytes(reinterpret_cast<quint8 *>(halfs.data()), rect);
        qFloatFromFloat16(result.data(), halfs.data(), qsizetype(count));
    } else {
        device->readBytes(reinterpret_cast<quint8 *>(result.data()), rect);
    }
    return result;
}

/// Largest channel difference, normalized to 0..1.
double maxDifference(const std::vector<float> &a, const std::vector<float> &b)
{
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, double(std::abs(a[i] - b[i])));
    }
    return worst;
}

double median(std::vector<double> samples)
{
    std::sort(samples.begin(), samples.end());
    return (samples[(samples.size() - 1) / 2] + samples[samples.size() / 2]) / 2;
}

enum class Stack {
    None,
    Desaturate,
    Posterize,
    Curves,
    DesaturatePosterizeSelected,
    ColorReference,
    DesaturateMask,
    ColorReferenceGroupMask
};

const char *stackName(Stack stack)
{
    switch (stack) {
    case Stack::None:
        return "none";
    case Stack::Desaturate:
        return "desaturate";
    case Stack::Posterize:
        return "posterize";
    case Stack::Curves:
        return "curves";
    case Stack::DesaturatePosterizeSelected:
        return "desaturate+posterize, selection";
    case Stack::ColorReference:
        return "color reference";
    case Stack::DesaturateMask:
        return "desaturate mask";
    case Stack::ColorReferenceGroupMask:
        return "color reference mask on the group";
    }
    return "?";
}

struct Scenario {
    Mode mode = Mode::Rgba8;
    Stack stack = Stack::None;
};

struct Result {
    double dabMedian = 0.0;
    double downloadsPerDab = 0.0;
    double refresh = 0.0;
    double downloadsPerRefresh = 0.0;
    double difference = 0.0;
};

// The size of the user's documents that have filter layers.
const QSize ImageSize(1754, 2480);
const QRect LayerContent(300, 500, 1200, 1200);
const QSize DabSize(256, 256);
constexpr int DabCount = 24;
constexpr int RefreshCount = 5;

KisFilterConfigurationSP filterConfig(const QString &id, const QString &xml)
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(id);
    KIS_ASSERT(filter);
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    config->fromXML(xml);
    return config->cloneWithResourcesSnapshot();
}

KisFilterConfigurationSP desaturateConfig()
{
    // Lightness, the type the user's documents use.
    return filterConfig("desaturate",
                        "<!DOCTYPE params>\n<params version=\"0\">\n"
                        " <param name=\"type\" type=\"string\"><![CDATA[0]]></param>\n</params>\n");
}

KisFilterConfigurationSP posterizeConfig()
{
    return filterConfig("posterize",
                        "<!DOCTYPE params>\n<params version=\"0\">\n"
                        " <param name=\"steps\" type=\"string\"><![CDATA[6]]></param>\n</params>\n");
}

KisFilterConfigurationSP curvesConfig()
{
    QString xml =
        "<!DOCTYPE params>\n<params version=\"1\">\n <param name=\"nTransfers\">8</param>\n"
        " <param name=\"curve0\">0,0;0.551807,0.415686;1,1;</param>\n";
    for (int i = 1; i < 8; ++i) {
        xml += QString(" <param name=\"curve%1\">0,0;1,1;</param>\n").arg(i);
    }
    xml += "</params>\n";
    return filterConfig("perchannel", xml);
}

KisFilterConfigurationSP colorReferenceConfig()
{
    // The filter's defaults: the Amber preset, Amount 100, Keep brightness.
    KisFilterSP filter = KisFilterRegistry::instance()->value("solstice_color_reference");
    KIS_ASSERT(filter);
    return filter->defaultConfiguration(KisGlobalResourcesInterface::instance())->cloneWithResourcesSnapshot();
}

/// A painted selection: a selected block with a soft band at its right edge.
KisSelectionSP paintedSelection(KisImageSP image)
{
    KisSelectionSP selection =
        new KisSelection(new KisDefaultBounds(image), toQShared(new KisImageResolutionProxy(image)));
    KisPixelSelectionSP pixels = selection->pixelSelection();
    pixels->select(QRect(200, 400, 900, 1400), MAX_SELECTED);
    for (int i = 0; i < 64; ++i) {
        pixels->select(QRect(1100 + i * 4, 400, 4, 1400), quint8(255 - i * 4));
    }
    selection->updateProjection();
    return selection;
}

Result run(const Scenario &s)
{
    const KoColorSpace *cs = colorSpaceFor(s.mode);
    const bool gpu = s.mode == Mode::F16Gpu || s.mode == Mode::F32Gpu;
    KisGpuMergeBatch::setEnabled(gpu);
    KisGpuTileBackend *backend = gpu ? KisGpuTileBackend::instance() : nullptr;
    auto waitGpu = [&]() {
        if (backend) {
            backend->context().waitIdle();
        }
    };

    KisImageSP image = new KisImage(nullptr, ImageSize.width(), ImageSize.height(), cs, "filter benchmark");
    const QRect bounds = image->bounds();

    KisPaintLayerSP background = new KisPaintLayer(image, "background", 255);
    background->paintDevice()->fill(bounds, KoColor(Qt::white, cs));
    image->addNode(background, image->root());

    // The mask scenarios paint inside a group, like the user's document
    // whose main group has a Color Reference filter mask.
    const bool grouped = s.stack == Stack::ColorReferenceGroupMask;
    KisGroupLayerSP group;
    if (grouped) {
        group = new KisGroupLayer(image, "group", 255);
        image->addNode(group, image->root());
    }
    KisPaintLayerSP painted = new KisPaintLayer(image, "painted", 255);
    fillRandom(painted->paintDevice(), LayerContent, 1);
    image->addNode(painted, grouped ? KisNodeSP(group) : image->root());

    auto addFilterMask = [&](KisNodeSP parent, KisFilterConfigurationSP config) {
        KisFilterMaskSP mask = new KisFilterMask(image, "filter mask");
        mask->initSelection(qobject_cast<KisLayer *>(parent.data()));
        mask->setFilter(config);
        image->addNode(mask, parent);
    };

    auto addFilter = [&](const QString &name, KisFilterConfigurationSP config, KisSelectionSP selection) {
        KisAdjustmentLayerSP layer = new KisAdjustmentLayer(image, name, config, selection);
        image->addNode(layer, image->root());
    };
    switch (s.stack) {
    case Stack::None:
        break;
    case Stack::Desaturate:
        addFilter("desaturate", desaturateConfig(), nullptr);
        break;
    case Stack::Posterize:
        addFilter("posterize", posterizeConfig(), nullptr);
        break;
    case Stack::Curves:
        addFilter("curves", curvesConfig(), nullptr);
        break;
    case Stack::DesaturatePosterizeSelected:
        // What the user's documents have: both layers with the same painted selection.
        addFilter("desaturate", desaturateConfig(), paintedSelection(image));
        addFilter("posterize", posterizeConfig(), paintedSelection(image));
        break;
    case Stack::ColorReference:
        addFilter("color reference", colorReferenceConfig(), nullptr);
        break;
    case Stack::DesaturateMask:
        addFilterMask(painted, desaturateConfig());
        break;
    case Stack::ColorReferenceGroupMask:
        addFilterMask(group, colorReferenceConfig());
        break;
    }

    KisPaintLayerSP top = new KisPaintLayer(image, "top", 180);
    fillRandom(top->paintDevice(), QRect(0, 0, 1600, 1600), 3);
    top->setCompositeOpId(COMPOSITE_MULT);
    image->addNode(top, image->root());

    image->refreshGraphAsync();
    image->waitForDone();
    waitGpu();

    Result result;
    QElapsedTimer timer;
    std::vector<double> samples;
    quint64 downloads = 0;
    for (int i = 0; i < DabCount; ++i) {
        const QRect rect(QPoint(400 + i * 20, 600 + i * 16), DabSize);
        KisPaintDeviceSP dab = new KisPaintDevice(cs);
        fillRandom(dab, rect, 100 + i);
        bool written = false;
        if (gpu) {
            // Like the GPU brush: the layer's tiles become GPU-valid, CPU-stale.
            KisGpuProjectionCompositor::Layer layer;
            layer.device = dab;
            layer.op = KisGpuBlendOp::Over;
            layer.opacity = 0.8f;
            QString error;
            written = KisGpuProjectionCompositor::composite(painted->paintDevice(), rect, {layer}, &error);
            if (!written) {
                qWarning() << "GPU dab failed, CPU dab used:" << error;
            }
        }
        if (!written) {
            KisPainter gc(painted->paintDevice());
            gc.setOpacityF(0.8);
            gc.bitBlt(rect.topLeft(), dab, rect);
        }
        waitGpu();

        const quint64 downloadsBefore = backend ? backend->downloadedTileCount() : 0;
        timer.start();
        painted->setDirty(rect);
        image->waitForDone();
        waitGpu();
        samples.push_back(timer.nsecsElapsed() / 1.0e6);
        downloads += (backend ? backend->downloadedTileCount() : 0) - downloadsBefore;
    }
    result.dabMedian = median(samples);
    result.downloadsPerDab = double(downloads) / DabCount;

    // Sanity check: the incremental result equals a full refresh.
    const std::vector<float> incremental = readNormalized(image->projection(), bounds);

    samples.clear();
    downloads = 0;
    for (int i = 0; i < RefreshCount; ++i) {
        waitGpu();
        const quint64 downloadsBefore = backend ? backend->downloadedTileCount() : 0;
        timer.start();
        image->refreshGraphAsync();
        image->waitForDone();
        waitGpu();
        samples.push_back(timer.nsecsElapsed() / 1.0e6);
        downloads += (backend ? backend->downloadedTileCount() : 0) - downloadsBefore;
    }
    result.refresh = median(samples);
    result.downloadsPerRefresh = double(downloads) / RefreshCount;
    result.difference = maxDifference(incremental, readNormalized(image->projection(), bounds));

    image->waitForDone();
    return result;
}
} // namespace

void KisFilterLayerBenchmark::benchmarkPaintedLayer()
{
    if (!qEnvironmentVariableIsSet("KRITA_FILTER_BENCHMARK")) {
        QSKIP("set KRITA_FILTER_BENCHMARK=1");
    }

    // KRITA_FILTER_BENCHMARK_GPU_ONLY=1: only the GPU engine scenarios.
    QVector<Mode> modes;
    if (!qEnvironmentVariableIsSet("KRITA_FILTER_BENCHMARK_GPU_ONLY")) {
        modes << Mode::Rgba8 << Mode::F16Cpu << Mode::F32Cpu;
    }
    if (KisGpuTileBackend::instance()) {
        modes << Mode::F16Gpu << Mode::F32Gpu;
    } else {
        qWarning() << "GPU engine unavailable:" << KisGpuTileBackend::unavailableReason();
    }

    qInfo().noquote() << QStringLiteral("Image %1x%2, %3 dabs of %4x%5 px, then %6 full refreshes; times in ms")
                             .arg(ImageSize.width())
                             .arg(ImageSize.height())
                             .arg(DabCount)
                             .arg(DabSize.width())
                             .arg(DabSize.height())
                             .arg(RefreshCount);
    qInfo().noquote() << "RESULT\tmode\tfilter layers\tdab median\tdownloads/dab\trefresh median\t"
                         "downloads/refresh\tmaxDiff";

    for (Mode mode : modes) {
        for (Stack stack : {Stack::None,
                            Stack::Desaturate,
                            Stack::Posterize,
                            Stack::Curves,
                            Stack::DesaturatePosterizeSelected,
                            Stack::ColorReference,
                            Stack::DesaturateMask,
                            Stack::ColorReferenceGroupMask}) {
            Scenario s;
            s.mode = mode;
            s.stack = stack;
            const Result r = run(s);
            qInfo().noquote() << QStringLiteral("RESULT\t%1\t%2\t%3\t%4\t%5\t%6\t%7")
                                     .arg(modeName(mode))
                                     .arg(stackName(stack))
                                     .arg(r.dabMedian, 0, 'f', 2)
                                     .arg(r.downloadsPerDab, 0, 'f', 1)
                                     .arg(r.refresh, 0, 'f', 2)
                                     .arg(r.downloadsPerRefresh, 0, 'f', 1)
                                     .arg(r.difference, 0, 'g', 3);
        }
    }
    KisGpuMergeBatch::setEnabled(false);
}

SIMPLE_TEST_MAIN(KisFilterLayerBenchmark)

#include "KisFilterLayerBenchmark.moc"
