/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>
#include <QtMath>

#include "KisBrushTestMain.h"

#include <QBuffer>
#include <QTemporaryDir>
#include <QThread>

#include <KisAsynchronousStrokeUpdateHelper.h>
#include <KisBrushStrokeLayer.h>
#include <KisDocument.h>
#include <KisGlobalResourcesInterface.h>
#include <KisLocalStrokeResources.h>
#include <KisPart.h>
#include <KisResourceModel.h>
#include <KoCanvasResourceProvider.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_abr_brush.h>
#include <kis_command_utils.h>
#include <kis_distance_information.h>
#include <kis_filter_strategy.h>
#include <kis_image.h>
#include <kis_layer_utils.h>
#include <kis_paint_device.h>
#include <kis_paint_information.h>
#include <kis_painter.h>
#include <kis_pixel_selection.h>
#include <kis_resources_snapshot.h>
#include <kis_selection.h>
#include <kis_stroke_strategy_undo_command_based.h>
#include <kis_transaction.h>
#include <kis_undo_stores.h>
#include <strokes/KisFreehandStrokeInfo.h>
#include <strokes/freehand_stroke.h>

/**
 * Brush Stroke Layer recording and replay (docs/agent/brush-stroke-layer.md):
 * a stroke painted on a KisBrushStrokeLayer through the brush tool's stroke
 * path is recorded, its record draws the same pixels again, and undo and
 * redo keep the record and the pixels together.
 */
class KisBrushStrokeLayerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testRecordAndRedraw_data();
    void testRecordAndRedraw();
    void testOnlyBrushStrokesOnPaintLayers();
    void testScaleImageRedraws();
    void testScaleRedrawsAroundOtherEdits();
    void testScaleFallsBackAfterOtherEdits();
    void testStrokeDataRoundTrip();
    void testKraRoundTrip();
    void testSmudgeOverStroke();
    void testTransparentColorsIgnored();
    void testBrushTipsSaved();
    void testRandomizedBrushesRedraw();
    void testStrokeStartRecorded();
    void testAbrTipSaved();
    void testSelectionMoveMovesStrokes();
    void testSelectionDuplicateCopiesStrokes();
    void testRecordCheckFindsOtherEdits();
    void testTextureFollowsMovedStrokes();
    void testScaleKeepsDeletedEdges();
    void testScaleKeepsHdrEdits_data();
    void testScaleKeepsHdrEdits();
    void testRejectInvalidStrokeJobs_data();
    void testRejectInvalidStrokeJobs();
    void testManyCopiesLoad();
    void testRejectDamagedStrokeData_data();
    void testRejectDamagedStrokeData();
    void testMirroredStrokeRecorded();
    void testRecordCheckFindsEditsBesideStrokes();
    void testRecordCheckAfterLayerReturns();
    void testFloatGrayEditsDetected();
    void testDeviceOffsetReadAtStart();
};

namespace
{
const QRect imageBounds(0, 0, 200, 100);

KisPaintOpPresetSP loadPreset(const QString &fileName)
{
    return KisGlobalResourcesInterface::instance()
        ->source<KisPaintOpPreset>(ResourceType::PaintOpPresets)
        .bestMatch(QString(), fileName, QString())
        .dynamicCast<KisPaintOpPreset>();
}

/// A stroke as the brush tool sends it: a point, lines with rising and
/// falling pressure, and a curve
void paintStroke(KisImageSP image,
                 KisNodeSP node,
                 KisPaintOpPresetSP preset,
                 const KoColor &color,
                 const KisDistanceInformation *start = nullptr,
                 const QPointF &shift = QPointF(),
                 bool mirror = false)
{
    KoCanvasResourceProvider provider;
    provider.setResource(KoCanvasResource::ForegroundColor, QVariant::fromValue(color));
    provider.setResource(KoCanvasResource::BackgroundColor,
                         QVariant::fromValue(KoColor(Qt::white, image->colorSpace())));
    provider.setResource(KoCanvasResource::CurrentPaintOpPreset, QVariant::fromValue(preset));
    provider.setResource(KoCanvasResource::Opacity, 1.0);
    provider.setResource(KoCanvasResource::CurrentCompositeOp, COMPOSITE_OVER);
    provider.setResource(KoCanvasResource::CurrentEffectiveCompositeOp, COMPOSITE_OVER);
    provider.setResource(KoCanvasResource::EffectiveZoom, 1.0);
    provider.setResource(KoCanvasResource::HdrExposure, 0.0);
    provider.setResource(KoCanvasResource::EraserMode, false);
    provider.setResource(KoCanvasResource::GlobalAlphaLock, false);
    provider.setResource(KoCanvasResource::MirrorHorizontal, mirror);
    provider.setResource(KoCanvasResource::MirrorVertical, mirror);
    provider.setResource(KoCanvasResource::EffectiveLodAvailability, false);
    provider.setResource(KoCanvasResource::Size, preset->settings()->paintOpSize());

    // the canvas mirror mirrors around the image's mirror axes (its centre)
    KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, node, &provider, nullptr, {}, preset);
    resources->setOpacity(1.0);
    resources->setMirroring(mirror, mirror);
    resources->setFGColorOverride(color);

    FreehandStrokeStrategy *stroke =
        new FreehandStrokeStrategy(resources,
                                   start ? new KisFreehandStrokeInfo(*start) : new KisFreehandStrokeInfo(),
                                   kundo2_noi18n("test stroke"));
    KisStrokeId id = image->startStroke(stroke);

    KisPaintInformation previous(QPointF(20, 50) + shift, 0.2);
    image->addJob(id, new FreehandStrokeStrategy::Data(0, previous));
    for (int i = 1; i <= 60; i++) {
        const qreal t = i / 60.0;
        KisPaintInformation next(QPointF(20 + t * 110, 50 + 30 * qSin(t * 2 * M_PI)) + shift, qSin(t * M_PI) + 0.05);
        next.setCurrentTime(i * 4);
        image->addJob(id, new FreehandStrokeStrategy::Data(0, previous, next));
        previous = next;
    }
    KisPaintInformation end(QPointF(180, 30) + shift, 0.5);
    end.setCurrentTime(300);
    image->addJob(
        id,
        new FreehandStrokeStrategy::Data(0, previous, QPointF(150, 90) + shift, QPointF(170, 10) + shift, end));
    image->addJob(id, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
    image->endStroke(id);
    image->waitForDone();
}

QImage toImage(KisPaintDeviceSP device)
{
    return device->convertToQImage(nullptr, imageBounds);
}

QImage toImage(KisPaintDeviceSP device, const QRect &rect)
{
    return device->convertToQImage(nullptr, rect);
}

/// The share of the stroke's pixels that are half transparent
qreal edgeBlur(const QImage &image)
{
    int covered = 0;
    int partial = 0;
    for (int y = 0; y < image.height(); y++) {
        for (int x = 0; x < image.width(); x++) {
            const int alpha = qAlpha(image.pixel(x, y));
            if (alpha > 25) {
                covered++;
                partial += alpha < 230;
            }
        }
    }
    return covered ? qreal(partial) / covered : 0.0;
}

void scaleImage4x(KisImageSP image)
{
    image->scaleImage(QSize(imageBounds.width() * 4, imageBounds.height() * 4),
                      image->xRes(),
                      image->yRes(),
                      KisFilterStrategyRegistry::instance()->value(QStringLiteral("Bicubic")));
    image->waitForDone();
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
} // namespace

void KisBrushStrokeLayerTest::testRecordAndRedraw_data()
{
    QTest::addColumn<QString>("preset");
    QTest::newRow("ink, build up") << QStringLiteral("d)_Ink-3_Gpen.kpp");
    QTest::newRow("pressure size, wash") << QStringLiteral("b)_Basic-5_Size_default.kpp");
}

void KisBrushStrokeLayerTest::testRecordAndRedraw()
{
    QFETCH(QString, preset);
    KisPaintOpPresetSP brush = loadPreset(preset);
    QVERIFY(brush);

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisSurrogateUndoStore *undoStore = new KisSurrogateUndoStore();
    KisImageSP image = new KisImage(undoStore, imageBounds.width(), imageBounds.height(), cs, "brush stroke layer");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());

