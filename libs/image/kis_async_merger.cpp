/* SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "kis_async_merger.h"
#include "KisPaintTrace.h"

#include <kis_debug.h>

#include <KoChannelInfo.h>
#include <KoCompositeOpRegistry.h>

#include "kis_node_visitor.h"
#include "kis_painter.h"
#include "kis_layer.h"
#include "kis_group_layer.h"
#include "kis_adjustment_layer.h"
#include "generator/kis_generator_layer.h"
#include "kis_external_layer_iface.h"
#include "kis_paint_layer.h"
#include "filter/kis_filter.h"
#include "filter/kis_filter_configuration.h"
#include "filter/kis_filter_registry.h"
#include "kis_selection.h"
#include "kis_clone_layer.h"
#include "kis_processing_information.h"
#include "kis_busy_progress_indicator.h"


#include "kis_merge_walker.h"
#include "kis_refresh_subtree_walker.h"

#include "kis_abstract_projection_plane.h"

#ifdef HAVE_KRITA_GPU_ENGINE
#include <QRegion>

#include "gpu/KisGpuProjectionCompositor.h"
#include "gpu/KisGpuTileAccess.h"
#include "kis_default_bounds_base.h"
#include "kis_pixel_selection.h"
#include "layerstyles/kis_layer_style_projection_plane.h"
#endif


//#define DEBUG_MERGER

#ifdef DEBUG_MERGER
#define DEBUG_NODE_ACTION(message, type, leaf, rect)            \
    qDebug() << message << type << ":" << leaf->node()->name() << rect << "rendered:" << leaf->shouldBeRendered()
#else
#define DEBUG_NODE_ACTION(message, type, leaf, rect)
#endif


#ifdef HAVE_KRITA_GPU_ENGINE
namespace
{
/**
 * GPU engine (phase 4.115): the filter of an adjustment layer in the form
 * the GPU computes, or null when the layer is filtered on the CPU
 * (KisGpuColorTransform::forNode()).
 */
KisGpuColorTransformSP gpuColorTransformFor(KisAdjustmentLayer *layer, KisPaintDeviceSP projection)
{
    if (!layer || !projection) {
        return KisGpuColorTransformSP();
    }
    // The layer's own device knows the image's bounds; a temporary
    // projection may not.
    return KisGpuColorTransform::forNode(KisNodeSP(layer),
                                         layer->filter(),
                                         projection->colorSpace(),
                                         layer->original()->defaultBounds());
}
} // namespace
#endif

class KisUpdateOriginalVisitor : public KisNodeVisitor
{
public:
    KisUpdateOriginalVisitor(const QRect &updateRect, KisPaintDeviceSP projection)
        : m_updateRect(updateRect),
          m_projection(projection)
        {
        }

    ~KisUpdateOriginalVisitor() override {
    }

public:
    using KisNodeVisitor::visit;

