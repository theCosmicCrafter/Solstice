/*
 * SPDX-FileCopyrightText: 2009 Lukáš Tvrdý (lukast.dev@gmail.com)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "kis_grid_paintop_settings_widget.h"

#include "kis_grid_paintop_settings.h"
#include "KisGridShapeOptionWidget.h"
#include "KisGridOpOptionWidget.h"


#include <KisColorOptionWidget.h>

#include <kis_paintop_settings_widget.h>
#include <KisPaintingModeOptionWidget.h>
#include <KisPaintOpOptionWidgetUtils.h>
#include <klocalizedstring.h>
#include <KisCompositeOpOptionWidget.h>
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

KisGridPaintOpSettingsWidget:: KisGridPaintOpSettingsWidget(QWidget* parent)
    : KisPaintOpSettingsWidget(parent)
{
    namespace kpowu = KisPaintOpOptionWidgetUtils;
    namespace kposu = KisPaintOpOptionStateUtils;

    // Solstice: the option states live in a shared model, so that one option
    // change writes that option and its dependents after the initial rewrite
    // (docs/agent/brush-option-shared-model.md); no option's written data depends on another
    KisPaintOpOptionsModel *model = new KisPaintOpOptionsModel(this);

    auto *gridOp = model->addOption(QStringLiteral("GridOp"), KisGridOpOptionData());
    auto *gridShape = model->addOption(QStringLiteral("GridShape"), KisGridShapeOptionData());
    auto *compositeOp = model->addOption(QStringLiteral("CompositeOp"), KisCompositeOpOptionData());
    auto *colorOptions = model->addOption(QStringLiteral("ColorOptions"), KisColorOptionData());
    auto *paintingMode = model->addOption(QStringLiteral("PaintingMode"), KisPaintingModeOptionData());

    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisGridOpOptionWidget>(gridOp), QStringLiteral("GridOp")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisGridShapeOptionWidget>(gridShape), QStringLiteral("GridShape")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisCompositeOpOptionWidget>(compositeOp),
                                       QStringLiteral("CompositeOp")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisColorOptionWidget>(colorOptions),
                                       QStringLiteral("ColorOptions")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisPaintingModeOptionWidget>(paintingMode),
                                       QStringLiteral("PaintingMode")));

    setOptionsModel(model);
}

KisGridPaintOpSettingsWidget::~ KisGridPaintOpSettingsWidget()
{
}

KisPropertiesConfigurationSP  KisGridPaintOpSettingsWidget::configuration() const
{
    KisGridPaintOpSettings* config = new KisGridPaintOpSettings(resourcesInterface());
    config->setProperty("paintop", "gridbrush"); // XXX: make this a const id string
    writeConfiguration(config);
    return config;
}
