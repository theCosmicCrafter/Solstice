/*
 *  SPDX-FileCopyrightText: 2008, 2009, 2010 Lukáš Tvrdý <lukast.dev@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "kis_deform_paintop_settings.h"
#include "kis_deform_paintop_settings_widget.h"
#include "KisDeformOptionWidget.h"

#include <kis_paintop_settings_widget.h>
#include "KisBrushSizeOptionWidget.h"

#include <KisStandardOptionData.h>
#include <KisSizeOptionWidget.h>
#include <KisAirbrushOptionWidget.h>
#include <KisPaintOpOptionStateUtils.h>
#include <KisCompositeOpOptionWidget.h>
#include <KisCurveOptionWidget.h>

namespace
{
/// Solstice: the option's checkbox can be shown in Tool Options under @p id,
/// the option's id in the options model (docs/agent/tool-options-brush.md)
KisPaintOpOption *withToolOptionsId(KisPaintOpOption *option, const QString &id)
{
    option->setToolOptionsId(id);
    return option;
}
} // namespace

KisDeformPaintOpSettingsWidget::KisDeformPaintOpSettingsWidget(QWidget* parent)
    : KisPaintOpSettingsWidget(parent)
{
    namespace kpowu = KisPaintOpOptionWidgetUtils;
    namespace kposu = KisPaintOpOptionStateUtils;

    // Option states live in a shared model so that one option change writes
    // that option after the initial rewrite (docs/agent/brush-option-shared-model.md).
    KisPaintOpOptionsModel *model = new KisPaintOpOptionsModel(this);

    auto *brushSize = model->addOption(QStringLiteral("BrushSize"), KisBrushSizeOptionData());
    auto *deform = model->addOption(QStringLiteral("Deform"), KisDeformOptionData());
    auto *compositeOp = model->addOption(QStringLiteral("CompositeOp"), KisCompositeOpOptionData());
    auto *opacity = model->addOption(QStringLiteral("Opacity"),
                                     KisOpacityOptionData(),
                                     &kposu::bakeCurveOption<KisOpacityOptionData>);
    auto *size =
        model->addOption(QStringLiteral("Size"), KisSizeOptionData(), &kposu::bakeCurveOption<KisSizeOptionData>);
    auto *rotation = model->addOption(QStringLiteral("Rotation"),
                                      KisRotationOptionData(),
                                      &kposu::bakeCurveOption<KisRotationOptionData>);
    auto *airbrush = model->addOption(QStringLiteral("Airbrush"), KisAirbrushOptionData());
    auto *rate =
        model->addOption(QStringLiteral("Rate"), KisRateOptionData(), &kposu::bakeCurveOption<KisRateOptionData>);

    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisBrushSizeOptionWidget>(brushSize), QStringLiteral("BrushSize")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidgetWithLodLimitations<KisDeformOptionWidget>(deform),
                                       QStringLiteral("Deform")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisCompositeOpOptionWidget>(compositeOp),
                                       QStringLiteral("CompositeOp")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createOpacityOptionWidget(kposu::curveCursor(opacity)), QStringLiteral("Opacity")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisSizeOptionWidget>(size), QStringLiteral("Size")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRotationOptionWidget(kposu::curveCursor(rotation)), QStringLiteral("Rotation")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisAirbrushOptionWidget>(airbrush), QStringLiteral("Airbrush")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRateOptionWidget(kposu::curveCursor(rate)), QStringLiteral("Rate")));

    setOptionsModel(model);
}

KisDeformPaintOpSettingsWidget::~ KisDeformPaintOpSettingsWidget()
{
}


KisPropertiesConfigurationSP KisDeformPaintOpSettingsWidget::configuration() const
{
    KisDeformPaintOpSettings* config = new KisDeformPaintOpSettings(resourcesInterface());
    config->setProperty("paintop", "deformBrush");
    writeConfiguration(config);
    return config;
}

