/*
 *  SPDX-FileCopyrightText: 2015 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "move_selection_stroke_test.h"

#include <simpletest.h>

#include <KoColor.h>

#include "kis_image.h"
#include "strokes/move_selection_stroke_strategy.h"
#include "stroke_testing_utils.h"
#include "kis_selection.h"
#include "commands/kis_selection_commands.h"
#include "strokes/move_stroke_strategy.h"
#include "kis_paint_layer.h"
#include "KisImageBarrierLock.h"
#include "kis_paint_device_frames_interface.h"
#include "kis_paint_device_debug_utils.h"
#include "kis_undo_stores.h"

#include <testui.h>

KisPaintDeviceSP lodDevice(KisPaintDeviceSP dev)
{
    KisPaintDeviceSP tmp = new KisPaintDevice(dev->colorSpace());
    dev->testingFetchLodDevice(tmp);
    return tmp;
}


void MoveSelectionStrokeTest::test()
{
    const QRect imageRect(0,0,800,800);
    KisImageSP image = utils::createImage(0, imageRect.size());
    QScopedPointer<KoCanvasResourceProvider> manager(
        utils::createResourceManager(image));

    image->setLodPreferences(KisLodPreferences(2));
    image->waitForDone();

    KisResourcesSnapshotSP resources =
        new KisResourcesSnapshot(image,
                                 image->root()->firstChild(),
                                 manager.data());

    KisNodeSP currentNode = resources->currentNode();
    KisPaintLayerSP currentPaintLayer = dynamic_cast<KisPaintLayer*>(currentNode.data());
    Q_ASSERT(currentPaintLayer);

    KisPaintDeviceSP device = currentNode->paintDevice();

    {
        KisImageBarrierLock lock(image);

        device->fill(QRect(0,0,400,400),     KoColor(Qt::red, image->colorSpace()));
        device->fill(QRect(400,0,400,400),   KoColor(Qt::green, image->colorSpace()));
        device->fill(QRect(0,400,400,400),   KoColor(Qt::blue, image->colorSpace()));
        device->fill(QRect(400,400,400,400), KoColor(Qt::yellow, image->colorSpace()));
    }

    {
        KisSelectionSP newSelection = new KisSelection();
        newSelection->pixelSelection()->select(QRect(200,200,400,400), OPACITY_OPAQUE_U8);

        KisSetGlobalSelectionCommand cmd(image, newSelection);
        cmd.redo();
    }

    KIS_DUMP_DEVICE_2(device, imageRect, "00_0_device", "mm");
    KIS_DUMP_DEVICE_2(image->globalSelection()->projection(), imageRect, "01_0_selection", "mm");

    {
        MoveSelectionStrokeStrategy *strategy =
            new MoveSelectionStrokeStrategy(currentPaintLayer,
                                            image->globalSelection(),
                                            image.data(),
                                            image.data());

        KisStrokeId id = image->startStroke(strategy);
        image->addJob(id, new MoveStrokeStrategy::Data(QPoint(100,100)));
        image->endStroke(id);

        image->waitForDone();

        KIS_DUMP_DEVICE_2(device, imageRect, "02_0_device", "mm");
        KIS_DUMP_DEVICE_2(lodDevice(device), imageRect, "02_1_device_lod", "mm");

        KIS_DUMP_DEVICE_2(image->globalSelection()->projection(), imageRect, "03_0_selection", "mm");
        KIS_DUMP_DEVICE_2(lodDevice(image->globalSelection()->projection()), imageRect, "03_1_selection_lod", "mm");

    }

    {
        MoveSelectionStrokeStrategy *strategy =
            new MoveSelectionStrokeStrategy(currentPaintLayer,
                                            image->globalSelection(),
                                            image.data(),
                                            image.data());

        KisStrokeId id = image->startStroke(strategy);
        image->addJob(id, new MoveStrokeStrategy::Data(QPoint(-200,50)));
        image->endStroke(id);

        image->waitForDone();

        KIS_DUMP_DEVICE_2(device, imageRect, "04_0_device", "mm");
        KIS_DUMP_DEVICE_2(lodDevice(device), imageRect, "04_1_device_lod", "mm");

        KIS_DUMP_DEVICE_2(image->globalSelection()->projection(), imageRect, "05_0_selection", "mm");
        KIS_DUMP_DEVICE_2(lodDevice(image->globalSelection()->projection()), imageRect, "05_1_selection_lod", "mm");
    }

}

namespace
{
/// An image whose bottom layer holds a red square at (0,0,100,100), selected
struct DuplicateFixture {
    DuplicateFixture()
        : undoStore(new KisSurrogateUndoStore())
        , image(utils::createImage(undoStore, QSize(400, 400)))
    {
        image->setLodPreferences(KisLodPreferences(2));
        image->waitForDone();

        layer = dynamic_cast<KisPaintLayer *>(image->root()->firstChild().data());
        {
            KisImageBarrierLock lock(image);
            layer->paintDevice()->fill(QRect(0, 0, 100, 100), KoColor(Qt::red, image->colorSpace()));
        }
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(QRect(0, 0, 100, 100), OPACITY_OPAQUE_U8);
        KisSetGlobalSelectionCommand cmd(image, selection);
        cmd.redo();
    }

    /// Ctrl-drag in the Move Tool: the selected pixels, copied @p offset away
    void duplicate(const QPoint &offset)
    {
        MoveSelectionStrokeStrategy *strategy =
            new MoveSelectionStrokeStrategy(layer, image->globalSelection(), image.data(), image.data(), true);
        KisStrokeId id = image->startStroke(strategy);
        image->addJob(id, new MoveStrokeStrategy::Data(offset));
        image->endStroke(id);
        image->waitForDone();
    }

    KisSurrogateUndoStore *undoStore;
    KisImageSP image;
    KisPaintLayerSP layer;
};

bool isRed(KisPaintDeviceSP device, int x, int y)
{
    KoColor color;
    device->pixel(x, y, &color);
    return color.toQColor() == QColor(Qt::red);
}

bool isTransparent(KisPaintDeviceSP device, int x, int y)
{
    KoColor color;
    device->pixel(x, y, &color);
    return color.opacityU8() == OPACITY_TRANSPARENT_U8;
}
} // namespace

/// A duplicate leaves the selected pixels in place, pastes the copy at the
/// offset and moves the selection to it, as one undo step; Instant
/// Preview (the level of detail clone) shows the same
void MoveSelectionStrokeTest::testDuplicateKeepsSource()
{
    DuplicateFixture f;
    KisPaintDeviceSP device = f.layer->paintDevice();

    f.duplicate(QPoint(200, 100));

    QVERIFY(isRed(device, 50, 50));
    QVERIFY(isRed(device, 250, 150));
    QVERIFY(isTransparent(device, 150, 50));
    QCOMPARE(device->exactBounds(), QRect(0, 0, 300, 200));
    QCOMPARE(f.image->globalSelection()->selectedExactRect(), QRect(200, 100, 100, 100));
    QCOMPARE(f.undoStore->presentCommand()->text().toString(), QString("Duplicate Selection"));

    // the preview at a quarter of the size
    KisPaintDeviceSP lod = lodDevice(device);
    QVERIFY(isRed(lod, 12, 12));
    QVERIFY(isRed(lod, 62, 37));
    QVERIFY(isTransparent(lod, 37, 12));

    f.undoStore->undo();
    f.image->waitForDone();
    QCOMPARE(device->exactBounds(), QRect(0, 0, 100, 100));
    QVERIFY(isRed(device, 50, 50));
    QCOMPARE(f.image->globalSelection()->selectedExactRect(), QRect(0, 0, 100, 100));

    f.undoStore->redo();
    f.image->waitForDone();
    QVERIFY(isRed(device, 50, 50));
    QVERIFY(isRed(device, 250, 150));
    QCOMPARE(f.image->globalSelection()->selectedExactRect(), QRect(200, 100, 100, 100));
}

/// Duplicating again copies the last copy: each stamp is its own undo step
void MoveSelectionStrokeTest::testDuplicateStamps()
{
    DuplicateFixture f;
    KisPaintDeviceSP device = f.layer->paintDevice();

    f.duplicate(QPoint(100, 0));
    f.duplicate(QPoint(100, 0));

    for (int x : {50, 150, 250}) {
        QVERIFY(isRed(device, x, 50));
    }
    QCOMPARE(device->exactBounds(), QRect(0, 0, 300, 100));
    QCOMPARE(f.image->globalSelection()->selectedExactRect(), QRect(200, 0, 100, 100));

    f.undoStore->undo();
    f.image->waitForDone();
    QCOMPARE(device->exactBounds(), QRect(0, 0, 200, 100));
    QCOMPARE(f.image->globalSelection()->selectedExactRect(), QRect(100, 0, 100, 100));

    f.undoStore->undo();
    f.image->waitForDone();
    QCOMPARE(device->exactBounds(), QRect(0, 0, 100, 100));
    QCOMPARE(f.image->globalSelection()->selectedExactRect(), QRect(0, 0, 100, 100));
}

KISTEST_MAIN(MoveSelectionStrokeTest)
