/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisSharpnessConfigWidget.h"
#include <KisGlobalResourcesInterface.h>
#include <KoColorModelStandardIds.h>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <filter/kis_filter_configuration.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>
KisSharpnessConfigWidget::KisSharpnessConfigWidget(QWidget *parent, KisPaintDeviceSP device)
    : KisConfigWidget(parent)
    , m_strength(new KisDoubleSliderSpinBox(this))
{
    m_strength->setObjectName("sharpnessStrength");
    m_strength->setRange(0, 100, 1);
    m_strength->setValue(50);
    m_strength->setSuffix(i18n("%"));
    auto *layout = new QFormLayout(this);
    layout->addRow(i18n("Strength:"), m_strength);
    if (device && device->colorSpace()->colorModelId() != RGBAColorModelID
        && device->colorSpace()->colorModelId() != GrayAColorModelID) {
        auto *notice = new QLabel(
            i18n("This color model is processed through floating-point RGB and converted back; appearance may change."),
            this);
        notice->setWordWrap(true);
        layout->addRow(notice);
    }
    connect(m_strength, &KisDoubleSliderSpinBox::valueChanged, this, [this] {
        Q_EMIT sigConfigurationItemChanged();
    });
}
void KisSharpnessConfigWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    const QSignalBlocker blocker(m_strength);
    m_strength->setValue(config->getDouble("strength", 50));
}
KisPropertiesConfigurationSP KisSharpnessConfigWidget::configuration() const
{
    KisFilterConfigurationSP config =
        new KisFilterConfiguration("solstice_sharpness", 1, KisGlobalResourcesInterface::instance());
    config->setProperty("strength", m_strength->value());
    return config;
}
