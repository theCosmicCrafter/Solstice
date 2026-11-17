/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <simpletest.h>

#include <QElapsedTimer>
#include <QObject>

#include <KisGpuContext.h>
#include <KisGpuLayerCompositor.h>
#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>

#include "gpu/KisGpuMergeBatch.h"
#include "gpu/KisGpuProjectionCompositor.h"
#include "gpu/KisGpuTileBackend.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"
#include "kis_painter.h"
#include "kis_pixel_selection.h"
#include "kis_selection.h"
#include "kis_transparency_mask.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

/**
 * Opt-in measurement of what effect masks cost while their layer is painted
 * (KRITA_MASK_BENCHMARK=1). Prints one line per scenario: the update time of
 * each dab (setDirty() of the layer until the image and the GPU are idle),
 * the GPU tiles read back to the CPU during those updates, and the time of
 * a refresh of the whole image.
 */
class KisMaskedLayerBenchmark : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void benchmarkPaintedLayer();
};

namespace
{
enum class Mode {
    Rgba8,
    F32Cpu,
    F32Gpu
};

const char *modeName(Mode mode)
{
    switch (mode) {
    case Mode::Rgba8:
        return "RGBA8";
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
    return KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(), Float32BitsColorDepthID.id(), QString());
}

void fillRandom(KisPaintDeviceSP device, const QRect &rect, quint32 seed)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const size_t count = size_t(rect.width()) * rect.height() * 4;
    if (device->pixelSize() == 16) {
        std::vector<float> floats(count);
        for (float &value : floats) {
            value = unit(random);
        }
        device->writeBytes(reinterpret_cast<const quint8 *>(floats.data()), rect);
    } else {
        std::vector<quint8> bytes(count);
        for (quint8 &value : bytes) {
            value = quint8(unit(random) * 255.0f);
        }
        device->writeBytes(bytes.data(), rect);
    }
}

/// Largest channel difference, normalized to 0..1.
double maxDifference(KisPaintDeviceSP a, KisPaintDeviceSP b, const QRect &rect)
{
    const size_t count = size_t(rect.width()) * rect.height() * 4;
    double worst = 0.0;
    if (a->pixelSize() == 16) {
        std::vector<float> x(count), y(count);
        a->readBytes(reinterpret_cast<quint8 *>(x.data()), rect);
        b->readBytes(reinterpret_cast<quint8 *>(y.data()), rect);
        for (size_t i = 0; i < count; ++i) {
            worst = std::max(worst, double(std::abs(x[i] - y[i])));
        }
    } else {
        std::vector<quint8> x(count), y(count);
        a->readBytes(x.data(), rect);
        b->readBytes(y.data(), rect);
        for (size_t i = 0; i < count; ++i) {
            worst = std::max(worst, std::abs(int(x[i]) - int(y[i])) / 255.0);
        }
    }
    return worst;
}

double median(std::vector<double> samples)
{
    std::sort(samples.begin(), samples.end());
    return (samples[(samples.size() - 1) / 2] + samples[samples.size() / 2]) / 2;
}

struct Scenario {
    Mode mode = Mode::Rgba8;
    int transparencyMasks = 0;
};

struct Result {
    double dabMedian = 0.0;
    double downloadsPerDab = 0.0;
    double gpuCompositesPerDab = 0.0;
    double refresh = 0.0;
    double downloadsPerRefresh = 0.0;
    double difference = 0.0;
};

const QSize ImageSize(2480, 3508);
const QRect LayerContent(400, 600, 1200, 1200);
const QSize DabSize(256, 256);
constexpr int DabCount = 24;
constexpr int RefreshCount = 5;

