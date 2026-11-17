/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_FILM_GRAIN_CONFIG_WIDGET_H
#define KIS_FILM_GRAIN_CONFIG_WIDGET_H

#include <kis_config_widget.h>

class QCheckBox;
class QSpinBox;
class KisDoubleSliderSpinBox;

class KisFilmGrainConfigWidget : public KisConfigWidget
{
public:
    explicit KisFilmGrainConfigWidget(QWidget *parent);
    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

private:
    KisDoubleSliderSpinBox *m_strength;
    QCheckBox *m_monochrome;
    QSpinBox *m_seed;
};

#endif
