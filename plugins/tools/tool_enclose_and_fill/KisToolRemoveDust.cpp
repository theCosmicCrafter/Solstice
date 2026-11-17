/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisToolRemoveDust.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QPointer>
#include <QPushButton>
#include <functional>

#include <KisOptionButtonStrip.h>
#include <KisOptionCollectionWidget.h>
#include <KisViewManager.h>
#include <KoCanvasBase.h>
#include <KoGroupButton.h>
#include <KoPointerEvent.h>
#include <canvas/kis_canvas2.h>
#include <kis_canvas_resource_provider.h>
#include <kis_color_label_selector_widget.h>
#include <kis_command_utils.h>
#include <kis_default_bounds.h>
#include <kis_floating_message.h>
#include <kis_icon_utils.h>
#include <kis_image.h>
#include <kis_image_animation_interface.h>
#include <kis_pixel_selection.h>
#include <kis_processing_applicator.h>
#include <kis_resources_snapshot.h>
#include <kis_selection.h>
#include <kis_selection_tool_helper.h>
#include <kis_slider_spin_box.h>
#include <kis_transaction.h>
#include <klocalizedstring.h>
#include <ksharedconfig.h>

#include "subtools/KisBrushEnclosingProducer.h"
#include "subtools/KisEllipseEnclosingProducer.h"
#include "subtools/KisLassoEnclosingProducer.h"
#include "subtools/KisPathEnclosingProducer.h"
#include "subtools/KisRectangleEnclosingProducer.h"

namespace
{
const QStringList actionIds = {QStringLiteral("remove"), QStringLiteral("select")};
const QStringList enclosingMethodIds = {QStringLiteral("rectangle"),
                                        QStringLiteral("ellipse"),
                                        QStringLiteral("path"),
                                        QStringLiteral("lasso"),
                                        QStringLiteral("brush")};
const QStringList modeIds = {QStringLiteral("transparency"), QStringLiteral("white"), QStringLiteral("gaps")};
const QStringList gapFillIds = {QStringLiteral("surrounding"), QStringLiteral("foreground")};
const QStringList referenceIds = {QStringLiteral("currentLayer"),
                                  QStringLiteral("allLayers"),
                                  QStringLiteral("colorLabeledLayers")};

// In the order of the buttons, which follows the selection tools.
const QList<SelectionAction> selectionActions = {SELECTION_REPLACE,
                                                 SELECTION_INTERSECT,
                                                 SELECTION_ADD,
                                                 SELECTION_SUBTRACT,
                                                 SELECTION_SYMMETRICDIFFERENCE};
const QStringList selectionActionIds = {QStringLiteral("replace"),
                                        QStringLiteral("intersect"),
                                        QStringLiteral("add"),
                                        QStringLiteral("subtract"),
                                        QStringLiteral("symmetricDifference")};

constexpr int defaultDustSize = 4;
constexpr int softMaximumDustSize = 50;

int readIndex(const KConfigGroup &group, const QString &key, const QStringList &ids)
{
    return qMax(0, ids.indexOf(group.readEntry(key, ids.first())));
}

void connectToggled(KisOptionButtonStrip *strip, std::function<void(int)> slot)
{
    QObject::connect(strip,
                     QOverload<int, bool>::of(&KisOptionButtonStrip::buttonToggled),
                     strip,
                     [slot](int index, bool checked) {
                         if (checked) {
                             slot(index);
                         }
                     });
}
} // namespace

KisToolRemoveDust::KisToolRemoveDust(KoCanvasBase *canvas)
    : KisDynamicDelegatedTool<KisToolShape>(canvas, QCursor())
{
    setObjectName("tool_remove_dust");
}

KisToolRemoveDust::~KisToolRemoveDust()
{
}

void KisToolRemoveDust::resetCursorStyle()
{
    KisDynamicDelegatedTool::resetCursorStyle();
    overrideCursorIfNotEditable();
}

