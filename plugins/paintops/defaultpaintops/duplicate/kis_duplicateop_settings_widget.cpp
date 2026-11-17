/*
 *  SPDX-FileCopyrightText: 2002 Patrick Julien <freak@codepimps.org>
 *  SPDX-FileCopyrightText: 2004-2008 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2004 Clarence Dang <dang@kde.org>
 *  SPDX-FileCopyrightText: 2004 Adrian Page <adrian@pagenet.plus.com>
 *  SPDX-FileCopyrightText: 2004 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_duplicateop_settings_widget.h"
#include "KisDuplicateOptionData.h"
#include "kis_duplicateop_settings.h"

#include <KisCompositeOpOptionWidget.h>
#include <KisDuplicateOptionWidget.h>
#include <KisMirrorOptionWidget.h>
#include <KisPaintOpOptionWidgetUtils.h>
#include <KisSizeOptionWidget.h>
#include <KisStandardOptionData.h>
#include <KisTextureOptionWidget.h>
#include <brushengine/kis_paintop_lod_limitations.h>
#include <kis_image.h>
#include <kis_paintop_settings_widget.h>
#include <kis_properties_configuration.h>
#include <KisBrushBasedOptionStates.h>
#include <KisCurveOptionWidget.h>
#include <KisPaintOpOptionStateUtils.h>
#include <KisPaintOpOptionsModel.h>
#include <kis_brush_option_widget.h>

namespace
{
KisBrushTipOptionState *addBrushTipOption(KisPaintOpOptionsModel *model)
{
    KisBrushTipOptionState *state =
        new KisBrushTipOptionState(QStringLiteral("BrushTip"), KisBrushOptionWidgetFlag::SupportsPrecision);
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

KisDuplicateOpSettingsWidget::KisDuplicateOpSettingsWidget(QWidget* parent, KisResourcesInterfaceSP resourcesInterface, KoCanvasResourcesInterfaceSP canvasResourcesInterface)
    : KisDuplicateOpSettingsWidget(parent, resourcesInterface, canvasResourcesInterface, new KisPaintOpOptionsModel())
{
}

KisDuplicateOpSettingsWidget::KisDuplicateOpSettingsWidget(QWidget *parent,
                                                           KisResourcesInterfaceSP resourcesInterface,
                                                           KoCanvasResourcesInterfaceSP canvasResourcesInterface,
                                                           KisPaintOpOptionsModel *model)
    : KisBrushBasedPaintopOptionWidget(KisBrushOptionWidgetFlag::SupportsPrecision,
                                       addBrushTipOption(model)->cursor(),
                                       parent)
{
    Q_UNUSED(canvasResourcesInterface)
    namespace kpowu = KisPaintOpOptionWidgetUtils;
    namespace kposu = KisPaintOpOptionStateUtils;
    namespace kbbos = KisBrushBasedOptionStates;

    setObjectName("brush option widget");

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
    auto *clone = model->addOption(QStringLiteral("Clone"), KisDuplicateOptionData());
    auto *texture = model->addOption(QStringLiteral("Texture"),
                                     KisTextureOptionData(),
                                     [resourcesInterface](const KisTextureOptionData &data) {
                                         return kbbos::bakeTextureOption(data, resourcesInterface);
                                     });
    auto *strength = model->addOption(QStringLiteral("Strength"),
                                      KisStrengthOptionData(),
                                      &kposu::bakeCurveOption<KisStrengthOptionData>);

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
        withToolOptionsId(kposu::createOptionWidget<KisDuplicateOptionWidget>(clone), QStringLiteral("Clone")));
    addPaintOpOption(withToolOptionsId(kposu::createOptionWidget<KisTextureOptionWidget>(texture, resourcesInterface),
                                       QStringLiteral("Texture")));
    addPaintOpOption(withToolOptionsId(
        new KisCurveOptionWidget(kposu::curveCursor(strength), KisPaintOpOption::TEXTURE, i18n("Weak"), i18n("Strong")),
        QStringLiteral("Strength")));

    setOptionsModel(model);
}

KisDuplicateOpSettingsWidget::~KisDuplicateOpSettingsWidget()
{
}

KisPropertiesConfigurationSP KisDuplicateOpSettingsWidget::configuration() const
{
    KisDuplicateOpSettings *config = new KisDuplicateOpSettings(resourcesInterface());
    config->setProperty("paintop", "duplicate"); // XXX: make this a const id string
    writeConfiguration(config);
    return config;
}

KisPaintopLodLimitations KisDuplicateOpSettingsWidget::lodLimitations() const
{
    KisPaintopLodLimitations l = KisBrushBasedPaintopOptionWidget::lodLimitations();
    l.blockers << KoID("clone-brush", i18nc("PaintOp instant preview limitation", "Clone Brush (temporarily disabled)"));
    return l;
}
