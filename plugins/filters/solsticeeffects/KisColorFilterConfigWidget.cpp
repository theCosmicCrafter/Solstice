/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisColorFilterConfigWidget.h"
#include "KisColorFilter.h"
#include <KisGlobalResourcesInterface.h>
#include <KisMainWindow.h>
#include <KisView.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpaceRegistry.h>
#include <QButtonGroup>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <filter/kis_filter_configuration.h>
#include <kis_default_bounds.h>
#include <kis_image.h>
#include <kis_node.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>
namespace
{
KisPaintDeviceSP sample()
{
    QImage image(96, 72, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor::fromHsvF(double(x) / 96, .6, .25 + .75 * y / 71));
    QPainter p(&image);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(216, 158, 117));
    p.drawEllipse(29, 10, 35, 47);
    p.setBrush(QColor(57, 38, 30));
    p.drawEllipse(28, 7, 37, 19);
    p.setBrush(QColor(60, 70, 135));
    p.drawEllipse(20, 52, 56, 37);
    p.end();
    KisPaintDeviceSP dev = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    dev->convertFromQImage(image, nullptr);
    return dev;
}
} // namespace
KisColorFilterConfigWidget::KisColorFilterConfigWidget(QWidget *parent, KisPaintDeviceSP device, bool useForMasks)
    : KisConfigWidget(parent)
    , m_presets(new QButtonGroup(this))
    , m_presetLabel(new QLabel(this))
{
    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    tabs->setObjectName("colorFilterTabs");
    auto *look = new QWidget(tabs), *adjust = new QWidget(tabs);
    tabs->addTab(look, i18n("Look"));
    tabs->addTab(adjust, i18n("Adjust"));
    layout->addWidget(tabs);
    auto *lookLayout = new QVBoxLayout(look);
    auto *grid = new QGridLayout();
    const auto names = KisColorFilter::presetNames();
    for (int i = 0; i < names.size(); ++i) {
        auto *button = new QToolButton(look);
        button->setObjectName(QString("colorLook%1").arg(i));
        button->setText(names[i]);
        button->setToolTip(names[i]);
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(QSize(80, 60));
        button->setMinimumSize(96, 86);
        m_presets->addButton(button, i);
        grid->addWidget(button, i / 3, i % 3);
    }
    lookLayout->addLayout(grid);
    lookLayout->addWidget(m_presetLabel);
    auto *lookForm = new QFormLayout();
    lookLayout->addLayout(lookForm);
    auto *adjustForm = new QFormLayout(adjust);
    const auto slider = [&](QFormLayout *form, const QString &key, const QString &label, int low, int high) {
        auto *control = new KisDoubleSliderSpinBox(this);
        control->setObjectName(key);
        control->setRange(low, high, 1);
        m_controls.insert(key, control);
        form->addRow(label, control);
        connect(control, &KisDoubleSliderSpinBox::valueChanged, this, [this, key](double v) {
            if (m_loading)
                return;
            m_values->setProperty(key, v);
            m_values->setProperty("preset", -1);
            updatePresetLabel();
            Q_EMIT sigConfigurationItemChanged();
        });
    };
    slider(lookForm, "amount", i18n("Amount:"), 0, 100);
    slider(adjustForm, "adjustBrightness", i18n("Brightness:"), -100, 100);
    slider(adjustForm, "adjustContrast", i18n("Contrast:"), -100, 100);
    slider(adjustForm, "adjustHue", i18n("Hue (degrees):"), -180, 180);
    slider(adjustForm, "adjustSaturation", i18n("Saturation:"), -100, 100);
    slider(adjustForm, "adjustTemperature", i18n("Color temperature:"), -100, 100);
    slider(adjustForm, "adjustTint", i18n("Tint:"), -100, 100);
    slider(adjustForm, "adjustSharpness", i18n("Sharpness:"), 0, 100);
    connect(m_presets, &QButtonGroup::idClicked, this, [this](int preset) {
        KisColorFilter::applyPreset(m_values, preset);
        updatePresetLabel();
        Q_EMIT sigConfigurationItemChanged();
    });
    auto *reset = new QPushButton(i18n("Reset Settings"), this);
    reset->setObjectName("colorFilterReset");
    layout->addWidget(reset);
    connect(reset, &QPushButton::clicked, this, [this] {
        setConfiguration(KisColorFilter().defaultConfiguration(KisGlobalResourcesInterface::instance()));
        Q_EMIT sigConfigurationItemChanged();
    });
    const bool gray = device && device->colorSpace()->colorModelId() == GrayAColorModelID;
    if (gray || (device && device->colorSpace()->colorModelId() != RGBAColorModelID)) {
        auto *notice = new QLabel(
            gray ? i18n("Gray images use Brightness, Contrast and Sharpness only. Convert to RGB to use colored looks.")
                 : i18n("This color model is processed through floating-point RGB and converted back; appearance may "
                        "change."),
            this);
        notice->setWordWrap(true);
        layout->addWidget(notice);
    }
    if (gray) {
        look->setEnabled(false);
        for (const QString &key : {"adjustHue", "adjustSaturation", "adjustTemperature", "adjustTint"})
            m_controls[key]->setEnabled(false);
        tabs->setCurrentIndex(1);
    }
    setConfiguration(KisColorFilter().defaultConfiguration(KisGlobalResourcesInterface::instance()));

    // Snapshot only while the image is idle. No waiting for an active stroke;
    // masks, detached devices and busy images use the explicit sample fallback.
    KisPaintDeviceSP snapshot;
    QRect bounds;
    KisImageSP image;
    // Resolve the owning window, never a global active view from another document.
    for (QWidget *ancestor = parent; ancestor; ancestor = ancestor->parentWidget()) {
        auto *window = qobject_cast<KisMainWindow *>(ancestor);
        if (!window || !window->activeView())
            continue;
        auto *view = window->activeView();
        const auto node = view->currentNode();
        if (device && node && node->paintDevice() == device)
            image = view->image();
        break;
    }
    if (!useForMasks && image && image->tryBarrierLock(true)) {
        try {
            bounds = image->bounds();
            if (!device->extent().intersected(bounds).isEmpty()) {
                snapshot = new KisPaintDevice(*device);
                // CopySnapshot holds the current full-resolution frame. Detach
                // its LOD/time bounds before queuing work or closing the view.
                snapshot->setDefaultBounds(new KisDefaultBounds());
            }
        } catch (...) {
            image->unlock();
            throw;
        }
        image->unlock();
    }
    auto *caption = new QLabel(snapshot ? i18n("Look previews: layer snapshot, Amount 100, neutral Adjust.")
                                        : i18n("Look previews: sample image, Amount 100, neutral Adjust."),
                               look);
    caption->setWordWrap(true);
    lookLayout->addWidget(caption);
    QTimer::singleShot(0, this, [this, snapshot, bounds] {
        if (snapshot) {
            QSize size = bounds.size();
            if (size.width() > 96 || size.height() > 72)
                size.scale(QSize(96, 72), Qt::KeepAspectRatio);
            size = size.expandedTo(QSize(1, 1));
            m_thumbnailRect = QRect(QPoint(), size);
            m_thumbnail = snapshot->createThumbnailDevice(size.width(), size.height(), bounds, m_thumbnailRect);
        } else {
            m_thumbnail = sample();
            m_thumbnailRect = QRect(0, 0, 96, 72);
        }
        renderThumbnail(0);
    });
}
void KisColorFilterConfigWidget::renderThumbnail(int index)
{
    KisColorFilter filter;
    auto config = filter.defaultConfiguration(KisGlobalResourcesInterface::instance());
    KisColorFilter::applyPreset(config, index);
    KisPaintDeviceSP preview = new KisPaintDevice(*m_thumbnail);
    filter.processImpl(preview, m_thumbnailRect, config, nullptr);
    const QImage image = preview->convertToQImage(KoColorSpaceRegistry::instance()->rgb8()->profile(), m_thumbnailRect);
    m_presets->button(index)->setIcon(QPixmap::fromImage(image));
    if (index + 1 < m_presets->buttons().size())
        QTimer::singleShot(0, this, [this, index] {
            renderThumbnail(index + 1);
        });
}
void KisColorFilterConfigWidget::updatePresetLabel()
{
    const int preset = m_values->getInt("preset", 0);
    m_presets->setExclusive(false);
    for (auto *button : m_presets->buttons())
        button->setChecked(m_presets->id(button) == preset);
    m_presets->setExclusive(true);
    const auto names = KisColorFilter::presetNames();
    m_presetLabel->setText(preset >= 0 && preset < names.size() ? names[preset] : i18n("Custom"));
}
void KisColorFilterConfigWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    m_loading = true;
    m_values = KisColorFilter().defaultConfiguration(KisGlobalResourcesInterface::instance());
    const auto properties = config->getProperties();
    for (auto it = properties.cbegin(); it != properties.cend(); ++it)
        m_values->setProperty(it.key(), it.value());
    for (auto it = m_controls.cbegin(); it != m_controls.cend(); ++it)
        it.value()->setValue(m_values->getDouble(it.key()));
    updatePresetLabel();
    m_loading = false;
}
KisPropertiesConfigurationSP KisColorFilterConfigWidget::configuration() const
{
    KisFilterConfigurationSP config =
        new KisFilterConfiguration("solstice_color_filter", 1, KisGlobalResourcesInterface::instance());
    const auto properties = m_values->getProperties();
    for (auto it = properties.cbegin(); it != properties.cend(); ++it)
        config->setProperty(it.key(), it.value());
    return config;
}