void KisToolRemoveDust::activate(const QSet<KoShape *> &shapes)
{
    KisDynamicDelegatedTool::activate(shapes);
    m_configGroup = KSharedConfig::openConfig()->group(toolId());
}

void KisToolRemoveDust::deactivate()
{
    m_referencePaintDevice = nullptr;
    m_referenceNodeList = nullptr;
    KisDynamicDelegatedTool::deactivate();
}

void KisToolRemoveDust::setupEnclosingSubtool()
{
    if (delegateTool()) {
        delegateTool()->deactivate();
    }

    using Delegate = KisDynamicDelegateTool<KisToolShape>;
    Delegate *newDelegateTool = nullptr;
    if (m_enclosingMethod == Ellipse) {
        newDelegateTool = reinterpret_cast<Delegate *>(new KisEllipseEnclosingProducer(canvas()));
    } else if (m_enclosingMethod == Path) {
        newDelegateTool = reinterpret_cast<Delegate *>(new KisPathEnclosingProducer(canvas()));
    } else if (m_enclosingMethod == Lasso) {
        newDelegateTool = reinterpret_cast<Delegate *>(new KisLassoEnclosingProducer(canvas()));
    } else if (m_enclosingMethod == Brush) {
        newDelegateTool = reinterpret_cast<Delegate *>(new KisBrushEnclosingProducer(canvas()));
    } else {
        newDelegateTool = reinterpret_cast<Delegate *>(new KisRectangleEnclosingProducer(canvas()));
    }
    setDelegateTool(newDelegateTool);
    setCursor(newDelegateTool->cursor());

    connect(delegateTool(),
            SIGNAL(enclosingMaskProduced(KisPixelSelectionSP)),
            SLOT(slotEnclosingMaskProduced(KisPixelSelectionSP)));

    if (isActivated()) {
        delegateTool()->activate(QSet<KoShape *>());
    }
}

bool KisToolRemoveDust::subtoolHasUserInteractionRunning() const
{
    if (!delegateTool()) {
        return false;
    }

    switch (m_enclosingMethod) {
    case Rectangle:
        return reinterpret_cast<KisRectangleEnclosingProducer *>(delegateTool())->hasUserInteractionRunning();
    case Ellipse:
        return reinterpret_cast<KisEllipseEnclosingProducer *>(delegateTool())->hasUserInteractionRunning();
    case Path:
        return reinterpret_cast<KisPathEnclosingProducer *>(delegateTool())->hasUserInteractionRunning();
    case Lasso:
        return reinterpret_cast<KisLassoEnclosingProducer *>(delegateTool())->hasUserInteractionRunning();
    case Brush:
        return reinterpret_cast<KisBrushEnclosingProducer *>(delegateTool())->hasUserInteractionRunning();
    }
    return false;
}

void KisToolRemoveDust::beginPrimaryAction(KoPointerEvent *event)
{
    KisNodeSP node = currentNode();
    const bool usable = m_action == Select
        ? node && node->projection() && selectionEditable()
        : node && !node->inherits("KisShapeLayer") && nodePaintAbility() == NodePaintAbility::PAINT;

    if (!usable) {
        KisCanvas2 *kisCanvas = static_cast<KisCanvas2 *>(canvas());
        kisCanvas->viewManager()->showFloatingMessage(i18n("You cannot use this tool with the selected layer type"),
                                                      QIcon(),
                                                      2000,
                                                      KisFloatingMessage::Medium,
                                                      Qt::AlignCenter);
        event->ignore();
        return;
    }

    if (m_action == Remove && !nodeEditable()) {
        event->ignore();
        return;
    }

    KisDynamicDelegatedTool::beginPrimaryAction(event);
}

// While a subtool interaction (a path, for example) is running, the alternate
// actions continue it, as in the Enclose and Fill Tool.
void KisToolRemoveDust::activateAlternateAction(AlternateAction action)
{
    if (subtoolHasUserInteractionRunning()) {
        delegateTool()->activatePrimaryAction();
        return;
    }
    KisDynamicDelegatedTool::activateAlternateAction(action);
}

