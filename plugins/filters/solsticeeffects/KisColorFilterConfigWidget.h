/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_COLOR_FILTER_CONFIG_WIDGET_H
#define KIS_COLOR_FILTER_CONFIG_WIDGET_H
#include <QMap>
#include <kis_config_widget.h>
class KisDoubleSliderSpinBox;
class QLabel;
class QButtonGroup;
class KisColorFilterConfigWidget : public KisConfigWidget
{
public:
    KisColorFilterConfigWidget(QWidget *parent, KisPaintDeviceSP device, bool useForMasks);
    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

private:
    void updatePresetLabel();
    void renderThumbnail(int index);
    KisPropertiesConfigurationSP m_values;
    QMap<QString, KisDoubleSliderSpinBox *> m_controls;
    QButtonGroup *m_presets;
    QLabel *m_presetLabel;
    KisPaintDeviceSP m_thumbnail;
    QRect m_thumbnailRect;
    bool m_loading = false;
};
#endif
