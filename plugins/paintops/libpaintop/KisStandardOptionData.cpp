/*
 *  SPDX-FileCopyrightText: 2022 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisStandardOptionData.h"

#include <KisPaintOpOptionWidgetUtils.h>
#include <KisPaintopSettingsIds.h>


namespace KisPaintOpOptionWidgetUtils {

namespace detail {
QString opacityMinLabel()
{
    return i18n("Transparent");
}
QString opacityMaxLabel()
{
    return i18n("Opaque");
}
QString rotationMinLabel()
{
    return i18n("-180°");
}
QString rotationMaxLabel()
{
    return i18n("180°");
}
KisCurveOptionWidget *createOpacityOptionWidgetImpl(KisPaintOpOption::PaintopCategory category, const QString &prefix)
{
    return createCurveOptionWidget(KisOpacityOptionData(prefix), category, opacityMinLabel(), opacityMaxLabel());
}
KisCurveOptionWidget *createRotationOptionWidgetImpl(KisPaintOpOption::PaintopCategory category, const QString &prefix)
{
    return createCurveOptionWidget(KisRotationOptionData(prefix), category, rotationMinLabel(), rotationMaxLabel());
}
}

KisCurveOptionWidget *createOpacityOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData,
                                    KisPaintOpOption::GENERAL,
                                    detail::opacityMinLabel(),
                                    detail::opacityMaxLabel());
}

KisCurveOptionWidget *createRotationOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData,
                                    KisPaintOpOption::GENERAL,
                                    detail::rotationMinLabel(),
                                    detail::rotationMaxLabel());
}

KisCurveOptionWidget *createRateOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData, KisPaintOpOption::COLOR);
}

KisCurveOptionWidget *createOpacityOptionWidget()
{
    return detail::createOpacityOptionWidgetImpl(KisPaintOpOption::GENERAL, "");
}

KisCurveOptionWidget *createFlowOptionWidget()
{
    return createCurveOptionWidget(KisFlowOptionData(),
                                   KisPaintOpOption::GENERAL);
}

KisCurveOptionWidget *createRatioOptionWidget()
{
    return createCurveOptionWidget(KisRatioOptionData(),
                                   KisPaintOpOption::GENERAL);
}

KisCurveOptionWidget *createSoftnessOptionWidget()
{
    return createCurveOptionWidget(KisSoftnessOptionData(),
                                   KisPaintOpOption::GENERAL,
                                   i18n("Soft"),
                                   i18n("Hard"));
}

KisCurveOptionWidget *createRotationOptionWidget()
{
    return detail::createRotationOptionWidgetImpl(KisPaintOpOption::GENERAL, "");
}

KisCurveOptionWidget *createDarkenOptionWidget()
{
    return createCurveOptionWidget(KisDarkenOptionData(),
                                   KisPaintOpOption::COLOR,
                                   i18n("0.0"),
                                   i18n("1.0"));
}

KisCurveOptionWidget *createMixOptionWidget()
{
    return createCurveOptionWidget(KisMixOptionData(),
                                   KisPaintOpOption::COLOR,
                                   i18nc("Background painting color", "Background"),
                                   i18nc("Foreground painting color", "Foreground"));
}

namespace detail {
QString hueMinLabel()
{
    // xgettext: no-c-format
    QString activeColorMsg = i18n("(0° is active color)");
    QString br("<br />");
    QString fullPercent = i18n("+180°");
    QString zeroPercent = i18n("-180°");

    return QString(zeroPercent + br + i18n("CCW hue") + br + activeColorMsg);
}

QString hueMaxLabel()
{
    // xgettext: no-c-format
    QString activeColorMsg = i18n("(0° is active color)");
    QString br("<br />");
    QString fullPercent = i18n("+180°");
    QString zeroPercent = i18n("-180°");

    return QString(fullPercent + br + i18n("CW hue"));
}

QString saturationMinLabel()
{
    // xgettext: no-c-format
    QString activeColorMsg = i18n("(0% is active color)");
    QString br("<br />");
    QString fullPercent = i18n("+100%");
    QString zeroPercent = i18n("-100%");

    return QString(zeroPercent + br + i18n("Less saturation ") + br + activeColorMsg);

}

QString saturationMaxLabel()
{
    // xgettext: no-c-format
    QString activeColorMsg = i18n("(0% is active color)");
    QString br("<br />");
    QString fullPercent = i18n("+100%");
    QString zeroPercent = i18n("-100%");

    return QString(fullPercent + br + i18n("More saturation"));
}

QString valueMinLabel()
{
    // xgettext: no-c-format
    QString activeColorMsg = i18n("(0% is active color)");
    QString br("<br />");
    QString fullPercent = i18n("+100%");
    QString zeroPercent = i18n("-100%");

    return QString(zeroPercent + br + i18nc("Lower HSV brightness", "Lower value ") + br + activeColorMsg);

}

QString valueMaxLabel()
{
    // xgettext: no-c-format
    QString activeColorMsg = i18n("(0% is active color)");
    QString br("<br />");
    QString fullPercent = i18n("+100%");
    QString zeroPercent = i18n("-100%");

    return QString(fullPercent + br + i18nc("Higher HSV brightness", "Higher value"));


}
}

KisCurveOptionWidget *createHueOptionWidget()
{
    return createCurveOptionWidget(KisHueOptionData(), KisPaintOpOption::COLOR,
                                   detail::hueMinLabel(),
                                   detail::hueMaxLabel(),
                                   -180, 180, i18n("°"));
}

KisCurveOptionWidget *createSaturationOptionWidget()
{
    return createCurveOptionWidget(KisSaturationOptionData(), KisPaintOpOption::COLOR,
                                   detail::saturationMinLabel(),
                                   detail::saturationMaxLabel(),
                                   -100, 100, i18n("%"));
}

KisCurveOptionWidget *createValueOptionWidget()
{
    return createCurveOptionWidget(KisValueOptionData(),
                                   KisPaintOpOption::COLOR,
                                   detail::valueMinLabel(),
                                   detail::valueMaxLabel(),
                                   -100, 100, i18n("%"));
}

KisCurveOptionWidget *createRateOptionWidget()
{
    return createCurveOptionWidget(KisRateOptionData(),
                                   KisPaintOpOption::COLOR);
}

KisCurveOptionWidget *createStrengthOptionWidget()
{
    return createCurveOptionWidget(KisStrengthOptionData(),
                                   KisPaintOpOption::TEXTURE);
}

KisCurveOptionWidget *createMaskingOpacityOptionWidget()
{
    return detail::createOpacityOptionWidgetImpl(KisPaintOpOption::MASKING_BRUSH, KisPaintOpUtils::MaskingBrushPresetPrefix);
}

KisCurveOptionWidget *createMaskingFlowOptionWidget()
{
    return createCurveOptionWidget(KisFlowOptionData(KisPaintOpUtils::MaskingBrushPresetPrefix),
                                   KisPaintOpOption::MASKING_BRUSH);
}

KisCurveOptionWidget *createMaskingRatioOptionWidget()
{
    return createCurveOptionWidget(KisRatioOptionData(KisPaintOpUtils::MaskingBrushPresetPrefix),
                                   KisPaintOpOption::MASKING_BRUSH);
}

KisCurveOptionWidget *createMaskingRotationOptionWidget()
{
    return detail::createRotationOptionWidgetImpl(KisPaintOpOption::MASKING_BRUSH, KisPaintOpUtils::MaskingBrushPresetPrefix);
}

// Solstice: variants bound to a state owned by KisPaintOpOptionsModel, with
// the same categories and labels as the variants above
// (docs/agent/brush-option-shared-model.md)

KisCurveOptionWidget *createFlowOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData, KisPaintOpOption::GENERAL);
}

KisCurveOptionWidget *createRatioOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData, KisPaintOpOption::GENERAL);
}

KisCurveOptionWidget *createSoftnessOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData, KisPaintOpOption::GENERAL, i18n("Soft"), i18n("Hard"));
}

KisCurveOptionWidget *createDarkenOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData, KisPaintOpOption::COLOR, i18n("0.0"), i18n("1.0"));
}

KisCurveOptionWidget *createMixOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData,
                                    KisPaintOpOption::COLOR,
                                    i18nc("Background painting color", "Background"),
                                    i18nc("Foreground painting color", "Foreground"));
}

KisCurveOptionWidget *createHueOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData,
                                    KisPaintOpOption::COLOR,
                                    detail::hueMinLabel(),
                                    detail::hueMaxLabel(),
                                    -180,
                                    180,
                                    i18n("°"));
}

KisCurveOptionWidget *createSaturationOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData,
                                    KisPaintOpOption::COLOR,
                                    detail::saturationMinLabel(),
                                    detail::saturationMaxLabel(),
                                    -100,
                                    100,
                                    i18n("%"));
}

KisCurveOptionWidget *createValueOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData,
                                    KisPaintOpOption::COLOR,
                                    detail::valueMinLabel(),
                                    detail::valueMaxLabel(),
                                    -100,
                                    100,
                                    i18n("%"));
}

KisCurveOptionWidget *createStrengthOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData, KisPaintOpOption::TEXTURE);
}

KisCurveOptionWidget *createMaskingOpacityOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData,
                                    KisPaintOpOption::MASKING_BRUSH,
                                    detail::opacityMinLabel(),
                                    detail::opacityMaxLabel());
}

KisCurveOptionWidget *createMaskingFlowOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData, KisPaintOpOption::MASKING_BRUSH);
}

KisCurveOptionWidget *createMaskingRatioOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData, KisPaintOpOption::MASKING_BRUSH);
}

KisCurveOptionWidget *createMaskingRotationOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData)
{
    return new KisCurveOptionWidget(optionData,
                                    KisPaintOpOption::MASKING_BRUSH,
                                    detail::rotationMinLabel(),
                                    detail::rotationMaxLabel());
}

}
