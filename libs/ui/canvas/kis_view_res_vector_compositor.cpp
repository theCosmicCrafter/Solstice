/*
 *  SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_view_res_vector_compositor.h"

#include <QVector>

#include <KoColorSpace.h>
#include <KoCompositeOpRegistry.h>

#include <kis_assert.h>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_layer.h>
#include <kis_layer_projection_plane.h>
#include <kis_paint_device.h>
#include <kis_projection_leaf.h>
#include <krita_utils.h>

#include "flake/kis_shape_layer.h"

namespace
{

/**
 * A channel-flags bit array is "empty" when all channels are active
 * (KisLayer::channelFlags() semantics). Peeling a unit whose composite
 * disabled channels is not expressible as an alpha subtraction, so such
 * units are not recompositable.
 */
bool defaultChannelFlags(const QBitArray &flags)
{
    if (flags.isEmpty())
        return true;
    for (int i = 0; i < flags.size(); i++) {
        if (!flags.testBit(i))
            return false;
    }
    return true;
}

/**
 * Whether the node's own merge step is the plain internal
 * KisLayerProjectionPlane with source-over semantics.
 * - dependsOnLowerNodes() filters out adjustment/filter/selection-mask
 *   leaves (their output depends on the backdrop, so it cannot be peeled
 *   or re-composited without recomputing the filter).
 * - KisLayerStyleFilterProjectionPlane replaces the internal plane for
 *   styled layers; detecting the internal plane class covers them.
 */
bool hasInternalProjectionPlane(const KisProjectionLeafSP &leaf)
{
    return !leaf->dependsOnLowerNodes() && dynamic_cast<KisLayerProjectionPlane *>(leaf->projectionPlane().data());
}

/**
 * Eligibility shared by every recompositable unit: source-over composite,
 * no inherit-alpha, default channel flags, projection in the image color
 * space (the peel arithmetic works on display-converted data only when all
 * contributing devices live in the same space).
 */
bool isSimpleOverUnit(KisLayer *layer, const KisProjectionLeafSP &leaf, const KoColorSpace *imageCs)
{
    KisPaintDeviceSP projection = layer->projection();

    return hasInternalProjectionPlane(leaf) && layer->compositeOpId() == COMPOSITE_OVER
        && !layer->alphaChannelDisabled() && !layer->layerStyle() && defaultChannelFlags(leaf->channelFlags())
        && projection && *projection->colorSpace() == *imageCs;
}

/**
 * A shape layer can only be re-rendered from its live shapes when the
 * projection is an unbiased mapping of the flake content:
 * - non-zero layer offset would shift the raster device but is not part of
 *   the shape transforms;
 * - effect masks modulate the raster projection and would be lost by a
 *   direct shape repaint (the unit stays recompositable as a raster unit,
 *   so correctness is preserved, just not crispness - a documented
 *   limitation).
 */
bool isEligibleShapeLayer(KisShapeLayer *layer)
{
    return layer->x() == 0 && layer->y() == 0 && !layer->hasEffectMasks();
}

/**
 * Collect the shape layers rendered inside an isolated group unit, in
 * bottom-to-top order. All contributing descendants must be eligible shape
 * layers or (nested) groups made only of those; the group's own merge step
 * is source-over, so flattening the shape list reproduces the group merge
 * exactly. `opacityAccum` carries the opacity of *isolated* ancestor
 * groups, which KisProjectionLeaf::opacity() does not merge (it only merges
 * pass-through ancestors).
 */
