/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SmartMattingTool.h"

#include "canvas/kis_canvas2.h"
#include "kis_command_utils.h"
#include "kis_paint_device.h"
#include "kis_pixel_selection.h"
#include "kis_selection_tool_helper.h"

SmartMattingOutlineTool::SmartMattingOutlineTool(KoCanvasBase *canvas)
    : KisToolOutlineBase(canvas, KisToolOutlineBase::SELECT, KisCursor::load("tool_smart_matting_cursor.png", 6, 6))
{
    setObjectName("tool_smart_matting");
}

SmartMattingTool::SmartMattingTool(KoCanvasBase *canvas, QSharedPointer<VisionModels> shared)
    : KisToolSelectBase<SmartMattingOutlineTool>(canvas, i18n("Smart Matting Selection"))
    , m_models(std::move(shared))
    , m_segmentation(m_models)
{
}

void SmartMattingTool::deactivate()
{
    m_segmentation.deactivate();
    m_models->unload(VisionMLTask::background_removal);
    Base::deactivate();
}

void SmartMattingTool::beginPrimaryAction(KoPointerEvent *event)
{
    Base::beginPrimaryAction(event);
}

void SmartMattingTool::beginShape()
{
    beginSelectInteraction();
}

void SmartMattingTool::endShape()
{
    endSelectInteraction();
}

void SmartMattingTool::finishOutline(const QVector<QPointF> &points)
{
    if (!selectionEditable() || points.size() < 3) {
        return;
    }

    SegmentationToolHelper::ImageInput input;
    input.canvas = canvas();
    input.image = currentImage();
    input.node = currentNode();
    input.sampleLayersMode = sampleLayersMode();
    input.colorLabelsSelected = colorLabelsSelected();

    SegmentationToolHelper::SelectionOptions options;
    options.action = selectionAction();
    options.grow = growSelection();
    options.feather = featherSelection();
    options.antiAlias = antiAliasSelection();

    m_segmentation.applyMattingMask(input, QPolygonF(points), options);
}

QWidget *SmartMattingTool::createOptionWidget()
{
    Base::createOptionWidget();
    KisSelectionOptions *selectionWidget = selectionOptionWidget();
    m_segmentation.addOptions(selectionWidget, false);
    VisionMLModelSelect *modelSelect = new VisionMLModelSelect(m_models, VisionMLTask::background_removal, true);
    selectionWidget->insertWidget(4, "smartMattingModelSection", modelSelect);
    return selectionWidget;
}

void SmartMattingTool::resetCursorStyle()
{
    if (selectionAction() == SELECTION_ADD) {
        useCursor(KisCursor::load("tool_smart_matting_cursor_add.png", 6, 6));
    } else if (selectionAction() == SELECTION_SUBTRACT) {
        useCursor(KisCursor::load("tool_smart_matting_cursor_sub.png", 6, 6));
    } else if (selectionAction() == SELECTION_INTERSECT) {
        useCursor(KisCursor::load("tool_smart_matting_cursor_inter.png", 6, 6));
    } else if (selectionAction() == SELECTION_SYMMETRICDIFFERENCE) {
        useCursor(KisCursor::load("tool_smart_matting_cursor_symdiff.png", 6, 6));
    } else {
        Base::resetCursorStyle();
    }
}
