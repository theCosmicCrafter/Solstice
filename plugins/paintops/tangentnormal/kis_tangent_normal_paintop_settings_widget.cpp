/*
 *  SPDX-FileCopyrightText: 2015 Wolthera van Hövell tot Westerflier <griffinvalley@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_tangent_normal_paintop_settings_widget.h"
#include "kis_brush_based_paintop_settings.h"
#include "KisTangentTiltOptionWidget.h"

#include <kis_properties_configuration.h>
#include <KisStandardOptionData.h>
#include <KisPaintOpOptionWidgetUtils.h>

#include <KisCompositeOpOptionWidget.h>
#include "KisSizeOptionWidget.h"
#include "KisSpacingOptionWidget.h"
#include "KisMirrorOptionWidget.h"
#include "KisSharpnessOptionWidget.h"
#include "KisScatterOptionWidget.h"
#include "KisAirbrushOptionWidget.h"
#include "KisPaintingModeOptionWidget.h"
#include <KisTextureOptionWidget.h>
#include <KisBrushBasedOptionStates.h>
#include <KisPaintOpOptionStateUtils.h>
#include <KisPaintOpOptionsModel.h>
#include <kis_brush_option_widget.h>

namespace
{
KisBrushTipOptionState *addBrushTipOption(KisPaintOpOptionsModel *model, KisBrushOptionWidgetFlags flags)
{
    KisBrushTipOptionState *state = new KisBrushTipOptionState(QStringLiteral("BrushTip"), flags);
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

namespace
{
const KisBrushOptionWidgetFlags tangentNormalFlags =
    KisBrushOptionWidgetFlag::SupportsPrecision | KisBrushOptionWidgetFlag::SupportsHSLBrushMode;
} // namespace

KisTangentNormalPaintOpSettingsWidget::KisTangentNormalPaintOpSettingsWidget(QWidget* parent, KisResourcesInterfaceSP resourcesInterface, KoCanvasResourcesInterfaceSP canvasResourcesInterface):
    KisTangentNormalPaintOpSettingsWidget(parent, resourcesInterface, canvasResourcesInterface, new KisPaintOpOptionsModel())
{
}

KisTangentNormalPaintOpSettingsWidget::KisTangentNormalPaintOpSettingsWidget(
    QWidget *parent,
    KisResourcesInterfaceSP resourcesInterface,
    KoCanvasResourcesInterfaceSP canvasResourcesInterface,
    KisPaintOpOptionsModel *model)
    : KisBrushBasedPaintopOptionWidget(tangentNormalFlags,
                                       addBrushTipOption(model, tangentNormalFlags)->cursor(),
                                       parent)
{
    Q_UNUSED(canvasResourcesInterface)
    namespace kpowu = KisPaintOpOptionWidgetUtils;
    namespace kposu = KisPaintOpOptionStateUtils;
    namespace kbbos = KisBrushBasedOptionStates;

    setObjectName("brush option widget");

    // Solstice: the option states live in a shared model, so that one option
    // change writes that option and its dependents after the initial rewrite
    // (docs/agent/brush-option-shared-model.md); no option's written data depends on another
    model->setParent(this);

    KisBrushTipOptionState *brushTip = static_cast<KisBrushTipOptionState *>(model->option(QStringLiteral("BrushTip")));
    brushTip->setResourcesInterfaceGetter([this]() {
        return this->resourcesInterface();
    });

    auto *compositeOp = model->addOption(QStringLiteral("CompositeOp"), KisCompositeOpOptionData());
    auto *opacity = model->addOption(QStringLiteral("Opacity"),
                                     KisOpacityOptionData(),
                                     &kposu::bakeCurveOption<KisOpacityOptionData>);
    auto *flow =
        model->addOption(QStringLiteral("Flow"), KisFlowOptionData(), &kposu::bakeCurveOption<KisFlowOptionData>);
    auto *size =
        model->addOption(QStringLiteral("Size"), KisSizeOptionData(), &kposu::bakeCurveOption<KisSizeOptionData>);
    auto *tangentTilt = model->addOption(QStringLiteral("TangentTilt"), KisTangentTiltOptionData());
    auto *spacing = model->addOption(QStringLiteral("Spacing"),
                                     KisSpacingOptionData(),
                                     &kposu::bakeCurveOption<KisSpacingOptionData>);
    auto *mirror =
        model->addOption(QStringLiteral("Mirror"), KisMirrorOptionData(), &kposu::bakeCurveOption<KisMirrorOptionData>);
    auto *softness = model->addOption(QStringLiteral("Softness"),
                                      KisSoftnessOptionData(),
                                      &kposu::bakeCurveOption<KisSoftnessOptionData>);
    auto *sharpness = model->addOption(QStringLiteral("Sharpness"),
                                       KisSharpnessOptionData(),
                                       &kposu::bakeCurveOption<KisSharpnessOptionData>);
    auto *scatter = model->addOption(QStringLiteral("Scatter"),
                                     KisScatterOptionData(),
                                     &kposu::bakeCurveOption<KisScatterOptionData>);
    auto *rotation = model->addOption(QStringLiteral("Rotation"),
                                      KisRotationOptionData(),
                                      &kposu::bakeCurveOption<KisRotationOptionData>);
    auto *airbrush = model->addOption(QStringLiteral("Airbrush"), KisAirbrushOptionData());
    auto *rate =
        model->addOption(QStringLiteral("Rate"), KisRateOptionData(), &kposu::bakeCurveOption<KisRateOptionData>);
    auto *paintingMode = model->addOption(QStringLiteral("PaintingMode"), KisPaintingModeOptionData());
    auto *texture = model->addOption(QStringLiteral("Texture"),
                                     KisTextureOptionData(),
                                     [resourcesInterface](const KisTextureOptionData &data) {
                                         return kbbos::bakeTextureOption(data, resourcesInterface);
                                     });
    auto *strength = model->addOption(QStringLiteral("Strength"),
                                      KisStrengthOptionData(),
                                      &kposu::bakeCurveOption<KisStrengthOptionData>);

    brushOptionWidget()->setToolOptionsId(QStringLiteral("BrushTip"));

    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisCompositeOpOptionWidget>(compositeOp),
                                       QStringLiteral("CompositeOp")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createOpacityOptionWidget(kposu::curveCursor(opacity)), QStringLiteral("Opacity")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createFlowOptionWidget(kposu::curveCursor(flow)), QStringLiteral("Flow")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisSizeOptionWidget>(size), QStringLiteral("Size")));

    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisTangentTiltOptionWidget>(tangentTilt),
                                       QStringLiteral("TangentTilt")));

    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisSpacingOptionWidget>(spacing), QStringLiteral("Spacing")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisMirrorOptionWidget>(mirror), QStringLiteral("Mirror")));

    addPaintOpOption(
        withToolOptionsId(kpowu::createSoftnessOptionWidget(kposu::curveCursor(softness)), QStringLiteral("Softness")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisSharpnessOptionWidget>(sharpness), QStringLiteral("Sharpness")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisScatterOptionWidget>(scatter), QStringLiteral("Scatter")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRotationOptionWidget(kposu::curveCursor(rotation)), QStringLiteral("Rotation")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisAirbrushOptionWidget>(airbrush), QStringLiteral("Airbrush")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRateOptionWidget(kposu::curveCursor(rate)), QStringLiteral("Rate")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisPaintingModeOptionWidget>(paintingMode),
                                       QStringLiteral("PaintingMode")));

    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisTextureOptionWidget>(texture, resourcesInterface),
                                       QStringLiteral("Texture")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createStrengthOptionWidget(kposu::curveCursor(strength)), QStringLiteral("Strength")));

    setOptionsModel(model);
}

KisTangentNormalPaintOpSettingsWidget::~KisTangentNormalPaintOpSettingsWidget() { }

KisPropertiesConfigurationSP KisTangentNormalPaintOpSettingsWidget::configuration() const
{
    KisBrushBasedPaintOpSettingsSP config = new KisBrushBasedPaintOpSettings(resourcesInterface());
    config->setProperty("paintop", "tangentnormal");
    writeConfiguration(config);
    return config;
}


