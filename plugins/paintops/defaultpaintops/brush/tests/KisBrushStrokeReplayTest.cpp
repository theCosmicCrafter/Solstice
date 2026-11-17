/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>
#include <QtMath>

#include "KisBrushTestMain.h"

#include <KisAsynchronousStrokeUpdateHelper.h>
#include <KisGlobalResourcesInterface.h>
#include <KoCanvasResourceProvider.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_information.h>
#include <kis_paint_layer.h>
#include <kis_resources_snapshot.h>
#include <strokes/KisFreehandStrokeInfo.h>
#include <strokes/freehand_stroke.h>

/**
 * Brush Stroke Layer replay feasibility (docs/agent/brush-stroke-layer.md):
 * can a recorded stroke be drawn again, exactly at the same size and sharply
 * at a larger one? The stroke is the point list a freehand tool sends after
 * smoothing; it is drawn through the normal stroke path
 * (FreehandStrokeStrategy) with a fixed random seed.
 */
class KisBrushStrokeReplayTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testReplay_data();
    void testReplay();
};

namespace
{
const QSize canvasSize(160, 64);
constexpr int replaySeed = 314159;

/// An S curve whose pressure rises and falls, sampled every 1.5 px, 4 ms
/// apart: what a freehand tool sends after smoothing
QVector<KisPaintInformation> recordedStroke()
{
    QVector<KisPaintInformation> points;
    const int count = 110;
    for (int i = 0; i < count; i++) {
        const qreal t = qreal(i) / (count - 1);
        const QPointF pos(16 + t * 128, 32 + 18 * qSin(t * 2 * M_PI));
        KisPaintInformation pi(pos, qBound(0.05, qSin(t * M_PI), 1.0));
        pi.setCurrentTime(i * 4);
        points << pi;
    }
    return points;
}

KisPaintOpPresetSP loadPreset(const QString &fileName)
{
    KisPaintOpPresetSP preset = KisGlobalResourcesInterface::instance()
                                    ->source<KisPaintOpPreset>(ResourceType::PaintOpPresets)
                                    .bestMatch(QString(), fileName, QString())
                                    .dynamicCast<KisPaintOpPreset>();
    return preset;
}

/// Draws @p points with @p preset at @p scale times the canvas size: the
/// positions and the brush's absolute sizes are scaled
QImage replay(KisPaintOpPresetSP preset, const QVector<KisPaintInformation> &points, qreal scale, int seed)
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const QSize size = canvasSize * scale;
    KisImageSP image = new KisImage(nullptr, size.width(), size.height(), cs, "replay");
    KisPaintLayerSP layer = new KisPaintLayer(image, "replay", OPACITY_OPAQUE_U8, cs);

    KisPaintOpPresetSP scaled = preset->clone().dynamicCast<KisPaintOpPreset>();
    KisPaintOpSettingsSP settings = scaled->settings();
    settings->setPaintOpSize(settings->paintOpSize() * scale);
    if (settings->hasProperty("Texture/Pattern/Scale")) {
        settings->setProperty("Texture/Pattern/Scale", settings->getDouble("Texture/Pattern/Scale") * scale);
    }

    KoCanvasResourceProvider provider;
    const KoColor black(Qt::black, cs);
    provider.setResource(KoCanvasResource::ForegroundColor, QVariant::fromValue(black));
    provider.setResource(KoCanvasResource::BackgroundColor, QVariant::fromValue(KoColor(Qt::white, cs)));
    provider.setResource(KoCanvasResource::CurrentPaintOpPreset, QVariant::fromValue(scaled));
    provider.setResource(KoCanvasResource::Opacity, 1.0);
    provider.setResource(KoCanvasResource::CurrentCompositeOp, COMPOSITE_OVER);
    provider.setResource(KoCanvasResource::CurrentEffectiveCompositeOp, COMPOSITE_OVER);
    provider.setResource(KoCanvasResource::EffectiveZoom, 1.0);
    provider.setResource(KoCanvasResource::HdrExposure, 0.0);
    provider.setResource(KoCanvasResource::EraserMode, false);
    provider.setResource(KoCanvasResource::GlobalAlphaLock, false);
    provider.setResource(KoCanvasResource::MirrorHorizontal, false);
    provider.setResource(KoCanvasResource::MirrorVertical, false);
    provider.setResource(KoCanvasResource::EffectiveLodAvailability, false);
    provider.setResource(KoCanvasResource::Size, settings->paintOpSize());

    KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, layer, &provider, nullptr, {}, scaled);
    resources->setOpacity(settings->paintOpOpacity());
    resources->setMirroring(false, false);
    resources->setFGColorOverride(black);

    FreehandStrokeStrategy *stroke =
        new FreehandStrokeStrategy(resources, new KisFreehandStrokeInfo(), kundo2_noi18n("replay"));
    stroke->setPreviewRandomSeed(seed);
    KisStrokeId id = image->startStroke(stroke);
    for (int i = 0; i + 1 < points.size(); i++) {
        KisPaintInformation from = points[i];
        KisPaintInformation to = points[i + 1];
        from.setPos(from.pos() * scale);
        to.setPos(to.pos() * scale);
        image->addJob(id, new FreehandStrokeStrategy::Data(0, from, to));
    }
    // the dabs are rendered asynchronously; an update job flushes them, as
    // a freehand tool's update timer does
    image->addJob(id, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
    image->endStroke(id);
    image->waitForDone();

    return layer->paintDevice()->convertToQImage(nullptr, image->bounds()).convertToFormat(QImage::Format_ARGB32);
}

