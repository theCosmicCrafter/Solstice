/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QRegularExpression>
#include <QTest>
#include <simpletest.h>

#include <KoColor.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>

#include "KisClipStackLayer.h"
#include "kis_group_layer.h"
#include "kis_image.h"
#include "kis_layer_properties_icons.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"
#include "kis_pixel_selection.h"
#include "kis_projection_leaf.h"
#include "kis_selection.h"
#include "kis_transparency_mask.h"
#include <kis_meta_data_merge_strategy_registry.h>
#include <kis_undo_stores.h>

namespace
{
const QRect imageRect(0, 0, 128, 128);

const KoColorSpace *rgb8()
{
    return KoColorSpaceRegistry::instance()->rgb8();
}

KisImageSP createImage(KisUndoStore *undoStore = nullptr)
{
    return new KisImage(undoStore, imageRect.width(), imageRect.height(), rgb8(), "clipping test");
}

KisPaintLayerSP addLayer(KisImageSP image,
                         KisNodeSP parent,
                         const QString &name,
                         const QRect &rect,
                         const QColor &color,
                         const QString &compositeOp = COMPOSITE_OVER,
                         quint8 opacity = OPACITY_OPAQUE_U8)
{
    KisPaintLayerSP layer = new KisPaintLayer(image, name, opacity, rgb8());
    layer->paintDevice()->fill(rect, KoColor(color, rgb8()));
    layer->setCompositeOpId(compositeOp);
    image->addNode(layer, parent, parent->lastChild());
    return layer;
}

KisGroupLayerSP addGroup(KisImageSP image,
                         KisNodeSP parent,
                         const QString &name,
                         const QString &compositeOp = COMPOSITE_OVER,
                         quint8 opacity = OPACITY_OPAQUE_U8)
{
    KisGroupLayerSP group = new KisGroupLayer(image, name, opacity);
    group->setCompositeOpId(compositeOp);
    image->addNode(group, parent, parent->lastChild());
    return group;
}

QByteArray refreshAndRead(KisImageSP image)
{
    image->initialRefreshGraph();
    image->waitForDone();

    QByteArray data(imageRect.width() * imageRect.height() * rgb8()->pixelSize(), '\0');
    image->projection()->readBytes(reinterpret_cast<quint8 *>(data.data()), imageRect);
    return data;
}

QByteArray read(KisImageSP image)
{
    image->waitForDone();

    QByteArray data(imageRect.width() * imageRect.height() * rgb8()->pixelSize(), '\0');
    image->projection()->readBytes(reinterpret_cast<quint8 *>(data.data()), imageRect);
    return data;
}

/**
 * The document every test starts from: a background, the base
 * "dress" (Multiply, 200), two clipped layers with their own blend modes
 * and opacities, and a normal layer on top.
 */
struct ClipScene {
    KisImageSP image;
    KisPaintLayerSP background;
    KisPaintLayerSP base;
    KisPaintLayerSP clip1;
    KisPaintLayerSP clip2;
    KisPaintLayerSP top;

    ClipScene(KisUndoStore *undoStore = nullptr)
    {
        image = createImage(undoStore);
        KisNodeSP root = image->root();
        background = addLayer(image, root, "background", imageRect, QColor(200, 200, 200));
        base = addLayer(image, root, "dress", QRect(20, 20, 60, 60), QColor(255, 0, 0), COMPOSITE_MULT, 200);
        clip1 = addLayer(image, root, "clip1", QRect(40, 0, 80, 100), QColor(0, 0, 255));
        clip2 = addLayer(image, root, "clip2", QRect(0, 50, 100, 20), QColor(0, 255, 0), COMPOSITE_SCREEN, 128);
        top = addLayer(image, root, "top", QRect(70, 70, 20, 20), QColor(255, 255, 0));
        clip1->setClipped(true);
        clip2->setClipped(true);
    }
};

/**
 * The same document built with today's tools: a group with the base's
 * blend mode and opacity around a plain base and inherit-alpha layers.
 */
struct InheritAlphaScene {
    KisImageSP image;
    KisPaintLayerSP base;
    KisPaintLayerSP clip1;