bool collectGroupShapeLayers(KisGroupLayer *group,
                             quint8 opacityAccum,
                             const KoColorSpace *imageCs,
                             QVector<QPair<KisShapeLayer *, quint8>> &out)
{
    // node children iterate bottom-to-top, matching the merge order
    for (KisNodeSP child = group->firstChild(); child; child = child->nextSibling()) {
        KisProjectionLeafSP leaf = child->projectionLeaf();
        if (!leaf || !leaf->visible() || leaf->isDroppedNode()) {
            // hidden subtrees are not part of the group merge
            continue;
        }

        if (KisShapeLayer *shapeLayer = qobject_cast<KisShapeLayer *>(child.data())) {
            if (!isEligibleShapeLayer(shapeLayer) || !isSimpleOverUnit(shapeLayer, leaf, imageCs)) {
                return false;
            }
            const quint8 effectiveOpacity = KritaUtils::mergeOpacityU8(leaf->opacity(), opacityAccum);
            out << qMakePair(shapeLayer, effectiveOpacity);
            continue;
        }

        KisGroupLayer *childGroup = qobject_cast<KisGroupLayer *>(child.data());
        if (!childGroup) {
            // raster/mask/other content inside the group buffer
            return false;
        }

        if (!childGroup->passThroughMode() && !isSimpleOverUnit(childGroup, leaf, imageCs)) {
            return false;
        }

        const quint8 childAccum = childGroup->passThroughMode()
            ? opacityAccum
            : KritaUtils::mergeOpacityU8(childGroup->opacity(), opacityAccum);

        if (!collectGroupShapeLayers(childGroup, childAccum, imageCs, out)) {
            return false;
        }
    }

    return true;
}

} // namespace

QVector<KisViewResVectorCompositor::Unit> KisViewResVectorCompositor::collectUnits(KisImageSP image)
{
    QVector<Unit> units;

    if (!image)
        return units;

    const KoColorSpace *imageCs = image->colorSpace();
    const QRect imageBounds = image->bounds();

    /**
     * Walking the root projection leaf's children yields the exact
     * flattened merge sequence used by the async merger: pass-through
     * groups are transparent (their children appear as units with merged
     * effective opacity/channel flags), selection-mask overlay leaves are
     * filtered, and hidden/dropped nodes report leaf->visible() == false.
     */
    KisProjectionLeafSP rootLeaf = image->root()->projectionLeaf();
    for (KisProjectionLeafSP leaf = rootLeaf->firstChild(); leaf; leaf = leaf->nextSibling()) {
        if (!leaf->isLayer() || !leaf->visible())
            continue;

        KisNodeSP node = leaf->node();
        KisLayer *layer = qobject_cast<KisLayer *>(node.data());
        if (!layer)
            continue;

        Unit unit;
        unit.node = node;
        unit.opacity = leaf->opacity();

        KisPaintDeviceSP projection = node->projection();
        unit.extent = projection ? projection->extent() & imageBounds : QRect();

        unit.recompositable = isSimpleOverUnit(layer, leaf, imageCs);

        if (unit.recompositable) {
            if (KisShapeLayer *shapeLayer = qobject_cast<KisShapeLayer *>(node.data())) {
                if (isEligibleShapeLayer(shapeLayer)) {
                    unit.isVector = true;
                    unit.shapeLayers << qMakePair(shapeLayer, quint8(255));
                }
            } else if (KisGroupLayer *group = qobject_cast<KisGroupLayer *>(node.data())) {
                // pass-through groups never reach this point: they are not
                // visible leaves. This is an isolated group; it is a vector
                // unit iff all its rendered descendants are shape layers.
                QVector<QPair<KisShapeLayer *, quint8>> shapes;
                if (collectGroupShapeLayers(group, quint8(255), imageCs, shapes) && !shapes.isEmpty()) {
                    unit.isVector = true;
                    unit.shapeLayers = shapes;
                }
            }
        }

        units << unit;
    }

    return units;
}

