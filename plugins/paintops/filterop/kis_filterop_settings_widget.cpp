/*
 *  SPDX-FileCopyrightText: 2002 Patrick Julien <freak@codepimps.org>
 *  SPDX-FileCopyrightText: 2004-2008 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2004 Clarence Dang <dang@kde.org>
 *  SPDX-FileCopyrightText: 2004 Adrian Page <adrian@pagenet.plus.com>
 *  SPDX-FileCopyrightText: 2004 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_filterop_settings_widget.h"
#include "kis_filterop_settings.h"

#include <kis_properties_configuration.h>
#include <filter/kis_filter.h>
#include <KisStandardOptionData.h>
#include <KisPaintOpOptionWidgetUtils.h>
#include <KisCompositeOpOptionWidget.h>
#include <KisSizeOptionWidget.h>
#include <KisMirrorOptionWidget.h>
#include <KisFilterOptionWidget.h>
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

KisFilterOpSettingsWidget::KisFilterOpSettingsWidget(QWidget* parent)
    : KisFilterOpSettingsWidget(parent, new KisPaintOpOptionsModel())
{
}

KisFilterOpSettingsWidget::KisFilterOpSettingsWidget(QWidget *parent, KisPaintOpOptionsModel *model)
    : KisBrushBasedPaintopOptionWidget(KisBrushOptionWidgetFlag::SupportsPrecision,
                                       addBrushTipOption(model, KisBrushOptionWidgetFlag::SupportsPrecision)->cursor(),
                                       parent)
{
    namespace kpowu = KisPaintOpOptionWidgetUtils;
    namespace kposu = KisPaintOpOptionStateUtils;

    setObjectName("filter option widget");

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
    auto *size =
        model->addOption(QStringLiteral("Size"), KisSizeOptionData(), &kposu::bakeCurveOption<KisSizeOptionData>);
    auto *rotation = model->addOption(QStringLiteral("Rotation"),
                                      KisRotationOptionData(),
                                      &kposu::bakeCurveOption<KisRotationOptionData>);
    auto *mirror =
        model->addOption(QStringLiteral("Mirror"), KisMirrorOptionData(), &kposu::bakeCurveOption<KisMirrorOptionData>);
    // written with the fallback filter while none is chosen
    auto *filter =
        model->addOption(QStringLiteral("Filter"), KisFilterOptionData(), &KisBrushBasedOptionStates::bakeFilterOption);

    brushOptionWidget()->setToolOptionsId(QStringLiteral("BrushTip"));

    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisCompositeOpOptionWidget>(compositeOp),
                                       QStringLiteral("CompositeOp")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createOpacityOptionWidget(kposu::curveCursor(opacity)), QStringLiteral("Opacity")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisSizeOptionWidget>(size), QStringLiteral("Size")));
    addPaintOpOption(
        withToolOptionsId(kpowu::createRotationOptionWidget(kposu::curveCursor(rotation)), QStringLiteral("Rotation")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisMirrorOptionWidget>(mirror), QStringLiteral("Mirror")));
    addPaintOpOption(
        withToolOptionsId(kposu::createOptionWidget<KisFilterOptionWidget>(filter), QStringLiteral("Filter")));

    setOptionsModel(model);
}

KisFilterOpSettingsWidget::~KisFilterOpSettingsWidget()
{
}

KisPropertiesConfigurationSP KisFilterOpSettingsWidget::configuration() const
{
    KisFilterOpSettings *config = new KisFilterOpSettings(resourcesInterface());
    config->setProperty("paintop", "filter"); // XXX: make this a const id string
    writeConfiguration(config);
    return config;
}