    InheritAlphaScene()
    {
        image = createImage();
        KisNodeSP root = image->root();
        addLayer(image, root, "background", imageRect, QColor(200, 200, 200));
        KisGroupLayerSP group = addGroup(image, root, "group", COMPOSITE_MULT, 200);
        base = addLayer(image, group, "dress", QRect(20, 20, 60, 60), QColor(255, 0, 0));
        clip1 = addLayer(image, group, "clip1", QRect(40, 0, 80, 100), QColor(0, 0, 255));
        KisPaintLayerSP clip2 =
            addLayer(image, group, "clip2", QRect(0, 50, 100, 20), QColor(0, 255, 0), COMPOSITE_SCREEN, 128);
        addLayer(image, root, "top", QRect(70, 70, 20, 20), QColor(255, 255, 0));
        clip1->disableAlphaChannel(true);
        clip2->disableAlphaChannel(true);
    }
};
} // namespace

class KisClippingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testLeafTree();
    void testMatchesInheritAlphaGroup();
    void testIncrementalUpdate();
    void testUnclipRestoresPlainComposition();
    void testHiddenBaseHidesRun();
    void testHiddenClippedLayer();
    void testOrphanClippedLayerIsNotClipped();
    void testGroupAsBase();
    void testBaseMaskClipsRun();
    void testNestedRunInGroup();
    void testPropertyExcludesInheritAlpha();
    void testPropertyChangeRefreshesRun();
    void testMergeDownBakesIntoBase();
    void testMergeDownOfClippedLayers();
    void testImageCopyKeepsClipping();
    void testMergeSelectedBaseAndClippedLayers();
    void testMergeSelectedClippedLayers();
    void testFlattenImageKeepsResult();
    void testToggleInheritAlphaTwiceOnClippedLayer();
};

void KisClippingTest::testLeafTree()
{
    ClipScene scene;

    KisProjectionLeafSP rootLeaf = scene.image->root()->projectionLeaf();
    KisProjectionLeafSP stackLeaf = scene.base->clipStackLayer()->projectionLeaf();

    QCOMPARE(rootLeaf->firstChild(), scene.background->projectionLeaf());
    QCOMPARE(scene.background->projectionLeaf()->nextSibling(), stackLeaf);
    QCOMPARE(stackLeaf->prevSibling(), scene.background->projectionLeaf());
    QCOMPARE(stackLeaf->nextSibling(), scene.top->projectionLeaf());
    QCOMPARE(scene.top->projectionLeaf()->prevSibling(), stackLeaf);
    QCOMPARE(stackLeaf->parent(), rootLeaf);
    QVERIFY(!stackLeaf->isRoot());
    QVERIFY(stackLeaf->isLayer());
    QVERIFY(stackLeaf->canHaveChildLayers());

    QCOMPARE(stackLeaf->firstChild(), scene.base->projectionLeaf());
    QCOMPARE(stackLeaf->lastChild(), scene.clip2->projectionLeaf());
    QCOMPARE(scene.base->projectionLeaf()->parent(), stackLeaf);
    QCOMPARE(scene.clip1->projectionLeaf()->parent(), stackLeaf);
    QVERIFY(!scene.base->projectionLeaf()->prevSibling());
    QCOMPARE(scene.base->projectionLeaf()->nextSibling(), scene.clip1->projectionLeaf());
    QCOMPARE(scene.clip1->projectionLeaf()->prevSibling(), scene.base->projectionLeaf());
    QCOMPARE(scene.clip2->projectionLeaf()->prevSibling(), scene.clip1->projectionLeaf());
    QVERIFY(!scene.clip2->projectionLeaf()->nextSibling());

    // the stack composites like the base; inside it the base is plain
    QCOMPARE(stackLeaf->compositeOpId(), QString(COMPOSITE_MULT));
    QCOMPARE(stackLeaf->opacity(), quint8(200));
    QCOMPARE(scene.base->projectionLeaf()->compositeOpId(), QString(COMPOSITE_OVER));
    QCOMPARE(scene.base->projectionLeaf()->opacity(), quint8(OPACITY_OPAQUE_U8));
    QVERIFY(scene.base->projectionLeaf()->channelFlags().isEmpty());

    const QBitArray clipFlags = scene.clip1->projectionLeaf()->channelFlags();
    QCOMPARE(clipFlags, rgb8()->channelFlags(true, false));

    // the node graph itself is unchanged
    QCOMPARE(scene.clip1->parent(), scene.image->root());
    QCOMPARE(scene.base->nextSibling(), KisNodeSP(scene.clip1));
}

