/*
 *  SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_TOOL_THROUGH_LAYER_ERASER_H
#define KIS_TOOL_THROUGH_LAYER_ERASER_H

#include "kis_tool_brush.h"

class QCheckBox;
class KisToolFreehandHelper;

/**
 * Erases with the current brush through the layers of the image, or of the
 * selected layers (docs/agent/through-layer-eraser.md).
 */
class KisToolThroughLayerEraser : public KisToolBrush
{
    Q_OBJECT
public:
    KisToolThroughLayerEraser(KoCanvasBase *canvas);
    ~KisToolThroughLayerEraser() override;

    void beginPrimaryAction(KoPointerEvent *event) override;

protected:
    QWidget *createOptionWidget() override;

private Q_SLOTS:
    void setSelectedLayersOnly(bool value);
    void setEraseAlphaLocked(bool value);

private:
    KisToolFreehandHelper *m_helper{nullptr};
    QCheckBox *m_chkSelectedLayersOnly{nullptr};
    QCheckBox *m_chkEraseAlphaLocked{nullptr};
    bool m_selectedLayersOnly{false};
    bool m_eraseAlphaLocked{true};
};

class KisToolThroughLayerEraserFactory : public KisToolBrushFactory
{
public:
    KisToolThroughLayerEraserFactory()
        : KisToolBrushFactory("KritaShape/KisToolThroughLayerEraser")
    {
        setToolTip(i18n("Through-Layer Eraser Tool"));

        setSection(ToolBoxSection::Shape);
        setIconName(koIconNameCStr("krita_tool_through_layer_eraser"));
        setPriority(12);
        setActivationShapeId(KRITA_TOOL_ACTIVATION_ID);
    }

    ~KisToolThroughLayerEraserFactory() override
    {
    }

    KoToolBase *createTool(KoCanvasBase *canvas) override
    {
        return new KisToolThroughLayerEraser(canvas);
    }
};

#endif // KIS_TOOL_THROUGH_LAYER_ERASER_H
