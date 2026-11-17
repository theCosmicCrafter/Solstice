/*
 *  SPDX-FileCopyrightText: 2010 Lukáš Tvrdý <lukast.dev@gmail.com>
 *  SPDX-FileCopyrightText: 2010 José Luis Vergara <pentalis@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_hatching_paintop_settings_widget.h"

#include "kis_hatching_paintop_settings.h"

#include "KisHatchingStandardOptions.h"

#include <kis_brush_option_widget.h>
#include <kis_paintop_settings_widget.h>
#include <KisPaintOpOptionWidgetUtils.h>

#include "KisHatchingOptionsWidget.h"
#include "KisHatchingPreferencesWidget.h"
#include <KisStandardOptionData.h>
#include <KisCompositeOpOptionWidget.h>
#include "KisSizeOptionWidget.h"
#include "KisMirrorOptionWidget.h"
#include <KisPaintingModeOptionWidget.h>
#include "KisTextureOptionWidget.h"
#include <KisBrushBasedOptionStates.h>
#include <KisCurveOptionWidget.h>
#include <KisPaintOpOptionStateUtils.h>
#include <KisPaintOpOptionsModel.h>

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

KisHatchingPaintOpSettingsWidget:: KisHatchingPaintOpSettingsWidget(QWidget* parent, KisResourcesInterfaceSP resourcesInterface, KoCanvasResourcesInterfaceSP canvasResourcesInterface)
    : KisHatchingPaintOpSettingsWidget(parent, resourcesInterface, canvasResourcesInterface, new KisPaintOpOptionsModel())
{
}

KisHatchingPaintOpSettingsWidget::KisHatchingPaintOpSettingsWidget(
    QWidget *parent,
    KisResourcesInterfaceSP resourcesInterface,
    KoCanvasResourcesInterfaceSP canvasResourcesInterface,
    KisPaintOpOptionsModel *model)
    : KisBrushBasedPaintopOptionWidget(KisBrushOptionWidgetFlag::SupportsPrecision,
                                       addBrushTipOption(model, KisBrushOptionWidgetFlag::SupportsPrecision)->cursor(),
                                       parent)
{
    Q_UNUSED(canvasResourcesInterface)
    namespace kpowu = KisPaintOpOptionWidgetUtils;
    namespace kposu = KisPaintOpOptionStateUtils;
    namespace kbbos = KisBrushBasedOptionStates;

    // Solstice: the option states live in a shared model, so that one option
    // change writes that option and its dependents after the initial rewrite
    // (docs/agent/brush-option-shared-model.md); no option's written data depends on another
    model->setParent(this);

    KisBrushTipOptionState *brushTip = static_cast<KisBrushTipOptionState *>(model->option(QStringLiteral("BrushTip")));
    brushTip->setResourcesInterfaceGetter([this]() {
        return this->resourcesInterface();
    });

    auto *hatchingOptions = model->addOption(QStringLiteral("HatchingOptions"), KisHatchingOptionsData());
    auto *hatchingPreferences = model->addOption(QStringLiteral("HatchingPreferences"), KisHatchingPreferencesData());
    auto *compositeOp = model->addOption(QStringLiteral("CompositeOp"), KisCompositeOpOptionData());
    auto *separation = model->addOption(QStringLiteral("Separation"),
                                        KisSeparationOptionData(),
                                        &kposu::bakeCurveOption<KisSeparationOptionData>);
    auto *thickness = model->addOption(QStringLiteral("Thickness"),
                                       KisThicknessOptionData(),
                                       &kposu::bakeCurveOption<KisThicknessOptionData>);
    auto *angle =
        model->addOption(QStringLiteral("Angle"), KisAngleOptionData(), &kposu::bakeCurveOption<KisAngleOptionData>);
    auto *crosshatching = model->addOption(QStringLiteral("Crosshatching"),
                                           KisCrosshatchingOptionData(),
                                           &kposu::bakeCurveOption<KisCrosshatchingOptionData>);
    auto *opacity = model->addOption(QStringLiteral("Opacity"),
                                     KisOpacityOptionData(),
                                     &kposu::bakeCurveOption<KisOpacityOptionData>);
    auto *size =
        model->addOption(QStringLiteral("Size"), KisSizeOptionData(), &kposu::bakeCurveOption<KisSizeOptionData>);
    auto *mirror =
        model->addOption(QStringLiteral("Mirror"), KisMirrorOptionData(), &kposu::bakeCurveOption<KisMirrorOptionData>);
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

    //-------Adding widgets to the screen------------

    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidgetWithLodLimitations<KisHatchingOptionsWidget>(hatchingOptions),
                          QStringLiteral("HatchingOptions")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisHatchingPreferencesWidget>(hatchingPreferences),
                                       QStringLiteral("HatchingPreferences")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisCompositeOpOptionWidget>(compositeOp),
                                       QStringLiteral("CompositeOp")));
    addPaintOpOption(withToolOptionsId(
        new KisCurveOptionWidget(kposu::curveCursor(separation), KisPaintOpOption::GENERAL, i18n("0.0"), i18n("1.0")),
        QStringLiteral("Separation")));
    addPaintOpOption(withToolOptionsId(
        new KisCurveOptionWidget(kposu::curveCursor(thickness), KisPaintOpOption::GENERAL, i18n("0.0"), i18n("1.0")),
        QStringLiteral("Thickness")));
    addPaintOpOption(withToolOptionsId(
        new KisCurveOptionWidget(kposu::curveCursor(angle), KisPaintOpOption::GENERAL, i18n("0.0"), i18n("1.0")),
        QStringLiteral("Angle")));
    addPaintOpOption(withToolOptionsId(new KisCurveOptionWidget(kposu::curveCursor(crosshatching),
                                                                KisPaintOpOption::GENERAL,
                                                                i18n("0.0"),
                                                                i18n("1.0")),
                                       QStringLiteral("Crosshatching")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createOpacityOptionWidget(kposu::curveCursor(opacity)), QStringLiteral("Opacity")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisSizeOptionWidget>(size), QStringLiteral("Size")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisMirrorOptionWidget>(mirror), QStringLiteral("Mirror")));

    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisPaintingModeOptionWidget>(paintingMode),
                                       QStringLiteral("PaintingMode")));

    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisTextureOptionWidget>(texture, resourcesInterface),
                                       QStringLiteral("Texture")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createStrengthOptionWidget(kposu::curveCursor(strength)), QStringLiteral("Strength")));

    setOptionsModel(model);
}

KisHatchingPaintOpSettingsWidget::~ KisHatchingPaintOpSettingsWidget()
{
}

KisPropertiesConfigurationSP  KisHatchingPaintOpSettingsWidget::configuration() const
{
    KisHatchingPaintOpSettingsSP config = new KisHatchingPaintOpSettings(resourcesInterface());
    config->setProperty("paintop", "hatchingbrush"); // XXX: make this a const id string
    writeConfiguration(config);
    return config;
}