Result run(const Scenario &s)
{
    const KoColorSpace *cs = colorSpaceFor(s.mode);
    const bool gpu = s.mode == Mode::F32Gpu;
    KisGpuMergeBatch::setEnabled(gpu);
    KisGpuTileBackend *backend = gpu ? KisGpuTileBackend::instance() : nullptr;
    auto waitGpu = [&]() {
        if (backend) {
            backend->context().waitIdle();
        }
    };

    KisImageSP image = new KisImage(nullptr, ImageSize.width(), ImageSize.height(), cs, "mask benchmark");
    const QRect bounds = image->bounds();

    KisPaintLayerSP background = new KisPaintLayer(image, "background", 255);
    background->paintDevice()->fill(bounds, KoColor(Qt::white, cs));
    image->addNode(background, image->root());

    KisPaintLayerSP painted = new KisPaintLayer(image, "painted", 255);
    fillRandom(painted->paintDevice(), LayerContent, 1);
    image->addNode(painted, image->root());

    for (int i = 0; i < s.transparencyMasks; ++i) {
        // A soft mask: a gradient, with a hole and an opaque band, so all of
        // 0, 255 and the values in between occur under the dabs.
        KisTransparencyMaskSP mask = new KisTransparencyMask(image, QStringLiteral("transparency %1").arg(i));
        mask->initSelection(KisLayerSP(painted));
        KisPixelSelectionSP pixels = mask->selection()->pixelSelection();
        const QRect maskRect = LayerContent.adjusted(-100, -100, 100, 100);
        std::vector<quint8> bytes(size_t(maskRect.width()) * maskRect.height());
        for (int y = 0; y < maskRect.height(); ++y) {
            for (int x = 0; x < maskRect.width(); ++x) {
                bytes[size_t(y) * maskRect.width() + x] = quint8((x + y * (i + 1)) % 256);
            }
        }
        pixels->writeBytes(bytes.data(), maskRect);
        pixels->clear(QRect(700 + 100 * i, 900, 300, 300));
        pixels->select(QRect(1000, 700 + 100 * i, 200, 500), 255);
        mask->selection()->updateProjection();
        image->addNode(mask, painted);
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
    quint64 composites = 0;
    for (int i = 0; i < DabCount; ++i) {
        const QRect rect(QPoint(500 + i * 20, 700 + i * 16), DabSize);
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
        const quint64 compositesBefore = KisGpuMergeBatch::gpuCompositeCount();
        timer.start();
        painted->setDirty(rect);
        image->waitForDone();
        waitGpu();
        samples.push_back(timer.nsecsElapsed() / 1.0e6);
        downloads += (backend ? backend->downloadedTileCount() : 0) - downloadsBefore;
        composites += KisGpuMergeBatch::gpuCompositeCount() - compositesBefore;
    }
    result.dabMedian = median(samples);
    result.downloadsPerDab = double(downloads) / DabCount;
    result.gpuCompositesPerDab = double(composites) / DabCount;

    // Sanity check: the incremental result equals a full refresh.
    KisPaintDeviceSP incremental = new KisPaintDevice(cs);
    KisPainter::copyAreaOptimized(bounds.topLeft(), image->projection(), incremental, bounds);

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
    result.difference = maxDifference(incremental, image->projection(), bounds);

    image->waitForDone();
    return result;
}
} // namespace

void KisMaskedLayerBenchmark::benchmarkPaintedLayer()
{
    if (!qEnvironmentVariableIsSet("KRITA_MASK_BENCHMARK")) {
        QSKIP("set KRITA_MASK_BENCHMARK=1");
    }

    // KRITA_MASK_BENCHMARK_GPU_ONLY=1: only the GPU engine scenarios.
    QVector<Mode> modes;
    if (!qEnvironmentVariableIsSet("KRITA_MASK_BENCHMARK_GPU_ONLY")) {
        modes << Mode::Rgba8 << Mode::F32Cpu;
    }
    if (KisGpuTileBackend::instance()) {
        modes << Mode::F32Gpu;
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
    qInfo().noquote()
        << "RESULT\tmode\ttransparency masks\tdab median\tdownloads/dab\tgpuComposites/dab\trefresh median\t"
           "downloads/refresh\tmaxDiff";

    for (Mode mode : modes) {
        for (int masks : {0, 1, 2}) {
            Scenario s;
            s.mode = mode;
            s.transparencyMasks = masks;
            const Result r = run(s);
            qInfo().noquote() << QStringLiteral("RESULT\t%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8")
                                     .arg(modeName(mode))
                                     .arg(masks)
                                     .arg(r.dabMedian, 0, 'f', 2)
                                     .arg(r.downloadsPerDab, 0, 'f', 1)
                                     .arg(r.gpuCompositesPerDab, 0, 'f', 1)
                                     .arg(r.refresh, 0, 'f', 2)
                                     .arg(r.downloadsPerRefresh, 0, 'f', 1)
                                     .arg(r.difference, 0, 'g', 3);
        }
    }
    KisGpuMergeBatch::setEnabled(false);
}

SIMPLE_TEST_MAIN(KisMaskedLayerBenchmark)

#include "KisMaskedLayerBenchmark.moc"