    bool visit(KisAdjustmentLayer* layer) override {
        if (!layer->visible()) return true;

        if (!m_projection) {
            warnImage << "ObligeChild mechanism has been activated for "
                "an adjustment layer! Do nothing...";
            layer->original()->clear();
            return true;
        }

        const QRect originalUpdateRect =
            layer->projectionPlane()->needRectForOriginal(m_updateRect);

        KisPaintDeviceSP originalDevice = layer->original();

#ifdef HAVE_KRITA_GPU_ENGINE
        // GPU engine (phase 4.115): filters with a GPU form
        if (filterOnGpu(layer, originalDevice, originalUpdateRect)) {
            return true;
        }
#endif

        originalDevice->clear(originalUpdateRect);

        const QRect applyRect = originalUpdateRect & m_projection->extent();

        // If the intersection of the updaterect and the projection extent is
        //      null, we are finish here.
        if(applyRect.isNull()) return true;

        KisFilterConfigurationSP filterConfig = layer->filter();
        if (!filterConfig) {
            /**
             * When an adjustment layer is just created, it may have no
             * filter inside. Then the layer has work as a pass-through
             * node. Just copy the merged data to the layer's original.
             */
            KisPainter::copyAreaOptimized(applyRect.topLeft(), m_projection, originalDevice, applyRect);
            return true;
        }

        KisSelectionSP selection = layer->fetchComposedInternalSelection(applyRect);
        const QRect filterRect = selection ? applyRect & selection->selectedRect() : applyRect;

        KisFilterSP filter = KisFilterRegistry::instance()->value(filterConfig->name());
        if (!filter) return false;

        KisPaintDeviceSP dstDevice = originalDevice;

        if (selection) {
            dstDevice = new KisPaintDevice(originalDevice->colorSpace());
        }

        if (!filterRect.isEmpty()) {
            KIS_ASSERT_RECOVER_NOOP(layer->busyProgressIndicator());
            layer->busyProgressIndicator()->update();

            // We do not create a transaction here, as srcDevice != dstDevice
            filter->process(m_projection, dstDevice, 0, filterRect, filterConfig.data(), 0);
        }

        if (selection) {
            KisPainter::copyAreaOptimized(applyRect.topLeft(), m_projection, originalDevice, applyRect);
            KisPainter::copyAreaOptimized(filterRect.topLeft(), dstDevice, originalDevice, filterRect, selection);
        }

        return true;
    }

    bool visit(KisExternalLayer*) override {
        return true;
    }

    bool visit(KisGeneratorLayer*) override {
        return true;
    }

    bool visit(KisPaintLayer*) override {
        return true;
    }

    bool visit(KisGroupLayer*) override {
        return true;
    }

    bool visit(KisCloneLayer*) override {
        return true;
    }

    bool visit(KisNode*) override {
        return true;
    }
    bool visit(KisFilterMask*) override {
        return true;
    }
    bool visit(KisTransformMask*) override {
        return true;
    }
    bool visit(KisTransparencyMask*) override {
        return true;
    }
    bool visit(KisSelectionMask*) override {
        return true;
    }
    bool visit(KisColorizeMask*) override {
        return true;
    }

private:
#ifdef HAVE_KRITA_GPU_ENGINE
    /**
     * GPU engine (phase 4.115): what visit(KisAdjustmentLayer*) does below,
     * with the projection filtered into the layer's original on the GPU, so
     * neither device is read back. Returns false when the CPU has to do it;
     * the original may then have been cleared outside the projection's
     * extent, which the CPU path does first anyway.
     */
    bool filterOnGpu(KisAdjustmentLayer *layer, KisPaintDeviceSP originalDevice, const QRect &originalUpdateRect)
    {
        const KisGpuColorTransformSP transform = gpuColorTransformFor(layer, m_projection);
        if (!transform || !(*originalDevice->colorSpace() == *m_projection->colorSpace())) {
            return false;
        }

        const QRect applyRect = originalUpdateRect & m_projection->extent();

        // The layer's selection: none of it (the usual case, everything is
        // selected) or its coverage bytes.
        QVector<quint8> coverage;
        if (!applyRect.isEmpty()) {
            KisSelectionSP selection = layer->fetchComposedInternalSelection(applyRect);
            KisPixelSelectionSP pixels = selection ? selection->projection() : KisPixelSelectionSP();
            // the extent of the tiles (KisPixelSelection hides extent())
            const KisPaintDevice *pixelDevice = pixels.data();
            const bool allSelected = !pixels
                || ((pixelDevice->extent() & applyRect).isEmpty() && *pixels->defaultPixel().data() == MAX_SELECTED);
            if (!allSelected) {
                const qint64 size = qint64(applyRect.width()) * applyRect.height();
                if (size > KisGpuProjectionCompositor::maxColorTransformCoveragePixels()) {
                    return false;
                }
                coverage.resize(int(size));
                pixels->readBytes(coverage.data(), applyRect);
            }
        }

        // The filtered rect is replaced as a whole; the rest is cleared.
        const QRegion outside = QRegion(originalUpdateRect).subtracted(applyRect);
        for (const QRect &rect : outside) {
            originalDevice->clear(rect);
        }
        if (applyRect.isEmpty()) {
            return true;
        }
        return KisGpuProjectionCompositor::transformColors(originalDevice,
                                                           applyRect,
                                                           m_projection,
                                                           *transform,
                                                           coverage.isEmpty() ? nullptr : coverage.constData());
    }
#endif