void KisClippingTest::testMatchesInheritAlphaGroup()
{
    ClipScene scene;
    InheritAlphaScene reference;

    QCOMPARE(refreshAndRead(scene.image), refreshAndRead(reference.image));
}

void KisClippingTest::testIncrementalUpdate()
{
    ClipScene scene;
    InheritAlphaScene reference;
    refreshAndRead(scene.image);
    refreshAndRead(reference.image);

    // a stroke on a clipped layer, and one on the base, update through the
    // merge walker instead of a full refresh
    const QRect clipStroke(10, 10, 50, 30);
    scene.clip1->paintDevice()->fill(clipStroke, KoColor(QColor(255, 0, 255), rgb8()));
    scene.clip1->setDirty(clipStroke);
    reference.clip1->paintDevice()->fill(clipStroke, KoColor(QColor(255, 0, 255), rgb8()));
    reference.clip1->setDirty(clipStroke);
    QCOMPARE(read(scene.image), read(reference.image));

    const QRect baseStroke(70, 30, 40, 20);
    scene.base->paintDevice()->fill(baseStroke, KoColor(QColor(0, 128, 0), rgb8()));
    scene.base->setDirty(baseStroke);
    reference.base->paintDevice()->fill(baseStroke, KoColor(QColor(0, 128, 0), rgb8()));
    reference.base->setDirty(baseStroke);
    QCOMPARE(read(scene.image), read(reference.image));

    // an incremental result equals a full refresh
    const QByteArray incremental = read(scene.image);
    QCOMPARE(refreshAndRead(scene.image), incremental);
}

void KisClippingTest::testUnclipRestoresPlainComposition()
{
    ClipScene scene;
    const QByteArray clipped = refreshAndRead(scene.image);

    scene.clip1->setClipped(false);
    scene.clip2->setClipped(false);
    scene.clip1->setDirty();
    scene.clip2->setDirty();
    const QByteArray unclipped = read(scene.image);

    KisImageSP plain = createImage();
    KisNodeSP root = plain->root();
    addLayer(plain, root, "background", imageRect, QColor(200, 200, 200));
    addLayer(plain, root, "dress", QRect(20, 20, 60, 60), QColor(255, 0, 0), COMPOSITE_MULT, 200);
    addLayer(plain, root, "clip1", QRect(40, 0, 80, 100), QColor(0, 0, 255));
    addLayer(plain, root, "clip2", QRect(0, 50, 100, 20), QColor(0, 255, 0), COMPOSITE_SCREEN, 128);
    addLayer(plain, root, "top", QRect(70, 70, 20, 20), QColor(255, 255, 0));

    QCOMPARE(unclipped, refreshAndRead(plain));
    QVERIFY(clipped != unclipped);
}

void KisClippingTest::testHiddenBaseHidesRun()
{
    ClipScene scene;
    scene.base->setVisible(false);

    KisImageSP expected = createImage();
    KisNodeSP root = expected->root();
    addLayer(expected, root, "background", imageRect, QColor(200, 200, 200));
    addLayer(expected, root, "top", QRect(70, 70, 20, 20), QColor(255, 255, 0));

    QCOMPARE(refreshAndRead(scene.image), refreshAndRead(expected));
}

