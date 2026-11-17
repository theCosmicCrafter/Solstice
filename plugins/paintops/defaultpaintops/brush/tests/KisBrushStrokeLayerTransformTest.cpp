/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>
#include <QtMath>

#include <atomic>

#include "KisBrushTestMain.h"

#include <KisAsynchronousStrokeUpdateHelper.h>
#include <KisBrushStrokeLayer.h>
#include <KisGlobalResourcesInterface.h>
#include <KisLodPreferences.h>
#include <KoCanvasResourceProvider.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <commands/kis_selection_commands.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_information.h>
#include <kis_pixel_selection.h>
#include <kis_resources_snapshot.h>
#include <kis_selection.h>
#include <kis_undo_stores.h>
#include <strokes/KisFreehandStrokeInfo.h>
#include <strokes/freehand_stroke.h>
#include <strokes/move_stroke_strategy.h>

#include "kis_transform_utils.h"
#include "strokes/inplace_transform_stroke_strategy.h"
#include "strokes/move_selection_stroke_strategy.h"
#include "tool_transform_args.h"
#include "transform_transaction_properties.h"

/**
 * The brush stroke layer with the Transform Tool
 * (docs/agent/brush-stroke-layer.md): a free transform that only
 * scales and moves draws the strokes again when it is applied; a rotation
 * keeps the transformed pixels.
 */
class KisBrushStrokeLayerTransformTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testScaleRedraws();
    void testRotationTransformsPixels();
    void testCancelledContinuedTransformRedraws();
    void testSelectionMoveWithInstantPreview();
    void testSelectionDuplicateWithInstantPreview();
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

void paintStroke(KisImageSP image, KisNodeSP node, KisPaintOpPresetSP preset)
{
    const KoColor color(Qt::black, image->colorSpace());
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
        new FreehandStrokeStrategy(resources, new KisFreehandStrokeInfo(), kundo2_noi18n("test stroke"));
    KisStrokeId id = image->startStroke(stroke);
    KisPaintInformation previous(QPointF(40, 50), 0.2);
    image->addJob(id, new FreehandStrokeStrategy::Data(0, previous));
    for (int i = 1; i <= 40; i++) {
        const qreal t = i / 40.0;
        KisPaintInformation next(QPointF(40 + t * 60, 50 + 20 * qSin(t * 2 * M_PI)), qSin(t * M_PI) + 0.05);
        next.setCurrentTime(i * 4);
        image->addJob(id, new FreehandStrokeStrategy::Data(0, previous, next));
        previous = next;
    }
    image->addJob(id, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
    image->endStroke(id);
    image->waitForDone();
}

QImage toImage(KisPaintDeviceSP device, const QRect &rect)
{
    return device->convertToQImage(nullptr, rect);
}

/// Starts the Transform Tool's in-place stroke on @p node; @p args become
/// the tool's initial arguments (those of the previous transform when it
/// continues it). False if the stroke did not start.
bool startTransform(KisImageSP image, KisNodeSP node, KisStrokeId *id, ToolTransformArgs *args)
{
    InplaceTransformStrokeStrategy *strategy = new InplaceTransformStrokeStrategy(ToolTransformArgs::FREE_TRANSFORM,
                                                                                  "Bicubic",
                                                                                  false,
                                                                                  {node},
                                                                                  nullptr,
                                                                                  nullptr,
                                                                                  image.data(),
                                                                                  image.data(),
                                                                                  image->root(),
                                                                                  false);
    std::atomic<bool> generated(false);
    QObject::connect(
        strategy,
        &InplaceTransformStrokeStrategy::sigTransactionGenerated,
        strategy,
        [&generated, args](TransformTransactionProperties, ToolTransformArgs initial, void *) {
            *args = initial;
            generated = true;
        },
        Qt::DirectConnection);

    *id = image->startStroke(strategy);
    for (int i = 0; i < 500 && !generated; i++) {
        QTest::qWait(10);
    }
    if (!generated) {
        image->cancelStroke(*id);
        return false;
    }
    return true;
}

/// Runs the Transform Tool's in-place stroke on @p node with the arguments
/// that @p change makes from the tool's initial ones; returns them
template<typename Change>
ToolTransformArgs transform(KisImageSP image, KisNodeSP node, Change change)
{
    KisStrokeId id;
    ToolTransformArgs args;
    if (!startTransform(image, node, &id, &args)) {
        return ToolTransformArgs();
    }

    change(args);
    image->addJob(id,
                  new InplaceTransformStrokeStrategy::UpdateTransformData(
                      args,
                      InplaceTransformStrokeStrategy::UpdateTransformData::PAINT_DEVICE));
    // the tool's update timer would apply it before the stroke ends
    image->addJob(id, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
    QTest::qWait(200);
    image->endStroke(id);
    image->waitForDone();
    QTest::qWait(50);
    image->waitForDone();
    return args;
}
} // namespace

void KisBrushStrokeLayerTransformTest::testScaleRedraws()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisSurrogateUndoStore *undoStore = new KisSurrogateUndoStore();
    KisImageSP image = new KisImage(undoStore, imageBounds.width(), imageBounds.height(), cs, "transform");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")));

    const QImage small = toImage(layer->paintDevice(), imageBounds);
    const QPointF firstPoint = layer->strokes().first()->jobs.first().pi1.pos();

    const ToolTransformArgs args = transform(image, layer, [](ToolTransformArgs &args) {
        args.setScaleX(1.8);
        args.setScaleY(1.8);
    });
    QVERIFY(args.mode() == ToolTransformArgs::FREE_TRANSFORM);
    const QTransform t = KisTransformUtils::MatricesPack(args).finalTransform();
    QVERIFY(t.type() <= QTransform::TxScale);

    // the record follows the transform and is what the layer shows
    QCOMPARE(layer->strokes().size(), 1);
    const QPointF mapped = layer->strokes().first()->jobs.first().pi1.pos();
    QVERIFY2((mapped - t.map(firstPoint)).manhattanLength() < 1e-6,
             qPrintable(QString("%1,%2").arg(mapped.x()).arg(mapped.y())));
    const QRect large = layer->paintDevice()->exactBounds() | imageBounds;
    const QPoint offset(layer->paintDevice()->x(), layer->paintDevice()->y());
    QCOMPARE(toImage(KisBrushStrokeLayer::renderStrokes(layer->strokes(), cs, large, offset), large),
             toImage(layer->paintDevice(), large));

    // undo restores the strokes and the pixels
    undoStore->undo();
    image->waitForDone();
    QCOMPARE(layer->strokes().first()->jobs.first().pi1.pos(), firstPoint);
    QCOMPARE(toImage(layer->paintDevice(), imageBounds), small);
}

