/*
 *  SPDX-FileCopyrightText: 2015 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_projection_leaf.h"

#include <KoColorSpace.h>

#include "kis_layer.h"
#include "kis_image.h"
#include "kis_mask.h"
#include "kis_group_layer.h"
#include "kis_selection_mask.h"
#include "kis_adjustment_layer.h"

#include "krita_utils.h"

#include "kis_refresh_subtree_walker.h"
#include "kis_async_merger.h"
#include "kis_node_graph_listener.h"
#include "kis_clone_layer.h"
#include "KisClipStackLayer.h"

#include <KoCompositeOpRegistry.h>

struct Q_DECL_HIDDEN KisProjectionLeaf::Private
{
    Private(KisNode *_node) : node(_node) {}

    KisNodeWSP node;
    bool isTemporaryHidden = false;

    static bool checkPassThrough(const KisNode *node) {
        const KisGroupLayer *group = qobject_cast<const KisGroupLayer*>(node);
        return group && group->passThroughMode();
    }

    static bool isSelectionMask(const KisNode *node) {
        return qobject_cast<const KisSelectionMask*>(node);
    }

    static KisNodeSP skipSelectionMasksForward(KisNodeSP node) {
        while (node && isSelectionMask(node)) {
            node = node->nextSibling();
        }
        return node;
    }

    static KisNodeSP skipSelectionMasksBackward(KisNodeSP node) {
        while (node && isSelectionMask(node)) {
            node = node->prevSibling();
        }
        return node;
    }

    bool checkParentPassThrough() {
        return node->parent() && checkPassThrough(node->parent());
    }

    bool checkThisPassThrough() {
        return checkPassThrough(node);
    }

    KisProjectionLeafSP overlayProjectionLeaf() const {
        return node && node->graphListener() && node->graphListener()->graphOverlayNode() ?
            node->graphListener()->graphOverlayNode()->projectionLeaf() : 0;
    }

    bool isTopmostNode() const {
        return !skipSelectionMasksForward(node->nextSibling()) &&
            node->parent() &&
            !node->parent()->parent();
    }

    KisNodeSP findRoot() const {
        KisNodeSP root = node;

        while (root->parent()) {
            root = root->parent();
        }

        return root;
    }

    void temporarySetPassThrough(bool value) {
        KisGroupLayer *group = qobject_cast<KisGroupLayer*>(node.data());
        if (!group) return;

        group->setPassThroughMode(value);
    }

    /**
     * Solstice: Clipping Mask. A clip run is a sequence of adjacent clipped
     * siblings; its base is the non-clipped layer directly below the run.
     * The projection leaf tree replaces the base and its run with the
     * base's KisClipStackLayer, whose children are the base and the run.
     * Pass-through groups take no part in clipping: they cannot be clipped
     * or be a base, and their children are not clipped.
     */
    static bool isClipLayer(const KisNode *node)
    {
        const KisLayer *layer = qobject_cast<const KisLayer *>(node);
        return layer && !layer->isFakeNode() && !qobject_cast<const KisClipStackLayer *>(node);
    }

    static bool parentAllowsClipping(const KisNode *node)
    {
        return node->parent() && !checkPassThrough(node->parent());
    }

    static bool isClippedNode(const KisNode *node)
    {
        return isClipLayer(node) && !checkPassThrough(node) && static_cast<const KisLayer *>(node)->isClipped()
            && parentAllowsClipping(node);
    }

    static bool canBeClipBase(const KisNode *node)
    {
        return isClipLayer(node) && !checkPassThrough(node) && !static_cast<const KisLayer *>(node)->isClipped()
            && parentAllowsClipping(node);
    }

    static KisLayer *clipBaseOf(const KisNode *node)
    {
        if (!isClippedNode(node))
            return nullptr;

        KisNodeSP prev = skipSelectionMasksBackward(node->prevSibling());
        while (prev && isClippedNode(prev)) {
            prev = skipSelectionMasksBackward(prev->prevSibling());
        }

        return prev && canBeClipBase(prev) ? static_cast<KisLayer *>(prev.data()) : nullptr;
    }

    static bool hasClipRun(const KisNode *node)
    {
        if (!canBeClipBase(node))
            return false;

        KisNodeSP next = skipSelectionMasksForward(node->nextSibling());
        return next && isClippedNode(next);
    }

    static KisNodeSP lastOfClipRun(KisNodeSP base)
    {
        KisNodeSP last = base;
        KisNodeSP next = skipSelectionMasksForward(base->nextSibling());
        while (next && isClippedNode(next)) {
            last = next;
            next = skipSelectionMasksForward(next->nextSibling());
        }
        return last;
    }

    /// The stack that contains \p node as its base or as a clipped layer
    static KisNodeSP clipStackOfMember(const KisNode *node)
    {
        if (hasClipRun(node)) {
            return static_cast<const KisLayer *>(node)->clipStackLayer().data();
        }
        if (KisLayer *base = clipBaseOf(node)) {
            return base->clipStackLayer().data();
        }
        return KisNodeSP();
    }

    /// The node that represents \p node among its parent's leaves
    static KisNodeSP clipRepresentative(KisNodeSP node)
    {
        if (!node)
            return node;
        KisNodeSP stack = clipStackOfMember(node);
        return stack ? stack : node;
    }

    /// The base when \p node is a clip stack
    static KisLayerSP clipStackBase(const KisNode *node)
    {
        const KisClipStackLayer *stack = qobject_cast<const KisClipStackLayer *>(node);
        return stack ? stack->base() : KisLayerSP();
    }

    static bool isTopmostNode(const KisNode *node)
    {
        return !skipSelectionMasksForward(node->nextSibling()) && node->parent() && !node->parent()->parent();
    }
};

