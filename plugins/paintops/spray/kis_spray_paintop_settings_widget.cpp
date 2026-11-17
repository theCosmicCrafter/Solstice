/*
 *  SPDX-FileCopyrightText: 2008, 2009, 2010 Lukáš Tvrdý <lukast.dev@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "kis_spray_paintop_settings_widget.h"

#include "kis_spray_paintop_settings.h"

#include <KisColorOptionWidget.h>
#include <kis_paintop_settings_widget.h>

#include <KisPaintingModeOptionWidget.h>
#include <KisPaintOpOptionWidgetUtils.h>
#include <kis_brush_option_widget.h>
#include <KisAirbrushOptionWidget.h>
#include <KisPaintOpOptionWidgetUtils.h>
#include <KisSizeOptionWidget.h>
#include <KisStandardOptionData.h>
#include <KisCompositeOpOptionWidget.h>
#include <KisSprayOpOptionWidget.h>
#include <KisSprayShapeDynamicsOptionWidget.h>
#include <KisSprayShapeOptionWidget.h>
#include <KisBrushBasedOptionStates.h>
#include <KisLager.h>
#include <KisPaintOpOptionStateUtils.h>
#include <KisPaintOpOptionsModel.h>

namespace
{
/// Solstice: the option's checkbox and page parameters can be shown in Tool
/// Options under @p id, the option's id in the options model
/// (docs/agent/tool-options-brush.md)
KisPaintOpOption *withToolOptionsId(KisPaintOpOption *option, const QString &id)
{
    option->setToolOptionsId(id);
    return option;
}
} // namespace

KisSprayPaintOpSettingsWidget:: KisSprayPaintOpSettingsWidget(QWidget* parent)
    : KisPaintOpSettingsWidget(parent)
{
    namespace kpowu = KisPaintOpOptionWidgetUtils;

    namespace kposu = KisPaintOpOptionStateUtils;

    // Solstice: the option states live in a shared model, so that one option
    // change writes that option and its dependents after the initial rewrite
    // (docs/agent/brush-option-shared-model.md). The spray shape page shows its size relative to the spray
    // area's diameter and scale but writes only its own data, so no option's
    // written data depends on another.
    KisPaintOpOptionsModel *model = new KisPaintOpOptionsModel(this);

    auto *sprayOp = model->addOption(QStringLiteral("SprayOp"), KisSprayOpOptionData());
    auto *sprayShape = model->addOption(QStringLiteral("SprayShape"), KisSprayShapeOptionData());
    KisBrushTipOptionState *brushTip =
        new KisBrushTipOptionState(QStringLiteral("BrushTip"), KisBrushOptionWidgetFlag::None);
    brushTip->setResourcesInterfaceGetter([this]() {
        return this->resourcesInterface();
    });
    model->addOption(brushTip);
    auto *opacity = model->addOption(QStringLiteral("Opacity"),
                                     KisOpacityOptionData(),
                                     &kposu::bakeCurveOption<KisOpacityOptionData>);
    auto *size =
        model->addOption(QStringLiteral("Size"), KisSizeOptionData(), &kposu::bakeCurveOption<KisSizeOptionData>);
    auto *compositeOp = model->addOption(QStringLiteral("CompositeOp"), KisCompositeOpOptionData());
    auto *shapeDynamics = model->addOption(QStringLiteral("ShapeDynamics"), KisSprayShapeDynamicsOptionData());
    auto *colorOptions = model->addOption(QStringLiteral("ColorOptions"), KisColorOptionData());
    auto *rotation = model->addOption(QStringLiteral("Rotation"),
                                      KisRotationOptionData(),
                                      &kposu::bakeCurveOption<KisRotationOptionData>);
    auto *airbrush = model->addOption(QStringLiteral("Airbrush"), KisAirbrushOptionData());
    auto *rate =
        model->addOption(QStringLiteral("Rate"), KisRateOptionData(), &kposu::bakeCurveOption<KisRateOptionData>);
    auto *paintingMode = model->addOption(QStringLiteral("PaintingMode"), KisPaintingModeOptionData());

    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisSprayOpOptionWidget>(sprayOp), QStringLiteral("SprayOp")));

    lager::cursor<int> diameter =
        sprayOp->cursor()[&KisSprayOpOptionData::diameter].zoom(kislager::lenses::do_static_cast<quint16, int>);
    lager::cursor<qreal> scale = sprayOp->cursor()[&KisSprayOpOptionData::scale];
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisSprayShapeOptionWidget>(sprayShape, diameter, scale),
                          QStringLiteral("SprayShape")));
    KisBrushOptionWidget *brushOption = new KisBrushOptionWidget(KisBrushOptionWidgetFlag::None, brushTip->cursor());
    brushOption->setToolOptionsId(QStringLiteral("BrushTip"));
    addPaintOpOption(brushOption);
    addPaintOpOption(
        withToolOptionsId(kpowu::createOpacityOptionWidget(kposu::curveCursor(opacity)), QStringLiteral("Opacity")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisSizeOptionWidget>(size), QStringLiteral("Size")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisCompositeOpOptionWidget>(compositeOp),
                                       QStringLiteral("CompositeOp")));

    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisSprayShapeDynamicsOptionWidget>(shapeDynamics),
                                       QStringLiteral("ShapeDynamics")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisColorOptionWidget>(colorOptions),
                                       QStringLiteral("ColorOptions")));

    addPaintOpOption(
        withToolOptionsId(kpowu::createRotationOptionWidget(kposu::curveCursor(rotation)), QStringLiteral("Rotation")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisAirbrushOptionWidget>(airbrush), QStringLiteral("Airbrush")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRateOptionWidget(kposu::curveCursor(rate)), QStringLiteral("Rate")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisPaintingModeOptionWidget>(paintingMode),
                                       QStringLiteral("PaintingMode")));

    setOptionsModel(model);
}

KisSprayPaintOpSettingsWidget::~ KisSprayPaintOpSettingsWidget()
{
}

KisPropertiesConfigurationSP  KisSprayPaintOpSettingsWidget::configuration() const
{
    KisSprayPaintOpSettings* config = new KisSprayPaintOpSettings(resourcesInterface());
    config->setProperty("paintop", "spraybrush"); // XXX: make this a const id string
    writeConfiguration(config);
    return config;
}
