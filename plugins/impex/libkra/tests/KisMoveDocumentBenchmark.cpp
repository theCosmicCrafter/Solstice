/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <simpletest.h>

#include <QElapsedTimer>
#include <QObject>

#include <KisDocument.h>
#include <KisPart.h>
#include <testui.h>

#include "gpu/KisGpuMergeBatch.h"
#include "kis_abstract_projection_plane.h"
#include "kis_image.h"
#include "kis_layer.h"
#include "kis_layer_utils.h"
#include "kis_mask.h"
#include "kis_psd_layer_style.h"
#include "kis_transform_mask.h"

#ifdef HAVE_KRITA_GPU_ENGINE
#include "gpu/KisGpuTileBackend.h"
#include <KisGpuContext.h>
#endif

#include <algorithm>
#include <vector>

/**
 * Opt-in measurement of Move Tool update costs on a real document
 * (docs/agent/gpu-engine.md, "Build, test, and install"):
 *
 *   KRITA_MOVE_BENCHMARK_FILE  a .kra file (required; the file is only read)
 *   KRITA_MOVE_BENCHMARK_NODE  name of the node to move (required)
 *   KRITA_MOVE_BENCHMARK_NODES=1  also print the bounds time of every node
 *
 * Each step does what MoveStrokeStrategy::doCanvasUpdate() does for one
 * pointer position, for the node and everything below it. One line per
 * variant: the GPU engine off and on, and with parts of the document turned
 * off (layer styles, masks) to see what they cost.
 */
class KisMoveDocumentBenchmark : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void benchmarkMove();
};

namespace
{
constexpr int StepCount = 12;

struct Variant {
    const char *name;
    bool gpu;
    bool layerStyles;
    bool masks;
};

double median(std::vector<double> samples)
{
    std::sort(samples.begin(), samples.end());
    return (samples[(samples.size() - 1) / 2] + samples[samples.size() / 2]) / 2;
}

void run(const QString &fileName, const QString &nodeName, const Variant &variant)
{
    KisGpuMergeBatch::setEnabled(variant.gpu);
#ifdef HAVE_KRITA_GPU_ENGINE
    KisGpuTileBackend *backend = variant.gpu ? KisGpuTileBackend::instance() : nullptr;
#endif
    auto waitGpu = [&]() {
#ifdef HAVE_KRITA_GPU_ENGINE
        if (backend) {
            backend->context().waitIdle();
        }
#endif
    };
    auto downloadCount = [&]() -> quint64 {
#ifdef HAVE_KRITA_GPU_ENGINE
        return backend ? backend->downloadedTileCount() : 0;
#else
        return 0;
#endif
    };

    QScopedPointer<KisDocument> document(KisPart::instance()->createDocument());
    document->setFileBatchMode(true);
    QVERIFY2(document->loadNativeFormat(fileName), qPrintable(fileName));
    KisImageSP image = document->image();
    image->waitForDone();

    KisNodeSP movedRoot = KisLayerUtils::findNodeByName(image->root(), nodeName);
    QVERIFY2(movedRoot, qPrintable(QStringLiteral("no node named %1").arg(nodeName)));

    // KRITA_MOVE_BENCHMARK_SHOW: a node to make visible first, for instance a
    // hidden group of filter layers above the moved node.
    const QString shownName = qEnvironmentVariable("KRITA_MOVE_BENCHMARK_SHOW");
    if (!shownName.isEmpty()) {
        KisNodeSP shown = KisLayerUtils::findNodeByName(image->root(), shownName);
        QVERIFY2(shown, qPrintable(QStringLiteral("no node named %1").arg(shownName)));
        shown->setVisible(true);
    }

    // The nodes the Move Tool moves (MoveStrokeStrategy::recursiveApplyNodes;
    // transform masks below a moved node stay as they are).
    QVector<KisNodeSP> nodes;
    QVector<QPoint> initialOffsets;
    int styledLayers = 0;
    int visibleMasks = 0;
    KisLayerUtils::recursiveApplyNodes(movedRoot, [&](KisNodeSP node) {
        KisLayer *layer = qobject_cast<KisLayer *>(node.data());
        if (layer && layer->layerStyle()) {
            ++styledLayers;
            if (!variant.layerStyles) {
                layer->setLayerStyle(KisPSDLayerStyleSP());
            }
        }
        if (KisMask *mask = qobject_cast<KisMask *>(node.data())) {
            if (mask->visible()) {
                ++visibleMasks;
                if (!variant.masks) {
                    mask->setVisible(false);
                }
            }
        }
        if (node != movedRoot && dynamic_cast<KisTransformMask *>(node.data())) {
            return;
        }
        nodes << node;
        initialOffsets << QPoint(node->x(), node->y());
    });

    image->refreshGraphAsync();
    image->waitForDone();
    waitGpu();

    QElapsedTimer timer;
    std::vector<double> stepSamples;
    std::vector<double> boundsSamples;
    QRect lastDirtyRect;
    const quint64 downloadsBefore = downloadCount();
    const quint64 compositesBefore = KisGpuMergeBatch::gpuCompositeCount();
    for (int i = 0; i < StepCount; ++i) {
        const QPoint delta(3 + 7 * i, 2 + 5 * i);
        timer.start();
        // MoveNodeStrategyBase::moveNodeCommon()
        QRect dirtyRect;
        for (int n = 0; n < nodes.size(); ++n) {
            KisNodeSP node = nodes[n];
            const QPoint newOffset = initialOffsets[n] + delta;
            QElapsedTimer nodeTimer;
            nodeTimer.start();
            QRect nodeRect = node->inherits("KisGroupLayer") ? node->projectionPlane()->looseUserVisibleBounds()
                                                             : node->projectionPlane()->tightUserVisibleBounds();
            nodeRect |= nodeRect.translated(newOffset - QPoint(node->x(), node->y()));
            node->setX(newOffset.x());
            node->setY(newOffset.y());
            dirtyRect |= nodeRect;
            if (i == 1 && qEnvironmentVariableIsSet("KRITA_MOVE_BENCHMARK_NODES")) {
                qInfo().noquote() << QStringLiteral("NODE	%1	%2	%3 ms	%4x%5	%6")
                                         .arg(variant.name)
                                         .arg(node->name())
                                         .arg(nodeTimer.nsecsElapsed() / 1.0e6, 0, 'f', 2)
                                         .arg(nodeRect.width())
                                         .arg(nodeRect.height())
                                         .arg(node->visible() ? "visible" : "hidden");
            }
        }
        boundsSamples.push_back(timer.nsecsElapsed() / 1.0e6);
        image->refreshGraphAsync(movedRoot, dirtyRect);
        image->waitForDone();
        waitGpu();
        stepSamples.push_back(timer.nsecsElapsed() / 1.0e6);
        lastDirtyRect = dirtyRect & image->bounds();
    }
    const double downloads = double(downloadCount() - downloadsBefore) / StepCount;
    const double composites = double(KisGpuMergeBatch::gpuCompositeCount() - compositesBefore) / StepCount;

    qInfo().noquote() << QStringLiteral("RESULT\t%1\t%2\t%3\t%4\t%5\t%6\t%7x%8\t%9\t%10\t%11")
                             .arg(variant.name)
                             .arg(median(stepSamples), 0, 'f', 2)
                             .arg(*std::max_element(stepSamples.begin(), stepSamples.end()), 0, 'f', 2)
                             .arg(median(boundsSamples), 0, 'f', 2)
                             .arg(downloads, 0, 'f', 1)
                             .arg(composites, 0, 'f', 1)
                             .arg(lastDirtyRect.width())
                             .arg(lastDirtyRect.height())
                             .arg(nodes.size())
                             .arg(styledLayers)
                             .arg(visibleMasks);

    image->waitForDone();
    waitGpu();
}
} // namespace