    paintStroke(image, layer, brush, KoColor(Qt::black, cs));
    paintStroke(image, layer, brush, KoColor(Qt::red, cs));
    const QImage painted = toImage(layer->paintDevice());
    QVERIFY(coveredPixels(painted) > 300);

    // recorded: two strokes, with their jobs and seeds
    QCOMPARE(layer->strokes().size(), 2);
    QCOMPARE(layer->strokes().first()->jobs.size(), 62);
    QVERIFY(layer->strokes().first()->seed != 0);
    QVERIFY(layer->strokes().first()->seed != layer->strokes().last()->seed);

    // the record draws the same pixels again
    auto redraw = [&]() {
        const QPoint offset(layer->paintDevice()->x(), layer->paintDevice()->y());
        return toImage(KisBrushStrokeLayer::renderStrokes(layer->strokes(), cs, imageBounds, offset));
    };
    QCOMPARE(redraw(), painted);

    // moving the layer moves the strokes with it
    layer->paintDevice()->moveTo(QPoint(7, -3));
    QCOMPARE(redraw(), toImage(layer->paintDevice()));
    layer->paintDevice()->moveTo(QPoint(0, 0));

    // undo removes the last stroke and its pixels together; redo restores both
    undoStore->undo();
    image->waitForDone();
    QCOMPARE(layer->strokes().size(), 1);
    QCOMPARE(redraw(), toImage(layer->paintDevice()));
    undoStore->redo();
    image->waitForDone();
    QCOMPARE(layer->strokes().size(), 2);
    QCOMPARE(toImage(layer->paintDevice()), painted);
}

/// A plain paint layer records nothing; the strokes are its own.
void KisBrushStrokeLayerTest::testOnlyBrushStrokesOnPaintLayers()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(nullptr, imageBounds.width(), imageBounds.height(), cs, "paint layer");
    KisPaintLayerSP paint = new KisPaintLayer(image, "paint", OPACITY_OPAQUE_U8, cs);
    image->addNode(paint, image->root());
    KisBrushStrokeLayerSP strokes = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(strokes, image->root());

    paintStroke(image, paint, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")), KoColor(Qt::black, cs));
    QVERIFY(strokes->strokes().isEmpty());

    // a duplicate keeps the record
    paintStroke(image, strokes, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")), KoColor(Qt::black, cs));
    KisNodeSP copy = strokes->clone();
    QVERIFY(copy->inherits("KisBrushStrokeLayer"));
    QCOMPARE(dynamic_cast<KisBrushStrokeLayer *>(copy.data())->strokes().size(), 1);
}

/// Stage 2b: scaling the image draws the strokes again at the new size;
/// undo returns to the small strokes and pixels.
void KisBrushStrokeLayerTest::testScaleImageRedraws()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisSurrogateUndoStore *undoStore = new KisSurrogateUndoStore();
    KisImageSP image = new KisImage(undoStore, imageBounds.width(), imageBounds.height(), cs, "scale");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    KisPaintLayerSP pixels = new KisPaintLayer(image, "pixels", OPACITY_OPAQUE_U8, cs);
    image->addNode(pixels, image->root());

    KisPaintOpPresetSP brush = loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp"));
    paintStroke(image, layer, brush, KoColor(Qt::black, cs));
    paintStroke(image, pixels, brush, KoColor(Qt::black, cs));
    const QImage small = toImage(layer->paintDevice());
    const QPointF firstPoint = layer->strokes().first()->jobs.first().pi1.pos();

    scaleImage4x(image);
    const QRect large(0, 0, imageBounds.width() * 4, imageBounds.height() * 4);
    QCOMPARE(image->bounds(), large);

    // the record follows the scale and is what the layer shows
    QCOMPARE(layer->strokes().size(), 1);
    QCOMPARE(layer->strokes().first()->jobs.first().pi1.pos(), firstPoint * 4);
    const QPoint offset(layer->paintDevice()->x(), layer->paintDevice()->y());
    QCOMPARE(toImage(KisBrushStrokeLayer::renderStrokes(layer->strokes(), cs, large, offset), large),
             toImage(layer->paintDevice(), large));

    // drawn again, it is sharper than the paint layer's resampled pixels
    const qreal redrawnBlur = edgeBlur(toImage(layer->paintDevice(), large));
    const qreal resampledBlur = edgeBlur(toImage(pixels->paintDevice(), large));
    qInfo() << "edge blur: redrawn" << redrawnBlur << "resampled" << resampledBlur;
    QVERIFY(redrawnBlur < resampledBlur * 0.5);

    // undo restores the small strokes and pixels
    undoStore->undo();
    image->waitForDone();
    QCOMPARE(image->bounds(), imageBounds);
    QCOMPARE(layer->strokes().first()->jobs.first().pi1.pos(), firstPoint);
    QCOMPARE(toImage(layer->paintDevice()), small);
}

/// Pixels that are not recorded strokes are never dropped: where the layer
/// is not its record, its pixels are scaled as on a paint layer, and the
/// rest is drawn again
void KisBrushStrokeLayerTest::testScaleRedrawsAroundOtherEdits()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image =
        new KisImage(new KisSurrogateUndoStore(), imageBounds.width(), imageBounds.height(), cs, "partial");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());

    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")), KoColor(Qt::black, cs));
    // an edit outside the brush tool (as a filter or a fill would make)
    layer->paintDevice()->fill(QRect(150, 70, 30, 20), KoColor(Qt::blue, cs));
    const QPointF firstPoint = layer->strokes().first()->jobs.first().pi1.pos();

    KisPaintLayerSP reference = new KisPaintLayer(image, "reference", OPACITY_OPAQUE_U8, cs);
    reference->paintDevice()->makeCloneFrom(layer->paintDevice(), layer->paintDevice()->extent());
    image->addNode(reference, image->root());

    scaleImage4x(image);
    const QRect large(0, 0, imageBounds.width() * 4, imageBounds.height() * 4);
    // the record follows the scale
    QCOMPARE(layer->strokes().first()->jobs.first().pi1.pos(), firstPoint * 4);
    // the fill keeps its scaled pixels
    const QRect fill(600, 280, 120, 80);
    QCOMPARE(toImage(layer->paintDevice(), kisGrowRect(fill, -8)),
             toImage(reference->paintDevice(), kisGrowRect(fill, -8)));
    // away from it, the strokes are drawn again
    const QRect away(0, 0, 400, 400);
    const QImage redrawn = toImage(KisBrushStrokeLayer::renderStrokes(layer->strokes(), cs, large, QPoint()), away);
    QCOMPARE(toImage(layer->paintDevice(), away), redrawn);
    QVERIFY(toImage(layer->paintDevice(), away) != toImage(reference->paintDevice(), away));
}

/// When most of the layer is not its record, it is scaled like a paint
/// layer, and its record is left as it was
void KisBrushStrokeLayerTest::testScaleFallsBackAfterOtherEdits()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image =
        new KisImage(new KisSurrogateUndoStore(), imageBounds.width(), imageBounds.height(), cs, "fallback");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());

    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")), KoColor(Qt::black, cs));
    layer->paintDevice()->fill(QRect(10, 10, 180, 80), KoColor(Qt::blue, cs));
    const QPointF firstPoint = layer->strokes().first()->jobs.first().pi1.pos();

    KisPaintLayerSP reference = new KisPaintLayer(image, "reference", OPACITY_OPAQUE_U8, cs);
    reference->paintDevice()->makeCloneFrom(layer->paintDevice(), layer->paintDevice()->extent());
    image->addNode(reference, image->root());

    scaleImage4x(image);
    const QRect large(0, 0, imageBounds.width() * 4, imageBounds.height() * 4);
    QCOMPARE(layer->strokes().first()->jobs.first().pi1.pos(), firstPoint);
    QCOMPARE(toImage(layer->paintDevice(), large), toImage(reference->paintDevice(), large));
}