void KisToolRemoveDust::deactivateAlternateAction(AlternateAction action)
{
    if (subtoolHasUserInteractionRunning()) {
        return;
    }
    KisDynamicDelegatedTool::deactivateAlternateAction(action);
}

void KisToolRemoveDust::beginAlternateAction(KoPointerEvent *event, AlternateAction action)
{
    if (subtoolHasUserInteractionRunning()) {
        delegateTool()->beginPrimaryAction(event);
        return;
    }
    KisDynamicDelegatedTool::beginAlternateAction(event, action);
    m_alternateActionStarted = true;
}

void KisToolRemoveDust::continueAlternateAction(KoPointerEvent *event, AlternateAction action)
{
    if (subtoolHasUserInteractionRunning()) {
        delegateTool()->continuePrimaryAction(event);
        return;
    }
    if (!m_alternateActionStarted) {
        return;
    }
    KisDynamicDelegatedTool::continueAlternateAction(event, action);
}

void KisToolRemoveDust::endAlternateAction(KoPointerEvent *event, AlternateAction action)
{
    if (subtoolHasUserInteractionRunning()) {
        delegateTool()->endPrimaryAction(event);
        return;
    }
    if (!m_alternateActionStarted) {
        return;
    }
    KisDynamicDelegatedTool::endAlternateAction(event, action);
    m_alternateActionStarted = false;
}

void KisToolRemoveDust::beginAlternateDoubleClickAction(KoPointerEvent *event, AlternateAction action)
{
    if (subtoolHasUserInteractionRunning()) {
        delegateTool()->beginPrimaryDoubleClickAction(event);
        return;
    }
    KisDynamicDelegatedTool::beginAlternateDoubleClickAction(event, action);
}

KisPaintDeviceSP KisToolRemoveDust::prepareReference(KisProcessingApplicator &applicator, KisNodeSP node)
{
    if (m_reference != ColorLabeledLayers) {
        // Regenerate the merged device when color labeled layers are chosen again.
        m_referenceNodeList.reset();
        m_referencePaintDevice = nullptr;
        if (m_reference == AllLayers) {
            return image()->projection();
        }
        return m_action == Select ? node->projection() : node->paintDevice();
    }

    const char *name = "Remove Dust Tool Reference Result Paint Device";
    if (!m_referenceNodeList) {
        m_referencePaintDevice = KisMergeLabeledLayersCommand::createRefPaintDevice(image(), name);
        m_referenceNodeList.reset(new KisMergeLabeledLayersCommand::ReferenceNodeInfoList);
    }
    KisPaintDeviceSP newReferencePaintDevice = KisMergeLabeledLayersCommand::createRefPaintDevice(image(), name);
    KisMergeLabeledLayersCommand::ReferenceNodeInfoListSP newReferenceNodeList(
        new KisMergeLabeledLayersCommand::ReferenceNodeInfoList);
    const int currentTime = image()->animationInterface()->currentTime();
    applicator.applyCommand(
        new KisMergeLabeledLayersCommand(image(),
                                         m_referenceNodeList,
                                         newReferenceNodeList,
                                         m_referencePaintDevice,
                                         newReferencePaintDevice,
                                         m_selectedColorLabels,
                                         KisMergeLabeledLayersCommand::GroupSelectionPolicy_SelectIfColorLabeled,
                                         m_previousTime != currentTime,
                                         m_useActiveLayer ? node : nullptr),
        KisStrokeJobData::SEQUENTIAL,
        KisStrokeJobData::EXCLUSIVE);
    m_referencePaintDevice = newReferencePaintDevice;
    m_referenceNodeList = newReferenceNodeList;
    m_previousTime = currentTime;
    return m_referencePaintDevice;
}