KisProjectionLeaf::KisProjectionLeaf(KisNode *node)
    : m_d(new Private(node))
{
}

KisProjectionLeaf::~KisProjectionLeaf()
{
}

KisProjectionLeafSP KisProjectionLeaf::parent() const
{
    KisNodeSP node;

    if (KisLayerSP base = Private::clipStackBase(m_d->node)) {
        node = base->parent();
    } else if (Private::isSelectionMask(m_d->node)) {
        if (m_d->overlayProjectionLeaf() == this) {
            node = m_d->findRoot();
        }
    } else if (KisNodeSP stack = Private::clipStackOfMember(m_d->node)) {
        return stack->projectionLeaf();
    } else {
        node = m_d->node->parent();
    }

    while (node && Private::checkPassThrough(node)) {
        node = node->parent();
    }

    return node ? node->projectionLeaf() : KisProjectionLeafSP();
}


KisProjectionLeafSP KisProjectionLeaf::firstChild() const
{
    if (KisLayerSP base = Private::clipStackBase(m_d->node)) {
        return base->projectionLeaf();
    }

    KisNodeSP node;

    if (!m_d->checkThisPassThrough()) {
        node = m_d->node->firstChild();
        node = Private::skipSelectionMasksForward(node);
        node = Private::clipRepresentative(node);
    }

    if (!node && isRoot()) {
        KisProjectionLeafSP overlayLeaf = m_d->overlayProjectionLeaf();
        if (overlayLeaf) {
            return overlayLeaf;
        }
    }

    return node ? node->projectionLeaf() : KisProjectionLeafSP();
}

KisProjectionLeafSP KisProjectionLeaf::lastChild() const
{
    if (KisLayerSP base = Private::clipStackBase(m_d->node)) {
        return Private::lastOfClipRun(base)->projectionLeaf();
    }

    KisNodeSP node;

    if (isRoot()) {
        KisProjectionLeafSP overlayLeaf = m_d->overlayProjectionLeaf();
        if (overlayLeaf) {
            return overlayLeaf;
        }
    }

    if (!m_d->checkThisPassThrough()) {
        node = m_d->node->lastChild();
        node = Private::skipSelectionMasksBackward(node);
        node = Private::clipRepresentative(node);
    }

    return node ? node->projectionLeaf() : KisProjectionLeafSP();
}

KisProjectionLeafSP KisProjectionLeaf::prevSibling() const
{
    if (Private::isSelectionMask(m_d->node)) {
        KisProjectionLeafSP leaf;

        if (m_d->overlayProjectionLeaf() == this) {
            KisNodeSP node = m_d->findRoot()->lastChild();
            node = Private::skipSelectionMasksBackward(node);
            node = Private::clipRepresentative(node);
            leaf = node->projectionLeaf();
        }

        return leaf;
    }

    if (KisLayerSP base = Private::clipStackBase(m_d->node)) {
        KisNodeSP node = Private::skipSelectionMasksBackward(base->prevSibling());
        node = Private::clipRepresentative(node);
        return node ? node->projectionLeaf() : KisProjectionLeafSP();
    }

    if (Private::hasClipRun(m_d->node)) {
        // the base is the first child of its stack
        return KisProjectionLeafSP();
    }

    if (Private::clipBaseOf(m_d->node)) {
        KisNodeSP node = Private::skipSelectionMasksBackward(m_d->node->prevSibling());
        return node ? node->projectionLeaf() : KisProjectionLeafSP();
    }

    KisNodeSP node;

    if (m_d->checkThisPassThrough()) {
        node = m_d->node->lastChild();
        node = Private::skipSelectionMasksBackward(node);
    }

    if (!node) {
        node = m_d->node->prevSibling();
        node = Private::skipSelectionMasksBackward(node);
    }

    const KisProjectionLeaf *leaf = this;
    while (!node && leaf->m_d->checkParentPassThrough()) {
        leaf = leaf->node()->parent()->projectionLeaf().data();
        node = leaf->node()->prevSibling();
        node = Private::skipSelectionMasksBackward(node);
    }

    node = Private::clipRepresentative(node);

    return node ? node->projectionLeaf() : KisProjectionLeafSP();
}

