/*
 *  SPDX-FileCopyrightText: 2016 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_roundmarkerop_settings_widget.h"
#include "kis_brush_based_paintop_settings.h"

#include <kis_properties_configuration.h>
#include <kis_paintop_settings_widget.h>
#include "kis_roundmarkerop_settings.h"
#include <KisRoundMarkerOpOptionWidget.h>
#include <KisPaintOpOptionWidgetUtils.h>
#include <KisCompositeOpOptionWidget.h>
#include "KisSizeOptionWidget.h"
#include "KisSpacingOptionWidget.h"
#include <KisPaintOpOptionStateUtils.h>
#include <KisPaintOpOptionsModel.h>
#include <KisStandardOptionData.h>

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

KisRoundMarkerOpSettingsWidget::KisRoundMarkerOpSettingsWidget(QWidget* parent)
    : KisPaintOpSettingsWidget(parent)
{
    namespace kpowu = KisPaintOpOptionWidgetUtils;

    setObjectName("roundmarker option widget");
    //setPrecisionEnabled(true);

    namespace kposu = KisPaintOpOptionStateUtils;

    // Solstice: the option states live in a shared model, so that one option
    // change writes that option and its dependents after the initial rewrite
    // (docs/agent/brush-option-shared-model.md); no option's written data depends on another
    KisPaintOpOptionsModel *model = new KisPaintOpOptionsModel(this);

    auto *roundMarker = model->addOption(QStringLiteral("RoundMarker"), KisRoundMarkerOpOptionData());
    auto *compositeOp = model->addOption(QStringLiteral("CompositeOp"), KisCompositeOpOptionData());
    auto *size =
        model->addOption(QStringLiteral("Size"), KisSizeOptionData(), &kposu::bakeCurveOption<KisSizeOptionData>);
    auto *spacing = model->addOption(QStringLiteral("Spacing"),
                                     KisSpacingOptionData(),
                                     &kposu::bakeCurveOption<KisSpacingOptionData>);

    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisRoundMarkerOpOptionWidget>(roundMarker),
                                       QStringLiteral("RoundMarker")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisCompositeOpOptionWidget>(compositeOp),
                                       QStringLiteral("CompositeOp")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisSizeOptionWidget>(size), QStringLiteral("Size")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisSpacingOptionWidget>(spacing), QStringLiteral("Spacing")));

    setOptionsModel(model);
}

KisRoundMarkerOpSettingsWidget::~KisRoundMarkerOpSettingsWidget() { }

KisPropertiesConfigurationSP KisRoundMarkerOpSettingsWidget::configuration() const
{
    KisRoundMarkerOpSettings *config = new KisRoundMarkerOpSettings(resourcesInterface());
    config->setProperty("paintop", "roundmarker");
    writeConfiguration(config);
    return config;
}
