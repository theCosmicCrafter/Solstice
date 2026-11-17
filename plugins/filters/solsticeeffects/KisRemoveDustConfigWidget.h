/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_REMOVE_DUST_CONFIG_WIDGET_H
#define KIS_REMOVE_DUST_CONFIG_WIDGET_H
#include <kis_config_widget.h>
class KisColorButton;
class KisDoubleSliderSpinBox;
class KisSliderSpinBox;
class QComboBox;
class KisRemoveDustConfigWidget : public KisConfigWidget
{
public:
    KisRemoveDustConfigWidget(QWidget *parent, KisPaintDeviceSP device);
    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

private:
    void updateModeControls();
    QComboBox *m_mode;
    KisSliderSpinBox *m_size;
    KisDoubleSliderSpinBox *m_tolerance;
    KisColorButton *m_color;
};
#endif