    QRect m_updateRect;
    KisPaintDeviceSP m_projection;
};


/*********************************************************************/
/*                     KisAsyncMerger                                */
/*********************************************************************/

void KisAsyncMerger::startMerge(KisBaseRectsWalker &walker, bool notifyClones) {
    KisPaintTrace::Scope trace("projection.merge", &walker, nullptr, walker.paintTraceId());
    KisMergeWalker::LeafStack &leafStack = walker.leafStack();
    if (leafStack.isEmpty()) {
        KisPaintTrace::link("path.projection.empty_walk", this, KisPaintTrace::currentFlow());
    }

    const bool useTempProjections = walker.needRectVaries();

    while(!leafStack.isEmpty()) {
        KisMergeWalker::JobItem item = leafStack.pop();
        KisProjectionLeafSP currentLeaf = item.m_leaf;

        /**
         * In some unidentified cases the nodes might be removed
         * while the updates are still running. We have no proof
         * of it yet, so just add a safety assert here.
         */
        KIS_SAFE_ASSERT_RECOVER_RETURN(currentLeaf);
        KIS_SAFE_ASSERT_RECOVER_RETURN(currentLeaf->node());

        // All the masks should be filtered by the walkers
        KIS_SAFE_ASSERT_RECOVER_RETURN(currentLeaf->isLayer());

        QRect applyRect = item.m_applyRect;

        // GPU engine: these steps read the projection composited so far
        if (currentLeaf->isRoot() || (item.m_position & KisMergeWalker::N_EXTRA)
            || currentLeaf->dependsOnLowerNodes()) {
            KisPaintTrace::Scope trace("merge.gpu_flush", currentLeaf->node().data());
            // Only the root's recalculation does not read it on the CPU...
            bool cpuReadsNext = !currentLeaf->isRoot();
#ifdef HAVE_KRITA_GPU_ENGINE
            // ... and an adjustment layer whose filter runs on the GPU (phase 4.115).
            if (cpuReadsNext
                && gpuColorTransformFor(qobject_cast<KisAdjustmentLayer *>(currentLeaf->node().data()),
                                        m_currentProjection)) {
                cpuReadsNext = false;
            }
#endif
            m_gpuBatch.flush(cpuReadsNext);
        }

        if (currentLeaf->isRoot()) {
            KisPaintTrace::Scope trace("merge.recalculate_root", currentLeaf->node().data());
            currentLeaf->projectionPlane()->recalculate(applyRect, walker.startNode(), item.m_renderFlags);
            KisPaintTrace::link("path.projection.root_recalculated",
                                currentLeaf->node().data(),
                                KisPaintTrace::currentFlow());
            continue;
        }

        if(item.m_position & KisMergeWalker::N_EXTRA) {
            // The type of layers that will not go to projection.

            DEBUG_NODE_ACTION("Updating", "N_EXTRA", currentLeaf, applyRect);
            KisUpdateOriginalVisitor originalVisitor(applyRect,
                                                     m_currentProjection);
            currentLeaf->accept(originalVisitor);
            currentLeaf->projectionPlane()->recalculate(applyRect, currentLeaf->node(), item.m_renderFlags);
            KisPaintTrace::link("path.projection.extra_recalculated",
                                currentLeaf->node().data(),
                                KisPaintTrace::currentFlow());
            continue;
        }


        if (!m_currentProjection) {
            setupProjection(currentLeaf, applyRect, useTempProjections);
        }

        KisUpdateOriginalVisitor originalVisitor(applyRect,
                                                 m_currentProjection);

        if(item.m_position & KisMergeWalker::N_FILTHY) {
            DEBUG_NODE_ACTION("Updating", "N_FILTHY", currentLeaf, applyRect);
            if (currentLeaf->shouldBeRendered()) {
                KisPaintTrace::Scope trace("merge.recalculate_filthy", currentLeaf->node().data());
                currentLeaf->accept(originalVisitor);
                currentLeaf->projectionPlane()->recalculate(applyRect, walker.startNode(), item.m_renderFlags);
            }
        }
        else if(item.m_position & KisMergeWalker::N_ABOVE_FILTHY) {
            DEBUG_NODE_ACTION("Updating", "N_ABOVE_FILTHY", currentLeaf, applyRect);
            if(currentLeaf->dependsOnLowerNodes()) {
                if (currentLeaf->shouldBeRendered()) {
                    currentLeaf->accept(originalVisitor);
                    currentLeaf->projectionPlane()->recalculate(applyRect, currentLeaf->node(), item.m_renderFlags);
                }
            }
        }
        else if(item.m_position & KisMergeWalker::N_FILTHY_PROJECTION) {
            DEBUG_NODE_ACTION("Updating", "N_FILTHY_PROJECTION", currentLeaf, applyRect);
            if (currentLeaf->shouldBeRendered()) {
                currentLeaf->projectionPlane()->recalculate(applyRect, walker.startNode(), item.m_renderFlags);
            }
        }
        else /*if(item.m_position & KisMergeWalker::N_BELOW_FILTHY)*/ {
            DEBUG_NODE_ACTION("Updating", "N_BELOW_FILTHY", currentLeaf, applyRect);
            /* nothing to do */
        }

        {
            KisPaintTrace::Scope trace("merge.composite", currentLeaf->node().data());
            compositeWithProjection(currentLeaf, applyRect);
        }

        if(item.m_position & KisMergeWalker::N_TOPMOST) {
            KisPaintTrace::Scope trace("merge.write_projection", currentLeaf->node().data());
            writeProjection(currentLeaf, useTempProjections, applyRect);
            resetProjection();
        }

        // FIXME: remove it from the inner loop and/or change to a warning!
        Q_ASSERT(currentLeaf->projection()->defaultBounds()->currentLevelOfDetail() ==
                 walker.levelOfDetail());
    }

    {
        KisPaintTrace::Scope trace("merge.gpu_flush", this);
        m_gpuBatch.flush();
    }

    if(notifyClones) {
        doNotifyClones(walker);
    }

    if(m_currentProjection) {
        warnImage << "BUG: The walker hasn't reached the root layer!";
        warnImage << "     Start node:" << walker.startNode() << "Requested rect:" << walker.requestedRect();
        warnImage << "     An inconsistency in the walkers occurred!";
        warnImage << "     Please report a bug describing how you got this message.";
        // reset projection to avoid artifacts in next merges and allow people to work further
        resetProjection();
    }
}

