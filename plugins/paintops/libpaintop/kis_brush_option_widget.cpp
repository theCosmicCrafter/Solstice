/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 2008 Boudewijn Rempt <boud@valdyas.org>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "kis_brush_option_widget.h"
#include <klocalizedstring.h>

#include <kis_image.h>
#include <kis_image_config.h>

#include "kis_brush_selection_widget.h"
#include "kis_auto_brush_widget.h"
#include "kis_predefined_brush_chooser.h"
#include "kis_brush.h"

#include <lager/state.hpp>
#include <optional>
#include "KisBrushModel.h"
#include "kis_precision_option.h"
#include "kis_paintop_lod_limitations.h"

#include "KisAutoBrushModel.h"
#include "KisPredefinedBrushModel.h"
#include "KisTextBrushModel.h"
#include "KisBrushTipOptionData.h"

struct KisBrushOptionWidget::Private
{
    Private(std::optional<lager::cursor<KisBrushTipOptionData>> externalData, KisBrushOptionWidgetFlags _flags)
        : data(externalData ? *externalData : lager::cursor<KisBrushTipOptionData>(ownData))
        , brushData(data[&KisBrushTipOptionData::brush])
        , brushPrecisionData(data[&KisBrushTipOptionData::precision])
        , commonBrushSizeData(data[&KisBrushTipOptionData::commonBrushSize])
        , autoBrushModel(brushData[&BrushData::common],
                         brushData[&BrushData::autoBrush],
                         commonBrushSizeData)
        , predefinedBrushModel(brushData[&BrushData::common],
                               brushData[&BrushData::predefinedBrush],
                               commonBrushSizeData,
                               _flags & KisBrushOptionWidgetFlag::SupportsHSLBrushMode)
        , textBrushModel(brushData[&BrushData::common],
                         brushData[&BrushData::textBrush])
        , flags(_flags)
    {
    }

    /// The state when the widget owns it (no external cursor). Solstice: the
    /// three former states are one value, KisBrushTipOptionData, so a shared
    /// model can hold it (docs/agent/brush-option-shared-model.md).
    lager::state<KisBrushTipOptionData, lager::automatic_tag> ownData;
    lager::cursor<KisBrushTipOptionData> data;
    lager::cursor<BrushData> brushData;
    lager::cursor<PrecisionData> brushPrecisionData;
    lager::cursor<qreal> commonBrushSizeData;

    KisAutoBrushModel autoBrushModel;
    KisPredefinedBrushModel predefinedBrushModel;
    KisTextBrushModel textBrushModel;

    KisBrushOptionWidgetFlags flags;
};

KisBrushOptionWidget::KisBrushOptionWidget(KisBrushOptionWidgetFlags flags)
    : KisBrushOptionWidget(new Private(std::nullopt, flags))
{
}

KisBrushOptionWidget::KisBrushOptionWidget(KisBrushOptionWidgetFlags flags,
                                           lager::cursor<KisBrushTipOptionData> optionData)
    : KisBrushOptionWidget(new Private(optionData, flags))
{
}