/// Stage 2c: the stroke data written for a .kra reads back as strokes that
/// draw the same pixels, with patterns carried in the data
void KisBrushStrokeLayerTest::testStrokeDataRoundTrip()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(new KisSurrogateUndoStore(), imageBounds.width(), imageBounds.height(), cs, "data");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    KisPaintOpPresetSP brush = loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp"));
    paintStroke(image, layer, brush, KoColor(Qt::black, cs));
    paintStroke(image, layer, brush, KoColor(QColor(200, 30, 60), cs));

    QVector<KisRecordedBrushStrokeSP> strokes = layer->strokes();
    QCOMPARE(strokes.size(), 2);
    // a pattern travels with the data
    KoPatternSP pattern = KisGlobalResourcesInterface::instance()
                              ->source<KoPattern>(ResourceType::Patterns)
                              .fallbackResource()
                              .dynamicCast<KoPattern>();
    QVERIFY(pattern);
    QSharedPointer<KisRecordedBrushStroke> withPattern(new KisRecordedBrushStroke(*strokes[1]));
    withPattern->pattern = pattern;
    strokes[1] = withPattern;

    QBuffer buffer;
    buffer.open(QBuffer::WriteOnly);
    QVERIFY(KisBrushStrokeLayer::saveStrokes(strokes, &buffer));
    qInfo() << "stroke data:" << buffer.data().size() << "bytes for"
            << strokes[0]->jobs.size() + strokes[1]->jobs.size() << "jobs";
    buffer.close();

    buffer.open(QBuffer::ReadOnly);
    QVector<KisRecordedBrushStrokeSP> loaded;
    QVERIFY(KisBrushStrokeLayer::loadStrokes(&buffer, &loaded));
    QCOMPARE(loaded.size(), 2);
    for (int i = 0; i < 2; i++) {
        QCOMPARE(loaded[i]->seed, strokes[i]->seed);
        QCOMPARE(loaded[i]->jobs.size(), strokes[i]->jobs.size());
        QCOMPARE(loaded[i]->fgColor, strokes[i]->fgColor);
        QCOMPARE(loaded[i]->compositeOpId, strokes[i]->compositeOpId);
        QCOMPARE(loaded[i]->deviceOffset, strokes[i]->deviceOffset);
        QCOMPARE(loaded[i]->preset->paintOp(), brush->paintOp());
    }
    // the two strokes share one stored preset
    QCOMPARE(loaded[0]->preset, loaded[1]->preset);
    QVERIFY(!loaded[0]->pattern);
    QVERIFY(loaded[1]->pattern);
    QCOMPARE(loaded[1]->pattern->pattern(), pattern->pattern());

    const QPoint offset(layer->paintDevice()->x(), layer->paintDevice()->y());
    QCOMPARE(toImage(KisBrushStrokeLayer::renderStrokes(loaded, cs, imageBounds, offset)),
             toImage(layer->paintDevice()));

    // damaged data is refused, not half read
    QByteArray damaged = buffer.data().left(buffer.data().size() / 2);
    QBuffer damagedBuffer(&damaged);
    damagedBuffer.open(QBuffer::ReadOnly);
    QVector<KisRecordedBrushStrokeSP> none;
    QVERIFY(!KisBrushStrokeLayer::loadStrokes(&damagedBuffer, &none));
    QVERIFY(none.isEmpty());
}

/// Stage 2c: a brush stroke layer saved in a .kra opens as a brush stroke
/// layer with its strokes, and enlarging it then draws them again
void KisBrushStrokeLayerTest::testKraRoundTrip()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("brush_stroke_layer.kra"));

    QImage pixels;
    QPointF firstPoint;
    {
        QScopedPointer<KisDocument> document(KisPart::instance()->createDocument());
        document->setFileBatchMode(true);
        KisImageSP image =
            new KisImage(document->createUndoStore(), imageBounds.width(), imageBounds.height(), cs, "kra");
        KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
        image->addNode(layer, image->root());
        KisPaintLayerSP plain = new KisPaintLayer(image, "plain", OPACITY_OPAQUE_U8, cs);
        image->addNode(plain, image->root());
        paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")), KoColor(Qt::black, cs));
        pixels = toImage(layer->paintDevice());
        firstPoint = layer->strokes().first()->jobs.first().pi1.pos();

        document->setCurrentImage(image);
        image->waitForDone();
        QVERIFY(document->exportDocumentSync(path, KisDocument::nativeFormatMimeType()));
    }

    QScopedPointer<KisDocument> document(KisPart::instance()->createDocument());
    document->setFileBatchMode(true);
    QVERIFY(document->loadNativeFormat(path));
    KisImageSP image = document->image();
    image->waitForDone();

    KisNodeSP strokesNode = KisLayerUtils::findNodeByName(image->root(), "strokes");
    KisBrushStrokeLayerSP layer = dynamic_cast<KisBrushStrokeLayer *>(strokesNode.data());
    QVERIFY(layer);
    QVERIFY(!dynamic_cast<KisBrushStrokeLayer *>(KisLayerUtils::findNodeByName(image->root(), "plain").data()));
    QCOMPARE(layer->strokes().size(), 1);
    QCOMPARE(toImage(layer->paintDevice()), pixels);

    scaleImage4x(image);
    QCOMPARE(layer->strokes().first()->jobs.first().pi1.pos(), firstPoint * 4);
}

/// A smudging (blur) stroke reads the strokes below it; drawn again, in a
/// canvas just around the strokes as the redraw uses, it gives the same
/// pixels, and scaling then draws it again
void KisBrushStrokeLayerTest::testSmudgeOverStroke()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(new KisSurrogateUndoStore(), 600, 400, cs, "smudge");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    KisPaintOpPresetSP blur = loadPreset(QStringLiteral("k)_Blender_Blur.kpp"));
    QVERIFY(blur);
    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")), KoColor(Qt::black, cs));
    paintStroke(image, layer, blur, KoColor(Qt::black, cs));
    QCOMPARE(layer->strokes().size(), 2);

    // compared premultiplied: smudging leaves transparent pixels with
    // colors that differ from the redraw, which do not show
    const QRect content = layer->paintDevice()->exactBounds();
    QCOMPARE(toImage(KisBrushStrokeLayer::renderStrokes(layer->strokes(), cs, content | QRect(0, 0, 1, 1), QPoint()),
                     content)
                 .convertToFormat(QImage::Format_ARGB32_Premultiplied),
             toImage(layer->paintDevice(), content).convertToFormat(QImage::Format_ARGB32_Premultiplied));

    const QPointF firstPoint = layer->strokes().last()->jobs.first().pi1.pos();
    image->scaleImage(QSize(2400, 1600),
                      image->xRes(),
                      image->yRes(),
                      KisFilterStrategyRegistry::instance()->value(QStringLiteral("Bicubic")));
    image->waitForDone();
    QCOMPARE(layer->strokes().last()->jobs.first().pi1.pos(), firstPoint * 4);
}

/// The color of a transparent pixel does not show: a smudging brush can
/// leave it different from what drawing again gives (seen with a blur brush
/// in an RGBA float document), and that must not stop the redraw
void KisBrushStrokeLayerTest::testTransparentColorsIgnored()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image =
        new KisImage(new KisSurrogateUndoStore(), imageBounds.width(), imageBounds.height(), cs, "alpha");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")), KoColor(Qt::black, cs));
    const QPointF firstPoint = layer->strokes().first()->jobs.first().pi1.pos();

    KoColor transparentWhite(Qt::white, cs);
    transparentWhite.setOpacity(OPACITY_TRANSPARENT_U8);
    layer->paintDevice()->fill(QRect(150, 70, 30, 20), transparentWhite);

    scaleImage4x(image);
    QCOMPARE(layer->strokes().first()->jobs.first().pi1.pos(), firstPoint * 4);
}

