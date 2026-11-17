/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisChromaticAberrationConfigWidget.h"

#include <KisGlobalResourcesInterface.h>
#include <KoColorModelStandardIds.h>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <filter/kis_filter_configuration.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>

#include <cmath>

KisChromaticAberrationConfigWidget::KisChromaticAberrationConfigWidget(QWidget *parent, KisPaintDeviceSP device)
    : KisConfigWidget(parent)
    , m_mode(new QComboBox(this))
    , m_strength(new KisDoubleSliderSpinBox(this))
    , m_angle(new KisDoubleSliderSpinBox(this))
    , m_centerX(new QDoubleSpinBox(this))
    , m_centerY(new QDoubleSpinBox(this))
    , m_radius(new QDoubleSpinBox(this))
{
    if (device) {
        const QRect bounds = device->defaultBounds()->bounds();
        if (!bounds.isEmpty()) {
            m_defaultCenter = QRectF(bounds).center() - QPointF(0.5, 0.5);
            m_defaultRadius = qMax(1.0, std::hypot(bounds.width(), bounds.height()) / 2.0);
        }
    }
    m_mode->addItems({i18n("Translation"), i18n("Radial")});
    m_strength->setRange(0.0, 100.0, 2);
    m_strength->setSuffix(i18n(" px"));
    m_angle->setRange(-180.0, 180.0, 2);
    m_angle->setSuffix(i18n("°"));
    for (auto *spin : {m_centerX, m_centerY, m_radius}) {
        spin->setDecimals(2);
        spin->setRange(spin == m_radius ? 1.0 : -1e9, 1e9);
        spin->setSuffix(i18n(" px"));
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this]() {
            Q_EMIT sigConfigurationItemChanged();
        });
    }
    auto *layout = new QFormLayout(this);
    layout->addRow(i18n("Mode:"), m_mode);
    layout->addRow(i18n("Strength:"), m_strength);
    layout->addRow(i18n("Angle:"), m_angle);
    layout->addRow(i18n("Center X:"), m_centerX);
    layout->addRow(i18n("Center Y:"), m_centerY);
    layout->addRow(i18n("Reference radius:"), m_radius);
    m_radius->setToolTip(
        i18n("Radial displacement increases from zero at the center to Strength at this radius, then stays constant."));
    if (device && device->colorSpace()->colorModelId() != RGBAColorModelID) {
        auto *notice = new QLabel(i18n("This color model is processed through floating-point RGB and converted back. "
                                       "The original opacity is preserved."),
                                  this);
        notice->setWordWrap(true);
        layout->addRow(notice);
    }
    connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        updateMode();
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(m_strength, &KisDoubleSliderSpinBox::valueChanged, this, [this]() {
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(m_angle, &KisDoubleSliderSpinBox::valueChanged, this, [this]() {
        Q_EMIT sigConfigurationItemChanged();
    });
    KisPropertiesConfigurationSP defaults = new KisPropertiesConfiguration();
    setConfiguration(defaults);
}

void KisChromaticAberrationConfigWidget::updateMode()
{
    const bool radial = m_mode->currentIndex() == 1;
    m_angle->setEnabled(!radial);
    m_centerX->setEnabled(radial);
    m_centerY->setEnabled(radial);
    m_radius->setEnabled(radial);
}

void KisChromaticAberrationConfigWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    const QSignalBlocker modeBlocker(m_mode), strengthBlocker(m_strength), angleBlocker(m_angle);
    const QSignalBlocker xBlocker(m_centerX), yBlocker(m_centerY), radiusBlocker(m_radius);
    m_mode->setCurrentIndex(config->getInt("mode", 0) == 1 ? 1 : 0);
    m_strength->setValue(config->getDouble("strength", 3.0));
    m_angle->setValue(std::remainder(config->getDouble("angle", 0.0), 360.0));
    m_centerX->setValue(config->getDouble("centerX", m_defaultCenter.x()));
    m_centerY->setValue(config->getDouble("centerY", m_defaultCenter.y()));
    m_radius->setValue(config->getDouble("radius", m_defaultRadius));
    updateMode();
}

KisPropertiesConfigurationSP KisChromaticAberrationConfigWidget::configuration() const
{
    KisFilterConfigurationSP config =
        new KisFilterConfiguration("solstice_chromatic_aberration", 1, KisGlobalResourcesInterface::instance());
    config->setProperty("mode", m_mode->currentIndex());
    config->setProperty("strength", m_strength->value());
    config->setProperty("angle", m_angle->value());
    config->setProperty("centerX", m_centerX->value());
    config->setProperty("centerY", m_centerY->value());
    config->setProperty("radius", m_radius->value());
    return config;
}
