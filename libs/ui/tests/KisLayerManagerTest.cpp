/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisLayerManagerTest.h"

#include <simpletest.h>
#include <KoColor.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoPathShape.h>
#include <kis_paint_device.h>
#include "kis_layer_manager.h"

void KisLayerManagerTest::testRectToPointsInPt()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);

    // Fill a 100x100 pixel rect with opaque black (alpha 255)
    const QRect rectPx(0, 0, 100, 100);
    dev->fill(rectPx, KoColor(Qt::black, cs));

    // Test standard 72 ppi: xRes = 1.0 (72/72), pt = px * 72 / 72 = 100 pt
    {
        KoPathShape *shape = KisLayerManager::traceDeviceToPathShape(dev, rectPx, 1.0, 1.0);
        QVERIFY(shape != nullptr);
        QRectF boundsPt = shape->outline().boundingRect();
        // Shape outline should span ~100x100 pt
        QVERIFY(qAbs(boundsPt.width() - 100.0) <= 1.0);
        QVERIFY(qAbs(boundsPt.height() - 100.0) <= 1.0);
        delete shape;
    }

    // Test 144 ppi: xRes = 2.0 (144/72), pt = px * 72 / 144 = 50 pt
    {
        KoPathShape *shape = KisLayerManager::traceDeviceToPathShape(dev, rectPx, 2.0, 2.0);
        QVERIFY(shape != nullptr);
        QRectF boundsPt = shape->outline().boundingRect();
        // 100 px at 144 ppi = 50 pt
        QVERIFY(qAbs(boundsPt.width() - 50.0) <= 1.0);
        QVERIFY(qAbs(boundsPt.height() - 50.0) <= 1.0);
        delete shape;
    }

    // Test 300 ppi: xRes = 300.0 / 72.0 = 4.16667
    {
        const qreal res = 300.0 / 72.0;
        KoPathShape *shape = KisLayerManager::traceDeviceToPathShape(dev, rectPx, res, res);
        QVERIFY(shape != nullptr);
        QRectF boundsPt = shape->outline().boundingRect();
        const qreal expectedPt = 100.0 * 72.0 / 300.0; // 24 pt
        QVERIFY(qAbs(boundsPt.width() - expectedPt) <= 1.0);
        QVERIFY(qAbs(boundsPt.height() - expectedPt) <= 1.0);
        delete shape;
    }
}

void KisLayerManagerTest::testDonutKeepsHole()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);

    // Create a 100x100 outer rect with a 40x40 transparent hole in the center (30, 30, 40, 40)
    const QRect outerRect(0, 0, 100, 100);
    dev->fill(outerRect, KoColor(Qt::black, cs));

    // Clear the center hole to transparent
    const QRect holeRect(30, 30, 40, 40);
    dev->clear(holeRect);

    KoPathShape *shape = KisLayerManager::traceDeviceToPathShape(dev, outerRect, 1.0, 1.0);
    QVERIFY(shape != nullptr);

    // Verify fill rule is preserved
    QCOMPARE(shape->fillRule(), shape->outline().fillRule());

    // Verify subpaths: outer boundary + inner hole boundary (at least 2 subpaths)
    QVERIFY(shape->subpathCount() >= 2);

    // Point inside outer ring should be contained (painted)
    QVERIFY(shape->outline().contains(QPointF(10.0, 10.0)));
    QVERIFY(shape->outline().contains(QPointF(85.0, 85.0)));

    // Center of the hole must NOT be filled/contained (the hole is preserved!)
    QVERIFY(!shape->outline().contains(QPointF(50.0, 50.0)));

    delete shape;
}

void KisLayerManagerTest::testEmptyLayerAddsNothing()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);

    // Device with no pixels painted has empty exact bounds
    QVERIFY(dev->exactBounds().isEmpty());

    KoPathShape *shape = KisLayerManager::traceDeviceToPathShape(dev, dev->exactBounds(), 1.0, 1.0);
    QVERIFY(shape == nullptr);

    // Device with an empty bounds rect also returns nullptr
    shape = KisLayerManager::traceDeviceToPathShape(dev, QRect(), 1.0, 1.0);
    QVERIFY(shape == nullptr);

    // Null device returns nullptr
    shape = KisLayerManager::traceDeviceToPathShape(nullptr, QRect(0, 0, 100, 100), 1.0, 1.0);
    QVERIFY(shape == nullptr);
}

void KisLayerManagerTest::testThreshold50Percent()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);

    // Test pixels below 50% threshold: alpha = 127
    QColor semiTransLow(0, 0, 0, 127);
    KoColor koSemiLow(semiTransLow, cs);
    dev->fill(QRect(0, 0, 50, 50), koSemiLow);

    // Alpha 127 is < 128 (50%), so binarization treats it as MIN_SELECTED (nothing to trace)
    KoPathShape *shapeLow = KisLayerManager::traceDeviceToPathShape(dev, dev->exactBounds(), 1.0, 1.0);
    QVERIFY(shapeLow == nullptr);

    // Test pixels at 50% threshold: alpha = 128
    dev->clear();
    QColor semiTransHigh(0, 0, 0, 128);
    KoColor koSemiHigh(semiTransHigh, cs);
    dev->fill(QRect(0, 0, 50, 50), koSemiHigh);

    // Alpha 128 is >= 128 (50%), so binarization treats it as MAX_SELECTED (valid shape created)
    KoPathShape *shapeHigh = KisLayerManager::traceDeviceToPathShape(dev, dev->exactBounds(), 1.0, 1.0);
    QVERIFY(shapeHigh != nullptr);
    QVERIFY(!shapeHigh->outline().isEmpty());
    delete shapeHigh;
}

SIMPLE_TEST_MAIN(KisLayerManagerTest)