void KisToolRemoveDust::slotEnclosingMaskProduced(KisPixelSelectionSP enclosingMask)
{
    KisNodeSP node = currentNode();
    KisImageSP image = this->image();
    KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2 *>(canvas());
    if (!node || !image || !kisCanvas || !enclosingMask) {
        return;
    }

    const bool select = m_action == Select;
    KisPaintDeviceSP target = select ? nullptr : node->paintDevice();
    if (!select && !target) {
        return;
    }

    KisDustDetector::Options options;
    options.mode = m_mode;
    options.dustSize = m_dustSize;
    options.tolerance = m_tolerance / 100.0;
    // A brush stroke rarely covers a whole speck, so touching it is enough.
    const KisDustDetector::Containment containment =
        m_enclosingMethod == Brush ? KisDustDetector::Touching : KisDustDetector::FullyInside;
    const KisDustDetector::Replacement replacement =
        m_mode == KisDustDetector::TransparentGaps && m_gapFill == ForegroundColor
        ? KisDustDetector::FixedColor
        : KisDustDetector::defaultReplacement(m_mode);
    const KoColor color = currentFgColor();
    const QRect imageBounds = image->bounds();

    KisSelectionSP activeSelection;
    if (!select) {
        KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, node, canvas()->resourceManager());
        activeSelection = resources->activeSelection();
    }

    KisProcessingApplicator applicator(image,
                                       node,
                                       KisProcessingApplicator::NONE,
                                       KisImageSignalVector(),
                                       select ? kundo2_i18n("Select Dust") : kundo2_i18n("Remove Dust"));

    KisPaintDeviceSP reference = prepareReference(applicator, node);
    KIS_SAFE_ASSERT_RECOVER(reference)
    {
        applicator.cancel();
        return;
    }

    QPointer<KisCanvas2> canvasPointer(kisCanvas);
    auto report = [canvasPointer, select](int count) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [canvasPointer, select, count]() {
                if (!canvasPointer) {
                    return;
                }
                const QString message = count == 0 ? i18n("No dust found")
                    : select                       ? i18np("Selected 1 dust spot", "Selected %1 dust spots", count)
                                                   : i18np("Removed 1 dust spot", "Removed %1 dust spots", count);
                canvasPointer->viewManager()->showFloatingMessage(message, QIcon(), 2000, KisFloatingMessage::Low);
            },
            Qt::QueuedConnection);
    };

    const auto findDust = [=](KisPixelSelectionSP enclosure) {
        const QRect rect = enclosure->selectedExactRect() & imageBounds;
        return KisDustDetector::detect(reference, rect, options, enclosure, containment);
    };

    if (select) {
        KisPixelSelectionSP selection = new KisPixelSelection(
            new KisSelectionDefaultBounds(node->projection() ? node->projection() : image->projection()));

        applicator.applyCommand(new KisCommandUtils::LambdaCommand([=]() mutable -> KUndo2Command * {
                                    const auto components = findDust(enclosingMask);
                                    KisDustDetector::select(selection, components);
                                    selection->invalidateOutlineCache();
                                    report(components.size());
                                    return nullptr;
                                }),
                                KisStrokeJobData::BARRIER);

        KisSelectionToolHelper helper(kisCanvas, kundo2_i18n("Select Dust"));
        helper.selectPixelSelection(applicator, selection, m_selectionAction);
    } else {
        applicator.applyCommand(new KisCommandUtils::LambdaCommand([=]() mutable -> KUndo2Command * {
                                    KisPixelSelectionSP enclosure = enclosingMask;
                                    if (activeSelection) {
                                        // Like painting, only change what is selected.
                                        enclosure = new KisPixelSelection(*enclosingMask);
                                        enclosure->applySelection(activeSelection->projection(), SELECTION_INTERSECT);
                                    }

                                    const auto components = findDust(enclosure);
                                    report(components.size());
                                    if (components.isEmpty()) {
                                        return nullptr;
                                    }

                                    KisTransaction transaction(target);
                                    KisDustDetector::apply(target, components, replacement, color);
                                    QRect dirtyRect;
                                    for (const auto &component : components) {
                                        dirtyRect |= component.bounds;
                                    }
                                    node->setDirty(dirtyRect);
                                    return transaction.endAndTake();
                                }),
                                KisStrokeJobData::SEQUENTIAL,
                                KisStrokeJobData::EXCLUSIVE);
    }

    applicator.end();
}

