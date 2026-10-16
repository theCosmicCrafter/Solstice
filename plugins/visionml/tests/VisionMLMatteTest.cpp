/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "VisionMLMatteTest.h"
#include "SegmentationMatteUtils.h"

void VisionMLMatteTest::testModulateMatteAlphaFull()
{
    // Full matte (255), full lasso mask (255), full layer alpha (255) -> 255
    const quint8 result = VisionMLMatte::modulateMatteAlpha(255, 255, 255);
    QCOMPARE(result, 255);
}

void VisionMLMatteTest::testModulateMatteAlphaZeros()
{
    // If matte is 0 -> 0
    QCOMPARE(VisionMLMatte::modulateMatteAlpha(0, 255, 255), 0);
    // If lasso mask is 0 -> 0 (clipped to lasso)
    QCOMPARE(VisionMLMatte::modulateMatteAlpha(255, 0, 255), 0);
    // If source alpha is 0 -> 0 (clipped to layer alpha)
    QCOMPARE(VisionMLMatte::modulateMatteAlpha(255, 255, 0), 0);
    // All 0 -> 0
    QCOMPARE(VisionMLMatte::modulateMatteAlpha(0, 0, 0), 0);
}

void VisionMLMatteTest::testModulateMatteAlphaAntialiasedMask()
{
    // Test that intermediate mask coverage is not binarized (preserved smoothly)
    // 128/255 mask on full matte and full source alpha:
    // (255 * 128 * 255 + 127*255) / (255*255) = (8323200 + 32385) / 65025 = 8355585 / 65025 = 128
    const quint8 result = VisionMLMatte::modulateMatteAlpha(255, 128, 255);
    QCOMPARE(result, 128);

    // Quarter coverage: 64/255 mask
    // (255 * 64 * 255 + 32385) / 65025 = (4161600 + 32385) / 65025 = 4193985 / 65025 = 64
    QCOMPARE(VisionMLMatte::modulateMatteAlpha(255, 64, 255), 64);
}

void VisionMLMatteTest::testModulateMatteAlphaSemiTransparent()
{
    // 50% matte (128), 100% mask (255), 50% alpha (128):
    // 128 * 255 * 128 = 4177920
    // num = 4177920 + 32385 = 4210305
    // 4210305 / 65025 = 64
    const quint8 result = VisionMLMatte::modulateMatteAlpha(128, 255, 128);
    QCOMPARE(result, 64);

    // Arbitrary values: 200 matte, 150 mask, 100 alpha:
    // num = 200 * 150 * 100 + 32385 = 3032385
    // 3032385 / 65025 = 46.63 -> 46
    QCOMPARE(VisionMLMatte::modulateMatteAlpha(200, 150, 100), 46);
}

void VisionMLMatteTest::testExpandRegionToMin()
{
    const QRect bounds(0, 0, 500, 500);
    // Center region of 20x20
    QRect region(100, 100, 20, 20);
    VisionMLMatte::expandRegionToMin(region, bounds, 64);

    QCOMPARE(region.width(), 64);
    QCOMPARE(region.height(), 64);
    QVERIFY(bounds.contains(region));
    // Symmetrical expansion: original center (110, 110)
    QCOMPARE(region.center(), QPoint(110, 110));
}

void VisionMLMatteTest::testExpandRegionBoundaryConditions()
{
    const QRect bounds(0, 0, 100, 100);

    // Near top-left edge
    QRect topLeftRegion(2, 2, 10, 10);
    VisionMLMatte::expandRegionToMin(topLeftRegion, bounds, 64);
    QCOMPARE(topLeftRegion.width(), 64);
    QCOMPARE(topLeftRegion.height(), 64);
    QVERIFY(bounds.contains(topLeftRegion));

    // Near bottom-right edge
    QRect bottomRightRegion(85, 85, 10, 10);
    VisionMLMatte::expandRegionToMin(bottomRightRegion, bounds, 64);
    QCOMPARE(bottomRightRegion.width(), 64);
    QCOMPARE(bottomRightRegion.height(), 64);
    QVERIFY(bounds.contains(bottomRightRegion));

    // Layer smaller than 64 in dimension should not expand
    const QRect smallBounds(0, 0, 40, 40);
    QRect smallRegion(5, 5, 10, 10);
    VisionMLMatte::expandRegionToMin(smallRegion, smallBounds, 64);
    QCOMPARE(smallRegion.width(), 10);
    QCOMPARE(smallRegion.height(), 10);
}

void VisionMLMatteTest::testClampRegionToMax()
{
    const QRect bounds(0, 0, 5000, 5000);
    // Region exceeding 3072
    QRect largeRegion(500, 500, 4000, 4000);
    const bool clamped = VisionMLMatte::clampRegionToMax(largeRegion, bounds, 3072);

    QVERIFY(clamped);
    QCOMPARE(largeRegion.width(), 3072);
    QCOMPARE(largeRegion.height(), 3072);
    QVERIFY(bounds.contains(largeRegion));
    // Clamped around center (original center was 2500, 2500)
    QCOMPARE(largeRegion.center(), QPoint(2500, 2500));

    // Region within limit is not clamped
    QRect normalRegion(100, 100, 500, 500);
    const bool normalClamped = VisionMLMatte::clampRegionToMax(normalRegion, bounds, 3072);
    QVERIFY(!normalClamped);
    QCOMPARE(normalRegion.width(), 500);
    QCOMPARE(normalRegion.height(), 500);
}

SIMPLE_TEST_MAIN(VisionMLMatteTest)
