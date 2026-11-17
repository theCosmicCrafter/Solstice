/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_LINE_ART_CONFIG_WIDGET_H
#define KIS_LINE_ART_CONFIG_WIDGET_H
#include <kis_config_widget.h>
class KisDoubleSliderSpinBox;
class KisSliderSpinBox;
class QComboBox;
class KisLineArtConfigWidget : public KisConfigWidget
{
public:
    KisLineArtConfigWidget(QWidget *parent, KisPaintDeviceSP device);
    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

private:
    KisDoubleSliderSpinBox *m_density;
    KisSliderSpinBox *m_radius;
    QComboBox *m_color;
};
#endif
