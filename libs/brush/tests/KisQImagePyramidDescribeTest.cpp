/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QRandomGenerator>
#include <QTest>

#include <cmath>

#include "kis_qimage_pyramid.h"

/**
 * GPU engine (Solstice, "Image tip dabs"): KisQImagePyramid::createImage()
 * draws described transforms with renderDescribed() instead of QPainter.
 * The result must stay within one 8-bit step of QPainter's per channel
 * (rounding of Qt's SIMD conversions), cover the same pixels, and be what
 * describedPixel() reports.
 */
class KisQImagePyramidDescribeTest : public QObject
{
    Q_OBJECT
private:
    static QImage testImage(int width, int height, bool gray)
    {
        QImage image(width, height, QImage::Format_ARGB32);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int v = (x * 37 + y * 91 + x * y * 7) % 256;
                int a = (x * 13 + y * 29) % 256;
                if ((x + y) % 11 == 0)
                    a = 0;
                if ((x * y) % 7 == 3)
                    a = 255;
                image.setPixel(x, y, gray ? qRgba(v, v, v, a) : qRgba(v, (v * 3 + y) % 256, (x * 5) % 256, a));
            }
        }
        return image;
    }

    struct Stats {
        int described = 0;
        int maxDifference = 0;
        qint64 differentPixels = 0;
        qint64 coverageMismatches = 0;
        qint64 pixels = 0;
    };

    static void compare(const QImage &expected, const QImage &actual, Stats &stats)
    {
        QCOMPARE(actual.size(), expected.size());
        for (int y = 0; y < expected.height(); ++y) {
            for (int x = 0; x < expected.width(); ++x) {
                const QRgb e = expected.pixel(x, y);
                const QRgb a = actual.pixel(x, y);
                ++stats.pixels;
                if (e == a)
                    continue;
                ++stats.differentPixels;
                if ((e == 0) != (a == 0))
                    ++stats.coverageMismatches;
                // What an alpha-mask brush takes from the pixel
                // (fillGrayBrushWithColor(): UINT8_MULT(255 - red, alpha)),
                // and the alpha. Unpremultiplied colors of nearly
                // transparent pixels amplify rounding differences.
                auto mask = [](QRgb p) {
                    const int t = (255 - qRed(p)) * qAlpha(p) + 0x80;
                    return ((t >> 8) + t) >> 8;
                };
                const int d = qMax(qAbs(mask(e) - mask(a)), qAbs(qAlpha(e) - qAlpha(a)));
                stats.maxDifference = qMax(stats.maxDifference, d);
            }
        }
    }

    static Stats compareMany(const QImage &base, int cases, quint32 seed, bool rotate)
    {
        KisQImagePyramid pyramid(base);
        QRandomGenerator random(seed);
        Stats stats;
        for (int i = 0; i < cases; ++i) {
            qreal scale = std::exp(std::log(0.02) + random.generateDouble() * (std::log(3.0) - std::log(0.02)));
            qreal ratio = 0.4 + random.generateDouble() * 2.1;
            qreal subX = random.generateDouble();
            qreal subY = random.generateDouble();
            const qreal rotation = rotate ? random.generateDouble() * 2 * M_PI : 0.0;
            if (i % 10 == 0 && !rotate) {
                // exact pyramid level scales without subpixel offset (identity)
                scale = std::ldexp(1.0, -(i / 10) % 4);
                ratio = 1.0;
                subX = subY = 0.0;
            } else if (i % 10 == 1) {
                subX = subY = 0.0;
            }
            const KisDabShape shape(scale, ratio, rotation);
            KisQImagePyramid::DabGeometry geometry;
            if (!pyramid.describeImage(shape, subX, subY, &geometry))
                continue;
            ++stats.described;
            const QImage actual = pyramid.createImage(shape, subX, subY);
            const QImage reference = KisQImagePyramid::renderDescribed(geometry);
            if (actual != reference) {
                qWarning() << "createImage() differs from renderDescribed()";
                stats.maxDifference = 1000;
            }
            compare(pyramid.createImageWithQPainter(shape, subX, subY).convertToFormat(QImage::Format_ARGB32),
                    actual,
                    stats);
        }
        qInfo() << (rotate ? "rotated" : "scaled") << "described" << stats.described << "of" << cases
                << "max channel difference" << stats.maxDifference << "different pixels" << stats.differentPixels
                << "of" << stats.pixels << "coverage mismatches" << stats.coverageMismatches;
        return stats;
    }

private Q_SLOTS:
    void testScaledTips()
    {
        for (const QImage &image : {testImage(47, 31, false), testImage(64, 64, true), testImage(300, 220, true)}) {
            const Stats stats = compareMany(image, image.width() > 100 ? 400 : 2000, 7, false);
            QCOMPARE(stats.described, image.width() > 100 ? 400 : 2000);
            QVERIFY(stats.maxDifference <= 1);
            QCOMPARE(stats.coverageMismatches, qint64(0));
        }
    }
    void testRotatedTips()
    {
        for (const QImage &image : {testImage(47, 31, false), testImage(64, 64, true), testImage(300, 220, true)}) {
            const Stats stats = compareMany(image, image.width() > 100 ? 300 : 1500, 11, true);
            QVERIFY(stats.described > 0);
            QVERIFY2(stats.maxDifference <= 2, qPrintable(QString::number(stats.maxDifference)));
        }
    }
    void benchmarkCreateImage()
    {
        // an opaque gray tip, like GBR/ABR masks after appendPyramidLevel()
        QImage opaque(256, 256, QImage::Format_ARGB32);
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) {
                const int v = (x * 37 + y * 91) % 256;
                opaque.setPixel(x, y, qRgb(v, v, v));
            }
        for (const QImage &tip : {testImage(256, 256, true), opaque}) {
            KisQImagePyramid pyramid(tip);
            qInfo() << (tip.hasAlphaChannel() && tip.pixel(1, 1) != opaque.pixel(1, 1) ? "varying alpha tip"
                                                                                       : "opaque tip");
            for (const bool rotate : {false, true}) {
                QElapsedTimer timer;
                for (const bool painter : {true, false}) {
                    QRandomGenerator random(3);
                    timer.start();
                    qint64 checksum = 0;
                    for (int i = 0; i < 400; ++i) {
                        const KisDabShape shape(0.3 + random.generateDouble(),
                                                1.0,
                                                rotate ? random.generateDouble() * 6 : 0.0);
                        const qreal sx = random.generateDouble();
                        const qreal sy = random.generateDouble();
                        const QImage image = painter ? pyramid.createImageWithQPainter(shape, sx, sy)
                                                     : pyramid.createImage(shape, sx, sy);
                        checksum += image.width();
                    }
                    qInfo() << (rotate ? "rotated" : "scaled") << (painter ? "QPainter" : "described")
                            << "ms per 400 dabs" << timer.elapsed() << checksum;
                }
            }
        }
    }
};

QTEST_MAIN(KisQImagePyramidDescribeTest)
#include "KisQImagePyramidDescribeTest.moc"