/// The brush tip images of the strokes' presets are saved with the strokes,
/// and the loaded strokes draw with the saved copies, so a file redraws
/// where the brush tips are not installed
void KisBrushStrokeLayerTest::testBrushTipsSaved()
{
    // a Pixel Brush preset with an image brush tip
    KisPaintOpPresetSP brush;
    KisResourceModel model(ResourceType::PaintOpPresets);
    for (int i = 0; i < model.rowCount() && !brush; i++) {
        KisPaintOpPresetSP preset = model.resourceForIndex(model.index(i, 0)).dynamicCast<KisPaintOpPreset>();
        if (preset && preset->paintOp().id() == "paintbrush") {
            const QString definition = preset->settings()->getString("brush_definition");
            if (definition.contains("gbr_brush") || definition.contains("png_brush")) {
                brush = preset;
            }
        }
    }
    QVERIFY(brush);
    qInfo() << "brush with an image tip:" << brush->name();

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(new KisSurrogateUndoStore(), imageBounds.width(), imageBounds.height(), cs, "tips");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image, layer, brush, KoColor(Qt::black, cs));

    QBuffer buffer;
    buffer.open(QBuffer::WriteOnly);
    QVERIFY(KisBrushStrokeLayer::saveStrokes(layer->strokes(), &buffer));
    buffer.close();
    buffer.open(QBuffer::ReadOnly);
    QVector<KisRecordedBrushStrokeSP> loaded;
    QVERIFY(KisBrushStrokeLayer::loadStrokes(&buffer, &loaded));
    QCOMPARE(loaded.size(), 1);

    // the loaded preset's brush tip is the saved copy, not the installed one
    const KisPaintOpPresetSP preset = loaded.first()->preset;
    int tips = 0;
    for (const KoResourceLoadResult &link : preset->linkedResources(preset->resourcesInterface())) {
        KoResourceSP tip = link.resource();
        QVERIFY(tip);
        if (tip->resourceType().first != ResourceType::Brushes) {
            continue;
        }
        tips++;
        KoResourceSP installed = KisGlobalResourcesInterface::instance()
                                     ->source(ResourceType::Brushes)
                                     .bestMatch(tip->md5Sum(), tip->filename(), tip->name());
        QVERIFY(installed);
        QVERIFY(tip.data() != installed.data());
        QCOMPARE(tip->md5Sum(), installed->md5Sum());
    }
    QVERIFY(tips > 0);

    // and draws the same pixels
    QCOMPARE(toImage(KisBrushStrokeLayer::renderStrokes(loaded, cs, imageBounds, QPoint()))
                 .convertToFormat(QImage::Format_ARGB32_Premultiplied),
             toImage(layer->paintDevice()).convertToFormat(QImage::Format_ARGB32_Premultiplied));
}

/// Brushes whose dabs would differ with the timing of the threads that
/// render them (an image pipe tip or masking brush choosing its images at
/// random, textures) still draw the same pixels again: a recorded stroke and
/// its redraw render their dabs one after another
/// (KisPainterBasedStrokeStrategy::setSequentialDabRendering())
void KisBrushStrokeLayerTest::testRandomizedBrushesRedraw()
{
    KisResourceModel model(ResourceType::PaintOpPresets);
    int tried = 0;
    int pipes = 0;
    for (int i = 0; i < model.rowCount() && tried < 16; i++) {
        KisPaintOpPresetSP preset = model.resourceForIndex(model.index(i, 0)).dynamicCast<KisPaintOpPreset>();
        if (!preset || preset->paintOp().id() != "paintbrush") {
            continue;
        }
        const KisPaintOpSettingsSP settings = preset->settings();
        const bool pipe = settings->getString("brush_definition").contains(".gih")
            || (settings->getBool("MaskingBrush/Enabled")
                && settings->getString("MaskingBrush/Preset/brush_definition").contains(".gih"));
        if (!pipe && !settings->getBool("Texture/Pattern/Enabled")) {
            continue;
        }
        tried++;
        pipes += pipe;

        const KoColorSpace *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        KisImageSP image =
            new KisImage(new KisSurrogateUndoStore(), imageBounds.width(), imageBounds.height(), cs, "random");
        KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
        image->addNode(layer, image->root());
        paintStroke(image, layer, preset, KoColor(Qt::black, cs));
        paintStroke(image, layer, preset, KoColor(Qt::red, cs));

        const QImage live = toImage(layer->paintDevice()).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const QImage redraw = toImage(KisBrushStrokeLayer::renderStrokes(layer->strokes(), cs, imageBounds, QPoint()))
                                  .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QVERIFY2(live == redraw, qPrintable(preset->name()));
    }
    qInfo() << "presets checked:" << tried << "with image pipes:" << pipes;
    QVERIFY(tried > 0);
}

/// The brush tool starts a stroke's dab spacing from the cursor's last
/// hover position and direction (KisToolFreehandHelper); that start places
/// every dab, so it is recorded and drawn again
void KisBrushStrokeLayerTest::testStrokeStartRecorded()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image =
        new KisImage(new KisSurrogateUndoStore(), imageBounds.width(), imageBounds.height(), cs, "start");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    const KisDistanceInformation hover(QPointF(16.3, 52.7), 0.4, 30, LONG_TIME, 0);
    paintStroke(image, layer, loadPreset(QStringLiteral("c)_Pencil-2.kpp")), KoColor(Qt::black, cs), &hover);

    QCOMPARE(layer->strokes().size(), 1);
    const KisRecordedBrushStroke &stroke = *layer->strokes().first();
    QCOMPARE(stroke.starts.size(), 1);
    QVERIFY(stroke.starts.first().hasLastDab);
    QCOMPARE(stroke.starts.first().lastPosition, QPointF(16.3, 52.7));
    QCOMPARE(stroke.starts.first().spacingUpdateInterval, 30.0);

    const QImage live = toImage(layer->paintDevice()).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QCOMPARE(toImage(KisBrushStrokeLayer::renderStrokes(layer->strokes(), cs, imageBounds, QPoint()))
                 .convertToFormat(QImage::Format_ARGB32_Premultiplied),
             live);

    // without its start, the stroke's dabs land elsewhere
    QSharedPointer<KisRecordedBrushStroke> withoutStart(new KisRecordedBrushStroke(stroke));
    withoutStart->starts.clear();
    QVERIFY(toImage(KisBrushStrokeLayer::renderStrokes({withoutStart}, cs, imageBounds, QPoint()))
                .convertToFormat(QImage::Format_ARGB32_Premultiplied)
            != live);

    // and it is saved
    QBuffer buffer;
    buffer.open(QBuffer::WriteOnly);
    QVERIFY(KisBrushStrokeLayer::saveStrokes(layer->strokes(), &buffer));
    buffer.close();
    buffer.open(QBuffer::ReadOnly);
    QVector<KisRecordedBrushStrokeSP> loaded;
    QVERIFY(KisBrushStrokeLayer::loadStrokes(&buffer, &loaded));
    QCOMPARE(loaded.first()->starts.size(), 1);
    QCOMPARE(loaded.first()->starts.first().lastPosition, QPointF(16.3, 52.7));
    QCOMPARE(loaded.first()->starts.first().lastAngle, 0.4);
}

