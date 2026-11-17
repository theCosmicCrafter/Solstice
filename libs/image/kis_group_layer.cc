/*
 *  SPDX-FileCopyrightText: 2005 C. Boemann <cbo@boemann.dk>
 *  SPDX-FileCopyrightText: 2007 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_group_layer.h"

#include <KoIcon.h>
#include <kis_icon.h>
#include <KoCompositeOpRegistry.h>
#include <KoColorSpace.h>
#include <KoColor.h>


#include "kis_node_visitor.h"
#include "kis_processing_visitor.h"
#include "kis_debug.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_default_bounds.h"
#include "kis_clone_layer.h"
#include "kis_selection_mask.h"
#include "kis_psd_layer_style.h"
#include "kis_layer_properties_icons.h"
#include "kis_lod_capable_layer_offset.h"
#include <kis_projection_leaf.h>
#include <kis_abstract_projection_plane.h>


struct Q_DECL_HIDDEN KisGroupLayer::Private
{
public:
    Private(KisDefaultBoundsBaseSP defaultBounds)
        : paintDevice(0)
        , offset(defaultBounds)
        , passThroughMode(false)
    {
    }

    KisPaintDeviceSP paintDevice;
    // Solstice: the group's position; the projection device stays at 0,0
    // (see setX()). It has a value of its own for the level of detail, as
    // the device's offset had.
    KisLodCapableLayerOffset offset;
    bool passThroughMode;

    std::tuple<KisPaintDeviceSP, bool> originalImpl() const;
};

KisGroupLayer::KisGroupLayer(KisImageWSP image, const QString &name, quint8 opacity, const KoColorSpace * colorSpace) :
    KisLayer(image, name, opacity),
    m_d(new Private(new KisDefaultBounds(image)))
{
    resetCache(colorSpace);
}

KisGroupLayer::KisGroupLayer(const KisGroupLayer &rhs) :
    KisLayer(rhs),
    m_d(new Private(new KisDefaultBounds(rhs.image())))
{
    m_d->paintDevice = new KisPaintDevice(*rhs.m_d->paintDevice.data());
    m_d->offset = rhs.m_d->offset;
    m_d->paintDevice->setDefaultPixel(const_cast<KisGroupLayer*>(&rhs)->m_d->paintDevice->defaultPixel());
    m_d->paintDevice->setProjectionDevice(true);
    m_d->passThroughMode = rhs.passThroughMode();
}

KisGroupLayer::~KisGroupLayer()
{
    delete m_d;
}

bool KisGroupLayer::checkCloneLayer(KisCloneLayerSP clone) const
{
    KisNodeSP source = clone->copyFrom();
    if (source) {
        if(!allowAsChild(source)) return false;

        if (source->inherits("KisGroupLayer")) {
            KisNodeSP newParent = const_cast<KisGroupLayer*>(this);
            while (newParent) {
                if (newParent == source) {
                    return false;
                }
                newParent = newParent->parent();
            }
        }
    }

    return true;
}

bool KisGroupLayer::checkNodeRecursively(KisNodeSP node) const
{
    KisCloneLayerSP cloneLayer = dynamic_cast<KisCloneLayer*>(node.data());
    if(cloneLayer) {
        return checkCloneLayer(cloneLayer);
    }
    else if (node->inherits("KisGroupLayer")) {
        KisNodeSP child = node->firstChild();
        while (child) {
            if (!checkNodeRecursively(child)) {
                return false;
            }
            child = child->nextSibling();
        }
    }

    return true;
}

bool KisGroupLayer::allowAsChild(KisNodeSP node) const
{
    if (!checkNodeRecursively(node)) return false;

    if (!parent()) {
        // We are the root layer, so we need to check
        // whether the node that's going to be added is
        // a selection mask; that is only allowed if
        // there isn't a global selection. See
        // BUG:294905

        if (node->inherits("KisSelectionMask")) {
            return !qobject_cast<KisSelectionMask*>(node.data())->active() || !selectionMask();
        }

        KisImageSP image = this->image();

        if (!image || !image->allowMasksOnRootNode()) {
            if (node->inherits("KisMask")) {
                return false;
            }
        }
    }

    return checkNodeRecursively(node);

}

const KoColorSpace * KisGroupLayer::colorSpace() const
{
    return m_d->paintDevice->colorSpace();
}

QIcon KisGroupLayer::icon() const
{
    return KisIconUtils::loadIcon("groupLayer");
}

void KisGroupLayer::setImage(KisImageWSP image)
{
    m_d->paintDevice->setDefaultBounds(new KisDefaultBounds(image));
    m_d->offset.setDefaultBounds(new KisDefaultBounds(image));

    /**
     * Updates are cropped to the image bounds, so the projection is never
     * refreshed outside of them. Pixels there can only come from another
     * image (a group copied between documents) and would stay forever,
     * e.g. in the layer thumbnail. Drop them.
     */
    if (image.isValid()) {
        m_d->paintDevice->crop(image->bounds());
    }

    KisLayer::setImage(image);
}

