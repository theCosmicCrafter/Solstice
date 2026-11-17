/*
 *  SPDX-FileCopyrightText: 2008 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2008 Lukáš Tvrdý <lukast.dev@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "kis_hairy_paintop_settings_widget.h"
#include "kis_hairy_paintop_settings.h"

#include "KisHairyBristleOptionWidget.h"
#include "KisHairyInkOptionWidget.h"

#include <KisPaintOpOptionWidgetUtils.h>
#include <KisCompositeOpOptionWidget.h>
#include <KisStandardOptionData.h>
#include <KisSizeOptionWidget.h>
#include <KisPaintingModeOptionWidget.h>
#include <kis_brush_option_widget.h>
#include <KisBrushBasedOptionStates.h>
#include <KisPaintOpOptionStateUtils.h>
#include <KisPaintOpOptionsModel.h>

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

KisHairyPaintOpSettingsWidget:: KisHairyPaintOpSettingsWidget(QWidget* parent)
    : KisHairyPaintOpSettingsWidget(parent, new KisPaintOpOptionsModel())
{
}

KisHairyPaintOpSettingsWidget::KisHairyPaintOpSettingsWidget(QWidget *parent, KisPaintOpOptionsModel *model)
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

    auto *bristle = model->addOption(QStringLiteral("Bristle"), KisHairyBristleOptionData());
    auto *ink = model->addOption(QStringLiteral("Ink"), KisHairyInkOptionData());
    auto *compositeOp = model->addOption(QStringLiteral("CompositeOp"), KisCompositeOpOptionData());
    auto *opacity = model->addOption(QStringLiteral("Opacity"),
                                     KisOpacityOptionData(),
                                     &kposu::bakeCurveOption<KisOpacityOptionData>);
    auto *size =
        model->addOption(QStringLiteral("Size"), KisSizeOptionData(), &kposu::bakeCurveOption<KisSizeOptionData>);
    auto *rotation = model->addOption(QStringLiteral("Rotation"),
                                      KisRotationOptionData(),
                                      &kposu::bakeCurveOption<KisRotationOptionData>);
    auto *paintingMode = model->addOption(QStringLiteral("PaintingMode"), KisPaintingModeOptionData());

    brushOptionWidget()->setToolOptionsId(QStringLiteral("BrushTip"));

    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidgetWithLodLimitations<KisHairyBristleOptionWidget>(bristle),
                          QStringLiteral("Bristle")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisHairyInkOptionWidget>(ink), QStringLiteral("Ink")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisCompositeOpOptionWidget>(compositeOp),
                                       QStringLiteral("CompositeOp")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createOpacityOptionWidget(kposu::curveCursor(opacity)), QStringLiteral("Opacity")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisSizeOptionWidget>(size), QStringLiteral("Size")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRotationOptionWidget(kposu::curveCursor(rotation)), QStringLiteral("Rotation")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisPaintingModeOptionWidget>(paintingMode),
                                       QStringLiteral("PaintingMode")));

    KisBrushOptionWidget *brushWidget = brushOptionWidget();
    QStringList hiddenOptions;
    hiddenOptions << "KisBrushChooser/lblSpacing"
                  << "KisBrushChooser/Spacing"
                  << "KisBrushChooser/ColorAsMask"
                  << "KisAutoBrushWidget/btnAntiAliasing"
                  << "KisAutoBrushWidget/grpFade"
                  << "KisAutoBrushWidget/lblDensity"
                  << "KisAutoBrushWidget/density"
                  << "KisAutoBrushWidget/lblSpacing"
                  << "KisAutoBrushWidget/spacingWidget"
                  << "KisAutoBrushWidget/lblRandomness"
                  << "KisAutoBrushWidget/inputRandomness"
                     ;
    brushWidget->hideOptions(hiddenOptions);

    setOptionsModel(model);
}

KisHairyPaintOpSettingsWidget::~ KisHairyPaintOpSettingsWidget()
{
}

KisPropertiesConfigurationSP  KisHairyPaintOpSettingsWidget::configuration() const
{
    KisHairyPaintOpSettings* config = new KisHairyPaintOpSettings(resourcesInterface());
    config->setProperty("paintop", "hairybrush"); // XXX: make this a const id string
    writeConfiguration(config);
    return config;
}
