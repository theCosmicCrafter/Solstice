/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisGlowConfigWidget.h"
#include <KisGlobalResourcesInterface.h>
#include <KoColorModelStandardIds.h>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <filter/kis_filter_configuration.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>
KisGlowConfigWidget::KisGlowConfigWidget(QWidget *parent, KisPaintDeviceSP device)
    : KisConfigWidget(parent)
    , m_strength(new KisDoubleSliderSpinBox(this))
    , m_area(new KisDoubleSliderSpinBox(this))
    , m_range(new KisDoubleSliderSpinBox(this))
{
    auto *layout = new QFormLayout(this);
    m_strength->setObjectName("glowStrength");
    m_area->setObjectName("glowEffectArea");
    m_range->setObjectName("glowBlurRange");
    for (auto *slider : {m_strength, m_area, m_range}) {
        slider->setRange(0, 100, 1);
        connect(slider, &KisDoubleSliderSpinBox::valueChanged, this, [this] {
            Q_EMIT sigConfigurationItemChanged();
        });
    }
    m_strength->setValue(50);
    m_area->setValue(50);
    m_range->setValue(30);
    layout->addRow(i18n("Strength:"), m_strength);
    layout->addRow(i18n("Effect area:"), m_area);
    layout->addRow(i18n("Blur range:"), m_range);
    m_area->setToolTip(i18n("Higher values include darker parts of the image in the glow."));
    m_range->setToolTip(
        i18n("Higher values spread the light farther. Zero brightens highlights without spreading them."));
    auto *notice =
        new QLabel(i18n("The original opacity is preserved. Glow does not extend outside painted areas."), this);
    notice->setWordWrap(true);
    layout->addRow(notice);
    if (device && device->colorSpace()->colorModelId() != RGBAColorModelID
        && device->colorSpace()->colorModelId() != GrayAColorModelID) {
        auto *fallback = new QLabel(
            i18n("This color model is processed through floating-point RGB and converted back; appearance may change."),
            this);
        fallback->setWordWrap(true);
        layout->addRow(fallback);
    }
}
void KisGlowConfigWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    const QSignalBlocker strength(m_strength), area(m_area), range(m_range);
    m_strength->setValue(config->getDouble("strength", 50));
    m_area->setValue(config->getDouble("effectArea", 50));
    m_range->setValue(config->getDouble("blurRange", 30));
}
KisPropertiesConfigurationSP KisGlowConfigWidget::configuration() const
{
    KisFilterConfigurationSP config =
        new KisFilterConfiguration("solstice_glow", 1, KisGlobalResourcesInterface::instance());
    config->setProperty("strength", m_strength->value());
    config->setProperty("effectArea", m_area->value());
    config->setProperty("blurRange", m_range->value());
    return config;
}
