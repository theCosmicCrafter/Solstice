/*
 *  SPDX-FileCopyrightText: 2010 Lukáš Tvrdý <lukast.dev@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_sketch_paintop_settings_widget.h"
#include "kis_sketch_paintop_settings.h"

#include <kis_paintop_settings_widget.h>
#include <KisPaintOpOptionWidgetUtils.h>

#include "KisSketchOpOptionWidget.h"
#include <KisCompositeOpOptionWidget.h>
#include <KisStandardOptionData.h>
#include "KisSizeOptionWidget.h"
#include "KisSketchStandardOptionData.h"
#include <KisAirbrushOptionWidget.h>
#include <KisPaintingModeOptionWidget.h>
#include <KisBrushBasedOptionStates.h>
#include <KisCurveOptionWidget.h>
#include <KisPaintOpOptionStateUtils.h>
#include <KisPaintOpOptionsModel.h>
#include <kis_brush_option_widget.h>

namespace
{
KisBrushTipOptionState *addBrushTipOption(KisPaintOpOptionsModel *model)
{
    KisBrushTipOptionState *state =
        new KisBrushTipOptionState(QStringLiteral("BrushTip"), KisBrushOptionWidgetFlag::None);
    model->addOption(state);
    return state;
}

/// Solstice: the option's checkbox and page parameters can be shown in Tool
/// Options under @p id, the option's id in the options model
/// (docs/agent/tool-options-brush.md)
KisPaintOpOption *withToolOptionsId(KisPaintOpOption *option, const QString &id)
{
    option->setToolOptionsId(id);
    return option;
}
} // namespace

KisSketchPaintOpSettingsWidget::KisSketchPaintOpSettingsWidget(QWidget* parent)
    : KisSketchPaintOpSettingsWidget(parent, new KisPaintOpOptionsModel())
{
}

KisSketchPaintOpSettingsWidget::KisSketchPaintOpSettingsWidget(QWidget *parent, KisPaintOpOptionsModel *model)
    : KisBrushBasedPaintopOptionWidget(KisBrushOptionWidgetFlag::None, addBrushTipOption(model)->cursor(), parent)
{
    namespace kpowu = KisPaintOpOptionWidgetUtils;
    namespace kposu = KisPaintOpOptionStateUtils;

    // Solstice: the option states live in a shared model, so that one option
    // change writes that option and its dependents after the initial rewrite
    // (docs/agent/brush-option-shared-model.md); no option's written data depends on another
    model->setParent(this);

    KisBrushTipOptionState *brushTip = static_cast<KisBrushTipOptionState *>(model->option(QStringLiteral("BrushTip")));
    brushTip->setResourcesInterfaceGetter([this]() {
        return this->resourcesInterface();
    });

    auto *sketch = model->addOption(QStringLiteral("Sketch"), KisSketchOpOptionData());
    auto *compositeOp = model->addOption(QStringLiteral("CompositeOp"), KisCompositeOpOptionData());
    auto *opacity = model->addOption(QStringLiteral("Opacity"),
                                     KisOpacityOptionData(),
                                     &kposu::bakeCurveOption<KisOpacityOptionData>);
    auto *size =
        model->addOption(QStringLiteral("Size"), KisSizeOptionData(), &kposu::bakeCurveOption<KisSizeOptionData>);
    auto *rotation = model->addOption(QStringLiteral("Rotation"),
                                      KisRotationOptionData(),
                                      &kposu::bakeCurveOption<KisRotationOptionData>);
    auto *lineWidth = model->addOption(QStringLiteral("LineWidth"),
                                       KisLineWidthOptionData(),
                                       &kposu::bakeCurveOption<KisLineWidthOptionData>);
    auto *offsetScale = model->addOption(QStringLiteral("OffsetScale"),
                                         KisOffsetScaleOptionData(),
                                         &kposu::bakeCurveOption<KisOffsetScaleOptionData>);
    auto *density = model->addOption(QStringLiteral("Density"),
                                     KisDensityOptionData(),
                                     &kposu::bakeCurveOption<KisDensityOptionData>);
    auto *airbrush = model->addOption(QStringLiteral("Airbrush"), KisAirbrushOptionData());
    auto *rate =
        model->addOption(QStringLiteral("Rate"), KisRateOptionData(), &kposu::bakeCurveOption<KisRateOptionData>);

    KisPaintingModeOptionData defaultModeData;
    defaultModeData.paintingMode = enumPaintingMode::BUILDUP;
    auto *paintingMode = model->addOption(QStringLiteral("PaintingMode"), defaultModeData);

    brushOptionWidget()->setToolOptionsId(QStringLiteral("BrushTip"));

    addPaintOpOption(withToolOptionsId(kposu::createOptionWidgetWithLodLimitations<KisSketchOpOptionWidget>(sketch),
                                       QStringLiteral("Sketch")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisCompositeOpOptionWidget>(compositeOp),
                                       QStringLiteral("CompositeOp")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createOpacityOptionWidget(kposu::curveCursor(opacity)), QStringLiteral("Opacity")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisSizeOptionWidget>(size), QStringLiteral("Size")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRotationOptionWidget(kposu::curveCursor(rotation)), QStringLiteral("Rotation")));
    addPaintOpOption(withToolOptionsId(
        new KisCurveOptionWidget(kposu::curveCursor(lineWidth), KisPaintOpOption::GENERAL, i18n("0%"), i18n("100%")),
        QStringLiteral("LineWidth")));
    addPaintOpOption(withToolOptionsId(
        new KisCurveOptionWidget(kposu::curveCursor(offsetScale), KisPaintOpOption::GENERAL, i18n("0%"), i18n("100%")),
        QStringLiteral("OffsetScale")));
    addPaintOpOption(withToolOptionsId(
        new KisCurveOptionWidget(kposu::curveCursor(density), KisPaintOpOption::GENERAL, i18n("0%"), i18n("100%")),
        QStringLiteral("Density")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisAirbrushOptionWidget>(airbrush, false),
                                       QStringLiteral("Airbrush")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRateOptionWidget(kposu::curveCursor(rate)), QStringLiteral("Rate")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisPaintingModeOptionWidget>(paintingMode),
                                       QStringLiteral("PaintingMode")));

    setOptionsModel(model);
}

KisSketchPaintOpSettingsWidget::~ KisSketchPaintOpSettingsWidget()
{
}

KisPropertiesConfigurationSP  KisSketchPaintOpSettingsWidget::configuration() const
{
    KisSketchPaintOpSettingsSP config = new KisSketchPaintOpSettings(resourcesInterface());
    config->setProperty("paintop", "sketchbrush"); // XXX: make this a const id string
    writeConfiguration(config);
    return config;
}

