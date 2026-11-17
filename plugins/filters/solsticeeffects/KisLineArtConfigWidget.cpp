/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisLineArtConfigWidget.h"
#include <KisGlobalResourcesInterface.h>
#include <KoColorModelStandardIds.h>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <filter/kis_filter_configuration.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>
KisLineArtConfigWidget::KisLineArtConfigWidget(QWidget *parent, KisPaintDeviceSP device)
    : KisConfigWidget(parent)
    , m_density(new KisDoubleSliderSpinBox(this))
    , m_radius(new KisSliderSpinBox(this))
    , m_color(new QComboBox(this))
{
    auto *layout = new QFormLayout(this);
    m_density->setObjectName("lineArtDensity");
    m_radius->setObjectName("lineArtPaperRadius");
    m_color->setObjectName("lineArtColor");
    m_density->setRange(0, 200, 1);
    m_density->setValue(100);
    m_radius->setRange(8, 256);
    m_radius->setValue(32);
    m_color->addItem(i18n("Black"), "black");
    m_color->addItem(i18n("Keep original"), "original");
    layout->addRow(i18n("Density:"), m_density);
    layout->addRow(i18n("Line color:"), m_color);
    layout->addRow(i18n("Paper correction radius (pixels):"), m_radius);
    m_density->setToolTip(
        i18n("Higher values retain more faint lines. Zero removes all ink; it does not bypass the filter."));
    m_radius->setToolTip(
        i18n("Choose a radius larger than the line width. Increase it for thick ink; reduce it for rapidly changing "
             "paper lighting."));
    auto *notice = new QLabel(i18n("Extracts dark marks on light paper into a transparent background. Very dark paper, "
                                   "sharp shadows and broad shaded areas may need manual correction."),
                              this);
    notice->setWordWrap(true);
    layout->addRow(notice);
    if (device && device->colorSpace()->colorModelId() != RGBAColorModelID
        && device->colorSpace()->colorModelId() != GrayAColorModelID) {
        auto *fallback = new QLabel(i18n("Paper brightness is measured through floating-point RGB. Keep original "
                                         "preserves the document's color channels."),
                                    this);
        fallback->setWordWrap(true);
        layout->addRow(fallback);
    }
    connect(m_density, &KisDoubleSliderSpinBox::valueChanged, this, [this] {
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(m_radius, &KisSliderSpinBox::valueChanged, this, [this] {
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(m_color, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        Q_EMIT sigConfigurationItemChanged();
    });
}
void KisLineArtConfigWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    const QSignalBlocker a(m_density), b(m_radius), c(m_color);
    m_density->setValue(config->getDouble("density", 100));
    m_radius->setValue(config->getInt("paperRadius", 32));
    m_color->setCurrentIndex(config->getString("lineColor", "black") == "original" ? 1 : 0);
}
KisPropertiesConfigurationSP KisLineArtConfigWidget::configuration() const
{
    KisFilterConfigurationSP config =
        new KisFilterConfiguration("solstice_line_art", 1, KisGlobalResourcesInterface::instance());
    config->setProperty("density", m_density->value());
    config->setProperty("paperRadius", m_radius->value());
    config->setProperty("lineColor", m_color->currentData().toString());
    return config;
}