KisLayerSP KisGroupLayer::createMergedLayerTemplate(KisLayerSP prevLayer)
{
    KisGroupLayer *prevGroup = dynamic_cast<KisGroupLayer*>(prevLayer.data());

    if (prevGroup && canMergeAndKeepBlendOptions(prevLayer)) {
        KisSharedPtr<KisGroupLayer> merged(new KisGroupLayer(*prevGroup));

        KisNodeSP child, cloned;

        for (child = firstChild(); child; child = child->nextSibling()) {
            cloned = child->clone();
            image()->addNode(cloned, merged);
        }

        if (!merged->passThroughMode()) {
            image()->refreshGraphAsync(merged);
        }

        return merged;
    } else
        return KisLayer::createMergedLayerTemplate(prevLayer);
}

void KisGroupLayer::fillMergedLayerTemplate(KisLayerSP dstLayer, KisLayerSP prevLayer, bool skipPaintingThisLayer)
{
    if (!dynamic_cast<KisGroupLayer*>(dstLayer.data())) {
        KisLayer::fillMergedLayerTemplate(dstLayer, prevLayer, skipPaintingThisLayer);
    }
}

void KisGroupLayer::resetCache(const KoColorSpace *colorSpace)
{
    if (!colorSpace)
        colorSpace = image()->colorSpace();

    Q_ASSERT(colorSpace);

    if (!m_d->paintDevice) {

        // Solstice: the projection stays at 0,0 (see setX())
        KisPaintDeviceSP dev = new KisPaintDevice(this, colorSpace, new KisDefaultBounds(image()));
        m_d->paintDevice = dev;
        m_d->paintDevice->setProjectionDevice(true);
    }
    else if(*m_d->paintDevice->colorSpace() != *colorSpace) {

        m_d->paintDevice->clear();
        m_d->paintDevice->convertTo(colorSpace);

    } else {

        m_d->paintDevice->clear();
    }
}

KisLayer* KisGroupLayer::onlyMeaningfulChild() const
{
    KisNode *child = firstChild().data();
    KisLayer *onlyLayer = 0;

    while (child) {
        KisLayer *layer = qobject_cast<KisLayer*>(child);
        if (layer && !layer->isFakeNode()) {
            if (onlyLayer) return 0;
            onlyLayer = layer;
        }
        child = child->nextSibling().data();
    }

    return onlyLayer;
}

KisPaintDeviceSP KisGroupLayer::tryObligeChild() const
{
    KisLayer *child = onlyMeaningfulChild();

    if (child) {
        KisPaintDeviceSP projection = child->projection();
        if (child->channelFlags().isEmpty() &&
                projection &&
                child->visible() &&
                (child->compositeOpId() == COMPOSITE_OVER ||
                 child->compositeOpId() == COMPOSITE_ALPHA_DARKEN ||
                 child->compositeOpId() == COMPOSITE_COPY) &&
                child->opacity() == OPACITY_OPAQUE_U8 &&
                *projection->colorSpace() == *colorSpace() &&
                !child->layerStyle()) {

            quint8 defaultOpacity =
                    m_d->paintDevice->defaultPixel().opacityU8();

            if(defaultOpacity == OPACITY_TRANSPARENT_U8) {
                return projection;
            }
        }
    }

    return 0;
}

std::tuple<KisPaintDeviceSP, bool> KisGroupLayer::originalImpl() const
{
    /**
     * We are too lazy! Let's our children work for us.
     * Try to use children's paintDevice if it's the only
     * one in stack and meets some conditions
     */
    KisPaintDeviceSP realOriginal = tryObligeChild();
    bool ownsOriginal = false;

    if (!realOriginal) {
        if (!childCount() && !m_d->paintDevice->extent().isEmpty()) {
            m_d->paintDevice->clear();
        }
        realOriginal = m_d->paintDevice;
        ownsOriginal = true;
    }

    return std::make_tuple(realOriginal, ownsOriginal);
}

KisPaintDeviceSP KisGroupLayer::original() const
{
    return std::get<0>(originalImpl());
}

