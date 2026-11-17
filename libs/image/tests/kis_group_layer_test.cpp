/*
 *  SPDX-FileCopyrightText: 2007 Boudewijn Rempt <boud@valdyas.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_group_layer_test.h"
#include <simpletest.h>

#include <testutil.h>

#include <KoColor.h>

#include "kis_group_layer.h"
#include "kis_paint_layer.h"
#include "kis_types.h"
#include "KoColorSpaceRegistry.h"
#include "kis_image.h"

#include "kis_undo_stores.h"


void KisGroupLayerTest::testProjection()
{
    const KoColorSpace * colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(0, 512, 512, colorSpace, "merge test");

}

#include "commands/kis_image_layer_remove_command.h"

void KisGroupLayerTest::testRemoveAndUndo()
{
    KisSurrogateUndoStore *undoStore = new KisSurrogateUndoStore();

//    QRect transpRect(50,50,300,300);
//    QRect blurRect(66,66,300,300);
//    QPoint blurShift(34,34);
//    QPoint cloneShift(75,75);

    QRect imageRect = QRect(0, 0, 64, 64);
    const KoColorSpace * cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(undoStore, imageRect.width(), imageRect.height(), cs, "merge test");

    QRect rect1(10, 10, 30, 30);
    QRect rect2(30, 30, 30, 30);

    KisPaintLayerSP paintLayer0 = new KisPaintLayer(image, "paint0", OPACITY_OPAQUE_U8);
    paintLayer0->paintDevice()->fill(imageRect, KoColor(Qt::white, cs));

    KisPaintLayerSP paintLayer1 = new KisPaintLayer(image, "paint1", OPACITY_OPAQUE_U8);
    paintLayer1->paintDevice()->fill(rect1, KoColor(Qt::red, cs));

    KisPaintLayerSP paintLayer2 = new KisPaintLayer(image, "paint2", OPACITY_OPAQUE_U8);
    paintLayer2->paintDevice()->fill(rect2, KoColor(Qt::blue, cs));


    KisGroupLayerSP groupLayer1 = new KisGroupLayer(image, "group1", OPACITY_OPAQUE_U8);

    image->addNode(paintLayer0, image->root());
    image->addNode(groupLayer1, image->root());
    image->addNode(paintLayer1, groupLayer1);
    image->addNode(paintLayer2, groupLayer1);

    image->initialRefreshGraph();
    QVERIFY(TestUtil::checkQImage(image->projection()->convertToQImage(0, imageRect),
                                  "group_layer_test",
                                  "undo_removal",
                                  "0_initial"));

    // touch original to cause clearing of the projection
    groupLayer1->original();

    image->undoAdapter()->addCommand(new KisImageLayerRemoveCommand(image, groupLayer1));
    image->waitForDone();

    // touch original again
    groupLayer1->original();

    QVERIFY(TestUtil::checkQImage(image->projection()->convertToQImage(0, imageRect),
                                  "group_layer_test",
                                  "undo_removal",
                                  "1_deleted"));

    undoStore->undo();
    image->waitForDone();

    // hey, man! don't sleep!
    groupLayer1->original();

    QVERIFY(TestUtil::checkQImage(image->projection()->convertToQImage(0, imageRect),
                                  "group_layer_test",
                                  "undo_removal",
                                  "2_undone"));
}

void KisGroupLayerTest::testCopyToSmallerImageCropsProjection()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();

    KisImageSP sourceImage = new KisImage(0, 200, 200, cs, "source");

    KisPaintLayerSP paintLayer1 = new KisPaintLayer(sourceImage, "paint1", OPACITY_OPAQUE_U8);
    paintLayer1->paintDevice()->fill(QRect(0, 0, 50, 50), KoColor(Qt::red, cs));

    KisPaintLayerSP paintLayer2 = new KisPaintLayer(sourceImage, "paint2", OPACITY_OPAQUE_U8);
    paintLayer2->paintDevice()->fill(QRect(80, 80, 60, 60), KoColor(Qt::blue, cs));

    // two children, so that the group composes into its own projection
    KisGroupLayerSP group = new KisGroupLayer(sourceImage, "group", OPACITY_OPAQUE_U8);
    sourceImage->addNode(group, sourceImage->root());
    sourceImage->addNode(paintLayer1, group);
    sourceImage->addNode(paintLayer2, group);

    sourceImage->initialRefreshGraph();
    QCOMPARE(group->original()->exactBounds(), QRect(0, 0, 140, 140));

    KisImageSP targetImage = new KisImage(0, 100, 100, cs, "target");
    KisNodeSP copy = group->clone();
    targetImage->addNode(copy, targetImage->root());

    // the pixels outside of the target image are never refreshed there
    QCOMPARE(copy->original()->exactBounds(), QRect(0, 0, 100, 100));

    // the children keep their pixels outside of the image
    QCOMPARE(copy->lastChild()->paintDevice()->exactBounds(), QRect(80, 80, 60, 60));

    targetImage->refreshGraphAsync();
    targetImage->waitForDone();
    QCOMPARE(copy->original()->exactBounds(), QRect(0, 0, 100, 100));
}

SIMPLE_TEST_MAIN(KisGroupLayerTest)


