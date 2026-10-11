/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QImage>
#include <QObject>
#include <QRandomGenerator>
#include <QTest>

#include <KisProceduralCircleDab.h>
#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpaceRegistry.h>
#include <KoStopGradient.h>
#include <brushengine/kis_paint_information.h>
#include <kis_dab_shape.h>
#include <kis_fixed_paint_device.h>
#include <kis_gbr_brush.h>

#include "KisBrushTestMain.h"

/**
 * GPU engine (Solstice, "Image tip dabs"): KisBrush::proceduralImageDab()
 * describes, for every brush application, exactly the RGBA F32 dab the CPU
 * generates (generateMaskAndApplyMaskOrCreateDab() or paintDevice()); the
 * GPU evaluates the same description (KisGpuBrushTest::
 * testGeneratedImageTipDabs).
 */
class KisImageTipDescriptionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testApplications_data();
    void testApplications();
};

namespace
{
QImage tipImage(bool gray)
{
    QImage image(37, 29, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const int value = (x * 37 + y * 91 + x * y * 7) % 256;
            int alpha = (x * 13 + y * 29) % 256;
            if ((x + y) % 11 == 0)
                alpha = 0;
            if ((x * y) % 7 == 3)
                alpha = 255;
            image.setPixel(x,
                           y,
                           gray ? qRgba(value, value, value, alpha)
                                : qRgba(value, (value * 3 + y) % 256, (x * 5) % 256, alpha));
        }
    }
    return image;
}
} // namespace

void KisImageTipDescriptionTest::testApplications_data()
{
    QTest::addColumn<int>("application");
    QTest::addColumn<bool>("gray");
    for (int application : {int(ALPHAMASK), int(IMAGESTAMP), int(LIGHTNESSMAP), int(GRADIENTMAP)}) {
        for (bool gray : {false, true}) {
            QTest::newRow(qPrintable(QStringLiteral("application%1-%2").arg(application).arg(gray ? "gray" : "color")))
                << application << gray;
        }
    }
}

void KisImageTipDescriptionTest::testApplications()
{
    QFETCH(int, application);
    QFETCH(bool, gray);
    const KoColorSpace *cs =
        KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(), Float32BitsColorDepthID.id(), QString());
    QVERIFY(cs);

    KisGbrBrushSP brush(new KisGbrBrush(tipImage(gray), QStringLiteral("description test tip")));
    brush->setValid(true);
    brush->setBrushApplication(enumBrushApplication(application));
    QCOMPARE(int(brush->brushApplication()), application);
    if (application == GRADIENTMAP) {
        QSharedPointer<KoStopGradient> gradient(new KoStopGradient());
        gradient->setStops({KoGradientStop(0.0, KoColor(QColor(20, 40, 200, 255), cs)),
                            KoGradientStop(0.4, KoColor(QColor(250, 120, 30, 90), cs)),
                            KoGradientStop(1.0, KoColor(QColor(240, 240, 10, 255), cs))});
        gradient->setValid(true);
        brush->setGradient(gradient);
    }

    KoColor paintColor(QColor(70, 180, 240), cs);
    const qreal lightnessStrength = 0.7;
    QRandomGenerator random(5);
    int described = 0;
    for (int i = 0; i < 60; ++i) {
        const KisDabShape shape(0.3 + 2.5 * random.generateDouble(),
                                0.5 + random.generateDouble(),
                                i % 3 ? random.generateDouble() * 6 : 0.0);
        const qreal sx = i % 5 ? random.generateDouble() : 0.0;
        const qreal sy = i % 5 ? random.generateDouble() : 0.0;
        const KisPaintInformation info;

        KisProceduralCircleDab description;
        if (!brush->proceduralImageDab(shape, sx, sy, paintColor.data(), cs, lightnessStrength, &description)) {
            continue;
        }
        ++described;

        // the CPU dab, as KisDabCacheUtils::generateDab() makes it
        KisFixedPaintDeviceSP dab = new KisFixedPaintDevice(cs);
        if (application == IMAGESTAMP) {
            dab = brush->paintDevice(cs, shape, info, sx, sy);
        } else {
            brush->mask(dab, paintColor, shape, info, sx, sy, 1.0, lightnessStrength);
        }
        const QRect bounds = dab->bounds();
        QCOMPARE(bounds.size(), description.imageTip->size);

        QVector<float> expected(bounds.width() * bounds.height() * 4);
        description.render(reinterpret_cast<quint8 *>(expected.data()), bounds.width(), bounds.height(), 0);
        const float *actual = reinterpret_cast<const float *>(dab->constData());
        int mismatches = 0;
        for (int p = 0; p < expected.size(); ++p) {
            if (std::memcmp(&expected[p], &actual[p], sizeof(float)) != 0 && !mismatches++) {
                qWarning() << "case" << i << "pixel" << (p / 4) % bounds.width() << (p / 4) / bounds.width()
                           << "channel" << p % 4 << "description" << expected[p] << "CPU" << actual[p];
            }
        }
        QVERIFY2(mismatches == 0, qPrintable(QStringLiteral("case %1: %2 mismatches").arg(i).arg(mismatches)));
    }
    QVERIFY(described >= 50);
}

SOLSTICE_BRUSH_TEST_MAIN(KisImageTipDescriptionTest)

#include "KisImageTipDescriptionTest.moc"
