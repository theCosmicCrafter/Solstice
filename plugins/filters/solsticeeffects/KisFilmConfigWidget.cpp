/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisFilmConfigWidget.h"
#include "KisFilmFilter.h"

#include <KisGlobalResourcesInterface.h>
#include <KoColorModelStandardIds.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <filter/kis_filter_configuration.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>

#include <cmath>
#include <limits>

KisFilmConfigWidget::KisFilmConfigWidget(QWidget *parent, KisPaintDeviceSP device)
    : KisConfigWidget(parent)
    , m_preset(new QComboBox(this))
    , m_tone(new QComboBox(this))
    , m_toneStrength(new KisDoubleSliderSpinBox(this))
    , m_aberration(new KisDoubleSliderSpinBox(this))
    , m_angle(new KisDoubleSliderSpinBox(this))
    , m_grain(new KisDoubleSliderSpinBox(this))
    , m_leak(new KisDoubleSliderSpinBox(this))
    , m_monochrome(new QCheckBox(i18n("Monochrome grain"), this))
    , m_seed(new QSpinBox(this))
    , m_leakX(new QDoubleSpinBox(this))
    , m_leakY(new QDoubleSpinBox(this))
    , m_leakRadius(new QDoubleSpinBox(this))
{
    if (device) {
        const QRect bounds = device->defaultBounds()->bounds();
        if (!bounds.isEmpty()) {
            m_defaultLeak = QPointF(bounds.right(), QRectF(bounds).center().y() - 0.5);
            m_defaultRadius = qMax(1.0, std::hypot(bounds.width(), bounds.height()) / 2.0);
        }
    }
    m_preset->setObjectName("filmPreset");
    m_tone->setObjectName("filmTone");
    m_toneStrength->setObjectName("filmToneStrength");
    m_seed->setObjectName("filmSeed");
    m_preset->addItems({i18n("Custom"), i18n("Vintage"), i18n("Modern"), i18n("Warm")});
    m_tone->addItems({i18n("None"), i18n("Fade"), i18n("Sepia"), i18n("Warm"), i18n("Cool")});
    for (auto *slider : {m_toneStrength, m_aberration, m_grain, m_leak}) {
        slider->setRange(0.0, 100.0, 2);
        connect(slider, &KisDoubleSliderSpinBox::valueChanged, this, [this]() {
            changed();
        });
    }
    m_aberration->setSuffix(i18n(" px"));
    m_angle->setRange(-180.0, 180.0, 2);
    m_angle->setSuffix(i18n("°"));
    connect(m_angle, &KisDoubleSliderSpinBox::valueChanged, this, [this]() {
        changed();
    });
    for (auto *spin : {m_leakX, m_leakY, m_leakRadius}) {
        spin->setDecimals(2);
        spin->setRange(spin == m_leakRadius ? 1.0 : -1e9, 1e9);
        spin->setSuffix(i18n(" px"));
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this]() {
            changed();
        });
    }
    m_seed->setRange(0, std::numeric_limits<int>::max());
    auto *newSeed = new QPushButton(i18n("New Seed"), this);
    auto *seedRow = new QHBoxLayout();
    seedRow->addWidget(m_seed);
    seedRow->addWidget(newSeed);

    auto *lookPage = new QWidget(this);
    auto *look = new QFormLayout(lookPage);
    look->addRow(i18n("Preset:"), m_preset);
    look->addRow(i18n("Color treatment:"), m_tone);
    look->addRow(i18n("Color strength:"), m_toneStrength);
    look->addRow(i18n("Chromatic aberration:"), m_aberration);
    look->addRow(i18n("Grain strength:"), m_grain);
    look->addRow(i18n("Light leak strength:"), m_leak);
    auto *detailPage = new QWidget(this);
    auto *details = new QFormLayout(detailPage);
    details->addRow(i18n("Aberration angle:"), m_angle);
    details->addRow(QString(), m_monochrome);
    details->addRow(i18n("Grain seed:"), seedRow);
    details->addRow(i18n("Light leak X:"), m_leakX);
    details->addRow(i18n("Light leak Y:"), m_leakY);
    details->addRow(i18n("Light leak radius:"), m_leakRadius);
    auto *tabs = new QTabWidget(this);
    tabs->addTab(lookPage, i18n("Look"));
    tabs->addTab(detailPage, i18n("Details"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    if (device && device->colorSpace()->colorModelId() != RGBAColorModelID) {
        auto *notice = new QLabel(i18n("Color treatment and chromatic aberration use floating-point RGB conversion for "
                                       "this color model. Original opacity is preserved."),
                                  this);
        notice->setWordWrap(true);
        layout->addWidget(notice);
    }
    connect(m_preset, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (index != KisFilmFilter::Custom) {
            auto config = configuration();
            KisFilmFilter::applyPreset(config, index);
            setConfiguration(config);
        }
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(m_tone, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        changed();
    });
    connect(m_monochrome, &QCheckBox::toggled, this, [this]() {
        changed();
    });
    connect(m_seed, &QSpinBox::valueChanged, this, [this]() {
        changed();
    });
    connect(newSeed, &QPushButton::clicked, this, [this]() {
        m_seed->setValue(QRandomGenerator::global()->bounded(std::numeric_limits<int>::max()));
    });
    KisFilmFilter filter;
    setConfiguration(filter.defaultConfiguration(KisGlobalResourcesInterface::instance()));
}

void KisFilmConfigWidget::changed()
{
    if (m_loading)
        return;
    const QSignalBlocker blocker(m_preset);
    m_preset->setCurrentIndex(KisFilmFilter::Custom);
    Q_EMIT sigConfigurationItemChanged();
}

void KisFilmConfigWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    m_loading = true;
    m_preset->setCurrentIndex(qBound(0, config->getInt("preset", KisFilmFilter::Custom), 3));
    m_tone->setCurrentIndex(qBound(0, config->getInt("toneType", KisFilmFilter::Sepia), 4));
    m_toneStrength->setValue(config->getDouble("toneStrength", 60.0));
    m_aberration->setValue(config->getDouble("aberrationStrength", 1.5));
    m_angle->setValue(config->getDouble("aberrationAngle", 90.0));
    m_grain->setValue(config->getDouble("grainStrength", 20.0));
    m_leak->setValue(config->getDouble("lightLeakStrength", 35.0));
    m_monochrome->setChecked(config->getBool("monochrome", true));
    m_seed->setValue(config->getInt("seed", 0));
    m_leakX->setValue(config->getDouble("lightLeakX", m_defaultLeak.x()));
    m_leakY->setValue(config->getDouble("lightLeakY", m_defaultLeak.y()));
    m_leakRadius->setValue(config->getDouble("lightLeakRadius", m_defaultRadius));
    m_loading = false;
}

KisPropertiesConfigurationSP KisFilmConfigWidget::configuration() const
{
    KisFilterConfigurationSP config =
        new KisFilterConfiguration("solstice_film", 1, KisGlobalResourcesInterface::instance());
    config->setProperty("preset", m_preset->currentIndex());
    config->setProperty("toneType", m_tone->currentIndex());
    config->setProperty("toneStrength", m_toneStrength->value());
    config->setProperty("aberrationStrength", m_aberration->value());
    config->setProperty("aberrationAngle", m_angle->value());
    config->setProperty("grainStrength", m_grain->value());
    config->setProperty("lightLeakStrength", m_leak->value());
    config->setProperty("monochrome", m_monochrome->isChecked());
    config->setProperty("seed", m_seed->value());
    config->setProperty("lightLeakX", m_leakX->value());
    config->setProperty("lightLeakY", m_leakY->value());
    config->setProperty("lightLeakRadius", m_leakRadius->value());
    return config;
}