/// An ABR brush tip has no file of its own: it is saved with the strokes as
/// a PNG, so the strokes redraw where the .abr file is not installed
void KisBrushStrokeLayerTest::testAbrTipSaved()
{
    // an ABR tip, as KisAbrBrushCollection makes one: a gray mask image
    QImage tipImage(24, 24, QImage::Format_RGB32);
    tipImage.fill(Qt::white);
    for (int y = 0; y < 24; y++) {
        for (int x = 0; x < 24; x++) {
            if ((x - 12) * (x - 12) + (y - 12) * (y - 12) < 100 && (x + y) % 3) {
                tipImage.setPixel(x, y, qRgb(40, 40, 40));
            }
        }
    }
    KisAbrBrushSP tip(new KisAbrBrush(QStringLiteral("solstice_test_abr_1"), nullptr));
    tip->setBrushTipImage(tipImage);
    tip->setName(QStringLiteral("solstice_test_abr_1"));
    tip->setMD5Sum(QStringLiteral("0123456789abcdef0123456789abcdef"));
    tip->setValid(true);
    QVERIFY(!KisGlobalResourcesInterface::instance()
                 ->source(ResourceType::Brushes)
                 .bestMatch(tip->md5Sum(), tip->filename(), tip->name()));

    // a Pixel Brush preset using it, with the tip in its own resources
    KisPaintOpPresetSP preset =
        loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp"))->clone().dynamicCast<KisPaintOpPreset>();
    QDomDocument doc;
    QDomElement element = doc.createElement(QStringLiteral("Brush"));
    tip->toXML(doc, element);
    doc.appendChild(element);
    preset->settings()->setProperty(QStringLiteral("brush_definition"), doc.toString());
    preset->setResourcesInterface(QSharedPointer<KisLocalStrokeResources>::create(QList<KoResourceSP>{tip}));

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(new KisSurrogateUndoStore(), imageBounds.width(), imageBounds.height(), cs, "abr");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image, layer, preset, KoColor(Qt::black, cs));
    QCOMPARE(layer->strokes().size(), 1);
    const QImage live = toImage(layer->paintDevice()).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QVERIFY(coveredPixels(live) > 300);

    QBuffer buffer;
    buffer.open(QBuffer::WriteOnly);
    QVERIFY(KisBrushStrokeLayer::saveStrokes(layer->strokes(), &buffer));
    buffer.close();
    buffer.open(QBuffer::ReadOnly);
    QVector<KisRecordedBrushStrokeSP> loaded;
    QVERIFY(KisBrushStrokeLayer::loadStrokes(&buffer, &loaded));
    QCOMPARE(loaded.size(), 1);

    // the loaded stroke draws with the saved tip
    const KisPaintOpPresetSP loadedPreset = loaded.first()->preset;
    bool found = false;
    for (const KoResourceLoadResult &link : loadedPreset->linkedResources(loadedPreset->resourcesInterface())) {
        if (KisAbrBrushSP saved = link.resource<KisAbrBrush>()) {
            found = true;
            QCOMPARE(saved->md5Sum(), tip->md5Sum());
            QCOMPARE(saved->brushTipImage(), tip->brushTipImage());
        }
    }
    QVERIFY(found);
    QCOMPARE(toImage(KisBrushStrokeLayer::renderStrokes(loaded, cs, imageBounds, QPoint()))
                 .convertToFormat(QImage::Format_ARGB32_Premultiplied),
             live);
}

namespace
{
/// What the Move Tool does with the pixels inside @p selection
/// (MoveSelectionStrokeStrategy): cuts them and pastes them over the rest
/// at @p offset
void moveSelectedPixels(KisPaintDeviceSP device, KisSelectionSP selection, const QPoint &offset)
{
    const QRect rect = selection->selectedRect();
    KisPaintDeviceSP moved = new KisPaintDevice(device->colorSpace());
    KisPainter cut(moved);
    cut.setSelection(selection);
    cut.bitBlt(rect.topLeft(), device, rect);
    cut.end();
    device->clearSelection(selection);
    moved->moveTo(moved->offset() + offset);
    const QRect movedRect = moved->exactBounds();
    KisPainter paste(device);
    paste.bitBlt(movedRect.topLeft(), moved, movedRect);
    paste.end();
}
} // namespace

/// Moving a selection with the Move Tool moves the strokes inside it, which
/// are then pasted over the others; a stroke the selection cuts leaves the
/// record unchanged
void KisBrushStrokeLayerTest::testSelectionMoveMovesStrokes()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
    const QRect bounds(0, 0, 420, 140);
    KisImageSP image = new KisImage(new KisSurrogateUndoStore(), bounds.width(), bounds.height(), cs, "move");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    KisPaintOpPresetSP preset = loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp"));
    paintStroke(image, layer, preset, KoColor(Qt::black, cs));
    paintStroke(image, layer, preset, KoColor(Qt::red, cs), nullptr, QPointF(210, 0));
    const QVector<KisRecordedBrushStrokeSP> before = layer->strokes();
    QCOMPARE(before.size(), 2);

    // a selection that cuts the black stroke
    {
        KisSelectionSP cutting = new KisSelection();
        cutting->pixelSelection()->select(QRect(0, 0, 90, 140));
        KisPaintDeviceSP mask = new KisPaintDevice(*cutting->projection());
        QVERIFY(!layer->createSelectionMoveCommand(mask, QPoint(5, 5)));
    }

    // the black stroke, moved over the red one: it comes last
    KisSelectionSP selection = new KisSelection();
    selection->pixelSelection()->select(QRect(0, 0, 205, 140));
    KisPaintDeviceSP mask = new KisPaintDevice(*selection->projection());
    const QPoint offset(200, 6);
    moveSelectedPixels(layer->paintDevice(), selection, offset);
    QScopedPointer<KUndo2Command> command(layer->createSelectionMoveCommand(mask, offset));
    QVERIFY(command);
    command->redo();
    const QVector<KisRecordedBrushStrokeSP> after = layer->strokes();
    QCOMPARE(after.size(), 2);
    QCOMPARE(after[0], before[1]);
    QCOMPARE(after[1]->jobs.first().pi1.pos(), before[0]->jobs.first().pi1.pos() + offset);
    QString difference;
    QVERIFY2(KisBrushStrokeLayer::strokesMatchPixels(after, layer->paintDevice(), &difference), qPrintable(difference));
    // in the old order the red stroke would be on top
    QVERIFY(!KisBrushStrokeLayer::strokesMatchPixels(
        {KisBrushStrokeLayer::transformedStrokes({before[0]},
                                                 QPoint(),
                                                 QTransform::fromTranslate(offset.x(), offset.y()))
             .first(),
         before[1]},
        layer->paintDevice(),
        &difference));

    command->undo();
    QCOMPARE(layer->strokes(), before);
}

/// Duplicating a selection with the Move Tool (Ctrl-drag) keeps the strokes
/// inside it and adds copies, moved and pasted over everything; a stroke
/// the selection cuts leaves the record unchanged
void KisBrushStrokeLayerTest::testSelectionDuplicateCopiesStrokes()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
    const QRect bounds(0, 0, 420, 140);
    KisImageSP image = new KisImage(new KisSurrogateUndoStore(), bounds.width(), bounds.height(), cs, "duplicate");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    KisPaintOpPresetSP preset = loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp"));
    paintStroke(image, layer, preset, KoColor(Qt::black, cs));
    paintStroke(image, layer, preset, KoColor(Qt::red, cs), nullptr, QPointF(210, 0));
    const QVector<KisRecordedBrushStrokeSP> before = layer->strokes();
    QCOMPARE(before.size(), 2);

    // a selection that cuts the black stroke
    {
        KisSelectionSP cutting = new KisSelection();
        cutting->pixelSelection()->select(QRect(0, 0, 90, 140));
        KisPaintDeviceSP mask = new KisPaintDevice(*cutting->projection());
        QVERIFY(!layer->createSelectionDuplicateCommand(mask, QPoint(5, 5)));
    }

    // a copy of the black stroke, over the red one: it comes last
    KisSelectionSP selection = new KisSelection();
    selection->pixelSelection()->select(QRect(0, 0, 205, 140));
    KisPaintDeviceSP mask = new KisPaintDevice(*selection->projection());
    QVERIFY(!layer->createSelectionDuplicateCommand(mask, QPoint()));
    const QPoint offset(200, 6);
    {
        // what MoveSelectionStrokeStrategy does in duplicate mode
        KisPaintDeviceSP device = layer->paintDevice();
        const QRect rect = selection->selectedRect();
        KisPaintDeviceSP copy = new KisPaintDevice(cs);
        KisPainter gc(copy);
        gc.setSelection(selection);
        gc.bitBlt(rect.topLeft(), device, rect);
        gc.end();
        copy->moveTo(copy->offset() + offset);
        const QRect copyRect = copy->exactBounds();
        KisPainter paste(device);
        paste.bitBlt(copyRect.topLeft(), copy, copyRect);
        paste.end();
    }
    QScopedPointer<KUndo2Command> command(layer->createSelectionDuplicateCommand(mask, offset));
    QVERIFY(command);
    command->redo();
    const QVector<KisRecordedBrushStrokeSP> after = layer->strokes();
    QCOMPARE(after.size(), 3);
    QCOMPARE(after[0], before[0]);
    QCOMPARE(after[1], before[1]);
    QCOMPARE(after[2]->jobs.first().pi1.pos(), before[0]->jobs.first().pi1.pos() + offset);
    QString difference;
    QVERIFY2(KisBrushStrokeLayer::strokesMatchPixels(after, layer->paintDevice(), &difference), qPrintable(difference));
    // the originals alone are not the pixels any more
    QVERIFY(!KisBrushStrokeLayer::strokesMatchPixels(before, layer->paintDevice(), &difference));

    command->undo();
    QCOMPARE(layer->strokes(), before);
}

