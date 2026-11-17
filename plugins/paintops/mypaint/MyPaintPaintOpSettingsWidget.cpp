/*
 * SPDX-FileCopyrightText: 2020 Ashwin Dhakaita <ashwingpdhakaita@gmail.com>
 * SPDX-FileCopyrightText: 2021 L. E. Segovia <amy@amyspark.me>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "MyPaintPaintOpSettingsWidget.h"

#include <klocalizedstring.h>

#include "MyPaintPaintOpSettings.h"
#include <KisAirbrushOptionWidget.h>
#include <KisPaintOpOptionWidgetUtils.h>

#include <MyPaintCurveOptionData.h>
#include <MyPaintCurveOptionWidget.h>
#include <MyPaintBasicOptionWidget.h>
#include <MyPaintStandardOptionData.h>

#include <kis_paintop_lod_limitations.h>
#include <KisPaintOpOptionStateUtils.h>
#include <KisPaintOpOptionsModel.h>

namespace KisPaintOpOptionWidgetUtils {

template <typename Data>
MyPaintCurveOptionWidget* createMyPaintCurveOptionWidget(Data data, const QString &yValueSuffix = "")
{
    const qreal yLimit = qAbs(data.strengthMaxValue - data.strengthMinValue);
    return createOptionWidget<MyPaintCurveOptionWidget>(std::move(data), yLimit, yValueSuffix);
}

template <typename Data>
MyPaintCurveOptionWidget* createMyPaintCurveOptionWidgetWithLodLimitations(Data data, const QString &yValueSuffix = "")
{
    const qreal yLimit = qAbs(data.strengthMaxValue - data.strengthMinValue);
    return createOptionWidgetWithLodLimitations<MyPaintCurveOptionWidget>(std::move(data), yLimit, yValueSuffix);
}

} // namespace KisPaintOpOptionWidgetUtils


KisMyPaintOpSettingsWidget:: KisMyPaintOpSettingsWidget(QWidget* parent)
    : KisPaintOpSettingsWidget(parent)
{
    /// TODO: move category into the widget itself, remove this
    /// overridden enum

    namespace kposu = KisPaintOpOptionStateUtils;

    // Solstice: the option states live in a shared model, so that one option
    // change writes that option and its dependents after the initial rewrite
    // (docs/agent/brush-option-shared-model.md). Every curve option patches
    // only its own part of the shared
    // MyPaint/json, which resetSettings() keeps, so writing one option gives
    // the same document as a full rewrite. The ids are MyPaint's setting names.
    KisPaintOpOptionsModel *model = new KisPaintOpOptionsModel(this);
    model->addSharedKey(QStringLiteral("MyPaint/json"));

    auto addCurveState = [model](auto data) {
        using Data = decltype(data);
        return model->addOption(data.id.id(), data, &kposu::bakeCurveOption<Data>);
    };
    auto yLimit = [](const auto *state) {
        return qAbs(state->data().strengthMaxValue - state->data().strengthMinValue);
    };
    auto addCurve = [this, yLimit](auto *state, MyPaintPaintopCategory category, const QString &yValueSuffix) {
        MyPaintCurveOptionWidget *widget =
            kposu::createOptionWidget<MyPaintCurveOptionWidget>(state, yLimit(state), yValueSuffix);
        widget->setToolOptionsId(state->id());
        addPaintOpOption(widget, category);
        return widget;
    };
    auto add = [addCurve,
                addCurveState](auto data, MyPaintPaintopCategory category, const QString &yValueSuffix = QString()) {
        return addCurve(addCurveState(data), category, yValueSuffix);
    };
    auto addWithLodLimitations = [this, yLimit, addCurveState](auto data, MyPaintPaintopCategory category) {
        auto *state = addCurveState(data);
        MyPaintCurveOptionWidget *widget =
            kposu::createOptionWidgetWithLodLimitations<MyPaintCurveOptionWidget>(state, yLimit(state), QString());
        widget->setToolOptionsId(state->id());
        addPaintOpOption(widget, category);
    };

    // the Basic page edits the base values of these three
    auto *radius = addCurveState(MyPaintRadiusLogarithmicData());
    auto *hardness = addCurveState(MyPaintHardnessData());
    auto *opacity = addCurveState(MyPaintOpacityData());
    auto *basic = model->addOption(QStringLiteral("Basic"), MyPaintBasicOptionData());

    MyPaintBasicOptionWidget *basicWidget = kposu::createOptionWidget<MyPaintBasicOptionWidget>(
        basic,
        kposu::curveCursor(radius)[&KisCurveOptionDataCommon::strengthValue],
        kposu::curveCursor(hardness)[&KisCurveOptionDataCommon::strengthValue],
        kposu::curveCursor(opacity)[&KisCurveOptionDataCommon::strengthValue]);
    basicWidget->setToolOptionsId(QStringLiteral("Basic"));
    KisPaintOpSettingsWidget::addPaintOpOption(basicWidget);

    m_radiusWidget = addCurve(radius, BASIC, QString());
    addWithLodLimitations(MyPaintRadiusByRandomData(), BASIC);
    addCurve(hardness, BASIC, QString());
    add(MyPaintAntiAliasingData(), BASIC);
    add(MyPaintEllipticalDabAngleData(), BASIC, "°");
    add(MyPaintEllipticalDabRatioData(), BASIC);
    add(MyPaintDirectionFilterData(), BASIC);
    add(MyPaintSnapToPixelsData(), BASIC);
    add(MyPaintPressureGainData(), BASIC);

    auto *airbrush = model->addOption(QStringLiteral("Airbrush"), KisAirbrushOptionData());
    KisAirbrushOptionWidget *airbrushWidget = kposu::createOptionWidget<KisAirbrushOptionWidget>(airbrush);
    airbrushWidget->setToolOptionsId(QStringLiteral("Airbrush"));
    addPaintOpOption(airbrushWidget, AIRBRUSH);

    add(MyPaintChangeColorHData(), COLOR);
    add(MyPaintChangeColorLData(), COLOR);
    add(MyPaintChangeColorVData(), COLOR);
    add(MyPaintChangeColorHSLSData(), COLOR);
    add(MyPaintChangeColorHSVSData(), COLOR);
    add(MyPaintColorizeData(), COLOR);
    add(MyPaintPosterizeData(), COLOR);
    add(MyPaintPosterizationLevelsData(), COLOR);

    add(MyPaintFineSpeedGammaData(), SPEED);
    add(MyPaintGrossSpeedGammaData(), SPEED);
    add(MyPaintFineSpeedSlownessData(), SPEED);
    add(MyPaintGrossSpeedSlownessData(), SPEED);
    add(MyPaintOffsetBySpeedData(), SPEED);
    add(MyPaintOffsetBySpeedFilterData(), SPEED);
    addWithLodLimitations(MyPaintOffsetByRandomData(), SPEED);

    add(MyPaintDabsPerBasicRadiusData(), DABS);
    add(MyPaintDabsPerActualRadiusData(), DABS);
    add(MyPaintDabsPerSecondData(), DABS);

    addCurve(opacity, OPACITY, QString());
    add(MyPaintOpaqueLinearizeData(), OPACITY);
    add(MyPaintOpaqueMultiplyData(), OPACITY);

    add(MyPaintSlowTrackingPerDabData(), TRACKING);
    add(MyPaintSlowTrackingData(), TRACKING);
    add(MyPaintTrackingNoiseData(), TRACKING);

    add(MyPaintSmudgeData(), SMUDGE);
    add(MyPaintSmudgeLengthData(), SMUDGE);
    add(MyPaintSmudgeLengthMultiplierData(), SMUDGE);
    add(MyPaintSmudgeRadiusLogData(), SMUDGE);
    add(MyPaintSmudgeTransparencyData(), SMUDGE);
    add(MyPaintSmudgeBucketData(), SMUDGE);

    add(MyPaintStrokeDurationLogData(), STROKE);
    add(MyPaintStrokeHoldtimeData(), STROKE);
    add(MyPaintStrokeThresholdData(), STROKE);

    add(MyPaintCustomInputData(), CUSTOM);
    add(MyPaintCustomInputSlownessData(), CUSTOM);

    setOptionsModel(model);
}

KisMyPaintOpSettingsWidget::~ KisMyPaintOpSettingsWidget()
{
}

KisPropertiesConfigurationSP  KisMyPaintOpSettingsWidget::configuration() const
{
    KisMyPaintOpSettings* config = new KisMyPaintOpSettings(resourcesInterface());
    config->setProperty("paintop", "mypaintbrush"); // XXX: make this a const id string
    writeConfiguration(config);
    return config;
}

lager::reader<qreal> KisMyPaintOpSettingsWidget::effectiveBrushSize() const
{
    return m_radiusWidget->strengthValueDenorm().map([] (qreal value) { return 2 * exp(value); });
}

void KisMyPaintOpSettingsWidget::addPaintOpOption(KisPaintOpOption *option, MyPaintPaintopCategory id)
{
    QString category;

    switch (id) {
    case BASIC:
        category = i18nc("Option Category", "Basic");
        break;
    case AIRBRUSH:
        category = i18n("Airbrush");
        break;
    case COLOR:
        category = i18nc("Option Category", "Color");
        break;
    case SPEED:
        category = i18nc("Option Category", "Speed");
        break;
    case DABS:
        category = i18nc("Option Category", "Dabs");
        break;
    case OPACITY:
        category = i18nc("Option Category", "Opacity");
        break;
    case TRACKING:
        category = i18nc("Option Category", "Tracking");
        break;
    case SMUDGE:
        category = i18nc("Option Category", "Smudge");
        break;
    case STROKE:
        category = i18nc("Option Category", "Stroke");
        break;
    case CUSTOM:
        category = i18nc("Option Category", "Custom");
        break;
    }

    return KisPaintOpSettingsWidget::addPaintOpOption(option, category);
}