KisProjectionLeafSP KisProjectionLeaf::nextSibling() const
{
    if (Private::isSelectionMask(m_d->node)) {
        return KisProjectionLeafSP();
    }

    if (Private::hasClipRun(m_d->node) || Private::clipBaseOf(m_d->node)) {
        KisNodeSP node = Private::skipSelectionMasksForward(m_d->node->nextSibling());
        return node && Private::isClippedNode(node) ? node->projectionLeaf() : KisProjectionLeafSP();
    }

    // a clip stack continues after the last layer of its run
    KisNodeSP lastNode = m_d->node;
    if (KisLayerSP base = Private::clipStackBase(m_d->node)) {
        lastNode = Private::lastOfClipRun(base);
    }

    KisProjectionLeafSP overlayLeaf = m_d->overlayProjectionLeaf();
    if (overlayLeaf && Private::isTopmostNode(lastNode)) {
        return overlayLeaf;
    }

    KisNodeSP node = lastNode->nextSibling();
    node = Private::skipSelectionMasksForward(node);

    while (node && Private::checkPassThrough(node) && node->firstChild()) {
        node = node->firstChild();
        node = Private::skipSelectionMasksForward(node);
    }

    if (!node && m_d->checkParentPassThrough()) {
        node = m_d->node->parent();
        node = Private::skipSelectionMasksForward(node);
    } else {
        node = Private::clipRepresentative(node);
    }

    return node ? node->projectionLeaf() : KisProjectionLeafSP();
}

KisNodeSP KisProjectionLeaf::node() const
{
    return m_d->node;
}

KisAbstractProjectionPlaneSP KisProjectionLeaf::projectionPlane() const
{
    return m_d->node->projectionPlane();
}

bool KisProjectionLeaf::accept(KisNodeVisitor &visitor)
{
    return m_d->node->accept(visitor);
}

KisPaintDeviceSP KisProjectionLeaf::original()
{
    return m_d->node->original();
}

KisPaintDeviceSP KisProjectionLeaf::projection()
{
    return m_d->node->projection();
}

KisPaintDeviceSP KisProjectionLeaf::lazyDestinationForSubtreeComposition()
{
    if (qobject_cast<const KisClipStackLayer *>(m_d->node.data())) {
        return m_d->node->original();
    }

    const KisGroupLayer *group = qobject_cast<const KisGroupLayer*>(m_d->node.data());
    return group ? group->lazyDestinationForSubtreeComposition() : nullptr;
}

bool KisProjectionLeaf::isRoot() const
{
    // a clip stack is outside the node graph and has no node parent
    return (bool)!m_d->node->parent() && !qobject_cast<const KisClipStackLayer *>(m_d->node.data());
}

bool KisProjectionLeaf::isLayer() const
{
    return (bool)qobject_cast<const KisLayer*>(m_d->node.data()) &&
        !m_d->node->isFakeNode();
}

bool KisProjectionLeaf::isMask() const
{
    return (bool)qobject_cast<const KisMask*>(m_d->node.data()) &&
        !m_d->node->isFakeNode();
}

bool KisProjectionLeaf::canHaveChildLayers() const
{
    return (bool)qobject_cast<const KisGroupLayer *>(m_d->node.data())
        || (bool)qobject_cast<const KisClipStackLayer *>(m_d->node.data());
}

bool KisProjectionLeaf::dependsOnLowerNodes() const
{
    return (bool)qobject_cast<const KisAdjustmentLayer*>(m_d->node.data());
}

bool KisProjectionLeaf::visible() const
{
    if (KisLayerSP base = Private::clipStackBase(m_d->node)) {
        return base->projectionLeaf()->visible();
    }

    if (m_d->isTemporaryHidden || isDroppedNode()) return false;

    // TODO: check opacity as well!

    bool hiddenByParentPassThrough = false;

    KisNodeSP node = m_d->node->parent();
    while (node && node->projectionLeaf()->m_d->checkThisPassThrough()) {
        hiddenByParentPassThrough |= !node->visible();
        node = node->parent();
    }

    return (m_d->node->visible(false) || m_d->node->isIsolatedRoot()) &&
        !m_d->checkThisPassThrough() &&
        !hiddenByParentPassThrough;
}

