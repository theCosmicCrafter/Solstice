/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_CRYSTAL_CONFIG_WIDGET_H
#define KIS_CRYSTAL_CONFIG_WIDGET_H

#include <QRect>
#include <kis_config_widget.h>

class QCheckBox;
class QGroupBox;
class QSpinBox;
class KisDoubleSliderSpinBox;

class KisCrystalConfigWidget : public KisConfigWidget
{
public:
    KisCrystalConfigWidget(QWidget *parent, KisPaintDeviceSP device);
    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

private:
    bool m_loading = false;
    KisDoubleSliderSpinBox *m_size;
    KisDoubleSliderSpinBox *m_irregularity;
    QCheckBox *m_tiling;
    QSpinBox *m_seed;
    QGroupBox *m_geometry;
    QSpinBox *m_x, *m_y, *m_width, *m_height;
    QRect m_defaultTile = QRect(0, 0, 1024, 1024);
};

#endif
