/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_TOOL_REMOVE_DUST_H
#define KIS_TOOL_REMOVE_DUST_H

#include <KisDustDetector.h>
#include <KisSelectionTags.h>
#include <commands_new/KisMergeLabeledLayersCommand.h>
#include <kconfiggroup.h>
#include <kis_tool_shape.h>

#include "subtools/KisDynamicDelegatedTool.h"

class KisColorLabelSelectorWidget;
class KisDoubleSliderSpinBox;
class KisOptionButtonStrip;
class KisOptionCollectionWidget;
class KisOptionCollectionWidgetWithHeader;
class KisProcessingApplicator;
class KisSliderSpinBox;
class QCheckBox;
class QComboBox;

/**
 * Removes or selects small specks and holes inside an enclosed area. It reuses
 * the enclosing subtools of the Enclose and Fill Tool; the detection itself is
 * KisDustDetector.
 */
class KisToolRemoveDust : public KisDynamicDelegatedTool<KisToolShape>
{
    Q_OBJECT

public:
    enum Action {
        Remove,
        Select
    };

    enum EnclosingMethod {
        Rectangle,
        Ellipse,
        Path,
        Lasso,
        Brush
    };

    enum GapFill {
        SurroundingColor,
        ForegroundColor
    };

    enum Reference {
        CurrentLayer,
        AllLayers,
        ColorLabeledLayers
    };

    explicit KisToolRemoveDust(KoCanvasBase *canvas);
    ~KisToolRemoveDust() override;

    void beginPrimaryAction(KoPointerEvent *event) override;
    void activateAlternateAction(AlternateAction action) override;
    void deactivateAlternateAction(AlternateAction action) override;
    void beginAlternateAction(KoPointerEvent *event, AlternateAction action) override;
    void continueAlternateAction(KoPointerEvent *event, AlternateAction action) override;
    void endAlternateAction(KoPointerEvent *event, AlternateAction action) override;
    void beginAlternateDoubleClickAction(KoPointerEvent *event, AlternateAction action) override;

    int flags() const override;
    QWidget *createOptionWidget() override;

public Q_SLOTS:
    void activate(const QSet<KoShape *> &shapes) override;
    void deactivate() override;

protected Q_SLOTS:
    void resetCursorStyle() override;

private Q_SLOTS:
    void slotEnclosingMaskProduced(KisPixelSelectionSP enclosingMask);

private:
    bool wantsAutoScroll() const override
    {
        return false;
    }

    void loadConfiguration();
    void saveConfiguration();
    void setupEnclosingSubtool();
    bool subtoolHasUserInteractionRunning() const;
    void updateOptionVisibility();
    void resetOptions();
    KisPaintDeviceSP prepareReference(KisProcessingApplicator &applicator, KisNodeSP node);

    Action m_action{Remove};
    EnclosingMethod m_enclosingMethod{Rectangle};
    KisDustDetector::Mode m_mode{KisDustDetector::DustOnTransparency};
    GapFill m_gapFill{SurroundingColor};
    int m_dustSize{4};
    qreal m_tolerance{KisDustDetector::defaultTolerancePercent};
    SelectionAction m_selectionAction{SELECTION_REPLACE};
    Reference m_reference{CurrentLayer};
    QList<int> m_selectedColorLabels;
    bool m_useActiveLayer{false};

    KisPaintDeviceSP m_referencePaintDevice;
    KisMergeLabeledLayersCommand::ReferenceNodeInfoListSP m_referenceNodeList;
    int m_previousTime{0};

    bool m_alternateActionStarted{false};
    KConfigGroup m_configGroup;

    KisOptionCollectionWidget *m_optionWidget{nullptr};
    KisOptionButtonStrip *m_actionStrip{nullptr};
    KisOptionButtonStrip *m_enclosingStrip{nullptr};
    QComboBox *m_modeCombo{nullptr};
    KisOptionButtonStrip *m_gapFillStrip{nullptr};
    KisSliderSpinBox *m_sizeSlider{nullptr};
    KisDoubleSliderSpinBox *m_toleranceSlider{nullptr};
    KisOptionButtonStrip *m_selectionActionStrip{nullptr};
    KisOptionButtonStrip *m_referenceStrip{nullptr};
    KisColorLabelSelectorWidget *m_labelsWidget{nullptr};
    QCheckBox *m_useActiveLayerCheckBox{nullptr};
    KisOptionCollectionWidgetWithHeader *m_detectionSection{nullptr};
    KisOptionCollectionWidgetWithHeader *m_referenceSection{nullptr};
};

#endif // KIS_TOOL_REMOVE_DUST_H
