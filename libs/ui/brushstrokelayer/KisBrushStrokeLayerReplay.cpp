/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisBrushStrokeLayer.h"
#include "KisBrushStrokeLayerPrivate.h"

#include <QtMath>

#include <KisAsynchronousStrokeUpdateHelper.h>
#include <KoCanvasResourceProvider.h>
#include <kis_distance_information.h>
#include <kis_image.h>
#include <kis_paintop_preset.h>
#include <kis_paintop_settings.h>
#include <kis_resources_snapshot.h>
#include <strokes/KisFreehandStrokeInfo.h>
#include <strokes/freehand_stroke.h>

using KisBrushStrokeLayerPrivate::lengthScale;

/// The scale a transform applies to lengths (the square root of its area
/// scale)
qreal KisBrushStrokeLayerPrivate::lengthScale(const QTransform &transform)
{
    return qSqrt(qAbs(transform.determinant()));
}

namespace
{
/**
 * Keeps a stroke's brush texture under the stroke when the stroke is drawn
 * elsewhere: points move by @p shift, then by @p transform (scaling and
 * moving). The texture is anchored to the canvas, not to the stroke: the
 * pattern pixel under image point X is X - offset, with the pattern scaled
 * by its scale (KisTextureOption). Without this a moved layer or selection
 * draws its strokes again over another part of the pattern. A random
 * offset (chosen per stroke) cannot follow.
 */
void mapTexture(KisPaintOpSettingsSP settings, const QTransform &transform, const QPointF &shift)
{
    if (!settings->hasProperty("Texture/Pattern/Scale")) {
        return;
    }
    const qreal scale = lengthScale(transform);
    if (!qFuzzyCompare(scale, 1.0)) {
        settings->setProperty("Texture/Pattern/Scale", settings->getDouble("Texture/Pattern/Scale") * scale);
    }
    // where the canvas origin of the stroke goes
    const QPointF origin = transform.map(shift);
    settings->setProperty("Texture/Pattern/OffsetX",
                          qRound(transform.m11() * settings->getInt("Texture/Pattern/OffsetX") + origin.x()));
    settings->setProperty("Texture/Pattern/OffsetY",
                          qRound(transform.m22() * settings->getInt("Texture/Pattern/OffsetY") + origin.y()));
}

/// A direction (radians) after @p transform
qreal mappedAngle(const QTransform &transform, qreal angle)
{
    const QPointF direction = transform.map(QPointF(qCos(angle), qSin(angle))) - transform.map(QPointF());
    return qAtan2(direction.y(), direction.x());
}

void drawStroke(KisImageSP image,
                KisPaintLayerSP layer,
                const KisRecordedBrushStroke &stroke,
                const QPointF &offset,
                const QTransform &transform)
{
    const qreal scale = lengthScale(transform);
    auto map = [&](const QPointF &point) {
        return transform.map(point + offset);
    };

    KisPaintOpPresetSP preset = stroke.preset->clone().dynamicCast<KisPaintOpPreset>();
    KisPaintOpSettingsSP settings = preset->settings();
    if (!qFuzzyCompare(scale, 1.0)) {
        settings->setPaintOpSize(settings->paintOpSize() * scale);
    }
    mapTexture(settings, transform, offset);

    KoCanvasResourceProvider provider;
    provider.setResource(KoCanvasResource::ForegroundColor, QVariant::fromValue(stroke.fgColor));
    provider.setResource(KoCanvasResource::BackgroundColor, QVariant::fromValue(stroke.bgColor));
    provider.setResource(KoCanvasResource::CurrentPaintOpPreset, QVariant::fromValue(preset));
    provider.setResource(KoCanvasResource::Opacity, stroke.opacity);
    provider.setResource(KoCanvasResource::CurrentCompositeOp, stroke.compositeOpId);
    provider.setResource(KoCanvasResource::CurrentEffectiveCompositeOp, stroke.compositeOpId);
    provider.setResource(KoCanvasResource::EffectiveZoom, stroke.effectiveZoom);
    provider.setResource(KoCanvasResource::HdrExposure, 0.0);
    provider.setResource(KoCanvasResource::EraserMode, false);
    provider.setResource(KoCanvasResource::GlobalAlphaLock, false);
    provider.setResource(KoCanvasResource::MirrorHorizontal, stroke.mirrorHorizontal);
    provider.setResource(KoCanvasResource::MirrorVertical, stroke.mirrorVertical);
    provider.setResource(KoCanvasResource::EffectiveLodAvailability, false);
    provider.setResource(KoCanvasResource::Size, settings->paintOpSize());
    if (stroke.pattern) {
        provider.setResource(KoCanvasResource::CurrentPattern, QVariant::fromValue(stroke.pattern));
    }
    if (stroke.gradient) {
        provider.setResource(KoCanvasResource::CurrentGradient, QVariant::fromValue(stroke.gradient));
    }

    KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, layer, &provider, nullptr, {}, preset);
    resources->setOpacity(stroke.opacity);
    // the axes are where they were in the image, moved with the stroke
    resources->setMirroring(stroke.mirrorHorizontal, stroke.mirrorVertical, map(stroke.mirrorAxesCenter));
    resources->setFGColorOverride(stroke.fgColor);
    resources->setBGColorOverride(stroke.bgColor);