void KisAsyncMerger::resetProjection() {
    m_currentProjection = 0;
    m_finalProjection = 0;
}

void KisAsyncMerger::setupProjection(KisProjectionLeafSP currentLeaf, const QRect& rect, bool useTempProjection) {
    KisPaintDeviceSP parentOriginal = currentLeaf->parent()->lazyDestinationForSubtreeComposition();

    if (parentOriginal) {
        if (useTempProjection) {
            if(!m_cachedPaintDevice)
                m_cachedPaintDevice = new KisPaintDevice(parentOriginal->colorSpace());
            m_currentProjection = m_cachedPaintDevice;
            m_currentProjection->prepareClone(parentOriginal);
            m_finalProjection = parentOriginal;
        }
        else {
            // GPU engine: clear with the first GPU composite (phase 4.111).
            // An adjustment layer reads the cleared rect on the CPU next.
            if (currentLeaf->dependsOnLowerNodes()
                || !m_gpuBatch.deferClear(currentLeaf->node(), parentOriginal, rect)) {
                parentOriginal->clear(rect);
            }
            m_finalProjection = m_currentProjection = parentOriginal;
        }
    }
    else {
        /**
         * It happened so that our parent uses our own projection as
         * its original. It means obligeChild mechanism works.
         * We won't initialise m_currentProjection. This will cause
         * writeProjection() and compositeWithProjection() do nothing
         * when called.
         */
        KisPaintTrace::link("path.projection.child_reused", currentLeaf->node().data(), KisPaintTrace::currentFlow());
    }
}