KisBrushOptionWidget::KisBrushOptionWidget(Private *d)
    : KisPaintOpOption(i18n("Brush Tip"), KisPaintOpOption::GENERAL, true),
      m_d(d)
{
    m_checkable = false;

    m_brushSelectionWidget = new KisBrushSelectionWidget(KisImageConfig(true).maxBrushSize(),
                                                         &m_d->autoBrushModel,
                                                         &m_d->predefinedBrushModel,
                                                         &m_d->textBrushModel,
                                                         m_d->brushData[&BrushData::type],
                                                         m_d->brushPrecisionData,
                                                         m_d->flags);
    m_brushSelectionWidget->hide();
    setConfigurationPage(m_brushSelectionWidget);

    setObjectName("KisBrushOptionWidget");

    // Solstice: page parameters that can be shown in Tool Options
    // (docs/agent/tool-options-brush.md, phase 3b); each tip type's
    // parameters only while that tip type is selected
    QWidget *autoTip = m_brushSelectionWidget->findChild<KisAutoBrushWidget *>();
    QWidget *predefinedTip = m_brushSelectionWidget->findChild<KisPredefinedBrushChooser *>();
    auto addParameter = [this](const QString &id,
                               const QString &label,
                               const char *controlName,
                               const char *labelName,
                               QWidget *modeWidget) {
        QWidget *control = m_brushSelectionWidget->findChild<QWidget *>(QLatin1String(controlName));
        QWidget *labelWidget =
            labelName ? m_brushSelectionWidget->findChild<QWidget *>(QLatin1String(labelName)) : nullptr;
        if (control) {
            addToolOptionsParameter(id, label, control, labelWidget, modeWidget);
        }
    };
    if (autoTip) {
        addParameter(QStringLiteral("Diameter"), i18n("Diameter"), "inputRadius", "lblDiameter", autoTip);
        addParameter(QStringLiteral("Ratio"), i18n("Ratio"), "inputRatio", "lblRatio", autoTip);
        // both fade values and their link as one parameter; the Soft mask
        // type replaces the Fade page with a curve
        if (QWidget *fadePage = autoTip->findChild<QWidget *>(QStringLiteral("PageFade"))) {
            addParameter(QStringLiteral("Fade"), i18n("Fade"), "grpFade", nullptr, fadePage);
        }
        addParameter(QStringLiteral("Angle"), i18n("Angle"), "inputAngle", "lblAngle", autoTip);
        addParameter(QStringLiteral("Density"), i18n("Density"), "density", "lblDensity", autoTip);
        addParameter(QStringLiteral("Spacing"), i18n("Spacing"), "spacingWidget", "lblSpacing", autoTip);
    }
    if (predefinedTip) {
        addParameter(QStringLiteral("PredefinedSize"),
                     i18n("Size"),
                     "brushSizeSpinBox",
                     "brushSizeLabel",
                     predefinedTip);
        addParameter(QStringLiteral("PredefinedAngle"),
                     i18n("Angle"),
                     "brushRotationAngleSelector",
                     "brushRotationLabel",
                     predefinedTip);
        addParameter(QStringLiteral("PredefinedSpacing"),
                     i18n("Spacing"),
                     "brushSpacingSelectionWidget",
                     "brushSpacingLabel",
                     predefinedTip);
    }
    if (m_d->flags & KisBrushOptionWidgetFlag::SupportsPrecision) {
        addParameter(QStringLiteral("Precision"), i18n("Precision"), "sliderPrecision", "lblPrecision", nullptr);
        addParameter(QStringLiteral("AutoPrecision"),
                     i18n("Auto Precision"),
                     "autoPrecisionCheckBox",
                     nullptr,
                     nullptr);
    }

    lager::watch(m_d->data, std::bind(&KisBrushOptionWidget::emitSettingChanged, this));
}

KisBrushOptionWidget::~KisBrushOptionWidget() = default;

KisBrushSP KisBrushOptionWidget::brush() const
{
    return m_brushSelectionWidget->brush();
}


void KisBrushOptionWidget::setImage(KisImageWSP image)
{
    m_brushSelectionWidget->setImage(image);
}

void KisBrushOptionWidget::writeOptionSetting(KisPropertiesConfigurationSP settings) const
{
    m_d->data->write(settings.data(), m_d->flags);
}

void KisBrushOptionWidget::readOptionSetting(const KisPropertiesConfigurationSP setting)
{
    KisBrushTipOptionData data = m_d->data.get();
    if (data.read(setting.data(), resourcesInterface(), m_d->flags)) {
        m_d->data.set(data);
    }
}

void KisBrushOptionWidget::hideOptions(const QStringList &options)
{
    m_brushSelectionWidget->hideOptions(options);

    // Solstice: a tip setting the engine hides gets no eye and is not shown
    // in Tool Options (docs/agent/tool-options-brush.md)
    Q_FOREACH (const ToolOptionsParameter &parameter, toolOptionsParameters()) {
        if (parameter.control && parameter.control->isHidden()) {
            removeToolOptionsParameter(parameter.id);
        }
    }
}

lager::reader<bool> KisBrushOptionWidget::lightnessModeEnabled() const
{
    const KisBrushOptionWidgetFlags flags = m_d->flags;
    return m_d->data.map([flags](const KisBrushTipOptionData &data) {
        return data.lightnessModeEnabled(flags);
    });
}

lager::reader<qreal> KisBrushOptionWidget::effectiveBrushSize() const
{
    return m_d->commonBrushSizeData;
}

lager::reader<BrushData> KisBrushOptionWidget::bakedBrushData() const
{
    const KisBrushOptionWidgetFlags flags = m_d->flags;
    return m_d->data.map([flags](const KisBrushTipOptionData &data) {
        return data.bakedBrushData(flags);
    });
}

KisPaintOpOption::OptionalLodLimitationsReader KisBrushOptionWidget::lodLimitationsReader() const
{
    return m_d->brushData.map(&KisBrushModel::brushLodLimitations);
}

#include "moc_kis_brush_option_widget.cpp"