QVector<KisViewResVectorCompositor::Job> KisViewResVectorCompositor::computeJobs(const QVector<Unit> &units)
{
    /**
     * For every vector unit compute the pixel-space closure of units that
     * overlap it: anything painting inside its coverage region has to be
     * peeled and re-composited as well to keep the z-order. Closures that
     * hit a non-recompositable unit (a "firewall": blend modes, inherit
     * alpha, adjustment layers, styles...) are dropped - those vectors keep
     * document-resolution rendering, which is a correctness-preserving
     * fallback, not a semantic change.
     */
    const int numUnits = units.size();

    // vector units whose closure is clean
    QVector<int> validTargets;

    for (int v = 0; v < numUnits; v++) {
        if (!units[v].isVector || units[v].extent.isEmpty())
            continue;

        QVector<int> closure{v};
        QRect u = units[v].extent;
        bool failed = false;

        while (!failed) {
            bool grew = false;
            for (int i = 0; i < numUnits; i++) {
                if (closure.contains(i))
                    continue;
                if (units[i].extent.isEmpty() || !units[i].extent.intersects(u))
                    continue;

                if (!units[i].recompositable) {
                    failed = true;
                    break;
                }

                closure << i;
                u |= units[i].extent;
                grew = true;
            }
            if (!grew)
                break;
        }

        if (failed || closure.size() > maxJobUnits || qint64(u.width()) * u.height() > maxJobPixels) {
            continue;
        }

        validTargets << v;
    }

    /**
     * Merge all required units into connected components: components start
     * as the individual target closures. Then iterate: (a) extend every
     * component to its full pixel-space closure over *all* units (an upper
     * unit painting inside the region must be re-composited too, or the
     * repaint would re-order against it), and (b) merge components that
     * share units or overlap in document space. Both steps may grow
     * membership, so repeat until stable. Components hitting a
     * non-recompositable unit or a size cap are dropped - their vectors
     * keep document-resolution rendering.
     */
    QVector<QVector<int>> components;
    components.reserve(validTargets.size());
    for (int t = 0; t < validTargets.size(); t++) {
        components << QVector<int>{validTargets[t]};
    }

    for (bool changed = true; changed;) {
        changed = false;

        // (a) extend each component to its pixel-space closure
        for (QVector<int> &members : components) {
            if (members.isEmpty())
                continue;

            QRect u;
            for (int idx : members)
                u |= units[idx].extent;

            bool grew = true;
            while (grew) {
                grew = false;
                for (int i = 0; i < numUnits; i++) {
                    if (members.contains(i))
                        continue;
                    if (units[i].extent.isEmpty() || !units[i].extent.intersects(u))
                        continue;

                    if (!units[i].recompositable) {
                        members.clear();
                        grew = false;
                        break;
                    }
                    members << i;
                    u |= units[i].extent;
                    grew = true;
                    changed = true;
                }
            }
        }

        // (b) merge components that share a unit or overlap in doc space
        for (int a = 0; a < components.size(); a++) {
            if (components[a].isEmpty())
                continue;
            for (int b = a + 1; b < components.size(); b++) {
                if (components[b].isEmpty())
                    continue;

                QRect ra, rb;
                for (int idx : components[a])
                    ra |= units[idx].extent;
                for (int idx : components[b])
                    rb |= units[idx].extent;

                bool shares = ra.intersects(rb);
                if (!shares) {
                    for (int idx : components[b]) {
                        if (components[a].contains(idx)) {
                            shares = true;
                            break;
                        }
                    }
                }

                if (shares) {
                    for (int idx : components[b]) {
                        if (!components[a].contains(idx))
                            components[a] << idx;
                    }
                    components[b].clear();
                    changed = true;
                }
            }
        }
    }

    QVector<Job> jobs;
    for (QVector<int> &members : components) {
        if (members.isEmpty())
            continue;

        std::sort(members.begin(), members.end());

        QRect u;
        for (int idx : members)
            u |= units[idx].extent;

        if (members.size() > maxJobUnits || qint64(u.width()) * u.height() > maxJobPixels) {
            continue;
        }

        Job job;
        job.docRect = u;
        job.units = members;
        jobs << job;
    }

    return jobs;
}

quint64 KisViewResVectorCompositor::unitsSignature(const QVector<Unit> &units)
{
    quint64 sig = 0xcbf29ce484222325ull;

    auto mix = [&sig](quint64 v) {
        sig ^= v;
        sig *= 0x100000001b3ull;
    };

    for (const Unit &unit : units) {
        KisNodeSP node = unit.node;
        mix(quint64(reinterpret_cast<quintptr>(node.data())));
        mix(quint64(unit.extent.x()) ^ (quint64(unit.extent.y()) << 16) ^ (quint64(unit.extent.width()) << 32)
            ^ (quint64(unit.extent.height()) << 48));
        mix(unit.opacity);
        mix((unit.recompositable ? 1 : 0) | (unit.isVector ? 2 : 0) | (quint64(unit.shapeLayers.size()) << 8));
        for (const auto &pair : unit.shapeLayers) {
            mix(quint64(reinterpret_cast<quintptr>(pair.first)));
            mix(pair.second);
        }
    }

    return sig;
}

