#include "SegmentationToolHelper.h"

#include "KisCursorOverrideLock.h"
#include "KisOptionButtonStrip.h"
#include "KoGroupButton.h"
#include "KoResourcePaths.h"
#include "SegmentationMatteUtils.h"
#include "canvas/kis_canvas2.h"
#include "canvas/kis_coordinates_converter.h"
#include "kis_command_utils.h"
#include "kis_default_bounds.h"
#include "kis_image_animation_interface.h"
#include "kis_paint_device.h"
#include "kis_selection.h"
#include "kis_selection_filters.h"
#include "kis_selection_tool_helper.h"

#include <QApplication>
#include <QDebug>
#include <QImage>
#include <QLibrary>
#include <QMessageBox>
#include <QPainter>
#include <QPolygonF>
#include <QRect>
#include <QThread>
#include <memory>

namespace
{

visp::i32x2 convert(QPoint const &point)
{
    return visp::i32x2{point.x(), point.y()};
}

visp::box_2d convert(QRect const &rect)
{
    return visp::box_2d{convert(rect.topLeft()), convert(rect.bottomRight())};
}

QRect imageBounds(QPoint offset, visp::i32x2 extent)
{
    return QRect(offset.x(), offset.y(), extent[0], extent[1]);
}

void adjustSelection(KisPixelSelectionSP const &selection, SegmentationToolHelper::SelectionOptions const &o)
{
    if (o.grow > 0) {
        KisGrowSelectionFilter biggy(o.grow, o.grow);
        biggy.process(selection, selection->selectedRect().adjusted(-o.grow, -o.grow, o.grow, o.grow));
    } else if (o.grow < 0) {
        KisShrinkSelectionFilter tiny(-o.grow, -o.grow, false);
        tiny.process(selection, selection->selectedRect());
    }
    if (o.feather > 0) {
        KisFeatherSelectionFilter feathery(o.feather);
        feathery.process(selection, selection->selectedRect().adjusted(-o.feather, -o.feather, o.feather, o.feather));
    } else if (o.antiAlias) {
        KisAntiAliasSelectionFilter antiAliasFilter;
        antiAliasFilter.process(selection, selection->selectedRect());
    }
}

} // namespace

bool operator==(SegmentationToolHelper::ImageInput const &a, SegmentationToolHelper::ImageInput const &b)
{
    return a.canvas == b.canvas && a.image == b.image && a.node == b.node && a.sampleLayersMode == b.sampleLayersMode
        && a.colorLabelsSelected == b.colorLabelsSelected;
}

bool operator!=(SegmentationToolHelper::ImageInput const &a, SegmentationToolHelper::ImageInput const &b)
{
    return !(a == b);
}

//
// SegmentationToolHelper

SegmentationToolHelper::SegmentationToolHelper(QSharedPointer<VisionModels> shared)
    : m_shared(std::move(shared))
{
}

void SegmentationToolHelper::processImage(ImageInput const &input, KisProcessingApplicator &applicator)
{
    KisPaintDeviceSP inputImage = selectPaintDevice(input, applicator);
    if (!inputImage) {
        return;
    }

    m_bounds = inputImage->exactBounds();

    if (m_mode == SegmentationMode::precise) {
        return; // No separate image processing step, everything happens in applySelectionMask.
    }

    KUndo2Command *cmd = new KisCommandUtils::LambdaCommand(
        [report = &m_errorReporter, inputImage, shared = m_shared.get()]() mutable -> KUndo2Command * {
            try {
                if (VisionMLImage image = VisionMLImage::prepare(*inputImage)) {
                    shared->encodeSegmentationImage(image.view);
                }
            } catch (const std::exception &e) {
                Q_EMIT report->errorOccurred(QString(e.what()));
            }
            return nullptr;
        });

    applicator.applyCommand(cmd, KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE);

    m_lastInput = input;
    m_requiresUpdate = false;
}

void SegmentationToolHelper::processImage(ImageInput const &input)
{
    KisProcessingApplicator applicator(input.image,
                                       input.node,
                                       KisProcessingApplicator::NO_IMAGE_UPDATES, // XXX
                                       KisImageSignalVector(),
                                       kundo2_i18n("Select Segment"));
    processImage(input, applicator);
    applicator.end();
}

