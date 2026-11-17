/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisBrushStrokeLayer.h"
#include "KisBrushStrokeLayerPrivate.h"

#include <QtMath>
#include <cmath>
#include <cstring>

#include <KisUsageLogger.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoCompositeOpRegistry.h>
#include <KoMixColorsOp.h>
#include <kis_command_utils.h>
#include <kis_debug.h>
#include <kis_global.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_painter.h>
#include <kis_paintop_preset.h>
#include <kis_paintop_settings.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_selection_filters.h>
#include <kis_sequential_iterator.h>
#include <kis_transaction.h>
#include <klocalizedstring.h>
#include <strokes/freehand_stroke.h>

using KisBrushStrokeLayerPrivate::lengthScale;
using KisBrushStrokeLayerPrivate::postImageMessage;

namespace
{
/// Whether pixels are compared in their own channels: floating point
/// spaces hold values beyond 0-1 (HDR, negative) that the 8-bit conversion
/// would clip; integer spaces are compared through 8 bits, within the
/// tolerance anyway
bool comparesNativeChannels(const KoColorSpace *cs)
{
    const KoID depth = cs->colorDepthId();
    return cs->alphaPos() >= 0
        && (depth == Float16BitsColorDepthID || depth == Float32BitsColorDepthID || depth == Float64BitsColorDepthID);
}

/// Whether two devices hold the same pixels across their combined bounds,
/// allowing a difference of 3/255 per channel; otherwise @p difference describes
/// how they differ. The colors are compared premultiplied by alpha: the
/// color of a transparent pixel does not show, and smudging brushes leave
/// it different from what drawing again gives. Compared in bands of rows,
/// so that a huge layer needs no image of its whole size.
bool sameContent(KisPaintDeviceSP a,
                 KisPaintDeviceSP b,
                 QString *difference,
                 KisPixelSelectionSP mismatch = nullptr,
                 qint64 *differingCount = nullptr,
                 qint64 *coveredCount = nullptr)
{
    // A deletion can shrink a's bounds. Pixels remaining only in the record
    // must still contribute to the mismatch mask, or a redraw restores them.
    const QRect rect = a->exactBounds() | b->exactBounds();
    const qint64 total = qint64(rect.width()) * rect.height();
    if (rect.isEmpty()) {
        *difference = QStringLiteral("both the layer and redrawn strokes are empty");
        if (differingCount) {
            *differingCount = 0;
        }
        if (coveredCount) {
            *coveredCount = 0;
        }
        return true;
    }
    const KoColorSpace *cs = a->colorSpace();
    const bool floating = comparesNativeChannels(cs);
    const int alpha = cs->alphaPos();
    QVector<float> channelsA(cs->channelCount());
    QVector<float> channelsB(cs->channelCount());
    qint64 differing = 0;
    qint64 covered = 0;
    qreal largest = 0;
    QRect area;
    // about 4M pixels per band
    const int bandHeight = int(qBound(qint64(1), (qint64(1) << 22) / rect.width(), qint64(rect.height())));
    QVector<quint8> selected;
    for (int bandTop = rect.y(); bandTop <= rect.bottom(); bandTop += bandHeight) {
        const QRect band(rect.x(), bandTop, rect.width(), qMin(bandHeight, rect.bottom() + 1 - bandTop));
        QImage first;
        QImage second;
        KisHLineConstIteratorSP iterA;
        KisHLineConstIteratorSP iterB;
        if (floating) {
            iterA = a->createHLineConstIteratorNG(band.x(), band.y(), band.width());
            iterB = b->createHLineConstIteratorNG(band.x(), band.y(), band.width());
        } else {
            first = a->convertToQImage(nullptr, band).convertToFormat(QImage::Format_ARGB32);
            second = b->convertToQImage(nullptr, band).convertToFormat(QImage::Format_ARGB32);
            if (first.size() != band.size() || second.size() != band.size()) {
                // out of memory: nothing can be said to match
                *difference = QStringLiteral("%1x%2 pixels could not be compared").arg(band.width()).arg(band.height());
                if (differingCount) {
                    *differingCount = total;
                }
                if (coveredCount) {
                    *coveredCount = total;
                }
                return false;
            }
        }
        bool bandDiffers = false;
        if (mismatch) {
            selected.fill(MIN_SELECTED, band.width() * band.height());
        }
        for (int y = 0; y < band.height(); y++) {
            const QRgb *lineA = floating ? nullptr : reinterpret_cast<const QRgb *>(first.constScanLine(y));
            const QRgb *lineB = floating ? nullptr : reinterpret_cast<const QRgb *>(second.constScanLine(y));
            for (int x = 0; x < band.width(); x++) {
                qreal channel = 0;
                if (floating) {
                    cs->normalisedChannelsValue(iterA->rawDataConst(), channelsA);
                    cs->normalisedChannelsValue(iterB->rawDataConst(), channelsB);
                    const qreal alphaA = channelsA[alpha];
                    const qreal alphaB = channelsB[alpha];
                    covered += qMax(alphaA, alphaB) > 3.0 / 255.0;
                    for (int c = 0; c < channelsA.size(); ++c) {
                        // Ignore the undefined color of transparent pixels, but
                        // retain HDR/negative values instead of clipping to RGB8.
                        const qreal p = c == alpha ? alphaA : (alphaA == 0 ? 0 : channelsA[c] * alphaA);
                        const qreal q = c == alpha ? alphaB : (alphaB == 0 ? 0 : channelsB[c] * alphaB);
                        const qreal delta = p == q ? 0 : qAbs(p - q) * 255.0;
                        channel = qMax(channel, std::isfinite(delta) ? delta : 255.0);
                    }
                    iterA->nextPixel();
                    iterB->nextPixel();
                } else {
                    const QRgb p = lineA[x];
                    const QRgb q = lineB[x];
                    const int alphaP = qAlpha(p);
                    const int alphaQ = qAlpha(q);
                    auto premultiplied = [](int channel, int alpha) {
                        return channel * alpha / 255;
                    };
                    channel = qMax(qMax(qAbs(alphaP - alphaQ),
                                        qAbs(premultiplied(qRed(p), alphaP) - premultiplied(qRed(q), alphaQ))),
                                   qMax(qAbs(premultiplied(qGreen(p), alphaP) - premultiplied(qGreen(q), alphaQ)),
                                        qAbs(premultiplied(qBlue(p), alphaP) - premultiplied(qBlue(q), alphaQ))));
                    covered += qMax(alphaP, alphaQ) > 3;
                }
                if (channel > 3) {
                    differing++;
                    largest = qMax(largest, channel);
                    area |= QRect(band.x() + x, band.y() + y, 1, 1);
                    if (mismatch) {
                        selected[y * band.width() + x] = MAX_SELECTED;
                        bandDiffers = true;
                    }
                }
            }
            if (floating) {
                iterA->nextRow();
                iterB->nextRow();
            }
        }
        if (bandDiffers) {
            mismatch->writeBytes(selected.constData(), band);
        }
    }
    if (differingCount) {
        *differingCount = differing;
    }
    if (coveredCount) {
        *coveredCount = covered;
    }
    *difference = QStringLiteral("%1 of %2 pixels differ, by up to %3, in %4,%5 %6x%7")
                      .arg(differing)
                      .arg(total)
                      .arg(largest)
                      .arg(area.x())
                      .arg(area.y())
                      .arg(area.width())
                      .arg(area.height());
    return differing == 0;
}

/// Replaces the layer's strokes; undo restores the previous ones. A redraw
/// writes pixels that are the record (@p recorded); otherwise the layer
/// checks the new record against its pixels.
class SetStrokesCommand : public KUndo2Command
{
public:
    SetStrokesCommand(KisBrushStrokeLayer *layer,
                      const QVector<KisRecordedBrushStrokeSP> &before,
                      const QVector<KisRecordedBrushStrokeSP> &after,
                      bool recorded)
        : m_layer(layer)
        , m_before(before)
        , m_after(after)
        , m_recorded(recorded)
    {
    }

