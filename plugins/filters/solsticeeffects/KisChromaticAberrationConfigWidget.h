/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_CHROMATIC_ABERRATION_CONFIG_WIDGET_H
#define KIS_CHROMATIC_ABERRATION_CONFIG_WIDGET_H

#include <QPointF>
#include <kis_config_widget.h>

class QComboBox;
class QDoubleSpinBox;
class KisDoubleSliderSpinBox;

class KisChromaticAberrationConfigWidget : public KisConfigWidget
{
public:
    KisChromaticAberrationConfigWidget(QWidget *parent, KisPaintDeviceSP device);
    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

private:
    void updateMode();
    QComboBox *m_mode;
    KisDoubleSliderSpinBox *m_strength;
    KisDoubleSliderSpinBox *m_angle;
    QDoubleSpinBox *m_centerX;
    QDoubleSpinBox *m_centerY;
    QDoubleSpinBox *m_radius;
    QPointF m_defaultCenter;
    double m_defaultRadius = 1000.0;
};

#endif
