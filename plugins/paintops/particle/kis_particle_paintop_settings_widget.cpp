/*
 *  SPDX-FileCopyrightText: 2008 Lukáš Tvrdý <lukast.dev@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_particle_paintop_settings_widget.h"

#include "KisParticleOpOptionWidget.h"
#include "kis_particle_paintop_settings.h"

#include <kis_paintop_settings_widget.h>
#include <KisPaintingModeOptionWidget.h>
#include <KisPaintOpOptionWidgetUtils.h>
#include <KisAirbrushOptionWidget.h>
#include <KisCompositeOpOptionWidget.h>
#include <KisStandardOptionData.h>
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

KisParticlePaintOpSettingsWidget:: KisParticlePaintOpSettingsWidget(QWidget* parent)
    : KisPaintOpSettingsWidget(parent)
{
    namespace kpowu = KisPaintOpOptionWidgetUtils;

    namespace kposu = KisPaintOpOptionStateUtils;

    // Solstice: the option states live in a shared model, so that one option
    // change writes that option and its dependents after the initial rewrite
    // (docs/agent/brush-option-shared-model.md); no option's written data depends on another
    KisPaintOpOptionsModel *model = new KisPaintOpOptionsModel(this);

    auto *particleOp = model->addOption(QStringLiteral("ParticleOp"), KisParticleOpOptionData());
    auto *compositeOp = model->addOption(QStringLiteral("CompositeOp"), KisCompositeOpOptionData());
    auto *airbrush = model->addOption(QStringLiteral("Airbrush"), KisAirbrushOptionData());
    auto *rate =
        model->addOption(QStringLiteral("Rate"), KisRateOptionData(), &kposu::bakeCurveOption<KisRateOptionData>);
    auto *paintingMode = model->addOption(QStringLiteral("PaintingMode"), KisPaintingModeOptionData());

    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidgetWithLodLimitations<KisParticleOpOptionWidget>(particleOp),
                          QStringLiteral("ParticleOp")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisCompositeOpOptionWidget>(compositeOp),
                                       QStringLiteral("CompositeOp")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisAirbrushOptionWidget>(airbrush, false),
                                       QStringLiteral("Airbrush")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRateOptionWidget(kposu::curveCursor(rate)), QStringLiteral("Rate")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisPaintingModeOptionWidget>(paintingMode),
                                       QStringLiteral("PaintingMode")));

    setOptionsModel(model);
}

KisParticlePaintOpSettingsWidget::~ KisParticlePaintOpSettingsWidget()
{
}

KisPropertiesConfigurationSP  KisParticlePaintOpSettingsWidget::configuration() const
{
    KisParticlePaintOpSettings* config = new KisParticlePaintOpSettings(resourcesInterface());
    config->setProperty("paintop", "particlebrush"); // XXX: make this a const id string
    writeConfiguration(config);
    return config;
}