void KisAsyncMerger::writeProjection(KisProjectionLeafSP topmostLeaf, bool useTempProjection, const QRect &rect) {
    Q_UNUSED(useTempProjection);
    if (!m_currentProjection) return;

    m_gpuBatch.flush();

    if(m_currentProjection != m_finalProjection) {
        bool copied = false;
#ifdef HAVE_KRITA_GPU_ENGINE
        // GPU engine (phase 4.114): the temporary projection (used when the
        // need rect varies: layer styles on groups, filter masks) was
        // composited on the GPU; a CPU copy would read it back.
        copied = topmostLeaf && KisGpuMergeBatch::mayCompositeOnGpu(topmostLeaf->node())
            && KisGpuProjectionCompositor::copyShifted(m_finalProjection, rect, m_currentProjection, QPoint());
#else
        Q_UNUSED(topmostLeaf);
#endif
        if (!copied) {
            KisPainter::copyAreaOptimized(rect.topLeft(), m_currentProjection, m_finalProjection, rect);
        }
    }
    DEBUG_NODE_ACTION("Writing projection", "", topmostLeaf->parent(), rect);
}

bool KisAsyncMerger::compositeWithProjection(KisProjectionLeafSP leaf, const QRect &rect) {
    if (!m_currentProjection) {
        KisPaintTrace::link("path.projection.no_target", leaf->node().data(), KisPaintTrace::currentFlow());
        return true;
    }
    if (!leaf->visible()) {
        KisPaintTrace::link("path.projection.invisible", leaf->node().data(), KisPaintTrace::currentFlow());
        return true;
    }

    // GPU engine: defer supported layers into one GPU dispatch
    if (m_gpuBatch.tryAdd(leaf, m_currentProjection, rect))
        return true;

#ifdef HAVE_KRITA_GPU_ENGINE
    // GPU engine (phase 4.114): a layer whose style is a Stroke the GPU
    // can draw is composited together with it.
    if (KisLayerStyleProjectionPlane *stylePlane =
            dynamic_cast<KisLayerStyleProjectionPlane *>(leaf->projectionPlane().data())) {
        if (stylePlane->gpuCompositesStyles()) {
            m_gpuBatch.flush();
            if (stylePlane->applyOnGpu(m_currentProjection, rect)) {
                return true;
            }
        }
    }
#endif

    m_gpuBatch.flush(true);

#ifdef HAVE_KRITA_GPU_ENGINE
    // GPU engine: this composite runs on the CPU (a layer style, a blend
    // mode the GPU does not have, ...). Read what it touches back in batches;
    // its tile locks would otherwise download one tile at a time.
    if (KisGpuMergeBatch::mayCompositeOnGpu(leaf->node())) {
        const QRect appliedRect = rect & leaf->projectionPlane()->looseUserVisibleBounds();
        if (!appliedRect.isEmpty()) {
            KisGpuTileAccess::syncToCpu(m_currentProjection, appliedRect);
            KisPaintDeviceSP source = leaf->projection();
            if (source) {
                KisGpuTileAccess::syncToCpu(source, leaf->projectionPlane()->needRect(appliedRect) & source->extent());
            }
        }
    }
#endif

    KisPainter gc(m_currentProjection);
    leaf->projectionPlane()->apply(&gc, rect);
    KisPaintTrace::link("path.projection.cpu", this, KisPaintTrace::currentFlow());

    DEBUG_NODE_ACTION("Compositing projection", "", leaf, rect);
    return true;
}

void KisAsyncMerger::doNotifyClones(KisBaseRectsWalker &walker) {
    KisBaseRectsWalker::CloneNotificationsVector &vector =
        walker.cloneNotifications();

    KisBaseRectsWalker::CloneNotificationsVector::iterator it;

    for(it = vector.begin(); it != vector.end(); ++it) {
        (*it).notify();
    }
}