void KisMoveDocumentBenchmark::benchmarkMove()
{
    const QString fileName = qEnvironmentVariable("KRITA_MOVE_BENCHMARK_FILE");
    const QString nodeName = qEnvironmentVariable("KRITA_MOVE_BENCHMARK_NODE");
    if (fileName.isEmpty() || nodeName.isEmpty()) {
        QSKIP("set KRITA_MOVE_BENCHMARK_FILE and KRITA_MOVE_BENCHMARK_NODE");
    }

    QVector<Variant> variants = {{"CPU", false, true, true},
                                 {"CPU, no layer styles", false, false, true},
                                 {"CPU, no layer styles, no masks", false, false, false}};
#ifdef HAVE_KRITA_GPU_ENGINE
    if (KisGpuTileBackend::instance()) {
        variants << Variant{"GPU", true, true, true} << Variant{"GPU, no layer styles", true, false, true}
                 << Variant{"GPU, no masks", true, true, false}
                 << Variant{"GPU, no layer styles, no masks", true, false, false};
    } else {
        qWarning() << "GPU engine unavailable:" << KisGpuTileBackend::unavailableReason();
    }
#endif

    qInfo().noquote()
        << QStringLiteral("%1, node \"%2\", %3 move steps; times in ms").arg(fileName).arg(nodeName).arg(StepCount);
    qInfo().noquote() << "RESULT\tvariant\tstep median\tstep max\tbounds median\tdownloads/step\tgpuComposites/step\t"
                         "dirty rect\tmoved nodes\tlayer styles\tvisible masks";
    for (const Variant &variant : variants) {
        run(fileName, nodeName, variant);
    }
    KisGpuMergeBatch::setEnabled(false);
}

KISTEST_MAIN(KisMoveDocumentBenchmark)

#include "KisMoveDocumentBenchmark.moc"