void KisClippingTest::testHiddenClippedLayer()
{
    ClipScene scene;
    InheritAlphaScene reference;
    scene.clip1->setVisible(false);
    reference.clip1->setVisible(false);

    QCOMPARE(refreshAndRead(scene.image), refreshAndRead(reference.image));
}

void KisClippingTest::testOrphanClippedLayerIsNotClipped()
{
    KisImageSP image = createImage();
    KisPaintLayerSP lowest = addLayer(image, image->root(), "lowest", QRect(10, 10, 50, 50), QColor(0, 0, 255));
    addLayer(image, image->root(), "above", QRect(30, 30, 50, 50), QColor(255, 0, 0));
    lowest->setClipped(true);

    QVERIFY(!lowest->projectionLeaf()->parent()->node()->inherits("KisClipStackLayer"));
    QCOMPARE(lowest->projectionLeaf()->parent(), image->root()->projectionLeaf());

    KisImageSP expected = createImage();
    addLayer(expected, expected->root(), "lowest", QRect(10, 10, 50, 50), QColor(0, 0, 255));
    addLayer(expected, expected->root(), "above", QRect(30, 30, 50, 50), QColor(255, 0, 0));

    QCOMPARE(refreshAndRead(image), refreshAndRead(expected));
}

void KisClippingTest::testGroupAsBase()
{
    KisImageSP image = createImage();
    KisNodeSP root = image->root();
    addLayer(image, root, "background", imageRect, QColor(200, 200, 200));
    KisGroupLayerSP baseGroup = addGroup(image, root, "base group", COMPOSITE_OVER, 220);
    addLayer(image, baseGroup, "part1", QRect(10, 10, 40, 40), QColor(255, 0, 0));
    addLayer(image, baseGroup, "part2", QRect(60, 60, 40, 40), QColor(0, 0, 255));
    KisPaintLayerSP clip = addLayer(image, root, "clip", QRect(0, 30, 128, 50), QColor(0, 255, 0), COMPOSITE_MULT);
    clip->setClipped(true);

    QCOMPARE(clip->projectionLeaf()->parent()->firstChild(), baseGroup->projectionLeaf());

    KisImageSP expected = createImage();
    KisNodeSP expectedRoot = expected->root();
    addLayer(expected, expectedRoot, "background", imageRect, QColor(200, 200, 200));
    KisGroupLayerSP outer = addGroup(expected, expectedRoot, "outer", COMPOSITE_OVER, 220);
    KisGroupLayerSP inner = addGroup(expected, outer, "base group");
    addLayer(expected, inner, "part1", QRect(10, 10, 40, 40), QColor(255, 0, 0));
    addLayer(expected, inner, "part2", QRect(60, 60, 40, 40), QColor(0, 0, 255));
    KisPaintLayerSP expectedClip =
        addLayer(expected, outer, "clip", QRect(0, 30, 128, 50), QColor(0, 255, 0), COMPOSITE_MULT);
    expectedClip->disableAlphaChannel(true);

    QCOMPARE(refreshAndRead(image), refreshAndRead(expected));
}

void KisClippingTest::testBaseMaskClipsRun()
{
    auto addMask = [](KisImageSP image, KisLayerSP layer) {
        KisTransparencyMaskSP mask = new KisTransparencyMask(image, "mask");
        mask->initSelection(layer);
        mask->selection()->pixelSelection()->clear();
        mask->selection()->pixelSelection()->select(QRect(0, 0, 50, 128));
        image->addNode(mask, layer);
    };

    ClipScene scene;
    InheritAlphaScene reference;
    addMask(scene.image, scene.base);
    addMask(reference.image, reference.base);

    QCOMPARE(refreshAndRead(scene.image), refreshAndRead(reference.image));
}