    // each copy's dab spacing starts as it did when the stroke was drawn
    QVector<KisFreehandStrokeInfo *> strokeInfos;
    for (int i = 0; i < qMax(1, stroke.strokeInfoCount); i++) {
        const KisRecordedBrushStroke::Start start =
            i < stroke.starts.size() ? stroke.starts[i] : KisRecordedBrushStroke::Start();
        if (start.hasLastDab) {
            strokeInfos << new KisFreehandStrokeInfo(KisDistanceInformation(map(start.lastPosition),
                                                                            mappedAngle(transform, start.lastAngle),
                                                                            start.spacingUpdateInterval,
                                                                            start.timingUpdateInterval,
                                                                            start.dabSeqNo));
        } else {
            strokeInfos << new KisFreehandStrokeInfo(
                KisDistanceInformation(start.spacingUpdateInterval, start.timingUpdateInterval, start.dabSeqNo));
        }
    }
    FreehandStrokeStrategy *strategy =
        new FreehandStrokeStrategy(resources, strokeInfos, kundo2_noi18n("redraw brush stroke"));
    strategy->setPreviewRandomSeed(stroke.seed);
    // as the stroke was drawn: its dabs one after another
    strategy->setSequentialDabRendering(true);
    KisStrokeId id = image->startStroke(strategy);

    for (const KisRecordedBrushStroke::Job &job : stroke.jobs) {
        KisPaintInformation pi1 = job.pi1;
        KisPaintInformation pi2 = job.pi2;
        pi1.setPos(map(pi1.pos()));
        pi2.setPos(map(pi2.pos()));
        switch (job.type) {
        case FreehandStrokeStrategy::Data::POINT:
            image->addJob(id, new FreehandStrokeStrategy::Data(job.strokeInfoId, pi1));
            break;
        case FreehandStrokeStrategy::Data::LINE:
            image->addJob(id, new FreehandStrokeStrategy::Data(job.strokeInfoId, pi1, pi2));
            break;
        case FreehandStrokeStrategy::Data::CURVE:
            image->addJob(
                id,
                new FreehandStrokeStrategy::Data(job.strokeInfoId, pi1, map(job.control1), map(job.control2), pi2));
            break;
        default:
            break;
        }
    }
    // the dabs are rendered asynchronously; an update job flushes them, as
    // the brush tool's update timer does
    image->addJob(id, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
    image->endStroke(id);
    image->waitForDone();
}
} // namespace

KisPaintDeviceSP KisBrushStrokeLayer::renderStrokes(const QVector<KisRecordedBrushStrokeSP> &strokes,
                                                    const KoColorSpace *colorSpace,
                                                    const QRect &imageBounds,
                                                    const QPoint &deviceOffset,
                                                    const QTransform &transform)
{
    const QRect bounds = transform.mapRect(QRectF(imageBounds)).toAlignedRect();
    KisImageSP image = new KisImage(nullptr,
                                    qMax(1, bounds.right() + 1),
                                    qMax(1, bounds.bottom() + 1),
                                    colorSpace,
                                    "brush stroke layer redraw");
    KisPaintLayerSP layer = new KisPaintLayer(image, "redraw", OPACITY_OPAQUE_U8, colorSpace);

    for (const KisRecordedBrushStrokeSP &stroke : strokes) {
        if (stroke && stroke->preset) {
            drawStroke(image, layer, *stroke, QPointF(deviceOffset - stroke->deviceOffset), transform);
        }
    }
    return layer->paintDevice();
}

QVector<KisRecordedBrushStrokeSP>
KisBrushStrokeLayer::transformedStrokes(const QVector<KisRecordedBrushStrokeSP> &strokes,
                                        const QPoint &deviceOffset,
                                        const QTransform &transform)
{
    const qreal scale = lengthScale(transform);
    QVector<KisRecordedBrushStrokeSP> result;
    for (const KisRecordedBrushStrokeSP &stroke : strokes) {
        QSharedPointer<KisRecordedBrushStroke> mapped(new KisRecordedBrushStroke(*stroke));
        const QPointF shift(deviceOffset - stroke->deviceOffset);
        auto map = [&](const QPointF &point) {
            return transform.map(point + shift);
        };
        for (KisRecordedBrushStroke::Job &job : mapped->jobs) {
            job.pi1.setPos(map(job.pi1.pos()));
            job.pi2.setPos(map(job.pi2.pos()));
            job.control1 = map(job.control1);
            job.control2 = map(job.control2);
        }
        for (KisRecordedBrushStroke::Start &start : mapped->starts) {
            if (start.hasLastDab) {
                start.lastPosition = map(start.lastPosition);
                start.lastAngle = mappedAngle(transform, start.lastAngle);
            }
        }
        mapped->mirrorAxesCenter = map(stroke->mirrorAxesCenter);
        mapped->deviceOffset = deviceOffset;

        mapped->preset = stroke->preset->clone().dynamicCast<KisPaintOpPreset>();
        KisPaintOpSettingsSP settings = mapped->preset->settings();
        // a move keeps the size as it is (setting it may round it)
        if (!qFuzzyCompare(scale, 1.0)) {
            settings->setPaintOpSize(settings->paintOpSize() * scale);
        }
        mapTexture(settings, transform, shift);
        result << mapped;
    }
    return result;
}
