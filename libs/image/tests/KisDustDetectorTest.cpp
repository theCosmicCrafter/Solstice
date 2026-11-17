/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisDustDetector.h"

#include <KoColor.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoUpdater.h>
#include <QRandomGenerator>
#include <QTest>
#include <kis_paint_device.h>
#include <kis_pixel_selection.h>
#include <simpletest.h>

namespace
{
const KoColorSpace *rgb8()
{
    return KoColorSpaceRegistry::instance()->rgb8();
}

KoColor color(const QColor &c)
{
    return KoColor(c, rgb8());
}

KisPaintDeviceSP transparentDevice()
{
    return new KisPaintDevice(rgb8());
}

KisPaintDeviceSP filledDevice(const QRect &rect, const QColor &c)
{
    KisPaintDeviceSP dev = transparentDevice();
    dev->fill(rect, color(c));
    return dev;
}

void fillRect(KisPaintDeviceSP dev, const QRect &rect, const QColor &c)
{
    dev->fill(rect, color(c));
}

QColor pixel(KisPaintDeviceSP dev, int x, int y)
{
    QColor c;
    dev->pixel(x, y, &c);
    return c;
}

QByteArray bytes(KisPaintDeviceSP dev, const QRect &rect)
{
    QByteArray data(rect.width() * rect.height() * dev->pixelSize(), '\0');
    dev->readBytes(reinterpret_cast<quint8 *>(data.data()), rect);
    return data;
}

KisDustDetector::Options options(KisDustDetector::Mode mode, int size, qreal tolerance = 0.0)
{
    KisDustDetector::Options o;
    o.mode = mode;
    o.dustSize = size;
    o.tolerance = tolerance;
    return o;
}

int totalPixels(const QVector<KisDustDetector::Component> &components)
{
    int count = 0;
    for (const auto &c : components) {
        count += c.pixels.size();
    }
    return count;
}
} // namespace

class KisDustDetectorTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testSpecksOnTransparency()
    {
        const QRect rect(0, 0, 64, 64);
        KisPaintDeviceSP dev = transparentDevice();
        fillRect(dev, QRect(5, 5, 1, 1), Qt::black); // single pixel
        fillRect(dev, QRect(10, 10, 2, 2), Qt::black); // 2x2 speck
        fillRect(dev, QRect(20, 20, 4, 4), Qt::black); // exactly the limit
        fillRect(dev, QRect(30, 30, 5, 4), Qt::black); // one pixel too wide
        fillRect(dev, QRect(0, 50, 40, 1), Qt::black); // a line

        const auto found = KisDustDetector::detect(dev, rect, options(KisDustDetector::DustOnTransparency, 4));
        QCOMPARE(found.size(), 3);
        QCOMPARE(totalPixels(found), 1 + 4 + 16);

        KisDustDetector::apply(dev, found, KisDustDetector::Erase);
        QCOMPARE(pixel(dev, 5, 5).alpha(), 0);
        QCOMPARE(pixel(dev, 11, 11).alpha(), 0);
        QCOMPARE(pixel(dev, 21, 21).alpha(), 0);
        QCOMPARE(pixel(dev, 31, 31).alpha(), 255);
        QCOMPARE(pixel(dev, 20, 50).alpha(), 255);
    }

    void testDiagonalConnectivity()
    {
        const QRect rect(0, 0, 32, 32);
        KisPaintDeviceSP dev = transparentDevice();
        for (int i = 0; i < 10; ++i) {
            fillRect(dev, QRect(5 + i, 5 + i, 1, 1), Qt::black);
        }

        QVERIFY(KisDustDetector::detect(dev, rect, options(KisDustDetector::DustOnTransparency, 4)).isEmpty());
    }

    void testFaintEdges()
    {
        const QRect rect(0, 0, 32, 32);
        KisPaintDeviceSP dev = transparentDevice();
        fillRect(dev, QRect(9, 9, 4, 4), QColor(0, 0, 0, 20));
        fillRect(dev, QRect(10, 10, 2, 2), Qt::black);

        // Any opacity counts by default, so the faint rim goes with the speck.
        auto found = KisDustDetector::detect(dev, rect, options(KisDustDetector::DustOnTransparency, 4));
        QCOMPARE(totalPixels(found), 16);

        // A tolerance above the rim's opacity leaves only the core.
        found = KisDustDetector::detect(dev, rect, options(KisDustDetector::DustOnTransparency, 4, 0.1));
        QCOMPARE(totalPixels(found), 4);
    }

    void testInvisibleBridge()
    {
        // Eraser leftovers can join a speck to a line with pixels that are
        // almost fully transparent.
        const QRect rect(0, 0, 48, 32);
        KisPaintDeviceSP dev = transparentDevice();
        fillRect(dev, QRect(0, 20, 48, 2), Qt::black);
        fillRect(dev, QRect(10, 10, 2, 2), Qt::black);
        fillRect(dev, QRect(11, 12, 1, 8), QColor(0, 0, 0, 3));

        QVERIFY(KisDustDetector::detect(dev, rect, options(KisDustDetector::DustOnTransparency, 4)).isEmpty());

        const qreal tolerance = KisDustDetector::defaultTolerancePercent / 100;
        const auto found =
            KisDustDetector::detect(dev, rect, options(KisDustDetector::DustOnTransparency, 4, tolerance));
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().bounds, QRect(10, 10, 2, 2));
    }

    void testDustOnWhite()
    {
        const QRect rect(0, 0, 48, 48);
        KisPaintDeviceSP dev = filledDevice(rect, Qt::white);
        fillRect(dev, QRect(5, 5, 2, 2), QColor(80, 80, 80));
        fillRect(dev, QRect(15, 15, 1, 1), QColor(250, 250, 250));
        fillRect(dev, QRect(0, 30, 48, 2), Qt::black);

        auto found = KisDustDetector::detect(dev, rect, options(KisDustDetector::DustOnWhite, 4));
        QCOMPARE(found.size(), 2);

        // Nearly white pixels count as white with a tolerance.
        found = KisDustDetector::detect(dev, rect, options(KisDustDetector::DustOnWhite, 4, 0.1));
        QCOMPARE(found.size(), 1);

        KisDustDetector::apply(dev, found, KisDustDetector::PaintWhite);
        QCOMPARE(pixel(dev, 5, 5), QColor(Qt::white));
        QCOMPARE(pixel(dev, 15, 15), QColor(250, 250, 250));
        QCOMPARE(pixel(dev, 10, 30), QColor(Qt::black));
    }

    void testTransparentGaps()
    {
        const QRect rect(0, 0, 64, 32);
        KisPaintDeviceSP dev = transparentDevice();
        fillRect(dev, QRect(0, 0, 40, 32), Qt::red);
        fillRect(dev, QRect(40, 0, 24, 32), Qt::blue);
        fillRect(dev, QRect(10, 10, 2, 2), Qt::transparent); // hole in red
        fillRect(dev, QRect(20, 20, 1, 1), QColor(255, 0, 0, 100)); // semi-transparent
        fillRect(dev, QRect(38, 5, 2, 3), Qt::transparent); // hole on the border, mostly red
        fillRect(dev, QRect(50, 0, 10, 10), Qt::transparent); // too large

        const auto found = KisDustDetector::detect(dev, rect, options(KisDustDetector::TransparentGaps, 4));
        QCOMPARE(found.size(), 3);

        KisDustDetector::apply(dev, found, KisDustDetector::SurroundingColor);
        QCOMPARE(pixel(dev, 10, 10), QColor(Qt::red));
        QCOMPARE(pixel(dev, 20, 20), QColor(Qt::red));
        QCOMPARE(pixel(dev, 38, 6), QColor(Qt::red));
        QCOMPARE(pixel(dev, 55, 5).alpha(), 0);
    }

    void testFixedColorAndSelection()
    {
        const QRect rect(0, 0, 32, 32);
        KisPaintDeviceSP dev = filledDevice(rect, Qt::red);
        fillRect(dev, QRect(10, 10, 2, 1), Qt::transparent);

        const auto found = KisDustDetector::detect(dev, rect, options(KisDustDetector::TransparentGaps, 4));
        QCOMPARE(found.size(), 1);

        KisPixelSelectionSP selection = new KisPixelSelection();
        KisDustDetector::select(selection, found);
        QCOMPARE(selection->selectedExactRect(), QRect(10, 10, 2, 1));

        KisDustDetector::apply(dev, found, KisDustDetector::FixedColor, color(Qt::green));
        QCOMPARE(pixel(dev, 11, 10), QColor(Qt::green));
    }

    void testEnclosure()
    {
        const QRect rect(0, 0, 32, 32);
        KisPaintDeviceSP dev = transparentDevice();
        fillRect(dev, QRect(4, 4, 2, 2), Qt::black); // inside
        fillRect(dev, QRect(9, 4, 2, 2), Qt::black); // straddles the edge
        fillRect(dev, QRect(20, 20, 2, 2), Qt::black); // outside

        KisPixelSelectionSP enclosure = new KisPixelSelection();
        enclosure->select(QRect(0, 0, 10, 10));
        const auto o = options(KisDustDetector::DustOnTransparency, 4);

        auto found = KisDustDetector::detect(dev, rect, o, enclosure, KisDustDetector::FullyInside);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().bounds, QRect(4, 4, 2, 2));

        found = KisDustDetector::detect(dev, rect, o, enclosure, KisDustDetector::Touching);
        QCOMPARE(found.size(), 2);
    }

    void testTilesMatchWholeArea_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("transparency") << int(KisDustDetector::DustOnTransparency);
        QTest::newRow("white") << int(KisDustDetector::DustOnWhite);
        QTest::newRow("gaps") << int(KisDustDetector::TransparentGaps);
    }

    void testTilesMatchWholeArea()
    {
        QFETCH(int, mode);
        const QRect rect(0, 0, 96, 96);
        const auto o = options(KisDustDetector::Mode(mode), 3);

        QRandomGenerator random(12345);
        KisPaintDeviceSP source =
            mode == KisDustDetector::DustOnTransparency ? transparentDevice() : filledDevice(rect, Qt::white);
        for (int i = 0; i < 300; ++i) {
            const QRect blob(random.bounded(96), random.bounded(96), 1 + random.bounded(5), 1 + random.bounded(5));
            const QColor c = mode == KisDustDetector::TransparentGaps
                ? QColor(0, 0, 0, random.bounded(2) ? 0 : 128)
                : QColor::fromRgb(random.bounded(200), random.bounded(200), random.bounded(200));
            fillRect(source, blob, c);
        }
        const auto replacement = KisDustDetector::defaultReplacement(KisDustDetector::Mode(mode));

        KisPaintDeviceSP whole = new KisPaintDevice(*source);
        KisDustDetector::apply(whole, KisDustDetector::detect(source, rect, o), replacement);

        KisPaintDeviceSP tiled = new KisPaintDevice(*source);
        for (int y = 0; y < rect.height(); y += 17) {
            for (int x = 0; x < rect.width(); x += 23) {
                const QRect tile = QRect(x, y, 23, 17) & rect;
                KisDustDetector::apply(tiled, KisDustDetector::detect(source, tile, o), replacement, KoColor(), tile);
            }
        }

        QVERIFY(bytes(whole, rect) != bytes(source, rect));
        QCOMPARE(bytes(tiled, rect), bytes(whole, rect));
    }

    void testCancellation()
    {
        const QRect rect(0, 0, 256, 256);
        KisPaintDeviceSP dev = transparentDevice();
        fillRect(dev, QRect(5, 5, 1, 1), Qt::black);

        KoDummyUpdaterHolder progress;
        QMetaObject::invokeMethod(progress.updater(), "setInterrupted", Qt::DirectConnection, Q_ARG(bool, true));
        QVERIFY(KisDustDetector::detect(dev, rect, options(KisDustDetector::DustOnTransparency, 4), progress.updater())
                    .isEmpty());
    }
};

SIMPLE_TEST_MAIN(KisDustDetectorTest)
#include "KisDustDetectorTest.moc"
