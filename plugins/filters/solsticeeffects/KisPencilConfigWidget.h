/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_PENCIL_CONFIG_WIDGET_H
#define KIS_PENCIL_CONFIG_WIDGET_H
#include <kis_config_widget.h>
class QCheckBox;
class QSpinBox;
class KisDoubleSliderSpinBox;
class KisPencilConfigWidget : public KisConfigWidget
{
public:
    KisPencilConfigWidget(QWidget *parent, KisPaintDeviceSP device);
    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

private:
    void updateEnabled();
    bool m_loading = false;
    QCheckBox *m_outlines, *m_hatching, *m_grayscale;
    KisDoubleSliderSpinBox *m_outlineStrength, *m_hatchStrength, *m_size, *m_roughness, *m_angle;
    QSpinBox *m_seed;
};
#endif