/// After an edit other than the brush, a layer of an open document checks
/// in the background whether its pixels are still its strokes; undoing the
/// edit makes them match again
void KisBrushStrokeLayerTest::testRecordCheckFindsOtherEdits()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisDocument *document = KisPart::instance()->createDocument();
    document->setFileBatchMode(true);
    KisImageSP image =
        new KisImage(document->createUndoStore(), imageBounds.width(), imageBounds.height(), cs, "check");
    document->setCurrentImage(image);
    KisPart::instance()->addDocument(document, false);
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")), KoColor(Qt::black, cs));
    QVERIFY(QTest::qWaitFor(
        [&]() {
            return layer->recordState() == KisBrushStrokeLayer::RecordState::Matching;
        },
        20000));

    KisTransaction transaction(layer->paintDevice());
    layer->paintDevice()->fill(QRect(150, 60, 20, 20), KoColor(Qt::blue, cs));
    layer->setDirty(QRect(150, 60, 20, 20));
    QVERIFY(QTest::qWaitFor(
        [&]() {
            return layer->recordState() == KisBrushStrokeLayer::RecordState::NotMatching;
        },
        20000));

    transaction.revert();
    QVERIFY(QTest::qWaitFor(
        [&]() {
            return layer->recordState() == KisBrushStrokeLayer::RecordState::Matching;
        },
        20000));

    KisPart::instance()->removeDocument(document);
}

/// A brush texture is anchored to the canvas; strokes drawn again after the
/// layer or a selection moved keep the part of the pattern they were drawn
/// with
void KisBrushStrokeLayerTest::testTextureFollowsMovedStrokes()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
    KisImageSP image = new KisImage(new KisSurrogateUndoStore(), 300, 160, cs, "texture");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    // a Pixel Brush preset with a texture and no random texture offset
    KisPaintOpPresetSP preset;
    KisResourceModel model(ResourceType::PaintOpPresets);
    for (int i = 0; i < model.rowCount() && !preset; i++) {
        KisPaintOpPresetSP candidate = model.resourceForIndex(model.index(i, 0)).dynamicCast<KisPaintOpPreset>();
        if (candidate && candidate->paintOp().id() == "paintbrush"
            && candidate->settings()->getBool("Texture/Pattern/Enabled")
            && !candidate->settings()->getBool("Texture/Pattern/isRandomOffsetX")
            && !candidate->settings()->getBool("Texture/Pattern/isRandomOffsetY")) {
            preset = candidate;
        }
    }
    QVERIFY(preset);
    qInfo() << "textured preset:" << preset->name();
    paintStroke(image, layer, preset, KoColor(Qt::black, cs));
    QString difference;
    QVERIFY2(KisBrushStrokeLayer::strokesMatchPixels(layer->strokes(), layer->paintDevice(), &difference),
             qPrintable(difference));

    // the layer moved (the Move Tool without a selection)
    layer->setX(7);
    layer->setY(-5);
    QVERIFY2(KisBrushStrokeLayer::strokesMatchPixels(layer->strokes(), layer->paintDevice(), &difference),
             qPrintable(difference));

    // the pixels moved within a selection
    KisSelectionSP selection = new KisSelection();
    selection->pixelSelection()->select(QRect(0, 0, 300, 160));
    KisPaintDeviceSP mask = new KisPaintDevice(*selection->projection());
    const QPoint offset(13, 9);
    moveSelectedPixels(layer->paintDevice(), selection, offset);
    QScopedPointer<KUndo2Command> command(layer->createSelectionMoveCommand(mask, offset));
    QVERIFY(command);
    command->redo();
    QVERIFY2(KisBrushStrokeLayer::strokesMatchPixels(layer->strokes(), layer->paintDevice(), &difference),
             qPrintable(difference));
}

void KisBrushStrokeLayerTest::testScaleKeepsDeletedEdges()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    auto *undo = new KisSurrogateUndoStore();
    KisImageSP image = new KisImage(undo, 200, 100, cs, "deleted edges");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")), KoColor(Qt::black, cs));
    layer->paintDevice()->clear(QRect(100, -100, 1000, 1000));
    const QImage before = toImage(layer->paintDevice());
    QString difference;
    QVERIFY(!KisBrushStrokeLayer::strokesMatchPixels(layer->strokes(), layer->paintDevice(), &difference));
    scaleImage4x(image);
    QCOMPARE(coveredPixels(toImage(layer->paintDevice(), QRect(440, 0, 360, 400))), 0);
    undo->undo();
    image->waitForDone();
    QCOMPARE(toImage(layer->paintDevice()), before);
    undo->redo();
    image->waitForDone();
    QCOMPARE(coveredPixels(toImage(layer->paintDevice(), QRect(440, 0, 360, 400))), 0);
}

void KisBrushStrokeLayerTest::testScaleKeepsHdrEdits_data()
{
    QTest::addColumn<QString>("depth");
    QTest::addColumn<float>("value");
    for (const QString &depth : {QStringLiteral("F16"), QStringLiteral("F32")}) {
        QTest::newRow(qPrintable(depth + "-HDR")) << depth << 4.0f;
        QTest::newRow(qPrintable(depth + "-negative")) << depth << -2.0f;
    }
}

void KisBrushStrokeLayerTest::testScaleKeepsHdrEdits()
{
    QFETCH(QString, depth);
    QFETCH(float, value);
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString());
    QVERIFY(cs);
    auto *undo = new KisSurrogateUndoStore();
    KisImageSP image = new KisImage(undo, 200, 100, cs, "HDR edits");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")), KoColor(Qt::white, cs));
    const QRect bounds = layer->paintDevice()->exactBounds();
    QByteArray pixels(bounds.width() * bounds.height() * cs->pixelSize(), '\0');
    layer->paintDevice()->readBytes(reinterpret_cast<quint8 *>(pixels.data()), bounds);
    QVector<float> channels(cs->channelCount());
    int changed = 0;
    for (int y = 0; y < bounds.height(); ++y) {
        for (int x = 0; x < bounds.width(); ++x) {
            quint8 *pixel = reinterpret_cast<quint8 *>(pixels.data()) + (y * bounds.width() + x) * cs->pixelSize();
            cs->normalisedChannelsValue(pixel, channels);
            // A small patch exercises partial redraw, including translucent edges.
            if (bounds.x() + x >= 65 && bounds.x() + x < 85 && channels[3] > 0.1f) {
                channels[0] = channels[1] = channels[2] = value;
                cs->fromNormalisedChannelsValue(pixel, channels);
                ++changed;
            }
        }
    }
    QVERIFY(changed > 0);
    layer->paintDevice()->writeBytes(reinterpret_cast<const quint8 *>(pixels.constData()), bounds);
    QString difference;
    QVERIFY(!KisBrushStrokeLayer::strokesMatchPixels(layer->strokes(), layer->paintDevice(), &difference));
    const QPointF firstPoint = layer->strokes().first()->jobs.first().pi1.pos();
    scaleImage4x(image);
    QCOMPARE(layer->strokes().first()->jobs.first().pi1.pos(), firstPoint * 4);
    const QRect large = layer->paintDevice()->exactBounds();
    QByteArray scaled(large.width() * large.height() * cs->pixelSize(), '\0');
    layer->paintDevice()->readBytes(reinterpret_cast<quint8 *>(scaled.data()), large);
    int preserved = 0;
    for (int i = 0; i < large.width() * large.height(); ++i) {
        cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(scaled.constData()) + i * cs->pixelSize(),
                                    channels);
        if (channels[3] > 0.1f && qAbs(channels[0] - value) < 0.05f)
            ++preserved;
    }
    QVERIFY(preserved > changed);
    undo->undo();
    image->waitForDone();
    QByteArray restored(pixels.size(), '\0');
    layer->paintDevice()->readBytes(reinterpret_cast<quint8 *>(restored.data()), bounds);
    QCOMPARE(restored, pixels);
}

