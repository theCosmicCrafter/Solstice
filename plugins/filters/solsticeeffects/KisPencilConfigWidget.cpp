/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisPencilConfigWidget.h"
#include <KisGlobalResourcesInterface.h>
#include <KoColorModelStandardIds.h>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QVBoxLayout>
#include <filter/kis_filter_configuration.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>
#include <limits>

KisPencilConfigWidget::KisPencilConfigWidget(QWidget *parent, KisPaintDeviceSP device)
    : KisConfigWidget(parent)
    , m_outlines(new QCheckBox(i18n("Outlines"), this))
    , m_hatching(new QCheckBox(i18n("Hatching"), this))
    , m_grayscale(new QCheckBox(i18n("Grayscale output"), this))
    , m_outlineStrength(new KisDoubleSliderSpinBox(this))
    , m_hatchStrength(new KisDoubleSliderSpinBox(this))
    , m_size(new KisDoubleSliderSpinBox(this))
    , m_roughness(new KisDoubleSliderSpinBox(this))
    , m_angle(new KisDoubleSliderSpinBox(this))
    , m_seed(new QSpinBox(this))
{
    m_outlines->setObjectName("pencilOutlines");
    m_hatching->setObjectName("pencilHatching");
    m_grayscale->setObjectName("pencilGrayscale");
    m_size->setObjectName("pencilHatchSize");
    m_seed->setObjectName("pencilSeed");
    m_angle->setObjectName("pencilAngle");
    const auto changed = [this]() {
        updateEnabled();
        if (!m_loading)
            Q_EMIT sigConfigurationItemChanged();
    };
    for (auto *slider : {m_outlineStrength, m_hatchStrength, m_roughness}) {
        slider->setRange(0.0, 100.0, 1);
        slider->setSuffix(i18n("%"));
    }
    m_size->setRange(1.0, 32.0, 2);
    m_size->setSuffix(i18n(" px"));
    m_angle->setRange(0.0, 180.0, 1);
    m_angle->setSuffix(i18n("°"));
    m_angle->setToolTip(i18n("Stroke direction clockwise from horizontal, anchored to document pixels."));
    m_roughness->setToolTip(
        i18n("Varies stroke position, length and intensity. Zero produces regular parallel lines."));
    m_seed->setRange(0, std::numeric_limits<int>::max());
    auto *newSeed = new QPushButton(i18n("New Seed"), this);
    newSeed->setObjectName("pencilNewSeed");
    auto *seedRow = new QHBoxLayout();
    seedRow->addWidget(m_seed);
    seedRow->addWidget(newSeed);
    auto *outlineForm = new QFormLayout();
    outlineForm->addRow(m_outlines);
    outlineForm->addRow(i18n("Outline strength:"), m_outlineStrength);
    auto *hatchForm = new QFormLayout();
    hatchForm->addRow(m_hatching);
    hatchForm->addRow(i18n("Hatching strength:"), m_hatchStrength);
    hatchForm->addRow(i18n("Hatching size:"), m_size);
    hatchForm->addRow(i18n("Roughness:"), m_roughness);
    hatchForm->addRow(i18n("Angle:"), m_angle);
    hatchForm->addRow(i18n("Seed:"), seedRow);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(outlineForm);
    layout->addLayout(hatchForm);
    layout->addWidget(m_grayscale);
    if (device && device->colorSpace()->colorModelId() != RGBAColorModelID) {
        auto *notice = new QLabel(
            i18n("This color model is processed through floating-point RGB and converted back; appearance may change."),
            this);
        notice->setWordWrap(true);
        layout->addWidget(notice);
    }
    for (auto *box : {m_outlines, m_hatching, m_grayscale})
        connect(box, &QCheckBox::toggled, this, changed);
    for (auto *slider : {m_outlineStrength, m_hatchStrength, m_size, m_roughness, m_angle})
        connect(slider, &KisDoubleSliderSpinBox::valueChanged, this, changed);
    connect(m_seed, &QSpinBox::valueChanged, this, changed);
    connect(newSeed, &QPushButton::clicked, this, [this]() {
        m_seed->setValue(QRandomGenerator::global()->bounded(std::numeric_limits<int>::max()));
    });
    connect(m_hatching, &QCheckBox::toggled, newSeed, &QWidget::setEnabled);
    setConfiguration(KisPropertiesConfigurationSP(new KisPropertiesConfiguration()));
}
void KisPencilConfigWidget::updateEnabled()
{
    m_outlineStrength->setEnabled(m_outlines->isChecked());
    for (auto *slider : {m_hatchStrength, m_size, m_roughness, m_angle})
        slider->setEnabled(m_hatching->isChecked());
    m_seed->setEnabled(m_hatching->isChecked());
    m_grayscale->setEnabled(m_outlines->isChecked() || m_hatching->isChecked());
}
void KisPencilConfigWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    m_loading = true;
    m_outlines->setChecked(config->getBool("outlines", true));
    m_outlineStrength->setValue(config->getDouble("outlineStrength", 60.0));
    m_hatching->setChecked(config->getBool("hatching", true));
    m_hatchStrength->setValue(config->getDouble("hatchStrength", 65.0));
    m_size->setValue(config->getDouble("hatchSize", 4.0));
    m_roughness->setValue(config->getDouble("roughness", 50.0));
    m_angle->setValue(config->getDouble("angle", 45.0));
    m_seed->setValue(config->getInt("seed", 0));
    m_grayscale->setChecked(config->getBool("grayscale", false));
    updateEnabled();
    m_loading = false;
}
KisPropertiesConfigurationSP KisPencilConfigWidget::configuration() const
{
    KisFilterConfigurationSP config =
        new KisFilterConfiguration("solstice_pencil", 1, KisGlobalResourcesInterface::instance());
    config->setProperty("outlines", m_outlines->isChecked());
    config->setProperty("outlineStrength", m_outlineStrength->value());
    config->setProperty("hatching", m_hatching->isChecked());
    config->setProperty("hatchStrength", m_hatchStrength->value());
    config->setProperty("hatchSize", m_size->value());
    config->setProperty("roughness", m_roughness->value());
    config->setProperty("angle", m_angle->value());
    config->setProperty("seed", m_seed->value());
    config->setProperty("grayscale", m_grayscale->isChecked());
    return config;
}