void SegmentationToolHelper::applySelectionMask(ImageInput const &input,
                                                QVariant prompt,
                                                SelectionOptions const &options)
{
    KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2 *>(input.canvas);
    if (!kisCanvas) {
        return;
    }

    auto cursorLock = std::shared_ptr<KisCursorOverrideLock>(
        new KisCursorOverrideLock(KisCursor::waitCursor()),
        [](KisCursorOverrideLock *lock) {
            if (QThread::currentThread() == qApp->thread()) {
                delete lock;
            } else {
                QMetaObject::invokeMethod(qApp, [lock]() {
                    delete lock;
                });
            }
        });
    KisProcessingApplicator applicator(input.image,
                                       input.node,
                                       KisProcessingApplicator::NO_IMAGE_UPDATES, // XXX
                                       KisImageSignalVector(),
                                       kundo2_i18n("Select Segment"));

    KisPaintDeviceSP inputImage = selectPaintDevice(input, applicator);
    if (!inputImage) {
        applicator.cancel();
        return;
    }

    const bool isColorLabeledMode = (input.sampleLayersMode == KisToolSelect::SampleColorLabeledLayers);
    if (m_mode == SegmentationMode::fast) {
        if (m_requiresUpdate || !m_shared->hasSegmentationImage() || input != m_lastInput) {
            processImage(input, applicator);
        }
    } else { // SegmentationMode::precise
        m_bounds = isColorLabeledMode
            ? (input.image ? input.image->bounds() : (input.node ? input.node->exactBounds() : QRect()))
            : inputImage->exactBounds();
    }

    KisSelectionToolHelper helper(kisCanvas, kundo2_i18n("Segment Selection"));
    bool noWork = false;

    if (prompt.canConvert<QRect>()) {
        QRect region = prompt.toRect().intersected(m_bounds);
        region.translate(-m_bounds.topLeft());
        noWork |= helper.tryDeselectCurrentSelection(QRectF(region), options.action);
        noWork |= helper.canShortcutToNoop(region, options.action);
        noWork |= region.isEmpty();
    } else if (prompt.canConvert<QPoint>()) {
        QPoint point = prompt.toPoint();
        noWork |= !m_bounds.contains(point, true);
    }
    if (noWork) {
        applicator.cancel();
        return;
    }

    KisPixelSelectionSP selection = new KisPixelSelection(new KisSelectionDefaultBounds(inputImage));
    const KisStrokeId strokeId = applicator.getStroke();
    KisImageSP image = input.image;

    KUndo2Command *cmd = new KisCommandUtils::LambdaCommand([mode = m_mode,
                                                             shared = m_shared.get(),
                                                             report = &m_errorReporter,
                                                             inputImage,
                                                             image,
                                                             strokeId,
                                                             bounds = m_bounds,
                                                             prompt,
                                                             selection,
                                                             options,
                                                             isColorLabeledMode,
                                                             cursorLock]() mutable -> KUndo2Command * {
        try {
            visp::image_data mask;
            if (mode == SegmentationMode::fast) {
                if (!shared->hasSegmentationImage()) {
                    if (image) {
                        image->cancelStroke(strokeId);
                    }
                    return nullptr; // Early out when there was no input image to process.
                }
                if (prompt.canConvert<QPoint>()) {
                    QPoint point = prompt.toPoint() - bounds.topLeft();
                    mask = shared->predictSegmentationMask(convert(point));
                } else {
                    QRect rect = prompt.toRect().intersected(bounds).translated(-bounds.topLeft());
                    mask = shared->predictSegmentationMask(convert(rect));
                }
                selection->writeBytes(mask.data.get(), imageBounds(bounds.topLeft(), mask.extent));
            } else {
                QRect rect = prompt.toRect().intersected(bounds);
                if (isColorLabeledMode) {
                    rect = rect.intersected(inputImage->exactBounds());
                }
                VisionMLImage img = VisionMLImage::prepare(*inputImage, rect);
                if (!img) {
                    if (image) {
                        image->cancelStroke(strokeId);
                    }
                    return nullptr;
                }
                mask = shared->removeBackground(img.view);
                selection->writeBytes(mask.data.get(), rect);
            }
            adjustSelection(selection, options);
            selection->invalidateOutlineCache();
        } catch (const std::exception &e) {
            Q_EMIT report->errorOccurred(QString(e.what()));
            if (image) {
                image->cancelStroke(strokeId);
            }
        }
        return nullptr;
    });
    applicator.applyCommand(cmd, KisStrokeJobData::SEQUENTIAL);

    helper.selectPixelSelection(applicator, selection, options.action);

    applicator.applyCommand(new KisCommandUtils::LambdaCommand([cursorLock]() mutable -> KUndo2Command * {
        cursorLock.reset();
        return nullptr;
    }), KisStrokeJobData::SEQUENTIAL);

    applicator.end();
}

