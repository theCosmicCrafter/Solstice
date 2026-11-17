/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QImage>
#include <QTest>

#include "TestControlImageCompare.h"

namespace
{
QImage filled(QRgb color, int width = 4, int height = 3)
{
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}
} // namespace

class TestControlImageCompareTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testIdentical();
    void testChannelTolerance();
    void testMismatchBudget();
    void testTransparentPixelsIgnoreColor();
    void testAlphaDifference();
    void testErrors();
};

void TestControlImageCompareTest::testIdentical()
{
    const QImage image = filled(qRgba(10, 20, 30, 255));
    const TestControlImageCompare::Result result = TestControlImageCompare::compare(image, image, {});
    QVERIFY(result.error.isEmpty());
    QVERIFY(result.matches);
    QCOMPARE(result.maxChannelDiff, 0);
    QCOMPARE(result.mismatchPixels, qint64(0));
}

void TestControlImageCompareTest::testChannelTolerance()
{
    const QImage reference = filled(qRgba(100, 100, 100, 255));
    QImage image = reference;
    image.setPixel(1, 1, qRgba(103, 100, 98, 255));

    TestControlImageCompare::Result result = TestControlImageCompare::compare(image, reference, {3, 0});
    QVERIFY(result.matches);
    QCOMPARE(result.maxChannelDiff, 3);
    QCOMPARE(result.mismatchPixels, qint64(0));

    result = TestControlImageCompare::compare(image, reference, {2, 0});
    QVERIFY(!result.matches);
    QCOMPARE(result.mismatchPixels, qint64(1));
}

void TestControlImageCompareTest::testMismatchBudget()
{
    const QImage reference = filled(qRgba(0, 0, 0, 255));
    QImage image = reference;
    image.setPixel(0, 0, qRgba(255, 0, 0, 255));
    image.setPixel(3, 2, qRgba(0, 0, 255, 255));

    TestControlImageCompare::Result result = TestControlImageCompare::compare(image, reference, {10, 2});
    QVERIFY(result.matches);
    QCOMPARE(result.mismatchPixels, qint64(2));
    QCOMPARE(result.maxChannelDiff, 255);

    result = TestControlImageCompare::compare(image, reference, {10, 1});
    QVERIFY(!result.matches);
}

void TestControlImageCompareTest::testTransparentPixelsIgnoreColor()
{
    // Fully transparent pixels may keep any color in the device.
    const TestControlImageCompare::Result result =
        TestControlImageCompare::compare(filled(qRgba(255, 0, 0, 0)), filled(qRgba(0, 255, 0, 0)), {});
    QVERIFY(result.matches);
    QCOMPARE(result.maxChannelDiff, 0);
}

void TestControlImageCompareTest::testAlphaDifference()
{
    const TestControlImageCompare::Result result =
        TestControlImageCompare::compare(filled(qRgba(0, 0, 0, 0)), filled(qRgba(0, 0, 0, 40)), {});
    QVERIFY(!result.matches);
    QCOMPARE(result.maxChannelDiff, 40);
    QCOMPARE(result.mismatchPixels, qint64(12));
}

void TestControlImageCompareTest::testErrors()
{
    TestControlImageCompare::Result result = TestControlImageCompare::compare(filled(0), filled(0, 5, 3), {});
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!result.matches);

    result = TestControlImageCompare::compare(QImage(), filled(0), {});
    QVERIFY(!result.error.isEmpty());
}

QTEST_GUILESS_MAIN(TestControlImageCompareTest)

#include "TestControlImageCompareTest.moc"
