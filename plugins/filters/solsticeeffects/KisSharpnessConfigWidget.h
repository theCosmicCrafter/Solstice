/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_SHARPNESS_CONFIG_WIDGET_H
#define KIS_SHARPNESS_CONFIG_WIDGET_H
#include <kis_config_widget.h>
class KisDoubleSliderSpinBox;
class KisSharpnessConfigWidget : public KisConfigWidget
{
public:
    explicit KisSharpnessConfigWidget(QWidget *parent, KisPaintDeviceSP device);
    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

private:
    KisDoubleSliderSpinBox *m_strength;
};
#endif