void SegmentationToolHelper::applyMattingMask(ImageInput const &input,
                                              QPolygonF const &polygon,
                                              SelectionOptions const &options)
{
    static constexpr int mattingPadding = 16;

    KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2 *>(input.canvas);
    if (!kisCanvas) {
        return;
    }

    // C6: tryDeselectCurrentSelection takes view coordinates of the unpadded lasso outline.
    const QRectF unpaddedBBox = polygon.boundingRect();
    const QRectF viewRect = kisCanvas->coordinatesConverter()->imageToWidget(unpaddedBBox);
    KisSelectionToolHelper helper(kisCanvas, kundo2_i18n("Smart Matting Selection"));
    if (helper.tryDeselectCurrentSelection(viewRect, options.action)) {
        return;
    }

    if (unpaddedBBox.isEmpty()) {
        return;
    }

    // C3: Keep wait cursor active throughout stroke execution using a shared_ptr released on stroke completion.
    auto cursorLock = std::shared_ptr<KisCursorOverrideLock>(
        new KisCursorOverrideLock(KisCursor::waitCursor()),
        [](KisCursorOverrideLock *lock) {
            if (QThread::currentThread() == qApp->thread()) {
                delete lock;
            } else {
                QMetaObject::invokeMethod(qApp, [lock]() {
                    delete lock;
                });
            }
        });

    KisProcessingApplicator applicator(input.image,
                                       input.node,
                                       KisProcessingApplicator::NO_IMAGE_UPDATES, // XXX
                                       KisImageSignalVector(),
                                       kundo2_i18n("Smart Matting Selection"));

    KisPaintDeviceSP inputImage = selectPaintDevice(input, applicator);
    if (!inputImage) {
        applicator.cancel();
        return;
    }

    // C1: In SampleColorLabeledLayers mode, the merge command is queued asynchronously,
    // so exactBounds() on the unrendered device is empty on the GUI thread.
    // Use image bounds (or node bounds) on the GUI thread, and perform exactBounds clipping inside the stroke lambda.
    const bool isColorLabeledMode = (input.sampleLayersMode == KisToolSelect::SampleColorLabeledLayers);
    const QRect bounds = isColorLabeledMode
        ? (input.image ? input.image->bounds() : (input.node ? input.node->exactBounds() : QRect()))
        : inputImage->exactBounds();

    // C4: Enforce minimum layer bounds of 64x64. If smaller, report error and return without touching undo.
    if (bounds.width() < 64 || bounds.height() < 64) {
        Q_EMIT m_errorReporter.errorOccurred(i18n("Image or layer is too small for smart matting (minimum 64x64 pixels)."));
        applicator.cancel();
        return;
    }

    QRect region = unpaddedBBox.toAlignedRect()
                       .adjusted(-mattingPadding, -mattingPadding, mattingPadding, mattingPadding)
                       .intersected(bounds);

    if (region.isEmpty() || helper.canShortcutToNoop(region, options.action)) {
        applicator.cancel();
        return;
    }

    // C4: Enforce min 64x64 by expanding region within bounds.
    VisionMLMatte::expandRegionToMin(region, bounds, 64);

    // C4: Cap crop size to 3072 clamped around the center, emitting a warning if clamped.
    const bool clamped = VisionMLMatte::clampRegionToMax(region, bounds, 3072);
    if (clamped) {
        Q_EMIT m_errorReporter.errorOccurred(i18n("Selection region exceeds 3072 pixels and was clamped to 3072 pixels around the selection center."));
    }

    KisPixelSelectionSP selection = new KisPixelSelection(new KisSelectionDefaultBounds(inputImage));
    const KisStrokeId strokeId = applicator.getStroke();
    KisImageSP image = input.image;

    KUndo2Command *cmd = new KisCommandUtils::LambdaCommand([shared = m_shared.get(),
                                                           report = &m_errorReporter,
                                                           inputImage,
                                                           image,
                                                           strokeId,
                                                           region,
                                                           polygon,
                                                           selection,
                                                           options,
                                                           isColorLabeledMode,
                                                           cursorLock]() mutable -> KUndo2Command * {
        try {
            QRect effectiveRegion = region;
            // C1: In SampleColorLabeledLayers mode, lazily clip to rendered exactBounds
            if (isColorLabeledMode) {
                const QRect mergedBounds = inputImage->exactBounds();
                if (mergedBounds.width() < 64 || mergedBounds.height() < 64) {
                    Q_EMIT report->errorOccurred(i18n("Merged color-labeled layers are too small for smart matting (minimum 64x64 pixels)."));
                    if (image) {
                        image->cancelStroke(strokeId);
                    }
                    return nullptr;
                }
                effectiveRegion = effectiveRegion.intersected(mergedBounds);
                if (effectiveRegion.isEmpty()) {
                    if (image) {
                        image->cancelStroke(strokeId);
                    }
                    return nullptr;
                }
                VisionMLMatte::expandRegionToMin(effectiveRegion, mergedBounds, 64);
            }

            VisionMLImage inputImg = VisionMLImage::prepare(*inputImage, effectiveRegion);
            if (!inputImg) {
                // C5: Avoid committing empty undo step on failure
                if (image) {
                    image->cancelStroke(strokeId);
                }
                return nullptr;
            }
            visp::image_data matte = shared->removeBackground(inputImg.view);

            // Rasterize the lasso outline: the matte is clipped to the drawn
            // region and to non-transparent pixels of the source image.
            QImage regionMask(effectiveRegion.size(), QImage::Format_Grayscale8);
            regionMask.fill(0);
            {
                QPainter painter(&regionMask);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                painter.setBrush(Qt::white);
                painter.drawPolygon(polygon.translated(-QPointF(effectiveRegion.topLeft())));
            }

            QRect maskRect(effectiveRegion.topLeft(), QSize(matte.extent[0], matte.extent[1]));
            if (matte.extent[0] != effectiveRegion.width() || matte.extent[1] != effectiveRegion.height()) {
                // Unexpected output size: fall back to the raw matte.
                selection->writeBytes(matte.data.get(), maskRect);
            } else {
                QByteArray bytes(maskRect.width() * maskRect.height(), 0);
                const uchar *matteData = matte.data.get();
                for (int y = 0; y < maskRect.height(); ++y) {
                    const uchar *maskRow = regionMask.constScanLine(y);
                    const uchar *srcRow = inputImg.data.constScanLine(y);
                    uchar *outRow = reinterpret_cast<uchar *>(bytes.data()) + y * maskRect.width();
                    for (int x = 0; x < maskRect.width(); ++x) {
                        const quint8 sourceAlpha = srcRow[x * 4 + 3]; // ARGB32 stores BGRA bytes
                        const quint8 maskVal = maskRow[x];
                        const quint8 matteVal = matteData[y * maskRect.width() + x];
                        // C2: Multiply matte * mask * alpha / 255^2 with proper rounding
                        outRow[x] = VisionMLMatte::modulateMatteAlpha(matteVal, maskVal, sourceAlpha);
                    }
                }
                selection->writeBytes(reinterpret_cast<const quint8 *>(bytes.constData()), maskRect);
            }
            adjustSelection(selection, options);
            selection->invalidateOutlineCache();
        } catch (const std::exception &e) {
            // C5: Report error and cancel stroke to avoid committing empty undo step
            Q_EMIT report->errorOccurred(QString(e.what()));
            if (image) {
                image->cancelStroke(strokeId);
            }
        }
        return nullptr;
    });
    applicator.applyCommand(cmd, KisStrokeJobData::SEQUENTIAL);

    helper.selectPixelSelection(applicator, selection, options.action);

    // C3: Finalizing command holds cursorLock alive until all previous commands finish
    applicator.applyCommand(new KisCommandUtils::LambdaCommand([cursorLock]() mutable -> KUndo2Command * {
        cursorLock.reset();
        return nullptr;
    }), KisStrokeJobData::SEQUENTIAL);

    applicator.end();
}