void KisBrushStrokeLayerTest::testRejectInvalidStrokeJobs_data()
{
    QTest::addColumn<int>("count");
    QTest::addColumn<int>("index");
    QTest::addColumn<int>("type");
    QTest::newRow("negative-index") << 1 << -1 << int(FreehandStrokeStrategy::Data::POINT);
    QTest::newRow("past-end-index") << 1 << 1 << int(FreehandStrokeStrategy::Data::POINT);
    QTest::newRow("zero-count") << 0 << 0 << int(FreehandStrokeStrategy::Data::POINT);
    QTest::newRow("negative-count") << -1 << 0 << int(FreehandStrokeStrategy::Data::POINT);
    QTest::newRow("excessive-count") << 4097 << 0 << int(FreehandStrokeStrategy::Data::POINT);
    QTest::newRow("invalid-type") << 1 << 0 << -1;
    QTest::newRow("unrecordable-type") << 1 << 0 << int(FreehandStrokeStrategy::Data::POLYGON);
}

void KisBrushStrokeLayerTest::testRejectInvalidStrokeJobs()
{
    QFETCH(int, count);
    QFETCH(int, index);
    QFETCH(int, type);
    auto stroke = QSharedPointer<KisRecordedBrushStroke>::create();
    stroke->preset = loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp"));
    QVERIFY(stroke->preset);
    stroke->strokeInfoCount = count;
    KisRecordedBrushStroke::Job job;
    job.type = type;
    job.strokeInfoId = index;
    stroke->jobs << job;
    QBuffer buffer;
    QVERIFY(buffer.open(QIODevice::ReadWrite));
    QVERIFY(KisBrushStrokeLayer::saveStrokes({stroke}, &buffer));
    buffer.seek(0);
    QVector<KisRecordedBrushStrokeSP> result{stroke};
    QVERIFY(!KisBrushStrokeLayer::loadStrokes(&buffer, &result));
    QCOMPARE(result, QVector<KisRecordedBrushStrokeSP>{stroke});
}

namespace
{
/// A recorded stroke of @p copies copies (as the Multibrush tool paints),
/// one point each
QSharedPointer<KisRecordedBrushStroke> multiCopyStroke(int copies)
{
    auto stroke = QSharedPointer<KisRecordedBrushStroke>::create();
    stroke->preset = loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp"));
    stroke->fgColor = KoColor(Qt::black, KoColorSpaceRegistry::instance()->rgb8());
    stroke->bgColor = KoColor(Qt::white, KoColorSpaceRegistry::instance()->rgb8());
    stroke->compositeOpId = COMPOSITE_OVER;
    stroke->strokeInfoCount = copies;
    for (int i = 0; i < copies; i++) {
        stroke->starts << KisRecordedBrushStroke::Start();
        KisRecordedBrushStroke::Job job;
        job.type = FreehandStrokeStrategy::Data::POINT;
        job.strokeInfoId = i;
        job.pi1 = KisPaintInformation(QPointF(10 + i % 20 * 9, 10 + i / 20 * 9), 1.0);
        stroke->jobs << job;
    }
    return stroke;
}

void scaleImageBy(KisImageSP image, int factor)
{
    image->scaleImage(image->bounds().size() * factor,
                      image->xRes(),
                      image->yRes(),
                      KisFilterStrategyRegistry::instance()->value(QStringLiteral("Bicubic")));
    image->waitForDone();
}

/// An open document's image, as the background check looks for layers in
/// open documents
KisDocument *openDocument(const QSize &size, const KoColorSpace *cs)
{
    KisDocument *document = KisPart::instance()->createDocument();
    document->setFileBatchMode(true);
    KisImageSP image = new KisImage(document->createUndoStore(), size.width(), size.height(), cs, "check");
    document->setCurrentImage(image);
    KisPart::instance()->addDocument(document, false);
    return document;
}

bool waitForRecordState(KisBrushStrokeLayerSP layer, KisBrushStrokeLayer::RecordState state)
{
    return QTest::qWaitFor(
        [&]() {
            return layer->recordState() == state;
        },
        20000);
}
} // namespace

/// The Multibrush tool paints up to 200 copies of a stroke (Snowflake) and
/// any number with Copy Translate: such a record reads back
void KisBrushStrokeLayerTest::testManyCopiesLoad()
{
    const QSharedPointer<KisRecordedBrushStroke> stroke = multiCopyStroke(200);
    QBuffer buffer;
    QVERIFY(buffer.open(QIODevice::ReadWrite));
    QVERIFY(KisBrushStrokeLayer::saveStrokes({stroke}, &buffer));
    buffer.seek(0);
    QVector<KisRecordedBrushStrokeSP> loaded;
    QVERIFY(KisBrushStrokeLayer::loadStrokes(&buffer, &loaded));
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first()->strokeInfoCount, 200);
    QCOMPARE(loaded.first()->starts.size(), 200);
    QCOMPARE(loaded.first()->jobs.last().strokeInfoId, 199);

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    QCOMPARE(toImage(KisBrushStrokeLayer::renderStrokes(loaded, cs, imageBounds, QPoint())),
             toImage(KisBrushStrokeLayer::renderStrokes({stroke}, cs, imageBounds, QPoint())));
}

void KisBrushStrokeLayerTest::testRejectDamagedStrokeData_data()
{
    QTest::addColumn<int>("damage");
    QTest::newRow("more starts than copies") << 0;
    QTest::newRow("NaN position") << 1;
    QTest::newRow("infinite control point") << 2;
    QTest::newRow("huge position") << 3;
    QTest::newRow("NaN mirror axes") << 4;
}

/// Damaged data is refused instead of drawing at impossible positions
void KisBrushStrokeLayerTest::testRejectDamagedStrokeData()
{
    QFETCH(int, damage);
    QSharedPointer<KisRecordedBrushStroke> stroke = multiCopyStroke(2);
    switch (damage) {
    case 0:
        stroke->starts << KisRecordedBrushStroke::Start();
        break;
    case 1:
        stroke->jobs[1].pi1.setPos(QPointF(qQNaN(), 3));
        break;
    case 2:
        stroke->jobs[0].type = FreehandStrokeStrategy::Data::CURVE;
        stroke->jobs[0].control2 = QPointF(5, qInf());
        break;
    case 3:
        stroke->jobs[1].pi1.setPos(QPointF(1e12, 3));
        break;
    case 4:
        stroke->mirrorHorizontal = true;
        stroke->mirrorAxesCenter = QPointF(qQNaN(), 0);
        break;
    }
    QBuffer buffer;
    QVERIFY(buffer.open(QIODevice::ReadWrite));
    QVERIFY(KisBrushStrokeLayer::saveStrokes({stroke}, &buffer));
    buffer.seek(0);
    QVector<KisRecordedBrushStrokeSP> result;
    QVERIFY(!KisBrushStrokeLayer::loadStrokes(&buffer, &result));
    QVERIFY(result.isEmpty());
}

