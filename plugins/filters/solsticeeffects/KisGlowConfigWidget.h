/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_GLOW_CONFIG_WIDGET_H
#define KIS_GLOW_CONFIG_WIDGET_H
#include <kis_config_widget.h>
class KisDoubleSliderSpinBox;
class KisGlowConfigWidget : public KisConfigWidget
{
public:
    KisGlowConfigWidget(QWidget *parent, KisPaintDeviceSP device);
    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

private:
    KisDoubleSliderSpinBox *m_strength, *m_area, *m_range;
};
#endif
