/*
 *  SPDX-FileCopyrightText: 2011 Silvio Heinrich <plassy@web.de>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_colorsmudgeop_settings_widget.h"
#include "kis_brush_based_paintop_settings.h"
#include "kis_brush_option_widget.h"

#include <kis_properties_configuration.h>
#include <kis_paintop_settings_widget.h>
#include "kis_colorsmudgeop_settings.h"
#include "kis_signals_blocker.h"
#include <KisAirbrushOptionWidget.h>
#include <KisPaintOpOptionWidgetUtils.h>
#include <KisTextureOptionWidget.h>
#include <KisStandardOptionData.h>
#include <KisCompositeOpOptionWidget.h>
#include <KisSpacingOptionWidget.h>
#include <KisSizeOptionWidget.h>
#include <KisMirrorOptionWidget.h>
#include <KisScatterOptionWidget.h>
#include <KisSmudgeLengthOptionWidget.h>
#include <KisPaintThicknessOptionWidget.h>
#include <KisSmudgeOverlayModeOptionWidget.h>
#include <KisBrushPropertiesModel.h>
#include <KisColorSmudgeStandardOptionData.h>
#include <KisSmudgeRadiusOptionData.h>
#include <KisZug.h>
#include <KisBrushBasedOptionStates.h>
#include <KisCurveOptionModel.h>
#include <KisCurveOptionWidget.h>
#include <KisPaintOpOptionStateUtils.h>
#include <KisPaintOpOptionsModel.h>
#include <lager/constant.hpp>


struct KisColorSmudgeOpSettingsWidget::Private
{
    Private(lager::reader<KisBrushModel::BrushData> brushData,
            KisResourcesInterfaceSP resourcesInterface)
        : brushPropertiesModel(brushData, resourcesInterface)
    {
    }

    KisBrushPropertiesModel brushPropertiesModel;
};

namespace
{
const KisBrushOptionWidgetFlags colorSmudgeFlags =
    KisBrushOptionWidgetFlag::SupportsPrecision | KisBrushOptionWidgetFlag::SupportsHSLBrushMode;

KisBrushTipOptionState *addBrushTipOption(KisPaintOpOptionsModel *model)
{
    KisBrushTipOptionState *state = new KisBrushTipOptionState(QStringLiteral("BrushTip"), colorSmudgeFlags);
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

/// A tip used as an image (color, lightness or gradient map) needs the new
/// smudge engine, as KisBrushPropertiesModel::brushApplication tells the
/// Smudge Length option
bool forcesNewSmudgeEngine(const KisBrushTipOptionState *brushTip)
{
    const KisBrushModel::BrushData brush = brushTip->data().bakedBrushData(brushTip->flags());
    const enumBrushApplication application =
        brush.type == KisBrushModel::Predefined ? brush.predefinedBrush.application : ALPHAMASK;
    return application > ALPHAMASK;
}
} // namespace

KisColorSmudgeOpSettingsWidget::KisColorSmudgeOpSettingsWidget(QWidget* parent, KisResourcesInterfaceSP resourcesInterface, KoCanvasResourcesInterfaceSP canvasResourcesInterface)
    : KisColorSmudgeOpSettingsWidget(parent, resourcesInterface, canvasResourcesInterface, new KisPaintOpOptionsModel())
{
}

KisColorSmudgeOpSettingsWidget::KisColorSmudgeOpSettingsWidget(QWidget *parent,
                                                               KisResourcesInterfaceSP resourcesInterface,
                                                               KoCanvasResourcesInterfaceSP canvasResourcesInterface,
                                                               KisPaintOpOptionsModel *model)
    : KisBrushBasedPaintopOptionWidget(colorSmudgeFlags, addBrushTipOption(model)->cursor(), parent)
    , m_d(new Private(brushOptionWidget()->bakedBrushData(), resourcesInterface))
{
    Q_UNUSED(canvasResourcesInterface)
    namespace kpowu = KisPaintOpOptionWidgetUtils;
    namespace kposu = KisPaintOpOptionStateUtils;
    namespace kbbos = KisBrushBasedOptionStates;

    setObjectName("brush option widget");

    // Solstice: the option states live in a shared model, so that one option
    // change writes that option and its dependents after the initial rewrite
    // (docs/agent/brush-option-shared-model.md)
    model->setParent(this);

    KisBrushTipOptionState *brushTip = static_cast<KisBrushTipOptionState *>(model->option(QStringLiteral("BrushTip")));
    brushTip->setResourcesInterfaceGetter([this]() {
        return this->resourcesInterface();
    });

    auto *compositeOp = model->addOption(QStringLiteral("CompositeOp"), KisCompositeOpOptionData());
    auto *opacity = model->addOption(QStringLiteral("Opacity"),
                                     KisOpacityOptionData(),
                                     &kposu::bakeCurveOption<KisOpacityOptionData>);
    auto *size =
        model->addOption(QStringLiteral("Size"), KisSizeOptionData(), &kposu::bakeCurveOption<KisSizeOptionData>);
    auto *ratio =
        model->addOption(QStringLiteral("Ratio"), KisRatioOptionData(), &kposu::bakeCurveOption<KisRatioOptionData>);
    auto *spacing = model->addOption(QStringLiteral("Spacing"),
                                     KisSpacingOptionData(),
                                     &kposu::bakeCurveOption<KisSpacingOptionData>);
    auto *mirror =
        model->addOption(QStringLiteral("Mirror"), KisMirrorOptionData(), &kposu::bakeCurveOption<KisMirrorOptionData>);

    // written with the new engine when the tip requires it
    auto *smudgeLength = model->addOption(QStringLiteral("SmudgeLength"),
                                          KisSmudgeLengthOptionData(),
                                          [brushTip](const KisSmudgeLengthOptionData &data) {
                                              KisSmudgeLengthOptionData result = kposu::bakeCurveOption(data);
                                              result.useNewEngine =
                                                  data.useNewEngine || forcesNewSmudgeEngine(brushTip);
                                              return result;
                                          });
    // its strength range is 0..1 with the new engine, 0..3 with the old one
    auto *smudgeRadius = model->addOption(
        QStringLiteral("SmudgeRadius"),
        KisSmudgeRadiusOptionData(),
        [brushTip, smudgeLength](const KisSmudgeRadiusOptionData &data) {
            const bool useNewEngine = smudgeLength->data().useNewEngine || forcesNewSmudgeEngine(brushTip);
            KisSmudgeRadiusOptionData result = data;
            static_cast<KisCurveOptionDataCommon &>(result) =
                KisCurveOptionModel::bakeOptionData(data, true, std::make_tuple(0.0, useNewEngine ? 1.0 : 3.0));
            return result;
        });
    auto *colorRate = model->addOption(QStringLiteral("ColorRate"),
                                       KisColorRateOptionData(),
                                       &kposu::bakeCurveOption<KisColorRateOptionData>);
    // only in the tip's lightness mode
    auto *paintThickness = model->addOption(
        QStringLiteral("PaintThickness"),
        KisPaintThicknessOptionData(),
        [brushTip](const KisPaintThicknessOptionData &data) {
            return kposu::bakeLinkedCurveOption(data, brushTip->data().lightnessModeEnabled(brushTip->flags()));
        });
    auto *rotation = model->addOption(QStringLiteral("Rotation"),
                                      KisRotationOptionData(),
                                      &kposu::bakeCurveOption<KisRotationOptionData>);
    auto *scatter = model->addOption(QStringLiteral("Scatter"),
                                     KisScatterOptionData(),
                                     &kposu::bakeCurveOption<KisScatterOptionData>);
    // not in the tip's lightness mode
    auto *overlayMode = model->addOption(QStringLiteral("OverlayMode"),
                                         KisSmudgeOverlayModeOptionData(),
                                         [brushTip](const KisSmudgeOverlayModeOptionData &data) {
                                             KisSmudgeOverlayModeOptionData result = data;
                                             result.isChecked &=
                                                 !brushTip->data().lightnessModeEnabled(brushTip->flags());
                                             return result;
                                         });
    auto *gradient = model->addOption(QStringLiteral("Gradient"),
                                      KisGradientOptionData(),
                                      &kposu::bakeCurveOption<KisGradientOptionData>);
    auto *hue = model->addOption(QStringLiteral("Hue"), KisHueOptionData(), &kposu::bakeCurveOption<KisHueOptionData>);
    auto *saturation = model->addOption(QStringLiteral("Saturation"),
                                        KisSaturationOptionData(),
                                        &kposu::bakeCurveOption<KisSaturationOptionData>);
    auto *value =
        model->addOption(QStringLiteral("Value"), KisValueOptionData(), &kposu::bakeCurveOption<KisValueOptionData>);
    auto *airbrush = model->addOption(QStringLiteral("Airbrush"), KisAirbrushOptionData());
    auto *rate =
        model->addOption(QStringLiteral("Rate"), KisRateOptionData(), &kposu::bakeCurveOption<KisRateOptionData>);
    auto *texture = model->addOption(QStringLiteral("Texture"),
                                     KisTextureOptionData(),
                                     [resourcesInterface](const KisTextureOptionData &data) {
                                         return kbbos::bakeTextureOption(data, resourcesInterface);
                                     });
    auto *strength = model->addOption(QStringLiteral("Strength"),
                                      KisStrengthOptionData(),
                                      &kposu::bakeCurveOption<KisStrengthOptionData>);

    // written (baked) data that depends on another option
    model->addDependency(QStringLiteral("SmudgeLength"), QStringLiteral("BrushTip"));
    model->addDependency(QStringLiteral("SmudgeRadius"), QStringLiteral("SmudgeLength"));
    model->addDependency(QStringLiteral("PaintThickness"), QStringLiteral("BrushTip"));
    model->addDependency(QStringLiteral("OverlayMode"), QStringLiteral("BrushTip"));

    brushOptionWidget()->setToolOptionsId(QStringLiteral("BrushTip"));

    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisCompositeOpOptionWidget>(compositeOp),
                                       QStringLiteral("CompositeOp")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createOpacityOptionWidget(kposu::curveCursor(opacity)), QStringLiteral("Opacity")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisSizeOptionWidget>(size), QStringLiteral("Size")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRatioOptionWidget(kposu::curveCursor(ratio)), QStringLiteral("Ratio")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisSpacingOptionWidget>(spacing), QStringLiteral("Spacing")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisMirrorOptionWidget>(mirror), QStringLiteral("Mirror")));

    KisSmudgeLengthOptionWidget *smudgeLengthWidget = kposu::createOptionWidget<KisSmudgeLengthOptionWidget>(
        smudgeLength,
        m_d->brushPropertiesModel.isBrushPierced,
        m_d->brushPropertiesModel.brushApplication.xform(kiszug::map_greater<int>(ALPHAMASK)));

    addPaintOpOption(withToolOptionsId(smudgeLengthWidget, QStringLiteral("SmudgeLength")));

    lager::reader<std::tuple<qreal, qreal>> rangeReader =
        smudgeLengthWidget->useNewEngine()
            .map([] (bool useNewEngine) {
                return std::make_tuple(0.0,
                                       useNewEngine ? 1.0 : 3.0);
            });

    KisCurveOptionWidget *smudgeRadiusWidget = new KisCurveOptionWidget(kposu::curveCursor(smudgeRadius),
                                                                        KisPaintOpOption::GENERAL,
                                                                        lager::make_constant(true),
                                                                        rangeReader);

    addPaintOpOption(withToolOptionsId(smudgeRadiusWidget, QStringLiteral("SmudgeRadius")));

    addPaintOpOption(
        withToolOptionsId(new KisCurveOptionWidget(kposu::curveCursor(colorRate), KisPaintOpOption::GENERAL),
                          QStringLiteral("ColorRate")));

    addPaintOpOption(withToolOptionsId(
        kposu::createOptionWidget<KisPaintThicknessOptionWidget>(paintThickness,
                                                                 brushOptionWidget()->lightnessModeEnabled()),
        QStringLiteral("PaintThickness")));

    addPaintOpOption(
        withToolOptionsId(kpowu::createRotationOptionWidget(kposu::curveCursor(rotation)), QStringLiteral("Rotation")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisScatterOptionWidget>(scatter), QStringLiteral("Scatter")));

    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisSmudgeOverlayModeOptionWidget>(
                                           overlayMode,
                                           brushOptionWidget()->lightnessModeEnabled().map(std::logical_not{})),
                                       QStringLiteral("OverlayMode")));

    addPaintOpOption(
        withToolOptionsId(new KisCurveOptionWidget(kposu::curveCursor(gradient), KisPaintOpOption::GENERAL),
                          QStringLiteral("Gradient")));

    addPaintOpOption(withToolOptionsId(kpowu::createHueOptionWidget(kposu::curveCursor(hue)), QStringLiteral("Hue")));
    addPaintOpOption(withToolOptionsId(kpowu::createSaturationOptionWidget(kposu::curveCursor(saturation)),
                                       QStringLiteral("Saturation")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createValueOptionWidget(kposu::curveCursor(value)), QStringLiteral("Value")));

    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisAirbrushOptionWidget>(airbrush), QStringLiteral("Airbrush")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRateOptionWidget(kposu::curveCursor(rate)), QStringLiteral("Rate")));

    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisTextureOptionWidget>(texture, resourcesInterface),
                                       QStringLiteral("Texture")));
    addPaintOpOption(withToolOptionsId(
        new KisCurveOptionWidget(kposu::curveCursor(strength), KisPaintOpOption::COLOR, i18n("Weak"), i18n("Strong")),
        QStringLiteral("Strength")));

    setOptionsModel(model);
}

KisColorSmudgeOpSettingsWidget::~KisColorSmudgeOpSettingsWidget() { }

KisPropertiesConfigurationSP KisColorSmudgeOpSettingsWidget::configuration() const
{
    KisColorSmudgeOpSettingsSP config = new KisColorSmudgeOpSettings(resourcesInterface());
    config->setProperty("paintop", "colorsmudge");
    writeConfiguration(config);
    return config;
}