/// A stroke drawn with the canvas mirror records the mirror and its axes:
/// drawn again, scaled and saved, its mirrored copies come back where they
/// were
void KisBrushStrokeLayerTest::testMirroredStrokeRecorded()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const QRect bounds(0, 0, 420, 300);
    KisSurrogateUndoStore *undoStore = new KisSurrogateUndoStore();
    KisImageSP image = new KisImage(undoStore, bounds.width(), bounds.height(), cs, "mirror");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image,
                layer,
                loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")),
                KoColor(Qt::black, cs),
                nullptr,
                {},
                true);

    QCOMPARE(layer->strokes().size(), 1);
    const KisRecordedBrushStrokeSP stroke = layer->strokes().first();
    QVERIFY(stroke->mirrorHorizontal);
    QVERIFY(stroke->mirrorVertical);
    QCOMPARE(stroke->mirrorAxesCenter, QPointF(210, 150));
    // the mirrored copies are painted (the stroke itself stays above y 100)
    QVERIFY(coveredPixels(toImage(layer->paintDevice(), QRect(220, 160, 200, 140))) > 300);

    QString difference;
    QVERIFY2(KisBrushStrokeLayer::strokesMatchPixels(layer->strokes(), layer->paintDevice(), &difference),
             qPrintable(difference));
    // without the mirror the record would not be the pixels
    QSharedPointer<KisRecordedBrushStroke> unmirrored(new KisRecordedBrushStroke(*stroke));
    unmirrored->mirrorHorizontal = false;
    unmirrored->mirrorVertical = false;
    QVERIFY(!KisBrushStrokeLayer::strokesMatchPixels({unmirrored}, layer->paintDevice(), &difference));

    // saved and read back
    QBuffer buffer;
    QVERIFY(buffer.open(QIODevice::ReadWrite));
    QVERIFY(KisBrushStrokeLayer::saveStrokes(layer->strokes(), &buffer));
    buffer.seek(0);
    QVector<KisRecordedBrushStrokeSP> loaded;
    QVERIFY(KisBrushStrokeLayer::loadStrokes(&buffer, &loaded));
    QCOMPARE(loaded.size(), 1);
    QVERIFY(loaded.first()->mirrorHorizontal && loaded.first()->mirrorVertical);
    QCOMPARE(loaded.first()->mirrorAxesCenter, stroke->mirrorAxesCenter);
    QVERIFY2(KisBrushStrokeLayer::strokesMatchPixels(loaded, layer->paintDevice(), &difference),
             qPrintable(difference));

    // a selection around the stroke but not its mirrored copies cuts it
    KisSelectionSP selection = new KisSelection();
    selection->pixelSelection()->select(QRect(0, 0, 205, 145));
    KisPaintDeviceSP mask = new KisPaintDevice(*selection->projection());
    QVERIFY(!layer->createSelectionMoveCommand(mask, QPoint(3, 3)));

    // scaling draws the copies again around the scaled axes
    scaleImageBy(image, 2);
    QCOMPARE(layer->strokes().first()->mirrorAxesCenter, QPointF(420, 300));
    const QRect large(QPoint(), bounds.size() * 2);
    QCOMPARE(toImage(KisBrushStrokeLayer::renderStrokes(layer->strokes(), cs, large, QPoint()), large),
             toImage(layer->paintDevice(), large));
    QVERIFY(layer->strokes().first()->jobs.first().pi1.pos() == stroke->jobs.first().pi1.pos() * 2);
}

/// An edit other than the brush is checked even when a recorded brush
/// stroke follows it before the check runs
void KisBrushStrokeLayerTest::testRecordCheckFindsEditsBesideStrokes()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisDocument *document = openDocument(imageBounds.size(), cs);
    KisImageSP image = document->image();
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    KisPaintOpPresetSP preset = loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp"));
    paintStroke(image, layer, preset, KoColor(Qt::black, cs));
    QVERIFY(waitForRecordState(layer, KisBrushStrokeLayer::RecordState::Matching));

    layer->paintDevice()->fill(QRect(150, 60, 20, 20), KoColor(Qt::blue, cs));
    layer->setDirty(QRect(150, 60, 20, 20));
    paintStroke(image, layer, preset, KoColor(Qt::red, cs), nullptr, QPointF(0, -20));
    QCOMPARE(layer->strokes().size(), 2);
    QVERIFY(waitForRecordState(layer, KisBrushStrokeLayer::RecordState::NotMatching));

    KisPart::instance()->removeDocument(document);
}

/// A layer asking for a check while it is out of the image (deleted, in the
/// undo history) is checked after it comes back
void KisBrushStrokeLayerTest::testRecordCheckAfterLayerReturns()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisDocument *document = openDocument(imageBounds.size(), cs);
    KisImageSP image = document->image();
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")), KoColor(Qt::black, cs));
    QVERIFY(waitForRecordState(layer, KisBrushStrokeLayer::RecordState::Matching));

    image->removeNode(layer);
    image->waitForDone();
    layer->setDirty(QRect(0, 0, 10, 10));
    // the check looks for it and does not find it
    QTest::qWait(3000);

    image->addNode(layer, image->root());
    image->waitForDone();
    layer->paintDevice()->fill(QRect(150, 60, 20, 20), KoColor(Qt::blue, cs));
    layer->setDirty(QRect(150, 60, 20, 20));
    QVERIFY(waitForRecordState(layer, KisBrushStrokeLayer::RecordState::NotMatching));

    KisPart::instance()->removeDocument(document);
}

/// Floating point spaces other than RGBA are compared in their own channels:
/// an HDR edit of a gray layer is not clipped away into a match
void KisBrushStrokeLayerTest::testFloatGrayEditsDetected()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->colorSpace("GRAYA", "F32", QString());
    QVERIFY(cs);
    KisImageSP image = new KisImage(new KisSurrogateUndoStore(), 200, 100, cs, "gray");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")), KoColor(Qt::white, cs));
    QString difference;
    QVERIFY2(KisBrushStrokeLayer::strokesMatchPixels(layer->strokes(), layer->paintDevice(), &difference),
             qPrintable(difference));

    const QRect bounds = layer->paintDevice()->exactBounds();
    QByteArray pixels(bounds.width() * bounds.height() * cs->pixelSize(), '\0');
    layer->paintDevice()->readBytes(reinterpret_cast<quint8 *>(pixels.data()), bounds);
    QVector<float> channels(cs->channelCount());
    int changed = 0;
    for (int i = 0; i < bounds.width() * bounds.height(); i++) {
        quint8 *pixel = reinterpret_cast<quint8 *>(pixels.data()) + i * cs->pixelSize();
        cs->normalisedChannelsValue(pixel, channels);
        if (channels[cs->alphaPos()] > 0.5f && changed < 50) {
            channels[0] = 4.0f;
            cs->fromNormalisedChannelsValue(pixel, channels);
            changed++;
        }
    }
    QCOMPARE(changed, 50);
    layer->paintDevice()->writeBytes(reinterpret_cast<const quint8 *>(pixels.constData()), bounds);
    QVERIFY(!KisBrushStrokeLayer::strokesMatchPixels(layer->strokes(), layer->paintDevice(), &difference));
}

/// The stroke records the layer's position when it starts drawing, after a
/// move queued before it
void KisBrushStrokeLayerTest::testDeviceOffsetReadAtStart()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image =
        new KisImage(new KisSurrogateUndoStore(), imageBounds.width(), imageBounds.height(), cs, "offset");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());

    // a slow move of the layer, still queued when the brush stroke begins
    KisStrokeStrategyUndoCommandBased *move =
        new KisStrokeStrategyUndoCommandBased(kundo2_noi18n("move"), false, image.data());
    KisStrokeId id = image->startStroke(move);
    KisBrushStrokeLayer *moved = layer.data();
    image->addJob(id, new KisStrokeStrategyUndoCommandBased::Data(new KisCommandUtils::LambdaCommand([moved]() {
                      QThread::msleep(300);
                      moved->setX(7);
                      moved->setY(-3);
                      return new KUndo2Command();
                  })));
    image->endStroke(id);
    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")), KoColor(Qt::black, cs));

    QCOMPARE(layer->strokes().first()->deviceOffset, QPoint(7, -3));
    QString difference;
    QVERIFY2(KisBrushStrokeLayer::strokesMatchPixels(layer->strokes(), layer->paintDevice(), &difference),
             qPrintable(difference));
}

SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES(KisBrushStrokeLayerTest, QStringLiteral("Krita_4_Default_Resources.bundle"))

#include "KisBrushStrokeLayerTest.moc"