int KisToolRemoveDust::flags() const
{
    return KisDynamicDelegatedTool::flags() | KisTool::FLAG_USES_CUSTOM_SIZE | KisTool::FLAG_USES_CUSTOM_PRESET;
}

void KisToolRemoveDust::loadConfiguration()
{
    m_configGroup = KSharedConfig::openConfig()->group(toolId());
    m_action = Action(readIndex(m_configGroup, "action", actionIds));
    m_enclosingMethod = EnclosingMethod(readIndex(m_configGroup, "enclosingMethod", enclosingMethodIds));
    m_mode = KisDustDetector::Mode(readIndex(m_configGroup, "mode", modeIds));
    m_gapFill = GapFill(readIndex(m_configGroup, "gapFill", gapFillIds));
    m_dustSize = qBound(KisDustDetector::minimumDustSize,
                        m_configGroup.readEntry<int>("dustSize", defaultDustSize),
                        KisDustDetector::maximumDustSize);
    m_tolerance =
        qBound(0.0, m_configGroup.readEntry<qreal>("tolerance", KisDustDetector::defaultTolerancePercent), 100.0);
    m_selectionAction = selectionActions[readIndex(m_configGroup, "selectionAction", selectionActionIds)];
    m_reference = Reference(readIndex(m_configGroup, "reference", referenceIds));
    m_selectedColorLabels.clear();
    const QStringList labels = m_configGroup.readEntry<QString>("colorLabels", "").split(',', Qt::SkipEmptyParts);
    for (const QString &label : labels) {
        bool ok = false;
        const int value = label.toInt(&ok);
        if (ok) {
            m_selectedColorLabels << value;
        }
    }
    m_useActiveLayer = m_configGroup.readEntry<bool>("useActiveLayer", false);

    setupEnclosingSubtool();
}

void KisToolRemoveDust::saveConfiguration()
{
    m_configGroup.writeEntry("action", actionIds[m_action]);
    m_configGroup.writeEntry("enclosingMethod", enclosingMethodIds[m_enclosingMethod]);
    m_configGroup.writeEntry("mode", modeIds[m_mode]);
    m_configGroup.writeEntry("gapFill", gapFillIds[m_gapFill]);
    m_configGroup.writeEntry("dustSize", m_dustSize);
    m_configGroup.writeEntry("tolerance", m_tolerance);
    m_configGroup.writeEntry("selectionAction", selectionActionIds[selectionActions.indexOf(m_selectionAction)]);
    m_configGroup.writeEntry("reference", referenceIds[m_reference]);
    QStringList labels;
    for (int label : m_selectedColorLabels) {
        labels << QString::number(label);
    }
    m_configGroup.writeEntry("colorLabels", labels.join(','));
    m_configGroup.writeEntry("useActiveLayer", m_useActiveLayer);
}

void KisToolRemoveDust::updateOptionVisibility()
{
    if (!m_optionWidget) {
        return;
    }

    const bool select = m_action == Select;
    const QSignalBlocker blocker(m_modeCombo);
    m_modeCombo->setItemText(0, select ? i18n("Select dust on transparency") : i18n("Remove dust from transparency"));
    m_modeCombo->setItemText(1,
                             select ? i18n("Select dust on white background")
                                    : i18n("Remove dust from white background"));
    m_modeCombo->setItemText(2, select ? i18n("Select transparent gaps") : i18n("Fill transparent gaps"));

    m_detectionSection->setWidgetVisible("gapFill", !select && m_mode == KisDustDetector::TransparentGaps);
    m_optionWidget->setWidgetVisible("sectionSelection", select);
    m_referenceSection->setWidgetVisible("labels", m_reference == ColorLabeledLayers);
}