    void redo() override
    {
        m_layer->setStrokes(m_after);
        if (m_recorded) {
            m_layer->markChangeRecorded();
        }
    }

    void undo() override
    {
        m_layer->setStrokes(m_before);
        if (m_recorded) {
            m_layer->markChangeRecorded();
        }
    }

private:
    KisBrushStrokeLayerSP m_layer;
    QVector<KisRecordedBrushStrokeSP> m_before;
    QVector<KisRecordedBrushStrokeSP> m_after;
    bool m_recorded;
};

enum class SelectionCoverage {
    Outside,
    Inside,
    Crossing,
};

/// Where @p stroke lies relative to @p mask (selection values 0-255): the
/// Move Tool cuts the selected part of each pixel, so a stroke moves whole
/// only if every pixel it covers is fully selected
SelectionCoverage selectionCoverage(const KisRecordedBrushStrokeSP &stroke,
                                    const QPoint &deviceOffset,
                                    KisPaintDeviceSP mask,
                                    const QRect &maskBounds,
                                    const KoColorSpace *colorSpace)
{
    // the dab centres, widened by the brush size (generously)
    const QPointF shift(deviceOffset - stroke->deviceOffset);
    QRectF centres;
    for (const KisRecordedBrushStroke::Job &job : stroke->jobs) {
        centres |= QRectF(job.pi1.pos() + shift, QSizeF(0.01, 0.01));
        centres |= QRectF(job.pi2.pos() + shift, QSizeF(0.01, 0.01));
        if (job.type == FreehandStrokeStrategy::Data::CURVE) {
            centres |= QRectF(job.control1 + shift, QSizeF(0.01, 0.01));
            centres |= QRectF(job.control2 + shift, QSizeF(0.01, 0.01));
        }
    }
    // and their mirror images (the canvas mirror)
    const QPointF axes = stroke->mirrorAxesCenter + shift;
    const QRectF own = centres;
    const qreal mirroredX = 2 * axes.x() - own.right();
    const qreal mirroredY = 2 * axes.y() - own.bottom();
    if (stroke->mirrorHorizontal) {
        centres |= QRectF(QPointF(mirroredX, own.y()), own.size());
    }
    if (stroke->mirrorVertical) {
        centres |= QRectF(QPointF(own.x(), mirroredY), own.size());
    }
    if (stroke->mirrorHorizontal && stroke->mirrorVertical) {
        centres |= QRectF(QPointF(mirroredX, mirroredY), own.size());
    }
    const qreal margin = stroke->preset->settings()->paintOpSize() + 4.0;
    const QRect rough = centres.adjusted(-margin, -margin, margin, margin).toAlignedRect();
    if (!rough.intersects(maskBounds)) {
        return SelectionCoverage::Outside;
    }
    bool allSelected = true;
    for (KisSequentialConstIterator it(mask, rough); allSelected && it.nextPixel();) {
        allSelected = *it.rawDataConst() == MAX_SELECTED;
    }
    if (allSelected) {
        return SelectionCoverage::Inside;
    }

    // near the edge: the pixels the stroke covers when drawn alone
    KisPaintDeviceSP drawn =
        KisBrushStrokeLayer::renderStrokes({stroke}, colorSpace, rough | QRect(0, 0, 1, 1), deviceOffset);
    const QRect area = drawn->exactBounds();
    if (area.isEmpty()) {
        return SelectionCoverage::Outside;
    }
    const QImage pixels = drawn->convertToQImage(nullptr, area).convertToFormat(QImage::Format_ARGB32);
    QVector<quint8> values(area.width() * area.height());
    mask->readBytes(values.data(), area);
    bool inside = true;
    bool outside = true;
    for (int y = 0; y < area.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(pixels.constScanLine(y));
        for (int x = 0; x < area.width(); x++) {
            const int alpha = qAlpha(line[x]);
            const int value = values[y * area.width() + x];
            // what the cut leaves behind or takes, within the redraw's
            // tolerance of 3
            inside = inside && alpha * (MAX_SELECTED - value) <= 3 * 255;
            outside = outside && alpha * value <= 3 * 255;
        }
    }
    return outside ? SelectionCoverage::Outside : (inside ? SelectionCoverage::Inside : SelectionCoverage::Crossing);
}

/// Sorts @p recorded into the strokes outside @p mask and those inside it;
/// false (with a warning naming @p action) if a stroke crosses its edge
bool splitBySelection(const KisBrushStrokeLayer *layer,
                      const QVector<KisRecordedBrushStrokeSP> &recorded,
                      KisPaintDeviceSP device,
                      KisPaintDeviceSP mask,
                      const QString &action,
                      QVector<KisRecordedBrushStrokeSP> *outside,
                      QVector<KisRecordedBrushStrokeSP> *inside)
{
    const QPoint deviceOffset(device->x(), device->y());
    const QRect maskBounds = mask->exactBounds();
    for (const KisRecordedBrushStrokeSP &stroke : recorded) {
        switch (selectionCoverage(stroke, deviceOffset, mask, maskBounds, device->colorSpace())) {
        case SelectionCoverage::Outside:
            *outside << stroke;
            break;
        case SelectionCoverage::Inside:
            *inside << stroke;
            break;
        case SelectionCoverage::Crossing: {
            const QString message =
                QStringLiteral(
                    "Brush stroke layer \"%1\": a recorded stroke crosses the edge of the %2 selection; "
                    "its record no longer matches the pixels")
                    .arg(layer->name())
                    .arg(action);
            warnKrita << message;
            KisUsageLogger::log(message);
            return false;
        }
        }
    }
    return true;
}
} // namespace

