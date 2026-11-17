/*
 *  SPDX-FileCopyrightText: 2002 Patrick Julien <freak@codepimps.org>
 *  SPDX-FileCopyrightText: 2004-2008 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2004 Clarence Dang <dang@kde.org>
 *  SPDX-FileCopyrightText: 2004 Adrian Page <adrian@pagenet.plus.com>
 *  SPDX-FileCopyrightText: 2004 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_brushop_settings_widget.h"
#include <KisBrushOpSettings.h>

#include <lager/state.hpp>

#include <KisMaskingBrushOption.h>
#include <KisPaintopSettingsIds.h>
#include "kis_brush_option_widget.h"
#include "KisSpacingOptionWidget.h"
#include "KisMirrorOptionWidget.h"
#include "KisSharpnessOptionWidget.h"
#include "KisScatterOptionWidget.h"
#include "KisAirbrushOptionWidget.h"
#include "KisCompositeOpOptionWidget.h"
#include "KisPaintingModeOptionWidget.h"
#include "KisColorSourceOptionWidget.h"
#include "KisLightnessStrengthOptionWidget.h"
#include "KisTextureOptionWidget.h"
#include "KisSizeOptionWidget.h"

#include <KisStandardOptionData.h>
#include <KisPaintOpOptionWidgetUtils.h>
#include <KisBrushBasedOptionStates.h>
#include <KisPaintOpOptionStateUtils.h>
#include <KisPaintOpOptionsModel.h>

namespace
{
const KisBrushOptionWidgetFlags brushOpFlags =
    KisBrushOptionWidgetFlag::SupportsPrecision | KisBrushOptionWidgetFlag::SupportsHSLBrushMode;

KisBrushTipOptionState *addBrushTipOption(KisPaintOpOptionsModel *model)
{
    KisBrushTipOptionState *state = new KisBrushTipOptionState(QStringLiteral("BrushTip"), brushOpFlags);
    model->addOption(state);
    return state;
}

/// Solstice: the option's checkbox can be shown in Tool Options under @p id,
/// the option's id in the options model (docs/agent/tool-options-brush.md)
KisPaintOpOption *withToolOptionsId(KisPaintOpOption *option, const QString &id)
{
    option->setToolOptionsId(id);
    return option;
}
} // namespace

KisBrushOpSettingsWidget::KisBrushOpSettingsWidget(QWidget* parent, KisResourcesInterfaceSP resourcesInterface, KoCanvasResourcesInterfaceSP canvasResourcesInterface)
    : KisBrushOpSettingsWidget(parent, resourcesInterface, canvasResourcesInterface, new KisPaintOpOptionsModel())
{
}

KisBrushOpSettingsWidget::KisBrushOpSettingsWidget(QWidget *parent,
                                                   KisResourcesInterfaceSP resourcesInterface,
                                                   KoCanvasResourcesInterfaceSP canvasResourcesInterface,
                                                   KisPaintOpOptionsModel *model)
    : KisBrushBasedPaintopOptionWidget(brushOpFlags, addBrushTipOption(model)->cursor(), parent)
{
    // TODO: pass into KisPaintOpSettingsWidget!
    Q_UNUSED(canvasResourcesInterface);

    setObjectName("brush option widget");

    namespace kpowu = KisPaintOpOptionWidgetUtils;
    namespace kposu = KisPaintOpOptionStateUtils;
    namespace kbbos = KisBrushBasedOptionStates;
    const QString maskingPrefix = KisPaintOpUtils::MaskingBrushPresetPrefix;

    // Solstice: the option states live in a shared model, so that one option
    // change writes that option and its dependents after the initial rewrite
    // (docs/agent/brush-option-shared-model.md). The brush tip is read first:
    // the masking brush reads with its size.
    model->setParent(this);

    KisBrushTipOptionState *brushTip = static_cast<KisBrushTipOptionState *>(model->option(QStringLiteral("BrushTip")));
    brushTip->setResourcesInterfaceGetter([this]() {
        return this->resourcesInterface();
    });

    KisMaskingBrushOptionState *masking =
        new KisMaskingBrushOptionState(QStringLiteral("MaskingBrush"), brushTip->effectiveBrushSize());
    masking->setResourcesInterfaceGetter([this]() {
        return this->resourcesInterface();
    });
    model->addOption(masking);

    auto *compositeOp = model->addOption(QStringLiteral("CompositeOp"), KisCompositeOpOptionData());
    auto *opacity = model->addOption(QStringLiteral("Opacity"),
                                     KisOpacityOptionData(),
                                     &kposu::bakeCurveOption<KisOpacityOptionData>);
    auto *flow =
        model->addOption(QStringLiteral("Flow"), KisFlowOptionData(), &kposu::bakeCurveOption<KisFlowOptionData>);
    auto *size =
        model->addOption(QStringLiteral("Size"), KisSizeOptionData(), &kposu::bakeCurveOption<KisSizeOptionData>);
    auto *ratio =
        model->addOption(QStringLiteral("Ratio"), KisRatioOptionData(), &kposu::bakeCurveOption<KisRatioOptionData>);
    auto *spacing = model->addOption(QStringLiteral("Spacing"),
                                     KisSpacingOptionData(),
                                     &kposu::bakeCurveOption<KisSpacingOptionData>);
    auto *mirror =
        model->addOption(QStringLiteral("Mirror"), KisMirrorOptionData(), &kposu::bakeCurveOption<KisMirrorOptionData>);
    auto *softness = model->addOption(QStringLiteral("Softness"),
                                      KisSoftnessOptionData(),
                                      &kposu::bakeCurveOption<KisSoftnessOptionData>);
    auto *rotation = model->addOption(QStringLiteral("Rotation"),
                                      KisRotationOptionData(),
                                      &kposu::bakeCurveOption<KisRotationOptionData>);
    auto *sharpness = model->addOption(QStringLiteral("Sharpness"),
                                       KisSharpnessOptionData(),
                                       &kposu::bakeCurveOption<KisSharpnessOptionData>);
    auto *lightnessStrength = model->addOption(
        QStringLiteral("LightnessStrength"),
        KisLightnessStrengthOptionData(),
        [brushTip](const KisLightnessStrengthOptionData &data) {
            return kposu::bakeLinkedCurveOption(data, brushTip->data().lightnessModeEnabled(brushTip->flags()));
        });
    auto *scatter = model->addOption(QStringLiteral("Scatter"),
                                     KisScatterOptionData(),
                                     &kposu::bakeCurveOption<KisScatterOptionData>);

    auto *colorSource = model->addOption(QStringLiteral("ColorSource"), KisColorSourceOptionData());
    auto *darken =
        model->addOption(QStringLiteral("Darken"), KisDarkenOptionData(), &kposu::bakeCurveOption<KisDarkenOptionData>);
    auto *mix = model->addOption(QStringLiteral("Mix"), KisMixOptionData(), &kposu::bakeCurveOption<KisMixOptionData>);
    auto *hue = model->addOption(QStringLiteral("Hue"), KisHueOptionData(), &kposu::bakeCurveOption<KisHueOptionData>);
    auto *saturation = model->addOption(QStringLiteral("Saturation"),
                                        KisSaturationOptionData(),
                                        &kposu::bakeCurveOption<KisSaturationOptionData>);
    auto *value =
        model->addOption(QStringLiteral("Value"), KisValueOptionData(), &kposu::bakeCurveOption<KisValueOptionData>);

    auto *airbrush = model->addOption(QStringLiteral("Airbrush"), KisAirbrushOptionData());
    auto *rate =
        model->addOption(QStringLiteral("Rate"), KisRateOptionData(), &kposu::bakeCurveOption<KisRateOptionData>);

    auto *paintingMode =
        model->addOption(QStringLiteral("PaintingMode"),
                         KisPaintingModeOptionData(),
                         [masking](const KisPaintingModeOptionData &data) {
                             return kbbos::bakePaintingModeOption(data, masking->data().masking.isEnabled);
                         });
    auto *texture = model->addOption(QStringLiteral("Texture"),
                                     KisTextureOptionData(),
                                     [resourcesInterface](const KisTextureOptionData &data) {
                                         return kbbos::bakeTextureOption(data, resourcesInterface);
                                     });
    auto *strength = model->addOption(QStringLiteral("Strength"),
                                      KisStrengthOptionData(),
                                      &kposu::bakeCurveOption<KisStrengthOptionData>);

    auto *maskingOpacity = model->addOption(QStringLiteral("MaskingOpacity"),
                                            KisOpacityOptionData(maskingPrefix),
                                            &kposu::bakeCurveOption<KisOpacityOptionData>);
    auto *maskingFlow = model->addOption(QStringLiteral("MaskingFlow"),
                                         KisFlowOptionData(maskingPrefix),
                                         &kposu::bakeCurveOption<KisFlowOptionData>);
    auto *maskingSize = model->addOption(QStringLiteral("MaskingSize"),
                                         KisSizeOptionData(maskingPrefix),
                                         &kposu::bakeCurveOption<KisSizeOptionData>);
    auto *maskingRatio = model->addOption(QStringLiteral("MaskingRatio"),
                                          KisRatioOptionData(maskingPrefix),
                                          &kposu::bakeCurveOption<KisRatioOptionData>);
    auto *maskingRotation = model->addOption(QStringLiteral("MaskingRotation"),
                                             KisRotationOptionData(maskingPrefix),
                                             &kposu::bakeCurveOption<KisRotationOptionData>);
    auto *maskingMirror = model->addOption(QStringLiteral("MaskingMirror"),
                                           KisMirrorOptionData(maskingPrefix),
                                           &kposu::bakeCurveOption<KisMirrorOptionData>);
    auto *maskingScatter = model->addOption(QStringLiteral("MaskingScatter"),
                                            KisScatterOptionData(maskingPrefix),
                                            &kposu::bakeCurveOption<KisScatterOptionData>);

    // written (baked) data that depends on another option
    model->addDependency(QStringLiteral("MaskingBrush"), QStringLiteral("BrushTip"));
    model->addDependency(QStringLiteral("LightnessStrength"), QStringLiteral("BrushTip"));
    model->addDependency(QStringLiteral("PaintingMode"), QStringLiteral("MaskingBrush"));

    // Brush tip options
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisCompositeOpOptionWidget>(compositeOp),
                                       QStringLiteral("CompositeOp")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createOpacityOptionWidget(kposu::curveCursor(opacity)), QStringLiteral("Opacity")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createFlowOptionWidget(kposu::curveCursor(flow)), QStringLiteral("Flow")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisSizeOptionWidget>(size), QStringLiteral("Size")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRatioOptionWidget(kposu::curveCursor(ratio)), QStringLiteral("Ratio")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisSpacingOptionWidget>(spacing), QStringLiteral("Spacing")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisMirrorOptionWidget>(mirror), QStringLiteral("Mirror")));

    addPaintOpOption(
        withToolOptionsId(kpowu::createSoftnessOptionWidget(kposu::curveCursor(softness)), QStringLiteral("Softness")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRotationOptionWidget(kposu::curveCursor(rotation)), QStringLiteral("Rotation")));

    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisSharpnessOptionWidget>(sharpness), QStringLiteral("Sharpness")));
    addPaintOpOption(withToolOptionsId(
        kposu::createOptionWidget<KisLightnessStrengthOptionWidget>(lightnessStrength,
                                                                    brushOptionWidget()->lightnessModeEnabled()),
        QStringLiteral("LightnessStrength")));

    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisScatterOptionWidget>(scatter), QStringLiteral("Scatter")));

    // Colors options
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisColorSourceOptionWidget>(colorSource),
                                       QStringLiteral("ColorSource")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createDarkenOptionWidget(kposu::curveCursor(darken)), QStringLiteral("Darken")));
    addPaintOpOption(withToolOptionsId(kpowu::createMixOptionWidget(kposu::curveCursor(mix)), QStringLiteral("Mix")));
    addPaintOpOption(withToolOptionsId(kpowu::createHueOptionWidget(kposu::curveCursor(hue)), QStringLiteral("Hue")));
    addPaintOpOption(withToolOptionsId(kpowu::createSaturationOptionWidget(kposu::curveCursor(saturation)),
                                       QStringLiteral("Saturation")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createValueOptionWidget(kposu::curveCursor(value)), QStringLiteral("Value")));

    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisAirbrushOptionWidget>(airbrush), QStringLiteral("Airbrush")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRateOptionWidget(kposu::curveCursor(rate)), QStringLiteral("Rate")));

    KisMaskingBrushOption *maskingOption =
        new KisMaskingBrushOption(masking->cursor(), brushOptionWidget()->effectiveBrushSize());
    addPaintOpOption(withToolOptionsId(
        kposu::createOptionWidget<KisPaintingModeOptionWidget>(paintingMode,
                                                               maskingOption->maskingBrushEnabledReader()),
        QStringLiteral("PaintingMode")));

    addPaintOpOption(withToolOptionsId(
        kposu::createOptionWidget<KisTextureOptionWidget>(texture,
                                                          resourcesInterface,
                                                          SupportsLightnessMode | SupportsGradientMode),
        QStringLiteral("Texture")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createStrengthOptionWidget(kposu::curveCursor(strength)), QStringLiteral("Strength")));

    addPaintOpOption(withToolOptionsId(maskingOption, QStringLiteral("MaskingBrush")));

    addPaintOpOption(withToolOptionsId(kpowu::createMaskingOpacityOptionWidget(kposu::curveCursor(maskingOpacity)),
                                       QStringLiteral("MaskingOpacity")));
    addPaintOpOption(withToolOptionsId(kpowu::createMaskingFlowOptionWidget(kposu::curveCursor(maskingFlow)),
                                       QStringLiteral("MaskingFlow")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisSizeOptionWidget>(maskingSize, KisPaintOpOption::MASKING_BRUSH),
                          QStringLiteral("MaskingSize")));
    addPaintOpOption(withToolOptionsId(kpowu::createMaskingRatioOptionWidget(kposu::curveCursor(maskingRatio)),
                                       QStringLiteral("MaskingRatio")));
    addPaintOpOption(withToolOptionsId(kpowu::createMaskingRotationOptionWidget(kposu::curveCursor(maskingRotation)),
                                       QStringLiteral("MaskingRotation")));
    addPaintOpOption(withToolOptionsId(
        kposu::createOptionWidget<KisMirrorOptionWidget>(maskingMirror, KisPaintOpOption::MASKING_BRUSH),
        QStringLiteral("MaskingMirror")));
    addPaintOpOption(withToolOptionsId(
        kposu::createOptionWidget<KisScatterOptionWidget>(maskingScatter, KisPaintOpOption::MASKING_BRUSH),
        QStringLiteral("MaskingScatter")));

    // the brush tip page's parameters can be shown in Tool Options
    brushOptionWidget()->setToolOptionsId(QStringLiteral("BrushTip"));

    setOptionsModel(model);
}

KisBrushOpSettingsWidget::~KisBrushOpSettingsWidget()
{
}

KisPropertiesConfigurationSP KisBrushOpSettingsWidget::configuration() const
{
    KisBrushBasedPaintOpSettingsSP config = new KisBrushOpSettings(resourcesInterface());
    config->setProperty("paintop", "paintbrush"); // XXX: make this a const id string
    writeConfiguration(config);
    return config;
}
