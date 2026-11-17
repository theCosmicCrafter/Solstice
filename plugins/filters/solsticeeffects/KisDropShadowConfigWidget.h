/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_DROP_SHADOW_CONFIG_WIDGET_H
#define KIS_DROP_SHADOW_CONFIG_WIDGET_H
#include <kis_config_widget.h>
class KisDoubleSliderSpinBox;
class KisColorButton;
class KisShadowOffsetPad;
class KisDropShadowConfigWidget : public KisConfigWidget
{
public:
    KisDropShadowConfigWidget(QWidget *parent, KisPaintDeviceSP device);
    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

private:
    KisDoubleSliderSpinBox *m_blur, *m_angle, *m_distance, *m_opacity;
    KisColorButton *m_color;
    KisShadowOffsetPad *m_pad;
    bool m_loading = false;
};
#endif
