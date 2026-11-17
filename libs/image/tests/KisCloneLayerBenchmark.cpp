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

#include "filter/kis_filter.h"
#include "filter/kis_filter_configuration.h"
#include "filter/kis_filter_registry.h"
#include "gpu/KisGpuMergeBatch.h"
#include "gpu/KisGpuProjectionCompositor.h"
#include "gpu/KisGpuTileBackend.h"
#include "kis_clone_layer.h"
#include "kis_filter_mask.h"
#include "kis_group_layer.h"
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
 * Opt-in measurement of Clone Layer update costs while its source is painted
 * (KRITA_CLONE_BENCHMARK=1). Prints one line per scenario: the update time of
 * each dab (setDirty() of the source until the image and the GPU are idle)
 * and the GPU tiles read back to the CPU during those updates.
 */
class KisCloneLayerBenchmark : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void benchmarkSourceStroke();
};

namespace
{
enum class Mode {
    Rgba8,
    F32Cpu,
    F32Gpu
};
enum class MaskKind {
    None,
    Transparency,
    Filter
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

const char *maskName(MaskKind mask)
{
    switch (mask) {
    case MaskKind::None:
        return "none";
    case MaskKind::Transparency:
        return "transparency";
    case MaskKind::Filter:
        return "blur";
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
    const int count = rect.width() * rect.height() * 4;
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
    const int count = rect.width() * rect.height() * 4;
    double worst = 0.0;
    if (a->pixelSize() == 16) {
        std::vector<float> x(count), y(count);
        a->readBytes(reinterpret_cast<quint8 *>(x.data()), rect);
        b->readBytes(reinterpret_cast<quint8 *>(y.data()), rect);
        for (int i = 0; i < count; ++i) {
            worst = std::max(worst, double(std::abs(x[i] - y[i])));
        }
    } else {
        std::vector<quint8> x(count), y(count);
        a->readBytes(x.data(), rect);
        b->readBytes(y.data(), rect);
        for (int i = 0; i < count; ++i) {
            worst = std::max(worst, std::abs(int(x[i]) - int(y[i])) / 255.0);
        }
    }
    return worst;
}

KisNodeSP nestedGroups(KisImageSP image, KisNodeSP parent, int depth, const QString &name)
{
    for (int i = 0; i < depth; ++i) {
        KisGroupLayerSP group = new KisGroupLayer(image, QStringLiteral("%1 group %2").arg(name).arg(i), 255);
        image->addNode(group, parent);
        parent = group;
    }
    return parent;
}

struct Scenario {
    Mode mode = Mode::Rgba8;
    bool clone = true;
    bool sourceIsGroup = false;
    int depth = 0;
    QPoint offset;
    MaskKind mask = MaskKind::None;
};

struct Result {
    double median = 0.0;
    double mean = 0.0;
    double downloadsPerDab = 0.0;
    double gpuCompositesPerDab = 0.0;
    double refresh = 0.0;
    double difference = 0.0;
};

const QSize ImageSize(2480, 3508);
const QRect SourceContent(400, 600, 1200, 1200);
const QSize DabSize(256, 256);
constexpr int DabCount = 24;

QRect dabRect(int i)
{
    const QRect rect(QPoint(500 + i * 20, 700 + i * 16), DabSize);
    // KRITA_CLONE_BENCHMARK_ALIGNED=1: whole 64 px tiles, to separate
    // partial-tile costs from the rest.
    return qEnvironmentVariableIsSet("KRITA_CLONE_BENCHMARK_ALIGNED") ? KisGpuMergeBatch::tileAligned(rect) : rect;
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

    KisImageSP image = new KisImage(nullptr, ImageSize.width(), ImageSize.height(), cs, "clone benchmark");
    const QRect bounds = image->bounds();

    KisPaintLayerSP background = new KisPaintLayer(image, "background", 255);
    background->paintDevice()->fill(bounds, KoColor(Qt::white, cs));
    image->addNode(background, image->root());

    KisNodeSP sourceParent = nestedGroups(image, image->root(), s.depth, "source");
    KisLayerSP source;
    KisPaintLayerSP painted = new KisPaintLayer(image, "painted", 255);
    fillRandom(painted->paintDevice(), SourceContent, 1);
    if (s.sourceIsGroup) {
        KisGroupLayerSP group = new KisGroupLayer(image, "source group", 255);
        image->addNode(group, sourceParent);
        KisPaintLayerSP under = new KisPaintLayer(image, "under", 255);
        fillRandom(under->paintDevice(), SourceContent.adjusted(200, 200, 200, 200), 2);
        image->addNode(under, group);
        image->addNode(painted, group);
        source = group;
    } else {
        image->addNode(painted, sourceParent);
        source = painted;
    }

    if (s.clone) {
        KisNodeSP cloneParent = nestedGroups(image, image->root(), s.depth, "clone");
        KisCloneLayerSP clone = new KisCloneLayer(source, image, "clone", 255);
        clone->setX(s.offset.x());
        clone->setY(s.offset.y());
        image->addNode(clone, cloneParent);

        if (s.mask == MaskKind::Transparency) {
            KisTransparencyMaskSP mask = new KisTransparencyMask(image, "transparency");
            mask->initSelection(KisLayerSP(clone));
            mask->selection()->pixelSelection()->clear(QRect(700, 900, 400, 400));
            mask->selection()->updateProjection();
            image->addNode(mask, clone);
        } else if (s.mask == MaskKind::Filter) {
            KisFilterSP blur = KisFilterRegistry::instance()->value("blur");
            KIS_ASSERT(blur);
            KisFilterConfigurationSP config = blur->defaultConfiguration(KisGlobalResourcesInterface::instance());
            KisFilterMaskSP mask = new KisFilterMask(image, "blur");
            mask->initSelection(KisLayerSP(clone));
            mask->setFilter(config->cloneWithResourcesSnapshot());
            image->addNode(mask, clone);
        }
    }

    KisPaintLayerSP top = new KisPaintLayer(image, "top", 180);
    fillRandom(top->paintDevice(), QRect(0, 0, 1600, 1600), 3);
    top->setCompositeOpId(COMPOSITE_MULT);
    image->addNode(top, image->root());

    Result result;
    QElapsedTimer timer;
    waitGpu();
    timer.start();
    image->refreshGraphAsync();
    image->waitForDone();
    waitGpu();
    result.refresh = timer.nsecsElapsed() / 1.0e6;

    std::vector<double> samples;
    quint64 downloads = 0;
    quint64 composites = 0;
    QRect strokeBounds;
    for (int i = 0; i < DabCount; ++i) {
        const QRect rect = dabRect(i);
        strokeBounds |= rect;
        KisPaintDeviceSP dab = new KisPaintDevice(cs);
        fillRandom(dab, rect, 100 + i);
        bool written = false;
        if (gpu) {
            // Like the GPU brush: the source tiles become GPU-valid, CPU-stale.
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

    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    result.median = (sorted[(sorted.size() - 1) / 2] + sorted[sorted.size() / 2]) / 2;
    double sum = 0.0;
    for (double sample : samples) {
        sum += sample;
    }
    result.mean = sum / samples.size();
    result.downloadsPerDab = double(downloads) / DabCount;
    result.gpuCompositesPerDab = double(composites) / DabCount;

    // Sanity check: the incremental result equals a full refresh, including
    // the clone's copy of the stroke (blur reach included).
    const QRect checkRect = (strokeBounds | strokeBounds.translated(s.offset)).adjusted(-64, -64, 64, 64) & bounds;
    KisPaintDeviceSP incremental = new KisPaintDevice(cs);
    KisPainter::copyAreaOptimized(checkRect.topLeft(), image->projection(), incremental, checkRect);
    image->refreshGraphAsync();
    image->waitForDone();
    waitGpu();
    result.difference = maxDifference(incremental, image->projection(), checkRect);

    image->waitForDone();
    return result;
}
} // namespace

void KisCloneLayerBenchmark::benchmarkSourceStroke()
{
    if (!qEnvironmentVariableIsSet("KRITA_CLONE_BENCHMARK")) {
        QSKIP("set KRITA_CLONE_BENCHMARK=1");
    }

    // KRITA_CLONE_BENCHMARK_GPU_ONLY=1: only the GPU engine scenarios.
    QVector<Mode> modes;
    if (!qEnvironmentVariableIsSet("KRITA_CLONE_BENCHMARK_GPU_ONLY")) {
        modes << Mode::Rgba8 << Mode::F32Cpu;
    }
    if (KisGpuTileBackend::instance()) {
        modes << Mode::F32Gpu;
    } else {
        qWarning() << "GPU engine unavailable:" << KisGpuTileBackend::unavailableReason();
    }

    qInfo().noquote() << QStringLiteral("Image %1x%2, %3 dabs of %4x%5 px; times per dab in ms")
                             .arg(ImageSize.width())
                             .arg(ImageSize.height())
                             .arg(DabCount)
                             .arg(DabSize.width())
                             .arg(DabSize.height());
    qInfo().noquote() << "RESULT\tmode\tsource\tdepth\tclone\toffset\tmask\tmedian\tmean\tdownloads/dab\tgpuComposites/"
                         "dab\trefresh\tmaxDiff";

    auto print = [](const Scenario &s, const Result &r) {
        qInfo().noquote() << QStringLiteral("RESULT\t%1\t%2\t%3\t%4\t%5,%6\t%7\t%8\t%9\t%10\t%11\t%12\t%13")
                                 .arg(modeName(s.mode))
                                 .arg(s.sourceIsGroup ? "group" : "layer")
                                 .arg(s.depth)
                                 .arg(s.clone ? "yes" : "no")
                                 .arg(s.offset.x())
                                 .arg(s.offset.y())
                                 .arg(s.clone ? maskName(s.mask) : "-")
                                 .arg(r.median, 0, 'f', 2)
                                 .arg(r.mean, 0, 'f', 2)
                                 .arg(r.downloadsPerDab, 0, 'f', 1)
                                 .arg(r.gpuCompositesPerDab, 0, 'f', 1)
                                 .arg(r.refresh, 0, 'f', 1)
                                 .arg(r.difference, 0, 'g', 3);
    };

    const QVector<QPoint> offsets = {QPoint(0, 0), QPoint(37, 23), QPoint(64, 128)};
    const QVector<MaskKind> masks = {MaskKind::None, MaskKind::Transparency, MaskKind::Filter};

    for (Mode mode : modes) {
        for (bool sourceIsGroup : {false, true}) {
            for (int depth : {0, 3}) {
                Scenario base;
                base.mode = mode;
                base.sourceIsGroup = sourceIsGroup;
                base.depth = depth;
                base.clone = false;
                print(base, run(base));

                for (const QPoint &offset : offsets) {
                    for (MaskKind mask : masks) {
                        Scenario s = base;
                        s.clone = true;
                        s.offset = offset;
                        s.mask = mask;
                        print(s, run(s));
                    }
                }
            }
        }
    }
    KisGpuMergeBatch::setEnabled(false);
}

SIMPLE_TEST_MAIN(KisCloneLayerBenchmark)

#include "KisCloneLayerBenchmark.moc"
