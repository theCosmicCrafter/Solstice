/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisFilmGrainConfigWidget.h"

#include <KisGlobalResourcesInterface.h>
#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSignalBlocker>
#include <QSpinBox>
#include <filter/kis_filter_configuration.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>

#include <limits>

KisFilmGrainConfigWidget::KisFilmGrainConfigWidget(QWidget *parent)
    : KisConfigWidget(parent)
    , m_strength(new KisDoubleSliderSpinBox(this))
    , m_monochrome(new QCheckBox(i18n("Monochrome"), this))
    , m_seed(new QSpinBox(this))
{
    m_strength->setRange(0.0, 100.0, 2);
    m_strength->setValue(30.0);
    m_seed->setRange(0, std::numeric_limits<int>::max());
    auto *newSeed = new QPushButton(i18n("New Seed"), this);
    auto *seedLayout = new QHBoxLayout();
    seedLayout->addWidget(m_seed);
    seedLayout->addWidget(newSeed);
    auto *layout = new QFormLayout(this);
    layout->addRow(i18n("Strength:"), m_strength);
    layout->addRow(QString(), m_monochrome);
    layout->addRow(i18n("Seed:"), seedLayout);

    connect(m_strength, &KisDoubleSliderSpinBox::valueChanged, this, [this]() {
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(m_monochrome, &QCheckBox::toggled, this, [this]() {
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(m_seed, &QSpinBox::valueChanged, this, [this]() {
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(newSeed, &QPushButton::clicked, this, [this]() {
        m_seed->setValue(QRandomGenerator::global()->bounded(std::numeric_limits<int>::max()));
    });
}

void KisFilmGrainConfigWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    const QSignalBlocker strengthBlocker(m_strength);
    const QSignalBlocker monochromeBlocker(m_monochrome);
    const QSignalBlocker seedBlocker(m_seed);
    m_strength->setValue(config->getDouble("strength", 30.0));
    m_monochrome->setChecked(config->getBool("monochrome", false));
    m_seed->setValue(config->getInt("seed", 0));
}

KisPropertiesConfigurationSP KisFilmGrainConfigWidget::configuration() const
{
    KisFilterConfigurationSP config =
        new KisFilterConfiguration("solstice_film_grain", 1, KisGlobalResourcesInterface::instance());
    config->setProperty("strength", m_strength->value());
    config->setProperty("monochrome", m_monochrome->isChecked());
    config->setProperty("seed", m_seed->value());
    return config;
}
