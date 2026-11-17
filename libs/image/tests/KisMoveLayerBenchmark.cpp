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
#include "kis_abstract_projection_plane.h"
#include "kis_group_layer.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"
#include "kis_painter.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

/**
 * Opt-in measurement of Move Tool update costs (KRITA_MOVE_BENCHMARK=1).
 * Each step does what MoveStrokeStrategy::doCanvasUpdate() does for one
 * pointer position: the dirty rect is the moved nodes' visible bounds before
 * and after the move, the nodes get their new offset, and the subtree is
 * refreshed. Prints one line per scenario: the time of a step (until the
 * image and the GPU are idle), the part of it spent on the bounds and the
 * offsets, and the GPU tiles read back to the CPU. Afterwards the layer stays
 * where the last step left it and dabs are painted on a layer above it, to
 * measure what a moved layer costs later.
 */
class KisMoveLayerBenchmark : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void benchmarkMove();
};

namespace
{
enum class Mode {
    Rgba8,
    F32Cpu,
    F32Gpu
};
enum class Moved {
    FullLayer,
    ObjectLayer,
    Group
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

const char *movedName(Moved moved)
{
    switch (moved) {
    case Moved::FullLayer:
        return "full layer";
    case Moved::ObjectLayer:
        return "object layer";
    case Moved::Group:
        return "group";
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
    Moved moved = Moved::FullLayer;
    bool aligned = false;
};

struct Result {
    double stepMedian = 0.0;
    double stepMean = 0.0;
    double boundsMedian = 0.0;
    double downloadsPerStep = 0.0;
    double gpuCompositesPerStep = 0.0;
    double dabMedian = 0.0;
    double downloadsPerDab = 0.0;
    double difference = 0.0;
};

const QSize ImageSize(2480, 3508);
const QRect ObjectContent(400, 600, 1200, 1200);
const QRect SecondObjectContent(900, 1500, 800, 800);
const QSize DabSize(256, 256);
constexpr int StepCount = 12;
constexpr int DabCount = 12;

QPoint stepOffset(int i, bool aligned)
{
    return aligned ? QPoint(64 * (i + 1), 64 * (i / 2 + 1)) : QPoint(37 + 61 * i, 23 + 35 * i);
}

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

    KisImageSP image = new KisImage(nullptr, ImageSize.width(), ImageSize.height(), cs, "move benchmark");
    const QRect bounds = image->bounds();

    KisPaintLayerSP background = new KisPaintLayer(image, "background", 255);
    background->paintDevice()->fill(bounds, KoColor(Qt::white, cs));
    image->addNode(background, image->root());

    KisPaintLayerSP below = new KisPaintLayer(image, "below", 255);
    fillRandom(below->paintDevice(), bounds, 1);
    image->addNode(below, image->root());

    // The nodes the Move Tool would move (MoveStrokeStrategy::recursiveApplyNodes).
    KisNodeSP movedRoot;
    QVector<KisNodeSP> movedNodes;
    if (s.moved == Moved::Group) {
        KisGroupLayerSP group = new KisGroupLayer(image, "moved group", 255);
        image->addNode(group, image->root());
        KisPaintLayerSP first = new KisPaintLayer(image, "first", 255);
        fillRandom(first->paintDevice(), ObjectContent, 2);
        image->addNode(first, group);
        KisPaintLayerSP second = new KisPaintLayer(image, "second", 255);
        fillRandom(second->paintDevice(), SecondObjectContent, 3);
        image->addNode(second, group);
        movedRoot = group;
        movedNodes << group << first << second;
    } else {
        KisPaintLayerSP layer = new KisPaintLayer(image, "moved", 255);
        fillRandom(layer->paintDevice(), s.moved == Moved::FullLayer ? bounds : ObjectContent, 2);
        image->addNode(layer, image->root());
        movedRoot = layer;
        movedNodes << layer;
    }

    KisPaintLayerSP above = new KisPaintLayer(image, "above", 180);
    fillRandom(above->paintDevice(), bounds, 4);
    above->setCompositeOpId(COMPOSITE_MULT);
    image->addNode(above, image->root());

    KisPaintLayerSP top = new KisPaintLayer(image, "top", 255);
    image->addNode(top, image->root());

    image->refreshGraphAsync();
    image->waitForDone();
    waitGpu();

    Result result;
    QElapsedTimer timer;
    std::vector<double> stepSamples;
    std::vector<double> boundsSamples;
    quint64 downloads = 0;
    quint64 composites = 0;
    for (int i = 0; i < StepCount; ++i) {
        const QPoint offset = stepOffset(i, s.aligned);
        const quint64 downloadsBefore = backend ? backend->downloadedTileCount() : 0;
        const quint64 compositesBefore = KisGpuMergeBatch::gpuCompositeCount();
        timer.start();
        // MoveNodeStrategyBase::moveNodeCommon(); every node starts at 0,0
        QRect dirtyRect;
        for (KisNodeSP node : movedNodes) {
            QRect nodeRect = node->inherits("KisGroupLayer") ? node->projectionPlane()->looseUserVisibleBounds()
                                                             : node->projectionPlane()->tightUserVisibleBounds();
            nodeRect |= nodeRect.translated(offset - QPoint(node->x(), node->y()));
            node->setX(offset.x());
            node->setY(offset.y());
            dirtyRect |= nodeRect;
        }
        boundsSamples.push_back(timer.nsecsElapsed() / 1.0e6);
        image->refreshGraphAsync(movedRoot, dirtyRect);
        image->waitForDone();
        waitGpu();
        stepSamples.push_back(timer.nsecsElapsed() / 1.0e6);
        downloads += (backend ? backend->downloadedTileCount() : 0) - downloadsBefore;
        composites += KisGpuMergeBatch::gpuCompositeCount() - compositesBefore;
    }
    result.stepMedian = median(stepSamples);
    double sum = 0.0;
    for (double sample : stepSamples) {
        sum += sample;
    }
    result.stepMean = sum / stepSamples.size();
    result.boundsMedian = median(boundsSamples);
    result.downloadsPerStep = double(downloads) / StepCount;
    result.gpuCompositesPerStep = double(composites) / StepCount;

    // The moved nodes stay where they are; paint on the layer above them.
    const QPoint parked = stepOffset(StepCount - 1, s.aligned);
    std::vector<double> dabSamples;
    downloads = 0;
    for (int i = 0; i < DabCount; ++i) {
        const QRect rect(ObjectContent.topLeft() + parked + QPoint(100 + i * 20, 100 + i * 16), DabSize);
        KisPaintDeviceSP dab = new KisPaintDevice(cs);
        fillRandom(dab, rect, 100 + i);
        bool written = false;
        if (gpu) {
            // Like the GPU brush: the painted tiles become GPU-valid, CPU-stale.
            KisGpuProjectionCompositor::Layer layer;
            layer.device = dab;
            layer.op = KisGpuBlendOp::Over;
            layer.opacity = 0.8f;
            QString error;
            written = KisGpuProjectionCompositor::composite(top->paintDevice(), rect, {layer}, &error);
            if (!written) {
                qWarning() << "GPU dab failed, CPU dab used:" << error;
            }
        }
        if (!written) {
            KisPainter gc(top->paintDevice());
            gc.setOpacityF(0.8);
            gc.bitBlt(rect.topLeft(), dab, rect);
        }
        waitGpu();

        const quint64 downloadsBefore = backend ? backend->downloadedTileCount() : 0;
        timer.start();
        top->setDirty(rect);
        image->waitForDone();
        waitGpu();
        dabSamples.push_back(timer.nsecsElapsed() / 1.0e6);
        downloads += (backend ? backend->downloadedTileCount() : 0) - downloadsBefore;
    }
    result.dabMedian = median(dabSamples);
    result.downloadsPerDab = double(downloads) / DabCount;

    // Sanity check: the incremental result equals a full refresh.
    KisPaintDeviceSP incremental = new KisPaintDevice(cs);
    KisPainter::copyAreaOptimized(bounds.topLeft(), image->projection(), incremental, bounds);
    image->refreshGraphAsync();
    image->waitForDone();
    waitGpu();
    result.difference = maxDifference(incremental, image->projection(), bounds);

    image->waitForDone();
    return result;
}
} // namespace

void KisMoveLayerBenchmark::benchmarkMove()
{
    if (!qEnvironmentVariableIsSet("KRITA_MOVE_BENCHMARK")) {
        QSKIP("set KRITA_MOVE_BENCHMARK=1");
    }

    // KRITA_MOVE_BENCHMARK_GPU_ONLY=1: only the GPU engine scenarios.
    QVector<Mode> modes;
    if (!qEnvironmentVariableIsSet("KRITA_MOVE_BENCHMARK_GPU_ONLY")) {
        modes << Mode::Rgba8 << Mode::F32Cpu;
    }
    if (KisGpuTileBackend::instance()) {
        modes << Mode::F32Gpu;
    } else {
        qWarning() << "GPU engine unavailable:" << KisGpuTileBackend::unavailableReason();
    }

    qInfo().noquote() << QStringLiteral("Image %1x%2, %3 move steps, then %4 dabs of %5x%6 px; times in ms")
                             .arg(ImageSize.width())
                             .arg(ImageSize.height())
                             .arg(StepCount)
                             .arg(DabCount)
                             .arg(DabSize.width())
                             .arg(DabSize.height());
    qInfo().noquote() << "RESULT\tmode\tmoved\toffsets\tstep median\tstep mean\tbounds median\tdownloads/step\t"
                         "gpuComposites/step\tdab median\tdownloads/dab\tmaxDiff";

    for (Mode mode : modes) {
        for (Moved moved : {Moved::FullLayer, Moved::ObjectLayer, Moved::Group}) {
            for (bool aligned : {true, false}) {
                Scenario s;
                s.mode = mode;
                s.moved = moved;
                s.aligned = aligned;
                const Result r = run(s);
                qInfo().noquote() << QStringLiteral("RESULT\t%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10\t%11")
                                         .arg(modeName(mode))
                                         .arg(movedName(moved))
                                         .arg(aligned ? "aligned" : "unaligned")
                                         .arg(r.stepMedian, 0, 'f', 2)
                                         .arg(r.stepMean, 0, 'f', 2)
                                         .arg(r.boundsMedian, 0, 'f', 2)
                                         .arg(r.downloadsPerStep, 0, 'f', 1)
                                         .arg(r.gpuCompositesPerStep, 0, 'f', 1)
                                         .arg(r.dabMedian, 0, 'f', 2)
                                         .arg(r.downloadsPerDab, 0, 'f', 1)
                                         .arg(r.difference, 0, 'g', 3);
            }
        }
    }
    KisGpuMergeBatch::setEnabled(false);
}

SIMPLE_TEST_MAIN(KisMoveLayerBenchmark)

#include "KisMoveLayerBenchmark.moc"
