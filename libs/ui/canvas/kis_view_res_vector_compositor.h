/*
 *  SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VIEW_RES_VECTOR_COMPOSITOR_H
#define KIS_VIEW_RES_VECTOR_COMPOSITOR_H

#include <QImage>
#include <QRect>
#include <QVector>
#include <functional>

#include <kritaui_export.h>

#include <kis_types.h>

class KisImage;
class KisShapeLayer;
class KisProjectionLeaf;

/**
 * \brief Compositing-aware view-resolution vector renderer.
 *
 * Problem (audit defect D7): KisShapeLayer rasterizes its vector shapes into
 * a document-resolution paint device (KisShapeLayerCanvas::m_projection).
 * That device is merged into the image projection and then upscaled by the
 * canvas to the current zoom. Zooming beyond 100% therefore magnifies
 * already-rasterized, antialiased edges instead of re-rendering the vector
 * geometry, so vector content gets blurry exactly when it should stay crisp.
 *
 * Two architectures were considered (see docs/agent/view-res-vectors.md):
 *
 *  A) A plain canvas overlay that paints the vector shapes at view
 *     resolution on top of the image (like assistant/reference decorations).
 *     Simple, but compositing-wrong: the overlay sits above *all* image
 *     content, so vectors wedged between raster layers, blended layers,
 *     masked layers or group stacks would render in the wrong order.
 *
 *  B) A view-dependent projection that re-rasterizes vector layers into the
 *     layer stack at the current zoom. Preserves compositing by construction
 *     but requires the whole image pipeline (tiled devices, merger, masks,
 *     clones, animation caches, undo extents) to become resolution
 *     independent - a massive and risky rework.
 *
 * This class implements a third option that keeps the strengths of both:
 * a *re-composition overlay*. For every eligible vector layer we rebuild the
 * part of the displayed projection that overlaps it, at display resolution:
 *
 *   1. The flattened merge sequence of the image root (as used by the async
 *      merger) is walked via projection leaves. Pass-through groups are
 *      already flattened by the leaf API; each entry is a "unit" with an
 *      effective opacity and a composite mode.
 *   2. Units whose document-space coverage rect touches the coverage of a
 *      vector unit must be re-composited as well, so stacking order is kept.
 *   3. For the union rect of each such "job" we recover the *backdrop* B,
 *      i.e. the composite of everything except the job's units, by
 *      un-blending each unit's effective source out of the displayed
 *      projection D. For source-over compositing D = B + S - a_s*B, so
 *      B = (D - S) / (1 - a_s) holds per channel and is exact up to u8
 *      rounding. Units that cannot be un-blended or re-composited
 *      (non-over blend modes, inherit-alpha, channel flags, adjustment
 *      layers, layer styles) act as "firewalls": vector units that overlap
 *      them keep their document-resolution rendering.
 *   4. The decoration then draws B plus every unit in z-order: raster units
 *      as their document-resolution pixels (identical to the canvas), vector
 *      units re-painted from their live KoShape content at the current
 *      device transform. The result is crisp vectors with Krita's normal
 *      compositing semantics for the entire "over-only" part of the stack.
 *
 * All the computationally expensive parts (unit analysis, backdrop
 * recovery, raster-unit patches) are cached in document space and are
 * independent of the view transform, so zooming/panning never re-runs them;
 * only the per-frame vector shape rasterization runs at view resolution.
 *
 * The class itself is canvas-agnostic (and unit-testable): the decoration
 * injects the display color conversion.
 */
class KRITAUI_EXPORT KisViewResVectorCompositor
{
public:
    /**
     * A single entry of the flattened root merge sequence ("who gets
     * composited onto the image projection, in order").
     */
    struct Unit {
        KisNodeWSP node;
        QRect extent; ///< coverage rect in image pixels
        quint8 opacity = 255; ///< effective opacity incl. pass-through ancestors
        bool recompositable = false; ///< can be un-blended and re-composited
        bool isVector = false; ///< should be re-rendered at view resolution
        /// shape layers that make up a vector unit (a single KisShapeLayer or
        /// all shape layers of an isolated group), in bottom-to-top order,
        /// paired with the opacity that applies to each child
        QVector<QPair<KisShapeLayer *, quint8>> shapeLayers;
    };

    /**
     * A contiguous group of units that must be peeled off the displayed
     * projection and re-composited together. Units are stored as indices
     * into the unit vector in bottom-to-top order; at least one of them is a
     * vector unit.
     */
    struct Job {
        QRect docRect; ///< union of the units' extents, image pixels
        QVector<int> units;
    };

    /// device -> un-premultiplied ARGB8 image conversion, injected by caller
    /// (decoration uses the display color converter, tests use plain
    /// convertToQImage). The rect is in image (device) pixels.
    using DeviceToQImageFn = std::function<QImage(KisPaintDeviceSP, const QRect &)>;

    /**
     * Walk the flattened merge sequence of the image root and classify every
     * contributing unit. Returned in bottom-to-top order.
     */
    static QVector<Unit> collectUnits(KisImageSP image);

    /**
     * Group the units into re-composition jobs: for every vector unit,
     * compute the closure of units whose coverage overlaps it in document
     * space (those would otherwise be buried by the vector repaint) and
     * merge overlapping closures into single jobs. Vector units whose
     * closure hits a non-recompositable unit are dropped (they keep the
     * document-resolution path). May return an empty list.
     */
    static QVector<Job> computeJobs(const QVector<Unit> &units);

    /**
     * A cheap signature of the unit list used to detect stack changes
     * (add/remove/reorder/opacity/composite/visibility/extent changes).
     */
    static quint64 unitsSignature(const QVector<Unit> &units);

    /**
     * Recover the backdrop of \a job: the displayed projection with every
     * job unit un-blended, in bottom-to-top order. Returns a premultiplied
     * ARGB32 image covering job.docRect, or a null image on failure.
     */
    static QImage
    buildBackdrop(KisImageSP image, const Job &job, const QVector<Unit> &units, const DeviceToQImageFn &convert);

    /**
     * The premultiplied ARGB32 patch a recompositable unit contributes
     * over \a docRect (unit projection premultiplied by its effective
     * opacity). Also used for vector units during backdrop recovery -
     * the peel needs the *raster* contribution even though the
     * decoration later repaints the unit at view resolution. Returns a
     * null image when the node or its projection vanished.
     */
    static QImage buildUnitPatch(const Unit &unit, const QRect &docRect, const DeviceToQImageFn &convert);

    /**
     * Remove one source-over unit from a premultiplied backdrop:
     * B = (D - S) / (1 - a_s), a_B = (a_D - a_s) / (1 - a_s), clamped.
     * Pixels where the source is (nearly) opaque are zeroed: the vector
     * repaint fully replaces them. Exposed for unit tests.
     */
    static void unblendSourceOver(QImage &backdropPremult, const QImage &sourcePremult);

    /// Hard safety cap on the peel/re-composite area, in image pixels.
    /// Keeps the document-space patch buffers (backdrop + one per unit)
    /// within a sane memory bound; bigger jobs keep doc-res rendering.
    static constexpr qint64 maxJobPixels = 16ll * 1000ll * 1000ll;
    /// Hard cap on the number of units re-composited in one job.
    static constexpr int maxJobUnits = 64;
};

#endif // KIS_VIEW_RES_VECTOR_COMPOSITOR_H
