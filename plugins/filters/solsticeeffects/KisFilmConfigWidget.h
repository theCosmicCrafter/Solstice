/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_FILM_CONFIG_WIDGET_H
#define KIS_FILM_CONFIG_WIDGET_H

#include <QPointF>
#include <kis_config_widget.h>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class KisDoubleSliderSpinBox;

class KisFilmConfigWidget : public KisConfigWidget
{
public:
    KisFilmConfigWidget(QWidget *parent, KisPaintDeviceSP device);
    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

private:
    void changed();
    bool m_loading = false;
    QComboBox *m_preset;
    QComboBox *m_tone;
    KisDoubleSliderSpinBox *m_toneStrength;
    KisDoubleSliderSpinBox *m_aberration;
    KisDoubleSliderSpinBox *m_angle;
    KisDoubleSliderSpinBox *m_grain;
    KisDoubleSliderSpinBox *m_leak;
    QCheckBox *m_monochrome;
    QSpinBox *m_seed;
    QDoubleSpinBox *m_leakX;
    QDoubleSpinBox *m_leakY;
    QDoubleSpinBox *m_leakRadius;
    QPointF m_defaultLeak = QPointF(1000.0, 500.0);
    double m_defaultRadius = 750.0;
};

#endif
