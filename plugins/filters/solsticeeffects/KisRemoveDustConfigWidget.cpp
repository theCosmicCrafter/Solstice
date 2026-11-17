/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisRemoveDustConfigWidget.h"
#include "KisRemoveDustFilter.h"
#include <KisGlobalResourcesInterface.h>
#include <KoColorSpaceRegistry.h>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <filter/kis_filter_configuration.h>
#include <kis_color_button.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>
KisRemoveDustConfigWidget::KisRemoveDustConfigWidget(QWidget *parent, KisPaintDeviceSP device)
    : KisConfigWidget(parent)
    , m_mode(new QComboBox(this))
    , m_size(new KisSliderSpinBox(this))
    , m_tolerance(new KisDoubleSliderSpinBox(this))
    , m_color(new KisColorButton(this))
{
    auto *layout = new QFormLayout(this);
    m_mode->setObjectName("removeDustMode");
    m_size->setObjectName("removeDustSize");
    m_tolerance->setObjectName("removeDustTolerance");
    m_color->setObjectName("removeDustColor");
    const QStringList ids = KisRemoveDustFilter::modeIds();
    m_mode->addItem(i18n("Remove dust from transparency"), ids[0]);
    m_mode->addItem(i18n("Remove dust from white background"), ids[1]);
    m_mode->addItem(i18n("Fill transparent gaps with surrounding color"), ids[2]);
    m_mode->addItem(i18n("Fill transparent gaps with a color"), ids[3]);
    m_size->setRange(KisDustDetector::minimumDustSize, 200);
    m_size->setValue(4);
    m_size->setSuffix(i18n(" px"));
    m_tolerance->setRange(0, 100, 1);
    m_tolerance->setValue(KisDustDetector::defaultTolerancePercent);
    m_tolerance->setSuffix(i18n("%"));
    m_color->setColor(KoColor(Qt::black, device ? device->colorSpace() : KoColorSpaceRegistry::instance()->rgb8()));
    layout->addRow(i18n("Mode:"), m_mode);
    layout->addRow(i18n("Dust size:"), m_size);
    layout->addRow(i18n("Tolerance:"), m_tolerance);
    layout->addRow(i18n("Fill color:"), m_color);
    m_mode->setToolTip(i18n("Choose what counts as dust and what replaces it."));
    m_size->setToolTip(i18n("Spots whose width and height are both at most this size are treated as dust."));
    auto *notice = new QLabel(i18n("Spots touching the edge of a selection may be changed only partly. Use the "
                                   "Remove Dust tool to work on an enclosed area."),
                              this);
    notice->setWordWrap(true);
    layout->addRow(notice);
    updateModeControls();
    connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        updateModeControls();
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(m_size, &KisSliderSpinBox::valueChanged, this, [this] {
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(m_tolerance, &KisDoubleSliderSpinBox::valueChanged, this, [this] {
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(m_color, &KisColorButton::changed, this, [this] {
        Q_EMIT sigConfigurationItemChanged();
    });
}
void KisRemoveDustConfigWidget::updateModeControls()
{
    const int mode = m_mode->currentIndex();
    m_color->setEnabled(mode == 3);
    m_tolerance->setToolTip(
        mode == 0 ? i18n("Pixels at or below this opacity count as transparent.")
            : mode == 1
            ? i18n("Colors this close to white count as white. Raise it for scanned or off-white paper.")
            : i18n("Pixels at least this much below full opacity count as gaps. Zero treats any pixel that is "
                   "not fully opaque as a gap."));
}
void KisRemoveDustConfigWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    {
        const QSignalBlocker a(m_mode), b(m_size), c(m_tolerance), d(m_color);
        const int index = m_mode->findData(config->getString("mode", KisRemoveDustFilter::modeIds()[0]));
        m_mode->setCurrentIndex(qMax(0, index));
        m_size->setValue(config->getInt("dustSize", 4));
        m_tolerance->setValue(config->getDouble("tolerance", KisDustDetector::defaultTolerancePercent));
        m_color->setColor(config->getColor("color", m_color->color()));
    }
    updateModeControls();
}
KisPropertiesConfigurationSP KisRemoveDustConfigWidget::configuration() const
{
    KisFilterConfigurationSP config =
        new KisFilterConfiguration("solstice_remove_dust", 1, KisGlobalResourcesInterface::instance());
    config->setProperty("mode", m_mode->currentData().toString());
    config->setProperty("dustSize", m_size->value());
    config->setProperty("tolerance", m_tolerance->value());
    config->setProperty("color", QVariant::fromValue(m_color->color()));
    return config;
}