KisPaintDeviceSP SegmentationToolHelper::selectPaintDevice(ImageInput const &input, KisProcessingApplicator &applicator)
{
    KisPaintDeviceSP layerImage;
    if (!input.node || !(layerImage = input.node->projection())) {
        return nullptr;
    }

    KisPaintDeviceSP inputImage;
    switch (input.sampleLayersMode) {
    case KisToolSelect::SampleAllLayers:
        inputImage = input.image->projection();
        break;
    case KisToolSelect::SampleCurrentLayer:
        inputImage = layerImage;
        break;
    case KisToolSelect::SampleColorLabeledLayers:
        inputImage = mergeColorLayers(input.image, input.colorLabelsSelected, applicator);
        break;
    }
    return inputImage;
}

KisPaintDeviceSP SegmentationToolHelper::mergeColorLayers(KisImageSP const &image,
                                                          QList<int> const &selectedLayers,
                                                          KisProcessingApplicator &applicator)
{
    if (!m_referenceNodeList) {
        m_referencePaintDevice =
            KisMergeLabeledLayersCommand::createRefPaintDevice(image, "Segmentation Tool Reference");
        m_referenceNodeList.reset(new KisMergeLabeledLayersCommand::ReferenceNodeInfoList);
    }
    KisPaintDeviceSP newReferencePaintDevice =
        KisMergeLabeledLayersCommand::createRefPaintDevice(image, "Segmentation Tool Reference");
    KisMergeLabeledLayersCommand::ReferenceNodeInfoListSP newReferenceNodeList(
        new KisMergeLabeledLayersCommand::ReferenceNodeInfoList);
    const int currentTime = image->animationInterface()->currentTime();
    applicator.applyCommand(
        new KisMergeLabeledLayersCommand(image,
                                         m_referenceNodeList,
                                         newReferenceNodeList,
                                         m_referencePaintDevice,
                                         newReferencePaintDevice,
                                         selectedLayers,
                                         KisMergeLabeledLayersCommand::GroupSelectionPolicy_SelectIfColorLabeled,
                                         m_previousTime != currentTime),
        KisStrokeJobData::SEQUENTIAL,
        KisStrokeJobData::EXCLUSIVE);
    m_referencePaintDevice = newReferencePaintDevice;
    m_referenceNodeList = newReferenceNodeList;
    m_previousTime = currentTime;
    return m_referencePaintDevice;
}