void KisClippingTest::testNestedRunInGroup()
{
    KisImageSP image = createImage();
    KisNodeSP root = image->root();
    addLayer(image, root, "background", imageRect, QColor(200, 200, 200));
    KisGroupLayerSP group = addGroup(image, root, "group");
    addLayer(image, group, "below", QRect(0, 0, 30, 128), QColor(0, 0, 0));
    KisPaintLayerSP base = addLayer(image, group, "base", QRect(20, 20, 60, 60), QColor(255, 0, 0));
    KisPaintLayerSP clip = addLayer(image, group, "clip", QRect(40, 0, 80, 100), QColor(0, 0, 255));
    clip->setClipped(true);

    QCOMPARE(group->projectionLeaf()->lastChild(), base->clipStackLayer()->projectionLeaf());

    KisImageSP expected = createImage();
    KisNodeSP expectedRoot = expected->root();
    addLayer(expected, expectedRoot, "background", imageRect, QColor(200, 200, 200));
    KisGroupLayerSP expectedGroup = addGroup(expected, expectedRoot, "group");
    addLayer(expected, expectedGroup, "below", QRect(0, 0, 30, 128), QColor(0, 0, 0));
    KisGroupLayerSP stack = addGroup(expected, expectedGroup, "stack");
    addLayer(expected, stack, "base", QRect(20, 20, 60, 60), QColor(255, 0, 0));
    KisPaintLayerSP expectedClip = addLayer(expected, stack, "clip", QRect(40, 0, 80, 100), QColor(0, 0, 255));
    expectedClip->disableAlphaChannel(true);

    QCOMPARE(refreshAndRead(image), refreshAndRead(expected));
}

namespace
{
void mergeDown(KisImageSP image, KisLayerSP layer)
{
    image->mergeDown(layer, KisMetaData::MergeStrategyRegistry::instance()->get("Drop"));
    image->waitForDone();
}

KisLayerSP findLayer(KisImageSP image, const QString &name)
{
    for (KisNodeSP node = image->root()->firstChild(); node; node = node->nextSibling()) {
        if (node->name() == name) {
            return qobject_cast<KisLayer *>(node.data());
        }
    }
    return KisLayerSP();
}
} // namespace

void KisClippingTest::testPropertyExcludesInheritAlpha()
{
    KisSurrogateUndoStore *undoStore = new KisSurrogateUndoStore();
    ClipScene scene(undoStore);
    scene.clip1->setClipped(false);
    scene.clip1->disableAlphaChannel(true);

    // switching clipping on switches inherit alpha off, and undo restores it
    KisLayerPropertiesIcons::setNodePropertyAutoUndo(scene.clip1, KisLayerPropertiesIcons::clipping, true, scene.image);
    scene.image->waitForDone();
    QVERIFY(scene.clip1->isClipped());
    QVERIFY(!scene.clip1->alphaChannelDisabled());
    undoStore->undo();
    scene.image->waitForDone();
    QVERIFY(!scene.clip1->isClipped());
    QVERIFY(scene.clip1->alphaChannelDisabled());

    // and the other way round
    scene.clip1->disableAlphaChannel(false);
    scene.clip1->setClipped(true);
    KisLayerPropertiesIcons::setNodePropertyAutoUndo(scene.clip1,
                                                     KisLayerPropertiesIcons::inheritAlpha,
                                                     true,
                                                     scene.image);
    scene.image->waitForDone();
    QVERIFY(!scene.clip1->isClipped());
    QVERIFY(scene.clip1->alphaChannelDisabled());
    undoStore->undo();
    scene.image->waitForDone();
    QVERIFY(scene.clip1->isClipped());
    QVERIFY(!scene.clip1->alphaChannelDisabled());
}

