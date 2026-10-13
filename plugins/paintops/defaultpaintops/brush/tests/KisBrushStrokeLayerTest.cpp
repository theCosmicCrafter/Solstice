/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>
#include <QtMath>

#include "KisBrushTestMain.h"

#include <QBuffer>
#include <QTemporaryDir>

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
#include <kis_transaction.h>
#include <kis_undo_stores.h>
#include <strokes/KisFreehandStrokeInfo.h>
#include <strokes/freehand_stroke.h>

/**
 * Stage 2a of the brush stroke layer plan (docs/agent/brush-stroke-layer-plan.md):
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
    void testRecordCheckFindsOtherEdits();
    void testTextureFollowsMovedStrokes();
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
                 const QPointF &shift = QPointF())
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
    provider.setResource(KoCanvasResource::MirrorHorizontal, false);
    provider.setResource(KoCanvasResource::MirrorVertical, false);
    provider.setResource(KoCanvasResource::EffectiveLodAvailability, false);
    provider.setResource(KoCanvasResource::Size, preset->settings()->paintOpSize());

    KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, node, &provider, nullptr, {}, preset);
    resources->setOpacity(1.0);
    resources->setMirroring(false, false);
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

SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES(KisBrushStrokeLayerTest, QStringLiteral("Krita_4_Default_Resources.bundle"))

#include "KisBrushStrokeLayerTest.moc"
