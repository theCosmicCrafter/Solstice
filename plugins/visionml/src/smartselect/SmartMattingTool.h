/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMART_MATTING_TOOL_H_
#define SMART_MATTING_TOOL_H_

#include "../segmentation/SegmentationToolHelper.h"
#include "KisSelectionToolFactoryBase.h"
#include "KisToolOutlineBase.h"
#include "kis_tool_select_base.h"
#include <kis_icon.h>

class SmartMattingOutlineTool : public KisToolOutlineBase
{
    Q_OBJECT
public:
    SmartMattingOutlineTool(KoCanvasBase *canvas);
};

// Lasso-style selection tool that refines the outlined region with a
// BiRefNet matting model (soft alpha edges) instead of the point/box
// segmentation models. Solstice-native port of the Smart Select workflow.
class SmartMattingTool : public KisToolSelectBase<SmartMattingOutlineTool>
{
    Q_OBJECT
public:
    using Base = KisToolSelectBase<SmartMattingOutlineTool>;

    explicit SmartMattingTool(KoCanvasBase *canvas, QSharedPointer<VisionModels>);
    QWidget *createOptionWidget() override;
    void resetCursorStyle() override;

    void beginPrimaryAction(KoPointerEvent *) override;

private:
    void finishOutline(const QVector<QPointF> &points) override;
    void beginShape() override;
    void endShape() override;

public Q_SLOTS:
    void deactivate() override;

protected:
    bool wantsAutoScroll() const override
    {
        return false;
    }

    bool isPixelOnly() const override
    {
        return true;
    }

    bool usesColorLabels() const override
    {
        return true;
    }

private:
    QSharedPointer<VisionModels> m_models;
    SegmentationToolHelper m_segmentation;
};

class SmartMattingToolFactory : public KisSelectionToolFactoryBase
{
public:
    SmartMattingToolFactory(QSharedPointer<VisionModels> shared)
        : KisSelectionToolFactoryBase("SmartMattingTool")
        , m_shared(std::move(shared))
    {
        setToolTip(i18n("Smart Matting Selection"));
        setSection(ToolBoxSection::Select);
        setIconName(koIconNameCStr("tool_smart_matting"));
        setPriority(11);
        setActivationShapeId(KRITA_TOOL_ACTIVATION_ID);
    }

    KoToolBase *createTool(KoCanvasBase *canvas) override
    {
        return new SmartMattingTool(canvas, m_shared);
    }

private:
    QSharedPointer<VisionModels> m_shared;
};

#endif // SMART_MATTING_TOOL_H_