/// Mean and maximum difference of the alpha channels (0-255)
QPair<qreal, int> alphaDifference(const QImage &a, const QImage &b)
{
    qint64 sum = 0;
    int maximum = 0;
    for (int y = 0; y < a.height(); y++) {
        for (int x = 0; x < a.width(); x++) {
            const int d = qAbs(qAlpha(a.pixel(x, y)) - qAlpha(b.pixel(x, y)));
            sum += d;
            maximum = qMax(maximum, d);
        }
    }
    return {qreal(sum) / (a.width() * a.height()), maximum};
}

int coveredPixels(const QImage &image)
{
    int covered = 0;
    for (int y = 0; y < image.height(); y++) {
        for (int x = 0; x < image.width(); x++) {
            covered += qAlpha(image.pixel(x, y)) > 25;
        }
    }
    return covered;
}

/// Edge blur: the share of the stroke's pixels (alpha above 10%) that are
/// half transparent (10% to 90%); a sharp outline has few of them
qreal edgeBlur(const QImage &image)
{
    int covered = 0;
    int partial = 0;
    for (int y = 0; y < image.height(); y++) {
        for (int x = 0; x < image.width(); x++) {
            const int alpha = qAlpha(image.pixel(x, y));
            if (alpha > 25) {
                covered++;
                if (alpha < 230) {
                    partial++;
                }
            }
        }
    }
    return covered ? qreal(partial) / covered : 0.0;
}
} // namespace

void KisBrushStrokeReplayTest::testReplay_data()
{
    QTest::addColumn<QString>("preset");
    QTest::newRow("auto tip, pressure size") << QStringLiteral("b)_Basic-5_Size_default.kpp");
    QTest::newRow("ink fineliner") << QStringLiteral("d)_Ink-2_Fineliner.kpp");
    QTest::newRow("ink G-pen") << QStringLiteral("d)_Ink-3_Gpen.kpp");
    QTest::newRow("pencil, textured") << QStringLiteral("c)_Pencil-2.kpp");
    QTest::newRow("image tip, rough ink") << QStringLiteral("d)_Ink-7_Brush_Rough.kpp");
}

void KisBrushStrokeReplayTest::testReplay()
{
    QFETCH(QString, preset);
    KisPaintOpPresetSP brush = loadPreset(preset);
    QVERIFY2(brush, qPrintable(preset));
    const QVector<KisPaintInformation> stroke = recordedStroke();

    // 1. the same stroke and seed give the same pixels
    const QImage first = replay(brush, stroke, 1.0, replaySeed);
    const QImage second = replay(brush, stroke, 1.0, replaySeed);
    QVERIFY2(coveredPixels(first) > 200, "nothing drawn");
    QCOMPARE(first, second);

    // 2. drawn at 4x and reduced to 1x, it matches the 1x stroke
    const QImage large = replay(brush, stroke, 4.0, replaySeed);
    const QImage reduced = large.scaled(canvasSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QPair<qreal, int> difference = alphaDifference(first, reduced);

    // 3. drawn at 4x, its outline is sharper than the 1x stroke enlarged
    const QImage enlarged = first.scaled(canvasSize * 4, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const qreal blurLarge = edgeBlur(large);
    const qreal blurEnlarged = edgeBlur(enlarged);

    qInfo().noquote() << QStringLiteral("%1: 4x reduced vs 1x alpha mean %2 max %3; edge blur 4x %4, 1x enlarged %5")
                             .arg(preset)
                             .arg(difference.first, 0, 'f', 2)
                             .arg(difference.second)
                             .arg(blurLarge, 0, 'f', 3)
                             .arg(blurEnlarged, 0, 'f', 3);
    if (qEnvironmentVariableIsSet("SOLSTICE_REPLAY_DUMP")) {
        const QString base = QString::fromLocal8Bit(qgetenv("SOLSTICE_REPLAY_DUMP")) + QLatin1Char('/')
            + QString(preset).remove(QLatin1Char(')')).remove(QStringLiteral(".kpp"));
        first.save(base + QStringLiteral("_1x.png"));
        large.save(base + QStringLiteral("_4x.png"));
        enlarged.save(base + QStringLiteral("_1x_enlarged.png"));
    }

    QVERIFY2(difference.first < 6.0, "the 4x stroke reduced differs from the 1x stroke");
    QVERIFY2(blurLarge < blurEnlarged, "the 4x stroke is not sharper than the enlarged 1x stroke");
}

SOLSTICE_BRUSH_TEST_MAIN(KisBrushStrokeReplayTest)

#include "KisBrushStrokeReplayTest.moc"