void KisBrushStrokeLayerTransformTest::testRotationTransformsPixels()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image =
        new KisImage(new KisSurrogateUndoStore(), imageBounds.width(), imageBounds.height(), cs, "rotate");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")));
    const QPointF firstPoint = layer->strokes().first()->jobs.first().pi1.pos();
    const QRect before = layer->paintDevice()->exactBounds();

    transform(image, layer, [](ToolTransformArgs &args) {
        args.setAZ(0.5);
    });

    // the pixels are rotated, the record is left as it was
    QCOMPARE(layer->strokes().first()->jobs.first().pi1.pos(), firstPoint);
    QVERIFY(layer->paintDevice()->exactBounds() != before);
}

/// Opening the Transform Tool again continues the last transform (it undoes
/// it first); cancelling that (Ctrl+Z) applies the last transform again,
/// which draws the strokes again as the first time
void KisBrushStrokeLayerTransformTest::testCancelledContinuedTransformRedraws()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisSurrogateUndoStore *undoStore = new KisSurrogateUndoStore();
    KisImageSP image = new KisImage(undoStore, imageBounds.width(), imageBounds.height(), cs, "continue");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")));
    const QPointF firstPoint = layer->strokes().first()->jobs.first().pi1.pos();

    const ToolTransformArgs args = transform(image, layer, [](ToolTransformArgs &args) {
        args.setScaleX(1.8);
        args.setScaleY(1.8);
    });
    const QTransform t = KisTransformUtils::MatricesPack(args).finalTransform();
    const QRect large = layer->paintDevice()->exactBounds() | imageBounds;
    const QImage scaled = toImage(layer->paintDevice(), large);

    KisStrokeId id;
    ToolTransformArgs continued;
    QVERIFY(startTransform(image, layer, &id, &continued));
    QCOMPARE(continued.scaleX(), 1.8);
    image->cancelStroke(id);
    image->waitForDone();
    QTest::qWait(50);
    image->waitForDone();

    // the record and the pixels are those of the first transform
    QCOMPARE(layer->strokes().size(), 1);
    const QPointF mapped = layer->strokes().first()->jobs.first().pi1.pos();
    QVERIFY2((mapped - t.map(firstPoint)).manhattanLength() < 1e-6,
             qPrintable(QString("%1,%2").arg(mapped.x()).arg(mapped.y())));
    QString difference;
    QVERIFY2(KisBrushStrokeLayer::strokesMatchPixels(layer->strokes(), layer->paintDevice(), &difference),
             qPrintable(difference));
    QCOMPARE(toImage(layer->paintDevice(), large), scaled);
}