quint8 KisProjectionLeaf::opacity() const
{
    // the stack takes the base's opacity; inside it, the base is opaque
    // (a base's parent is never pass-through, so its own values apply)
    if (KisLayerSP base = Private::clipStackBase(m_d->node)) {
        return base->opacity();
    }
    if (Private::hasClipRun(m_d->node)) {
        return OPACITY_OPAQUE_U8;
    }

    quint8 resultOpacity = m_d->node->opacity();

    if (m_d->checkParentPassThrough()) {
        quint8 parentOpacity = m_d->node->parent()->projectionLeaf()->opacity();

        resultOpacity = KritaUtils::mergeOpacityU8(resultOpacity, parentOpacity);
    }

    return resultOpacity;
}

QString KisProjectionLeaf::compositeOpId() const
{
    // the stack takes the base's blend mode; inside it, the base is Normal
    if (KisLayerSP base = Private::clipStackBase(m_d->node)) {
        return base->compositeOpId();
    }
    if (Private::hasClipRun(m_d->node)) {
        return COMPOSITE_OVER;
    }

    return m_d->node->compositeOpId();
}

bool KisProjectionLeaf::isClipStackMember() const
{
    return (bool)Private::clipStackOfMember(m_d->node);
}

QBitArray KisProjectionLeaf::channelFlags() const
{
    QBitArray channelFlags;

    // the stack takes the base's flags; inside it, the base uses all channels
    if (KisLayerSP base = Private::clipStackBase(m_d->node)) {
        return base->channelFlags();
    }
    if (Private::hasClipRun(m_d->node)) {
        return channelFlags;
    }

    KisLayer *layer = qobject_cast<KisLayer*>(m_d->node.data());
    if (!layer) return channelFlags;

    // a clipped layer keeps the alpha of the stack below it
    if (Private::clipBaseOf(m_d->node)) {
        const QBitArray noAlpha = layer->colorSpace()->channelFlags(true, false);
        channelFlags = layer->channelFlags();
        return channelFlags.isEmpty() ? noAlpha : (channelFlags & noAlpha);
    }

    channelFlags = layer->channelFlags();

    if (m_d->checkParentPassThrough()) {
        QBitArray parentChannelFlags;

        if (*m_d->node->colorSpace() ==
            *m_d->node->parent()->colorSpace()) {

            KisLayer *parentLayer = qobject_cast<KisLayer*>(m_d->node->parent().data());
            parentChannelFlags = parentLayer->channelFlags();
        }

        channelFlags = KritaUtils::mergeChannelFlags(channelFlags, parentChannelFlags);
    }

    return channelFlags;
}

bool KisProjectionLeaf::isStillInGraph() const
{
    if (KisLayerSP base = Private::clipStackBase(m_d->node)) {
        return (bool)base->graphListener();
    }
    return (bool)m_d->node->graphListener();
}

bool KisProjectionLeaf::hasClones() const
{
    KisLayer *layer = qobject_cast<KisLayer*>(m_d->node.data());
    return layer ? layer->hasClones() : false;
}

bool KisProjectionLeaf::isDroppedNode() const
{
    return dropReason() != NodeAvailable;
}

bool KisProjectionLeaf::shouldBeRendered() const
{
    return visible() || hasClones();
}

KisProjectionLeaf::NodeDropReason KisProjectionLeaf::dropReason() const
{
    if (qobject_cast<KisMask*>(m_d->node.data()) &&
            m_d->checkParentPassThrough()) {

        return DropPassThroughMask;
    }

    KisCloneLayer *cloneLayer = qobject_cast<KisCloneLayer*>(m_d->node.data());
    if (cloneLayer && cloneLayer->copyFrom()) {
        KisProjectionLeafSP leaf = cloneLayer->copyFrom()->projectionLeaf();

        if (leaf->m_d->checkThisPassThrough()) {
            return DropPassThroughClone;
        }
    }

    return NodeAvailable;
}

bool KisProjectionLeaf::isOverlayProjectionLeaf() const
{
    return this == m_d->overlayProjectionLeaf();
}

void KisProjectionLeaf::setTemporaryHiddenFromRendering(bool value)
{
    m_d->isTemporaryHidden = value;
}

bool KisProjectionLeaf::isTemporaryHiddenFromRendering() const
{
    return m_d->isTemporaryHidden;
}

/**
 * This method is rather slow and dangerous. It should be executes in
 * exclusive environment only.
 */
void KisProjectionLeaf::explicitlyRegeneratePassThroughProjection()
{
    if (!m_d->checkThisPassThrough()) return;

    m_d->temporarySetPassThrough(false);

    const QRect updateRect = projection()->defaultBounds()->bounds();

    KisRefreshSubtreeWalker walker(updateRect);
    walker.collectRects(m_d->node, updateRect);

    KisAsyncMerger merger;
    merger.startMerge(walker);

    m_d->temporarySetPassThrough(true);
}