void KisToolRemoveDust::resetOptions()
{
    m_actionStrip->button(Remove)->setChecked(true);
    m_enclosingStrip->button(Rectangle)->setChecked(true);
    m_modeCombo->setCurrentIndex(KisDustDetector::DustOnTransparency);
    m_gapFillStrip->button(SurroundingColor)->setChecked(true);
    m_sizeSlider->setValue(defaultDustSize);
    m_toleranceSlider->setValue(KisDustDetector::defaultTolerancePercent);
    m_selectionActionStrip->button(0)->setChecked(true);
    m_referenceStrip->button(CurrentLayer)->setChecked(true);
    m_labelsWidget->setSelection({});
    m_useActiveLayerCheckBox->setChecked(false);
}

QWidget *KisToolRemoveDust::createOptionWidget()
{
    loadConfiguration();

    m_actionStrip = new KisOptionButtonStrip;
    m_actionStrip->addButton(i18nc("Remove Dust tool action", "Remove"));
    m_actionStrip->addButton(i18nc("Remove Dust tool action", "Select"));

    m_enclosingStrip = new KisOptionButtonStrip;
    m_enclosingStrip->addButton(KisIconUtils::loadIcon("tool_rect_selection"));
    m_enclosingStrip->addButton(KisIconUtils::loadIcon("tool_elliptical_selection"));
    m_enclosingStrip->addButton(KisIconUtils::loadIcon("tool_path_selection"));
    m_enclosingStrip->addButton(KisIconUtils::loadIcon("tool_outline_selection"));
    m_enclosingStrip->addButton(KisIconUtils::loadIcon("krita_tool_freehand"));

    m_modeCombo = new QComboBox;
    m_modeCombo->addItem(QString());
    m_modeCombo->addItem(QString());
    m_modeCombo->addItem(QString());

    m_gapFillStrip = new KisOptionButtonStrip;
    m_gapFillStrip->addButton(i18n("Surrounding color"));
    m_gapFillStrip->addButton(i18n("Foreground color"));

    m_sizeSlider = new KisSliderSpinBox;
    m_sizeSlider->setPrefix(i18nc("The 'dust size' spinbox prefix in Remove Dust tool options", "Dust size: "));
    m_sizeSlider->setSuffix(i18n(" px"));
    m_sizeSlider->setRange(KisDustDetector::minimumDustSize, KisDustDetector::maximumDustSize);
    m_sizeSlider->setSoftRange(KisDustDetector::minimumDustSize, softMaximumDustSize);

    m_toleranceSlider = new KisDoubleSliderSpinBox;
    m_toleranceSlider->setPrefix(i18nc("The 'tolerance' spinbox prefix in Remove Dust tool options", "Tolerance: "));
    m_toleranceSlider->setSuffix(i18n("%"));
    m_toleranceSlider->setRange(0.0, 100.0, 1);

    m_selectionActionStrip = new KisOptionButtonStrip;
    m_selectionActionStrip->addButton(KisIconUtils::loadIcon("selection_replace"));
    m_selectionActionStrip->addButton(KisIconUtils::loadIcon("selection_intersect"));
    m_selectionActionStrip->addButton(KisIconUtils::loadIcon("selection_add"));
    m_selectionActionStrip->addButton(KisIconUtils::loadIcon("selection_subtract"));
    m_selectionActionStrip->addButton(KisIconUtils::loadIcon("selection_symmetric_difference"));

    m_referenceStrip = new KisOptionButtonStrip;
    m_referenceStrip->addButton(KisIconUtils::loadIcon("current-layer"));
    m_referenceStrip->addButton(KisIconUtils::loadIcon("all-layers"));
    m_referenceStrip->addButton(KisIconUtils::loadIcon("tag"));
    m_labelsWidget = new KisColorLabelSelectorWidget;
    m_labelsWidget->setExclusive(false);
    m_labelsWidget->setButtonSize(20);
    m_labelsWidget->setButtonWrapEnabled(true);
    m_labelsWidget->setMouseDragEnabled(true);
    m_useActiveLayerCheckBox = new QCheckBox(i18n("Use active layer"));

    QPushButton *resetButton = new QPushButton(i18nc("The 'reset' button in Remove Dust tool options", "Reset"));

    m_actionStrip->button(Remove)->setToolTip(i18n("Remove the dust found in the enclosed area"));
    m_actionStrip->button(Select)->setToolTip(i18n("Select the dust found in the enclosed area"));
    m_enclosingStrip->button(Rectangle)->setToolTip(i18n("Rectangle"));
    m_enclosingStrip->button(Ellipse)->setToolTip(i18n("Ellipse"));
    m_enclosingStrip->button(Path)->setToolTip(i18n("Bezier Curve"));
    m_enclosingStrip->button(Lasso)->setToolTip(i18n("Lasso"));
    m_enclosingStrip->button(Brush)->setToolTip(i18n("Brush: also catches dust that the stroke only partly covers"));
    m_modeCombo->setToolTip(i18n("Choose what counts as dust"));
    m_gapFillStrip->button(SurroundingColor)->setToolTip(i18n("Fill each gap with the main color around it"));
    m_gapFillStrip->button(ForegroundColor)->setToolTip(i18n("Fill gaps with the foreground color"));
    m_sizeSlider->setToolTip(i18n("Spots whose width and height are both at most this size are treated as dust"));
    m_toleranceSlider->setToolTip(
        i18n("Transparency: pixels at or below this opacity count as transparent. White background: colors this "
             "close to white count as white. Gaps: pixels at least this much below full opacity count as gaps."));
    m_selectionActionStrip->button(0)->setToolTip(i18n("Replace"));
    m_selectionActionStrip->button(1)->setToolTip(i18n("Intersect"));
    m_selectionActionStrip->button(2)->setToolTip(i18n("Add"));
    m_selectionActionStrip->button(3)->setToolTip(i18n("Subtract"));
    m_selectionActionStrip->button(4)->setToolTip(i18n("Symmetric Difference"));
    m_referenceStrip->button(CurrentLayer)->setToolTip(i18n("Find dust in the active layer"));
    m_referenceStrip->button(AllLayers)->setToolTip(i18n("Find dust in the merging of all layers"));
    m_referenceStrip->button(ColorLabeledLayers)
        ->setToolTip(i18n("Find dust in the merging of layers with specific color labels"));
    m_useActiveLayerCheckBox->setToolTip(
        i18n("Includes the active layer in the merging of layers with specific color labels"));
    resetButton->setToolTip(i18n("Reset the options to their default values"));

    m_optionWidget = new KisOptionCollectionWidget;
    m_optionWidget->setContentsMargins(0, 10, 0, 10);
    m_optionWidget->setSeparatorsVisible(true);

    auto *sectionAction = new KisOptionCollectionWidgetWithHeader(
        i18nc("The 'action' section label in Remove Dust tool options", "Action"));
    sectionAction->setPrimaryWidget(m_actionStrip);
    m_optionWidget->appendWidget("sectionAction", sectionAction);

    auto *sectionEnclosingMethod = new KisOptionCollectionWidgetWithHeader(
        i18nc("The 'enclosing method' section label in Remove Dust tool options", "Enclosing method"));
    sectionEnclosingMethod->setPrimaryWidget(m_enclosingStrip);
    m_optionWidget->appendWidget("sectionEnclosingMethod", sectionEnclosingMethod);

    m_detectionSection =
        new KisOptionCollectionWidgetWithHeader(i18nc("The 'dust' section label in Remove Dust tool options", "Dust"));
    m_detectionSection->setPrimaryWidget(m_modeCombo);
    m_detectionSection->appendWidget("gapFill", m_gapFillStrip);
    m_detectionSection->appendWidget("size", m_sizeSlider);
    m_detectionSection->appendWidget("tolerance", m_toleranceSlider);
    m_optionWidget->appendWidget("sectionDetection", m_detectionSection);

    auto *sectionSelection = new KisOptionCollectionWidgetWithHeader(
        i18nc("The 'selection mode' section label in Remove Dust tool options", "Selection mode"));
    sectionSelection->setPrimaryWidget(m_selectionActionStrip);
    m_optionWidget->appendWidget("sectionSelection", sectionSelection);

    auto *labelsGroup = new KisOptionCollectionWidget;
    labelsGroup->appendWidget("labelWidget", m_labelsWidget);
    labelsGroup->appendWidget("useActiveLayer", m_useActiveLayerCheckBox);
    labelsGroup->setWidgetsMargin(0);
    m_referenceSection = new KisOptionCollectionWidgetWithHeader(
        i18nc("The 'reference' section label in Remove Dust tool options", "Reference"));
    m_referenceSection->setPrimaryWidget(m_referenceStrip);
    m_referenceSection->appendWidget("labels", labelsGroup);
    m_optionWidget->appendWidget("sectionReference", m_referenceSection);

    m_optionWidget->appendWidget("buttonReset", resetButton);

    m_actionStrip->button(m_action)->setChecked(true);
    m_enclosingStrip->button(m_enclosingMethod)->setChecked(true);
    m_modeCombo->setCurrentIndex(m_mode);
    m_gapFillStrip->button(m_gapFill)->setChecked(true);
    m_sizeSlider->setValue(m_dustSize);
    m_toleranceSlider->setValue(m_tolerance);
    m_selectionActionStrip->button(selectionActions.indexOf(m_selectionAction))->setChecked(true);
    m_referenceStrip->button(m_reference)->setChecked(true);
    m_labelsWidget->setSelection(m_selectedColorLabels);
    m_useActiveLayerCheckBox->setChecked(m_useActiveLayer);
    updateOptionVisibility();

    connectToggled(m_actionStrip, [this](int index) {
        m_action = Action(index);
        updateOptionVisibility();
        saveConfiguration();
    });
    connectToggled(m_enclosingStrip, [this](int index) {
        m_enclosingMethod = EnclosingMethod(index);
        setupEnclosingSubtool();
        saveConfiguration();
    });
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_mode = KisDustDetector::Mode(qBound(0, index, int(KisDustDetector::TransparentGaps)));
        updateOptionVisibility();
        saveConfiguration();
    });
    connectToggled(m_gapFillStrip, [this](int index) {
        m_gapFill = GapFill(index);
        saveConfiguration();
    });
    connect(m_sizeSlider, QOverload<int>::of(&KisSliderSpinBox::valueChanged), this, [this](int value) {
        m_dustSize = value;
        saveConfiguration();
    });
    connect(m_toleranceSlider, QOverload<qreal>::of(&KisDoubleSliderSpinBox::valueChanged), this, [this](qreal value) {
        m_tolerance = value;
        saveConfiguration();
    });
    connectToggled(m_selectionActionStrip, [this](int index) {
        m_selectionAction = selectionActions[index];
        saveConfiguration();
    });
    connectToggled(m_referenceStrip, [this](int index) {
        m_reference = Reference(index);
        updateOptionVisibility();
        saveConfiguration();
    });
    connect(m_labelsWidget, &KisColorLabelSelectorWidget::selectionChanged, this, [this]() {
        m_selectedColorLabels = m_labelsWidget->selection();
        saveConfiguration();
    });
    connect(m_useActiveLayerCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        m_useActiveLayer = checked;
        saveConfiguration();
    });
    connect(resetButton, &QPushButton::clicked, this, [this]() {
        resetOptions();
    });

    return m_optionWidget;
}
