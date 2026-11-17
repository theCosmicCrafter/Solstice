/*
 *  SPDX-FileCopyrightText: 2008, 2010 Lukáš Tvrdý <lukast.dev@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <kis_curve_paintop_settings_widget.h>
#include <kis_properties_configuration.h>
#include <kis_curve_paintop_settings.h>

#include <KisPaintingModeOptionWidget.h>
#include <KisCurveOpOptionWidget.h>
#include <KisPaintOpOptionWidgetUtils.h>
#include <KisCompositeOpOptionWidget.h>
#include <KisStandardOptionData.h>
#include <KisCurveStandardOptionData.h>
#include <KisCurveOptionWidget.h>
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

KisCurvePaintOpSettingsWidget:: KisCurvePaintOpSettingsWidget(QWidget* parent)
    : KisPaintOpSettingsWidget(parent)
{
    namespace kpowu = KisPaintOpOptionWidgetUtils;

    namespace kposu = KisPaintOpOptionStateUtils;

    // Solstice: the option states live in a shared model, so that one option
    // change writes that option and its dependents after the initial rewrite
    // (docs/agent/brush-option-shared-model.md); no option's written data depends on another
    KisPaintOpOptionsModel *model = new KisPaintOpOptionsModel(this);

    auto *curveOp = model->addOption(QStringLiteral("CurveOp"), KisCurveOpOptionData());
    auto *opacity = model->addOption(QStringLiteral("Opacity"),
                                     KisOpacityOptionData(),
                                     &kposu::bakeCurveOption<KisOpacityOptionData>);
    auto *lineWidth = model->addOption(QStringLiteral("LineWidth"),
                                       KisLineWidthOptionData(),
                                       &kposu::bakeCurveOption<KisLineWidthOptionData>);
    auto *curvesOpacity = model->addOption(QStringLiteral("CurvesOpacity"),
                                           KisCurvesOpacityOptionData(),
                                           &kposu::bakeCurveOption<KisCurvesOpacityOptionData>);
    auto *compositeOp = model->addOption(QStringLiteral("CompositeOp"), KisCompositeOpOptionData());
    auto *paintingMode = model->addOption(QStringLiteral("PaintingMode"), KisPaintingModeOptionData());

    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisCurveOpOptionWidget>(curveOp), QStringLiteral("CurveOp")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createOpacityOptionWidget(kposu::curveCursor(opacity)), QStringLiteral("Opacity")));
    addPaintOpOption(withToolOptionsId(
        new KisCurveOptionWidget(kposu::curveCursor(lineWidth), KisPaintOpOption::GENERAL, i18n("0%"), i18n("100%")),
        QStringLiteral("LineWidth")));
    addPaintOpOption(withToolOptionsId(new KisCurveOptionWidget(kposu::curveCursor(curvesOpacity),
                                                                KisPaintOpOption::GENERAL,
                                                                i18n("0%"),
                                                                i18n("100%")),
                                       QStringLiteral("CurvesOpacity")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisCompositeOpOptionWidget>(compositeOp),
                                       QStringLiteral("CompositeOp")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisPaintingModeOptionWidget>(paintingMode),
                                       QStringLiteral("PaintingMode")));

    setOptionsModel(model);
}

KisCurvePaintOpSettingsWidget::~ KisCurvePaintOpSettingsWidget()
{
}


KisPropertiesConfigurationSP  KisCurvePaintOpSettingsWidget::configuration() const
{
    KisCurvePaintOpSettings* config = new KisCurvePaintOpSettings(resourcesInterface());
    config->setProperty("paintop", "curvebrush"); // XXX: make this a const id string
    writeConfiguration(config);
    return config;
}

