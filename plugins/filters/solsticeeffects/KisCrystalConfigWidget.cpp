/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisCrystalConfigWidget.h"
#include <KisGlobalResourcesInterface.h>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QVBoxLayout>
#include <filter/kis_filter_configuration.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>
#include <limits>

KisCrystalConfigWidget::KisCrystalConfigWidget(QWidget *parent, KisPaintDeviceSP device)
    : KisConfigWidget(parent)
    , m_size(new KisDoubleSliderSpinBox(this))
    , m_irregularity(new KisDoubleSliderSpinBox(this))
    , m_tiling(new QCheckBox(i18n("Seamless tiling"), this))
    , m_seed(new QSpinBox(this))
    , m_geometry(new QGroupBox(i18n("Tile bounds (document pixels)"), this))
    , m_x(new QSpinBox(this))
    , m_y(new QSpinBox(this))
    , m_width(new QSpinBox(this))
    , m_height(new QSpinBox(this))
{
    if (device) {
        const QRect bounds = device->defaultBounds()->bounds();
        if (!bounds.isEmpty() && bounds.width() <= 1000000 && bounds.height() <= 1000000)
            m_defaultTile = bounds;
    }
    const auto changed = [this]() {
        if (!m_loading)
            Q_EMIT sigConfigurationItemChanged();
    };
    m_size->setObjectName("crystalCellSize");
    m_tiling->setObjectName("crystalTiling");
    m_seed->setObjectName("crystalSeed");
    m_size->setRange(1.0, 512.0, 2);
    m_size->setSuffix(i18n(" px"));
    m_irregularity->setRange(0.0, 100.0, 2);
    m_irregularity->setSuffix(i18n("%"));
    m_seed->setRange(0, std::numeric_limits<int>::max());
    auto *newSeed = new QPushButton(i18n("New Seed"), this);
    auto *seedRow = new QHBoxLayout();
    seedRow->addWidget(m_seed);
    seedRow->addWidget(newSeed);
    auto *mainForm = new QFormLayout();
    mainForm->addRow(i18n("Cell size:"), m_size);
    mainForm->addRow(i18n("Irregularity:"), m_irregularity);
    mainForm->addRow(i18n("Seed:"), seedRow);
    mainForm->addRow(QString(), m_tiling);
    auto *geometryForm = new QFormLayout(m_geometry);
    for (auto *spin : {m_x, m_y, m_width, m_height}) {
        const bool dimension = spin == m_width || spin == m_height;
        spin->setRange(dimension ? 1 : -1000000000, dimension ? 1000000 : 1000000000);
        connect(spin, &QSpinBox::valueChanged, this, changed);
    }
    geometryForm->addRow(i18n("Origin X:"), m_x);
    geometryForm->addRow(i18n("Origin Y:"), m_y);
    geometryForm->addRow(i18n("Width:"), m_width);
    geometryForm->addRow(i18n("Height:"), m_height);
    m_geometry->setToolTip(
        i18n("Cell spacing fits this tile. Colors wrap within each source tile; repeating source tiles produce "
             "repeating output. Opacity is preserved."));
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(mainForm);
    layout->addWidget(m_geometry);
    connect(m_size, &KisDoubleSliderSpinBox::valueChanged, this, changed);
    connect(m_irregularity, &KisDoubleSliderSpinBox::valueChanged, this, changed);
    connect(m_seed, &QSpinBox::valueChanged, this, changed);
    connect(m_tiling, &QCheckBox::toggled, this, [this, changed](bool enabled) {
        m_geometry->setEnabled(enabled);
        changed();
    });
    connect(newSeed, &QPushButton::clicked, this, [this]() {
        m_seed->setValue(QRandomGenerator::global()->bounded(std::numeric_limits<int>::max()));
    });
    setConfiguration(KisPropertiesConfigurationSP(new KisPropertiesConfiguration()));
}

void KisCrystalConfigWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    m_loading = true;
    m_size->setValue(config->getDouble("cellSize", 20.0));
    m_irregularity->setValue(config->getDouble("irregularity", 100.0));
    m_tiling->setChecked(config->getBool("tiling", false));
    m_geometry->setEnabled(m_tiling->isChecked());
    m_seed->setValue(config->getInt("seed", 0));
    m_x->setValue(config->getInt("tileX", m_defaultTile.x()));
    m_y->setValue(config->getInt("tileY", m_defaultTile.y()));
    m_width->setValue(config->getInt("tileWidth", m_defaultTile.width()));
    m_height->setValue(config->getInt("tileHeight", m_defaultTile.height()));
    m_loading = false;
}

KisPropertiesConfigurationSP KisCrystalConfigWidget::configuration() const
{
    KisFilterConfigurationSP config =
        new KisFilterConfiguration("solstice_crystal", 1, KisGlobalResourcesInterface::instance());
    config->setProperty("cellSize", m_size->value());
    config->setProperty("irregularity", m_irregularity->value());
    config->setProperty("tiling", m_tiling->isChecked());
    config->setProperty("seed", m_seed->value());
    config->setProperty("tileX", m_x->value());
    config->setProperty("tileY", m_y->value());
    config->setProperty("tileWidth", m_width->value());
    config->setProperty("tileHeight", m_height->value());
    return config;
}