void KisViewResVectorCompositor::unblendSourceOver(QImage &backdropPremult, const QImage &sourcePremult)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(backdropPremult.format() == QImage::Format_ARGB32_Premultiplied);
    KIS_SAFE_ASSERT_RECOVER_RETURN(sourcePremult.format() == QImage::Format_ARGB32_Premultiplied);
    KIS_SAFE_ASSERT_RECOVER_RETURN(backdropPremult.size() == sourcePremult.size());

    const int w = backdropPremult.width();
    const int h = backdropPremult.height();

    for (int y = 0; y < h; y++) {
        QRgb *dst = reinterpret_cast<QRgb *>(backdropPremult.scanLine(y));
        const QRgb *src = reinterpret_cast<const QRgb *>(sourcePremult.constScanLine(y));

        for (int x = 0; x < w; x++) {
            const int sa = qAlpha(src[x]);
            if (sa == 0)
                continue;

            if (sa >= 255) {
                // the source fully covers the backdrop here; the repaint
                // completely replaces the pixel
                dst[x] = 0;
                continue;
            }

            /**
             * D = S + B * (1 - a_s)  (premultiplied source-over)
             * => B = (D - S) / (1 - a_s),   a_B = (a_D - a_s) / (1 - a_s)
             */
            const qreal inv = 255.0 / (255 - sa);

            const int da = qAlpha(dst[x]);
            const int na = qBound(0, qRound((da - sa) * inv), 255);
            const int nr = qBound(0, qRound((qRed(dst[x]) - qRed(src[x])) * inv), 255);
            const int ng = qBound(0, qRound((qGreen(dst[x]) - qGreen(src[x])) * inv), 255);
            const int nb = qBound(0, qRound((qBlue(dst[x]) - qBlue(src[x])) * inv), 255);

            dst[x] = qRgba(nr, ng, nb, na);
        }
    }
}

QImage
KisViewResVectorCompositor::buildUnitPatch(const Unit &unit, const QRect &docRect, const DeviceToQImageFn &convert)
{
    KisNodeSP node = unit.node;
    if (!node || docRect.isEmpty())
        return QImage();

    KisPaintDeviceSP projection = node->projection();
    if (!projection)
        return QImage();

    QImage image = convert(projection, docRect);
    if (image.isNull())
        return QImage();

    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    if (unit.opacity != 255) {
        // premultiply the whole contribution by the unit's effective
        // opacity: S = o * U, applied to every premultiplied channel
        const qreal scale = unit.opacity / 255.0;
        const int w = image.width();
        const int h = image.height();
        for (int y = 0; y < h; y++) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < w; x++) {
                const int a = qAlpha(line[x]);
                if (a == 0)
                    continue;
                line[x] = qRgba(qRound(qRed(line[x]) * scale),
                                qRound(qGreen(line[x]) * scale),
                                qRound(qBlue(line[x]) * scale),
                                qRound(a * scale));
            }
        }
    }

    return image;
}

QImage KisViewResVectorCompositor::buildBackdrop(KisImageSP image,
                                                 const Job &job,
                                                 const QVector<Unit> &units,
                                                 const DeviceToQImageFn &convert)
{
    if (!image || job.docRect.isEmpty())
        return QImage();

    KisPaintDeviceSP projection = image->projection();
    if (!projection)
        return QImage();

    QImage backdrop = convert(projection, job.docRect);
    if (backdrop.isNull())
        return QImage();
    backdrop = backdrop.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // peel top unit first
    for (int i = job.units.size() - 1; i >= 0; i--) {
        const Unit &unit = units[job.units[i]];
        if (unit.opacity == 0)
            continue;

        const QImage source = buildUnitPatch(unit, job.docRect, convert);
        if (source.isNull())
            continue;

        unblendSourceOver(backdrop, source);
    }

    return backdrop;
}