void KisClippingTest::testPropertyChangeRefreshesRun()
{
    // clip is clipped to middle; clipping middle moves clip onto the base,
    // which changes pixels outside middle's own bounds
    KisSurrogateUndoStore *undoStore = new KisSurrogateUndoStore();
    KisImageSP image = createImage(undoStore);
    KisNodeSP root = image->root();
    addLayer(image, root, "background", imageRect, QColor(200, 200, 200));
    addLayer(image, root, "base", QRect(0, 0, 100, 100), QColor(255, 0, 0));
    KisPaintLayerSP middle = addLayer(image, root, "middle", QRect(40, 40, 20, 20), QColor(0, 255, 0));
    KisPaintLayerSP clip = addLayer(image, root, "clip", QRect(0, 0, 128, 128), QColor(0, 0, 255));
    clip->setClipped(true);
    refreshAndRead(image);

    KisLayerPropertiesIcons::setNodePropertyAutoUndo(middle, KisLayerPropertiesIcons::clipping, true, image);
    const QByteArray updated = read(image);
    QCOMPARE(updated, refreshAndRead(image));

    undoStore->undo();
    const QByteArray restored = read(image);
    QCOMPARE(restored, refreshAndRead(image));
    QVERIFY(restored != updated);
}

void KisClippingTest::testMergeDownBakesIntoBase()
{
    ClipScene scene;
    const QByteArray before = refreshAndRead(scene.image);

    mergeDown(scene.image, scene.clip1);

    KisLayerSP merged = findLayer(scene.image, "dress");
    QVERIFY(merged);
    QVERIFY(!findLayer(scene.image, "clip1"));
    QVERIFY(!merged->isClipped());
    QCOMPARE(merged->compositeOpId(), QString(COMPOSITE_MULT));
    QCOMPARE(merged->opacity(), quint8(200));

    // clip2 now clips to the merged layer and nothing looks different
    KisLayerSP clip2 = findLayer(scene.image, "clip2");
    QVERIFY(clip2 && clip2->isClipped());
    QCOMPARE(clip2->projectionLeaf()->parent(), merged->clipStackLayer()->projectionLeaf());
    QCOMPARE(read(scene.image), before);
    QCOMPARE(refreshAndRead(scene.image), before);
}

void KisClippingTest::testMergeDownOfClippedLayers()
{
    ClipScene scene;
    scene.clip2->setCompositeOpId(COMPOSITE_OVER);
    scene.clip2->setOpacity(OPACITY_OPAQUE_U8);
    const QByteArray before = refreshAndRead(scene.image);

    // two clipped layers with the same blend options merge into one
    // clipped layer that looks the same
    mergeDown(scene.image, scene.clip2);

    KisLayerSP merged = findLayer(scene.image, "clip1");
    QVERIFY(merged);
    QVERIFY(!findLayer(scene.image, "clip2"));
    QVERIFY(merged->isClipped());
    QCOMPARE(merged->projectionLeaf()->parent(), scene.base->clipStackLayer()->projectionLeaf());
    QCOMPARE(refreshAndRead(scene.image), before);
}

void KisClippingTest::testImageCopyKeepsClipping()
{
    // Saving copies the image. The copy's bases must not take a strong
    // reference to the image under construction: releasing it deleted the
    // copy (crash on saving a .kra with clipping, 2026-10-27).
    ClipScene scene;
    const QByteArray before = refreshAndRead(scene.image);

    KisImageSP copy = scene.image->clone(true);
    QVERIFY(copy);
    KisLayerSP clip1 = findLayer(copy, "clip1");
    KisLayerSP base = findLayer(copy, "dress");
    QVERIFY(clip1 && base);
    QVERIFY(clip1->isClipped());
    QVERIFY(!base->isClipped());
    QCOMPARE(clip1->projectionLeaf()->parent(), base->clipStackLayer()->projectionLeaf());
    QCOMPARE(refreshAndRead(copy), before);
}