bool KisBrushStrokeLayer::strokesMatchPixels(const QVector<KisRecordedBrushStrokeSP> &strokes,
                                             KisPaintDeviceSP original,
                                             QString *difference)
{
    const QPoint offset(original->x(), original->y());
    const QRect bounds = original->exactBounds();
    KisPaintDeviceSP current = renderStrokes(strokes, original->colorSpace(), bounds | QRect(0, 0, 1, 1), offset);
    return sameContent(original, current, difference);
}

KUndo2Command *KisBrushStrokeLayer::createSelectionMoveCommand(KisPaintDeviceSP mask, const QPoint &offset)
{
    const QVector<KisRecordedBrushStrokeSP> recorded = strokes();
    KisPaintDeviceSP device = paintDevice();
    if (recorded.isEmpty() || !device || !mask || offset.isNull() || device->keyframeChannel()) {
        return nullptr;
    }
    QVector<KisRecordedBrushStrokeSP> staying;
    QVector<KisRecordedBrushStrokeSP> moving;
    if (!splitBySelection(this, recorded, device, mask, QStringLiteral("moved"), &staying, &moving)
        || moving.isEmpty()) {
        return nullptr;
    }
    // the moved pixels are pasted over the rest
    const QPoint deviceOffset(device->x(), device->y());
    const QVector<KisRecordedBrushStrokeSP> after =
        staying + transformedStrokes(moving, deviceOffset, QTransform::fromTranslate(offset.x(), offset.y()));
    return new SetStrokesCommand(this, recorded, after, false);
}