void SegmentationToolHelper::deactivate()
{
    m_referencePaintDevice = nullptr;
    m_referenceNodeList = nullptr;
    m_shared->unload(VisionMLTask::segmentation);
}

void SegmentationToolHelper::addOptions(KisSelectionOptions *selectionWidget, bool showMode)
{
    if (showMode) {
        KisOptionButtonStrip *modeSelect = new KisOptionButtonStrip;
        m_modeFastButton = modeSelect->addButton(i18n("Fast"));
        m_modeFastButton->setChecked(m_mode == SegmentationMode::fast);
        m_modePreciseButton = modeSelect->addButton(i18n("Precise"));
        m_modePreciseButton->setChecked(m_mode == SegmentationMode::precise);

        KisOptionCollectionWidgetWithHeader *segmentationModeSection =
            new KisOptionCollectionWidgetWithHeader(i18n("Mode"));
        segmentationModeSection->setPrimaryWidget(modeSelect);
        selectionWidget->insertWidget(2, "segmentationModeSection", segmentationModeSection);

        connect(modeSelect,
                SIGNAL(buttonToggled(KoGroupButton *, bool)),
                this,
                SLOT(switchMode(KoGroupButton *, bool)));
    }

    VisionMLBackendWidget *backendSelect = new VisionMLBackendWidget(m_shared);
    selectionWidget->insertWidget(3, "segmentationBackendSection", backendSelect);
}

void SegmentationToolHelper::switchMode(KoGroupButton *button, bool checked)
{
    if (checked) {
        m_mode = button == m_modeFastButton ? SegmentationMode::fast : SegmentationMode::precise;
        m_requiresUpdate = true;
    }
}