/// With Instant Preview, the Move Tool's preview at a lower level of
/// detail does not move the recorded strokes; the full stroke moves them
/// once
void KisBrushStrokeLayerTransformTest::testSelectionMoveWithInstantPreview()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const QRect bounds(0, 0, 400, 200);
    KisImageSP image = new KisImage(new KisSurrogateUndoStore(), bounds.width(), bounds.height(), cs, "lod move");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")));
    const QPointF firstPoint = layer->strokes().first()->jobs.first().pi1.pos();

    image->setLodPreferences(KisLodPreferences(2));
    image->waitForDone();
    {
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(QRect(0, 0, 200, 200), OPACITY_OPAQUE_U8);
        KisSetGlobalSelectionCommand command(image, selection);
        command.redo();
    }

    const QPoint offset(160, 40);
    MoveSelectionStrokeStrategy *strategy =
        new MoveSelectionStrokeStrategy(layer, image->globalSelection(), image.data(), image.data());
    KisStrokeId id = image->startStroke(strategy);
    image->addJob(id, new MoveStrokeStrategy::Data(offset));
    image->endStroke(id);
    image->waitForDone();

    QCOMPARE(layer->strokes().size(), 1);
    QCOMPARE(layer->strokes().first()->jobs.first().pi1.pos(), firstPoint + offset);
    QString difference;
    QVERIFY2(KisBrushStrokeLayer::strokesMatchPixels(layer->strokes(), layer->paintDevice(), &difference),
             qPrintable(difference));
}

/// The same for a duplicate (Ctrl-drag): the full stroke adds one moved
/// copy of the recorded stroke and keeps the original; undo removes it
void KisBrushStrokeLayerTransformTest::testSelectionDuplicateWithInstantPreview()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const QRect bounds(0, 0, 400, 200);
    KisSurrogateUndoStore *undoStore = new KisSurrogateUndoStore();
    KisImageSP image = new KisImage(undoStore, bounds.width(), bounds.height(), cs, "lod duplicate");
    KisBrushStrokeLayerSP layer = new KisBrushStrokeLayer(image, "strokes", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    paintStroke(image, layer, loadPreset(QStringLiteral("d)_Ink-3_Gpen.kpp")));
    const KisRecordedBrushStrokeSP original = layer->strokes().first();
    const QPointF firstPoint = original->jobs.first().pi1.pos();

    image->setLodPreferences(KisLodPreferences(2));
    image->waitForDone();
    {
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(QRect(0, 0, 200, 200), OPACITY_OPAQUE_U8);
        KisSetGlobalSelectionCommand command(image, selection);
        command.redo();
    }

    const QPoint offset(160, 40);
    MoveSelectionStrokeStrategy *strategy =
        new MoveSelectionStrokeStrategy(layer, image->globalSelection(), image.data(), image.data(), true);
    KisStrokeId id = image->startStroke(strategy);
    image->addJob(id, new MoveStrokeStrategy::Data(offset));
    image->endStroke(id);
    image->waitForDone();

    QCOMPARE(layer->strokes().size(), 2);
    QCOMPARE(layer->strokes().first(), original);
    QCOMPARE(layer->strokes().last()->jobs.first().pi1.pos(), firstPoint + offset);
    QString difference;
    QVERIFY2(KisBrushStrokeLayer::strokesMatchPixels(layer->strokes(), layer->paintDevice(), &difference),
             qPrintable(difference));

    undoStore->undo();
    image->waitForDone();
    QCOMPARE(layer->strokes().size(), 1);
    QCOMPARE(layer->strokes().first(), original);
    QVERIFY2(KisBrushStrokeLayer::strokesMatchPixels(layer->strokes(), layer->paintDevice(), &difference),
             qPrintable(difference));
}

SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES(KisBrushStrokeLayerTransformTest,
                                      QStringLiteral("Krita_4_Default_Resources.bundle"))

#include "KisBrushStrokeLayerTransformTest.moc"