KUndo2Command *KisBrushStrokeLayer::createSelectionDuplicateCommand(KisPaintDeviceSP mask, const QPoint &offset)
{
    const QVector<KisRecordedBrushStrokeSP> recorded = strokes();
    KisPaintDeviceSP device = paintDevice();
    if (recorded.isEmpty() || !device || !mask || offset.isNull() || device->keyframeChannel()) {
        return nullptr;
    }
    QVector<KisRecordedBrushStrokeSP> outside;
    QVector<KisRecordedBrushStrokeSP> copied;
    if (!splitBySelection(this, recorded, device, mask, QStringLiteral("duplicated"), &outside, &copied)
        || copied.isEmpty()) {
        return nullptr;
    }
    // the originals stay; the copy is pasted over everything
    const QPoint deviceOffset(device->x(), device->y());
    const QVector<KisRecordedBrushStrokeSP> after =
        recorded + transformedStrokes(copied, deviceOffset, QTransform::fromTranslate(offset.x(), offset.y()));
    return new SetStrokesCommand(this, recorded, after, false);
}

KUndo2Command *KisBrushStrokeLayer::createTransformRedrawCommand(const QTransform &transform,
                                                                 KisPaintDeviceSP original,
                                                                 const PixelTransform &transformPixels)
{
    const QVector<KisRecordedBrushStrokeSP> recorded = strokes();
    KisPaintDeviceSP device = paintDevice();
    if (recorded.isEmpty() || !device || !original || device->keyframeChannel()) {
        return nullptr;
    }
    // scaling and moving only: a rotated or mirrored brush tip would not
    // match the transformed pixels
    if (transform.type() > QTransform::TxScale || transform.m11() <= 0.0 || transform.m22() <= 0.0) {
        return nullptr;
    }

    const QPoint offset(original->x(), original->y());
    const QRect bounds = original->exactBounds();
    const KoColorSpace *colorSpace = device->colorSpace();

    // where the layer holds just its recorded strokes
    KisPaintDeviceSP current = renderStrokes(recorded, colorSpace, bounds | QRect(0, 0, 1, 1), offset);
    KisPixelSelectionSP mismatch = new KisPixelSelection();
    QString difference;
    qint64 differing = 0;
    qint64 covered = 0;
    const bool same = sameContent(original, current, &difference, mismatch, &differing, &covered);
    // mostly not the record (or nothing to draw): the pixels are
    // transformed as on a paint layer
    if (!same && (bounds.isEmpty() || !transformPixels || differing * 2 > covered)) {
        const QString message = QStringLiteral(
                                    "Brush stroke layer \"%1\": its pixels are not its %2 recorded strokes (%3); "
                                    "transforming the pixels instead of drawing again")
                                    .arg(name())
                                    .arg(recorded.size())
                                    .arg(difference);
        warnKrita << message;
        KisUsageLogger::log(message);
        return nullptr;
    }

    const QVector<KisRecordedBrushStrokeSP> mapped = transformedStrokes(recorded, offset, transform);
    const QRect mappedBounds = transform.mapRect(QRectF(bounds)).toAlignedRect();
    KisPaintDeviceSP redrawn = renderStrokes(mapped, colorSpace, mappedBounds | QRect(0, 0, 1, 1), offset);

    if (!same) {
        // the part that is not the record keeps its pixels, transformed as
        // the caller transforms pixels and blended in at the edge; the
        // region grows to take in the antialiased edges around it
        const QRect mismatchRect = mismatch->selectedExactRect();
        KisGrowSelectionFilter(2, 2).process(mismatch, kisGrowRect(mismatchRect, 2));
        KisPaintDeviceSP pixels = transformPixels(original);
        KisPaintDeviceSP region = transformPixels(mismatch);

        KisSelectionSP selection = new KisSelection();
        const QRect regionRect = region->exactBounds();
        KisPainter::copyAreaOptimized(regionRect.topLeft(), region, selection->pixelSelection(), regionRect);
        const int feather = qMax(1, qRound(lengthScale(transform)));
        KisFeatherSelectionFilter(feather).process(selection->pixelSelection(), kisGrowRect(regionRect, feather));

        const QRect blendRect = selection->pixelSelection()->selectedExactRect();
        if (comparesNativeChannels(colorSpace)) {
            // Masked COMPOSITE_COPY can clamp HDR in its SIMD path. Mix in
            // native channels so the very edits we detected survive the blend.
            KisSequentialConstIterator src(pixels, blendRect);
            KisSequentialConstIterator mask(selection->pixelSelection(), blendRect);
            KisSequentialIterator dst(redrawn, blendRect);
            while (dst.nextPixel() && src.nextPixel() && mask.nextPixel()) {
                const quint8 amount = *mask.rawDataConst();
                if (amount == 255) {
                    std::memcpy(dst.rawData(), src.rawDataConst(), colorSpace->pixelSize());
                } else if (amount) {
                    const quint8 *colors[] = {dst.rawDataConst(), src.rawDataConst()};
                    const qint16 weights[] = {qint16(255 - amount), qint16(amount)};
                    colorSpace->mixColorsOp()->mixColors(colors, weights, 2, dst.rawData());
                }
            }
        } else {
            KisPainter gc(redrawn);
            gc.setCompositeOpId(COMPOSITE_COPY);
            gc.setSelection(selection);
            gc.bitBlt(blendRect.topLeft(), pixels, blendRect);
            gc.end();
        }

        const int percent = qMax(1, qRound(100.0 * differing / qMax(qint64(1), covered)));
        KisUsageLogger::log(QStringLiteral("Brush stroke layer \"%1\": drawn again except where its pixels are not "
                                           "its %2 recorded strokes (%3); that part is transformed as pixels")
                                .arg(name())
                                .arg(recorded.size())
                                .arg(difference));
        postImageMessage(image().toStrongRef().data(),
                         i18nc("floating message",
                               "\"%1\" was drawn again, except about %2% of it that no longer matches its recorded "
                               "brush strokes: that part was scaled as pixels.",
                               name(),
                               percent));
    }

    // the redraw's pixel changes, in undo and redo too, are the record's
    KisCommandUtils::CompositeCommand *command = new KisCommandUtils::CompositeCommand();
    command->setText(kundo2_i18n("Redraw Brush Strokes"));
    command->addCommand(new KisRecordedChangeScopeCommand(this, KisRecordedChangeScopeCommand::Opening));
    command->addCommand(new SetStrokesCommand(this, recorded, mapped, true));

    KisTransaction transaction(device);
    device->clear();
    const QRect redrawnRect = redrawn->extent();
    if (!redrawnRect.isEmpty()) {
        KisPainter::copyAreaOptimized(redrawnRect.topLeft(), redrawn, device, redrawnRect);
    }
    command->addCommand(transaction.endAndTake());
    command->addCommand(new KisRecordedChangeScopeCommand(this, KisRecordedChangeScopeCommand::Closing));
    beginRecordedChange();
    setDirty();
    endRecordedChange();

    return command;
}