void KisClippingTest::testMergeSelectedBaseAndClippedLayers()
{
    // with a Normal base at full opacity, Merge Selected of a whole stack
    // gives one unclipped layer that looks the same
    ClipScene scene;
    scene.base->setCompositeOpId(COMPOSITE_OVER);
    scene.base->setOpacity(OPACITY_OPAQUE_U8);
    const QByteArray before = refreshAndRead(scene.image);

    scene.image->mergeMultipleLayers(KisNodeList() << scene.base << scene.clip1 << scene.clip2, scene.clip2);
    scene.image->waitForDone();

    QVERIFY(!findLayer(scene.image, "clip1"));
    QVERIFY(!findLayer(scene.image, "clip2"));
    int clippedCount = 0;
    for (KisNodeSP node = scene.image->root()->firstChild(); node; node = node->nextSibling()) {
        KisLayer *layer = qobject_cast<KisLayer *>(node.data());
        clippedCount += layer && layer->isClipped();
    }
    QCOMPARE(clippedCount, 0);
    QCOMPARE(scene.image->root()->childCount(), 3u);
    QCOMPARE(refreshAndRead(scene.image), before);
}

void KisClippingTest::testMergeSelectedClippedLayers()
{
    // clipped layers alone merge into one clipped layer
    ClipScene scene;
    scene.clip2->setCompositeOpId(COMPOSITE_OVER);
    scene.clip2->setOpacity(OPACITY_OPAQUE_U8);
    const QByteArray before = refreshAndRead(scene.image);

    scene.image->mergeMultipleLayers(KisNodeList() << scene.clip1 << scene.clip2, scene.clip2);
    scene.image->waitForDone();

    QVERIFY(!findLayer(scene.image, "clip2"));
    KisLayerSP merged = qobject_cast<KisLayer *>(scene.base->nextSibling().data());
    QVERIFY(merged);
    QVERIFY(merged->isClipped());
    QCOMPARE(merged->projectionLeaf()->parent(), scene.base->clipStackLayer()->projectionLeaf());
    QCOMPARE(refreshAndRead(scene.image), before);
}

void KisClippingTest::testFlattenImageKeepsResult()
{
    ClipScene scene;
    const QByteArray before = refreshAndRead(scene.image);

    scene.image->flatten(scene.top);
    scene.image->waitForDone();

    QCOMPARE(scene.image->root()->childCount(), 1u);
    QCOMPARE(refreshAndRead(scene.image), before);
}

void KisClippingTest::testToggleInheritAlphaTwiceOnClippedLayer()
{
    // Toggling inherit alpha twice on a clipped layer merges the two undo
    // commands. Their property lists must record that clipping went off,
    // or the merge asserts (Shift+G twice, 2026-10-27).
    QTest::failOnWarning(QRegularExpression(QStringLiteral("SAFE ASSERT")));

    KisSurrogateUndoStore *undoStore = new KisSurrogateUndoStore();
    ClipScene scene(undoStore);
    refreshAndRead(scene.image);

    KisLayerPropertiesIcons::setNodePropertyAutoUndo(scene.clip1,
                                                     KisLayerPropertiesIcons::inheritAlpha,
                                                     true,
                                                     scene.image);
    scene.image->waitForDone();
    QVERIFY(!scene.clip1->isClipped());
    QVERIFY(scene.clip1->alphaChannelDisabled());

    KisLayerPropertiesIcons::setNodePropertyAutoUndo(scene.clip1,
                                                     KisLayerPropertiesIcons::inheritAlpha,
                                                     false,
                                                     scene.image);
    scene.image->waitForDone();
    QVERIFY(!scene.clip1->isClipped());
    QVERIFY(!scene.clip1->alphaChannelDisabled());

    // however the commands merged, undoing all of them restores clipping
    undoStore->undoAll();
    scene.image->waitForDone();
    QVERIFY(scene.clip1->isClipped());
    QVERIFY(!scene.clip1->alphaChannelDisabled());
}

SIMPLE_TEST_MAIN(KisClippingTest)
#include "KisClippingTest.moc"