KisPaintDeviceSP KisGroupLayer::lazyDestinationForSubtreeComposition() const
{
    KisPaintDeviceSP originalDev;
    bool ownsOriginal = false;
    std::tie(originalDev, ownsOriginal) = originalImpl();

    return ownsOriginal ? originalDev : nullptr;
}

QRect KisGroupLayer::amortizedProjectionRectForCleanupInChangePass() const
{
    return hasEffectMasks() ? projection()->exactBoundsAmortized() : m_d->paintDevice->exactBoundsAmortized();
}

KisPaintDeviceSP KisGroupLayer::paintDevice() const
{
    return 0;
}

bool KisGroupLayer::projectionIsValid() const
{
    return !tryObligeChild();
}

void KisGroupLayer::setDefaultProjectionColor(KoColor color)
{
    m_d->paintDevice->setDefaultPixel(color);
}

KoColor KisGroupLayer::defaultProjectionColor() const
{
    KoColor color(m_d->paintDevice->defaultPixel(), m_d->paintDevice->colorSpace());
    return color;
}

bool KisGroupLayer::passThroughMode() const
{
    return m_d->passThroughMode;
}

void KisGroupLayer::setPassThroughMode(bool value)
{
    if (m_d->passThroughMode == value) return;

    m_d->passThroughMode = value;
    if (m_d->passThroughMode) {
        resetCache(colorSpace());
    }
    baseNodeChangedCallback();
    baseNodeInvalidateAllFramesCallback();
    notifyChildMaskChanged();
}

KisBaseNode::PropertyList KisGroupLayer::sectionModelProperties() const
{
    KisBaseNode::PropertyList l = KisLayer::sectionModelProperties();

    l << KisLayerPropertiesIcons::getProperty(KisLayerPropertiesIcons::passThrough, passThroughMode());

    return l;
}

void KisGroupLayer::setSectionModelProperties(const KisBaseNode::PropertyList &properties)
{
    Q_FOREACH (const KisBaseNode::Property &property, properties) {
        if (property.name == i18n("Pass Through")) {
            setPassThroughMode(property.state.toBool());
        }
    }

    KisLayer::setSectionModelProperties(properties);
}

bool KisGroupLayer::accept(KisNodeVisitor &v)
{
    return v.visit(this);
}

void KisGroupLayer::accept(KisProcessingVisitor &visitor, KisUndoAdapter *undoAdapter)
{
    return visitor.visit(this, undoAdapter);
}

qint32 KisGroupLayer::x() const
{
    return m_d->offset->x();
}

qint32 KisGroupLayer::y() const
{
    return m_d->offset->y();
}

/**
 * Solstice: a group's position is a number only; its projection device stays
 * at 0,0. Upstream moved the device with the group, which shifted the old
 * pixels along until the children were composited again (every mover
 * refreshes the old and the new area anyway). A projection off the 64 px
 * image grid cannot be composited in place by the GPU engine, so a moved
 * group and all its children fell back to the CPU
 * (docs/agent/gpu-engine.md, phase 4.112).
 */
void KisGroupLayer::setX(qint32 x)
{
    m_d->offset->setX(x);
}

void KisGroupLayer::setY(qint32 y)
{
    m_d->offset->setY(y);
}

void KisGroupLayer::syncLodCache()
{
    KisLayer::syncLodCache();
    m_d->offset.syncLodCache();
}

struct ExtentPolicy
{
    inline QRect operator() (const KisNode *node) {
        return node->projectionPlane()->looseUserVisibleBounds();
    }
};

struct ExactBoundsPolicy
{
    inline QRect operator() (const KisNode *node) {
        return node->projectionPlane()->tightUserVisibleBounds();
    }
};

template <class MetricPolicy>
QRect collectRects(const KisNode *node, MetricPolicy policy)
{
    QRect accumulator;

    const KisNode *child = node->firstChild();
    while (child) {
        if (child->projectionLeaf()->isLayer() &&
                child->projectionLeaf()->visible()) {

            accumulator |= policy(child);
        }
        child = child->nextSibling();
    }

    return accumulator;
}

QRect KisGroupLayer::extent() const
{
    return m_d->passThroughMode ?
        calculateChildrenLooseUserVisibleBounds() :
        KisLayer::extent();
}

QRect KisGroupLayer::exactBounds() const
{
    return m_d->passThroughMode ?
        calculateChildrenTightUserVisibleBounds() :
        KisLayer::exactBounds();
}

QRect KisGroupLayer::calculateChildrenTightUserVisibleBounds() const
{
    return collectRects(this, ExactBoundsPolicy());
}

QRect KisGroupLayer::calculateChildrenLooseUserVisibleBounds() const
{
    return collectRects(this, ExtentPolicy());
}