KUndo2Command *KisBrushStrokeLayer::createRerenderCommand()
{
    const QVector<KisRecordedBrushStrokeSP> recorded = strokes();
    KisPaintDeviceSP device = paintDevice();
    if (recorded.isEmpty() || !device || device->keyframeChannel()) {
        return nullptr;
    }

    const QPoint offset(device->x(), device->y());
    // a clipped or cut stroke reaches outside the pixels the layer has
    // now: cover the image and the device's extent
    KisImageSP ownerImage = image().toStrongRef();
    const QRect bounds = (ownerImage ? ownerImage->bounds() : QRect()) | device->exactBounds();
    KisPaintDeviceSP redrawn = renderStrokes(recorded, device->colorSpace(), bounds | QRect(0, 0, 1, 1), offset);

    KisCommandUtils::CompositeCommand *command = new KisCommandUtils::CompositeCommand();
    command->setText(kundo2_i18n("Re-render Brush Strokes"));

    KisTransaction transaction(device);
    device->clear();
    const QRect redrawnRect = redrawn->extent();
    if (!redrawnRect.isEmpty()) {
        KisPainter::copyAreaOptimized(redrawnRect.topLeft(), redrawn, device, redrawnRect);
    }
    command->addCommand(transaction.endAndTake());
    // no markChangeRecorded(): the background check finds that the pixels
    // are the record again and takes down the out-of-sync warning
    setDirty();

    return command;
}

KUndo2Command *KisBrushStrokeLayer::createForgetStrokesCommand()
{
    const QVector<KisRecordedBrushStrokeSP> recorded = strokes();
    if (recorded.isEmpty()) {
        return nullptr;
    }
    KisCommandUtils::CompositeCommand *command = new KisCommandUtils::CompositeCommand();
    command->setText(kundo2_i18n("Forget Recorded Strokes"));
    command->addCommand(new SetStrokesCommand(this, recorded, {}, false));
    return command;
}
