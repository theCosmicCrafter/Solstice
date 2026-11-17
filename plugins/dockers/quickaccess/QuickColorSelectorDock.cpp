/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickColorSelectorDock.h"

#include <KisColorSelectorConfiguration.h>
#include <KisViewManager.h>
#include <KisVisualColorSelector.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpaceRegistry.h>
#include <kconfiggroup.h>
#include <kis_canvas2.h>
#include <kis_canvas_resource_provider.h>
#include <kis_display_color_converter.h>
#include <kis_icon_utils.h>
#include <kis_image.h>
#include <klocalizedstring.h>
#include <ksharedconfig.h>

#include <QButtonGroup>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPixmap>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace
{
const char ShapeKey[] = "SelectorShape";

/// Tells every HueSVC (docker and popup) that the stored shape changed.
class ShapeNotifier : public QObject
{
    Q_OBJECT
public:
    static ShapeNotifier *instance()
    {
        static ShapeNotifier notifier;
        return &notifier;
    }
Q_SIGNALS:
    void shapeChanged();
};
} // namespace

QuickColorSelectorWidget::Shape QuickColorSelectorWidget::storedShape()
{
    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessHueSVC"));
    return config.readEntry(ShapeKey, QStringLiteral("slider")) == QStringLiteral("ring") ? Shape::Ring : Shape::Slider;
}

KisColorSelectorConfiguration QuickColorSelectorWidget::configuration(Shape shape)
{
    using KCSC = KisColorSelectorConfiguration;
    return shape == Shape::Ring ? KCSC(KCSC::Square, KCSC::Ring, KCSC::SV, KCSC::H)
                                : KCSC(KCSC::Square, KCSC::Slider, KCSC::SV, KCSC::H);
}

QuickColorSelectorWidget::QuickColorSelectorWidget(QWidget *parent)
    : QWidget(parent)
{
    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessHueSVC"));
    m_rgbPercentage = config.readEntry("RgbDisplayMode", QStringLiteral("percentage")) != QStringLiteral("value");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(6);

    // Header: small overlapping swatches like the Wide Gamut Color
    // Selector's (24 px), and the shape button at the right.
    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    auto *swatches = new QWidget(this);
    swatches->setFixedSize(24, 24);
    m_background = new QToolButton(swatches);
    m_foreground = new QToolButton(swatches);
    m_background->setGeometry(8, 8, 16, 16);
    m_foreground->setGeometry(0, 0, 16, 16);
    m_background->setToolTip(i18nc("@info:tooltip", "Background color; click to swap colors"));
    m_foreground->setToolTip(i18nc("@info:tooltip", "Foreground color; click to swap colors"));
    header->addWidget(swatches, 0, Qt::AlignLeft | Qt::AlignTop);
    header->addStretch();

    m_shapeButton = new QToolButton(this);
    m_shapeButton->setIcon(KisIconUtils::loadIcon(QStringLiteral("view-choose")));
    m_shapeButton->setAutoRaise(true);
    m_shapeButton->setPopupMode(QToolButton::InstantPopup);
    m_shapeButton->setToolTip(i18nc("@info:tooltip", "Selector shape"));
    auto *shapeMenu = new QMenu(m_shapeButton);
    auto *shapeRow = new QWidget(shapeMenu);
    auto *shapeLayout = new QHBoxLayout(shapeRow);
    shapeLayout->setContentsMargins(8, 8, 8, 8);
    m_shapeGroup = new QButtonGroup(shapeRow);
    for (Shape shape : {Shape::Slider, Shape::Ring}) {
        auto *button = new QToolButton(shapeRow);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setIconSize(QSize(72, 72));
        button->setToolTip(shape == Shape::Ring ? i18nc("@info:tooltip", "Square inside a hue ring")
                                                : i18nc("@info:tooltip", "Hue bar and square"));
        m_shapeGroup->addButton(button, int(shape));
        shapeLayout->addWidget(button);
    }
    auto *shapeAction = new QWidgetAction(shapeMenu);
    shapeAction->setDefaultWidget(shapeRow);
    shapeMenu->addAction(shapeAction);
    connect(shapeMenu, &QMenu::aboutToShow, this, &QuickColorSelectorWidget::updateShapeIcons);
    connect(m_shapeGroup, &QButtonGroup::idClicked, this, [shapeMenu](int id) {
        shapeMenu->close();
        KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessHueSVC"));
        config.writeEntry(ShapeKey, Shape(id) == Shape::Ring ? QStringLiteral("ring") : QStringLiteral("slider"));
        config.sync();
        Q_EMIT ShapeNotifier::instance()->shapeChanged();
    });
    m_shapeButton->setMenu(shapeMenu);
    header->addWidget(m_shapeButton, 0, Qt::AlignRight | Qt::AlignTop);
    layout->addLayout(header);

    m_selector = new KisVisualColorSelector(this);
    m_selector->setMinimumSize(210, 210);
    m_selector->setMinimumSliderWidth(16);
    // Fill the docker's height: the hue strip and the square take all spare
    // height, the channel rows below keep their natural (fixed) height.
    m_selector->setStretchLimit(100.0);
    m_selector->setSliderPosition(Qt::LeftEdge);
    m_selector->setRenderMode(KisVisualColorSelector::StaticBackground);
    setShape(storedShape());
    connect(ShapeNotifier::instance(), &ShapeNotifier::shapeChanged, this, &QuickColorSelectorWidget::slotShapeChanged);
    layout->addWidget(m_selector, 1);

    auto *channels = new QGridLayout;
    channels->setContentsMargins(0, 0, 0, 0);
    channels->setHorizontalSpacing(4);
    channels->setVerticalSpacing(3);
    const QStringList names{QStringLiteral("H"),
                            QStringLiteral("S"),
                            QStringLiteral("V"),
                            QStringLiteral("R"),
                            QStringLiteral("G"),
                            QStringLiteral("B")};
    QList<QSlider **> sliderTargets{&m_hueSlider,
                                    &m_saturationSlider,
                                    &m_valueSlider,
                                    &m_redSlider,
                                    &m_greenSlider,
                                    &m_blueSlider};
    QList<QSpinBox **> spinTargets{&m_hue, &m_saturation, &m_value, &m_red, &m_green, &m_blue};
    for (int row = 0; row < names.size(); ++row) {
        auto *label = new QLabel(names.at(row), this);
        label->setFixedWidth(fontMetrics().horizontalAdvance(QStringLiteral("W")) + 2);
        auto *slider = new QSlider(Qt::Horizontal, this);
        auto *spin = new QSpinBox(this);
        const bool hue = row == 0;
        const bool rgb = row >= 3;
        const int maximum = hue ? 359 : (rgb && !m_rgbPercentage ? 255 : 100);
        slider->setRange(0, maximum);
        spin->setRange(0, maximum);
        spin->setFixedWidth(72);
        if (hue)
            spin->setSuffix(QStringLiteral("°"));
        else if (!rgb || m_rgbPercentage)
            spin->setSuffix(QStringLiteral("%"));
        *sliderTargets.at(row) = slider;
        *spinTargets.at(row) = spin;
        connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), slider, &QSlider::setValue);
        channels->addWidget(label, row, 0);
        channels->addWidget(slider, row, 1);
        channels->addWidget(spin, row, 2);
    }
    layout->addLayout(channels);

    connect(m_selector,
            &KisVisualColorSelector::sigNewColor,
            this,
            &QuickColorSelectorWidget::slotSelectorColorChanged);
    connect(m_hue, qOverload<int>(&QSpinBox::valueChanged), this, &QuickColorSelectorWidget::slotHsvChanged);
    connect(m_saturation, qOverload<int>(&QSpinBox::valueChanged), this, &QuickColorSelectorWidget::slotHsvChanged);
    connect(m_value, qOverload<int>(&QSpinBox::valueChanged), this, &QuickColorSelectorWidget::slotHsvChanged);
    connect(m_red, qOverload<int>(&QSpinBox::valueChanged), this, &QuickColorSelectorWidget::slotRgbChanged);
    connect(m_green, qOverload<int>(&QSpinBox::valueChanged), this, &QuickColorSelectorWidget::slotRgbChanged);
    connect(m_blue, qOverload<int>(&QSpinBox::valueChanged), this, &QuickColorSelectorWidget::slotRgbChanged);
    connect(m_foreground, &QToolButton::clicked, this, &QuickColorSelectorWidget::slotSwapColors);
    connect(m_background, &QToolButton::clicked, this, &QuickColorSelectorWidget::slotSwapColors);
    setEnabled(false);
}

void QuickColorSelectorWidget::setCanvas(KisCanvas2 *canvas)
{
    if (m_resourceProvider)
        disconnect(m_resourceProvider, nullptr, this, nullptr);
    for (const QMetaObject::Connection &connection : std::as_const(m_colorSpaceConnections))
        disconnect(connection);
    m_colorSpaceConnections.clear();
    m_canvas = canvas;
    m_resourceProvider =
        m_canvas && m_canvas->viewManager() ? m_canvas->viewManager()->canvasResourceProvider() : nullptr;
    setEnabled(m_resourceProvider);
    if (!m_resourceProvider) {
        m_selector->setDisplayRenderer(nullptr);
        return;
    }
    m_selector->setDisplayRenderer(m_canvas->displayColorConverter()->displayRendererInterface());
    // The converter reports current-node and node color space changes; the
    // image's own signal covers the fallback of selectorColorSpace().
    m_colorSpaceConnections.append(connect(m_canvas->displayColorConverter(),
                                           &KisDisplayColorConverter::displayConfigurationChanged,
                                           this,
                                           &QuickColorSelectorWidget::slotUpdateColorSpace));
    if (KisImageSP image = m_canvas->image()) {
        m_colorSpaceConnections.append(connect(image.data(),
                                               &KisImage::sigColorSpaceChanged,
                                               this,
                                               &QuickColorSelectorWidget::slotUpdateColorSpace));
        m_colorSpaceConnections.append(
            connect(image.data(), &KisImage::sigProfileChanged, this, &QuickColorSelectorWidget::slotUpdateColorSpace));
    }
    slotUpdateColorSpace();
    connect(m_resourceProvider,
            &KisCanvasResourceProvider::sigFGColorChanged,
            this,
            &QuickColorSelectorWidget::slotCanvasColorChanged);
    connect(m_resourceProvider, &KisCanvasResourceProvider::sigBGColorChanged, this, [this](const KoColor &) {
        updateSwatches();
    });
    slotCanvasColorChanged(m_resourceProvider->fgColor());
}

const KoColorSpace *QuickColorSelectorWidget::selectorColorSpace() const
{
    // HueSVC picks hue, saturation and value. A non-RGB node, such as a filter
    // or fill layer or a mask (their devices are GrayA selections) or a gray
    // layer, would reduce KisVisualColorSelector to a single lightness bar.
    const KoColorSpace *node = m_canvas->displayColorConverter()->nodeColorSpace();
    if (node && node->colorModelId() == RGBAColorModelID)
        return node;
    const KisImageSP image = m_canvas->image();
    if (image && image->colorSpace()->colorModelId() == RGBAColorModelID)
        return image->colorSpace();
    return KoColorSpaceRegistry::instance()->rgb8();
}

void QuickColorSelectorWidget::slotUpdateColorSpace()
{
    if (!m_canvas || !m_resourceProvider)
        return;
    const KoColorSpace *colorSpace = selectorColorSpace();
    if (colorSpace == m_colorSpace)
        return;
    m_colorSpace = colorSpace;
    m_selector->slotSetColorSpace(colorSpace);
    // A new color space resets the selector's color to black.
    slotCanvasColorChanged(m_resourceProvider->fgColor());
}

void QuickColorSelectorWidget::slotSelectorColorChanged(const KoColor &color)
{
    if (m_syncing || !m_resourceProvider)
        return;
    m_resourceProvider->setFGColor(color);
    updateRgb(color);
    updateSwatches();
}

void QuickColorSelectorWidget::slotCanvasColorChanged(const KoColor &color)
{
    m_syncing = true;
    m_selector->slotSetColor(color);
    updateRgb(color);
    updateSwatches();
    m_syncing = false;
}

void QuickColorSelectorWidget::slotRgbChanged()
{
    if (m_syncing || !m_resourceProvider)
        return;
    const KoColor current = m_resourceProvider->fgColor();
    const auto channel = [this](int value) {
        return m_rgbPercentage ? qRound(value * 255.0 / 100.0) : value;
    };
    const KoColor color(QColor(channel(m_red->value()), channel(m_green->value()), channel(m_blue->value())),
                        current.colorSpace());
    m_resourceProvider->setFGColor(color);
}

void QuickColorSelectorWidget::slotHsvChanged()
{
    if (m_syncing || !m_resourceProvider)
        return;
    const QColor rgb = QColor::fromHsv(m_hue->value(),
                                       qRound(m_saturation->value() * 255.0 / 100.0),
                                       qRound(m_value->value() * 255.0 / 100.0));
    const KoColor current = m_resourceProvider->fgColor();
    m_resourceProvider->setFGColor(KoColor(rgb, current.colorSpace()));
}

void QuickColorSelectorWidget::slotSwapColors()
{
    if (!m_resourceProvider)
        return;
    const KoColor foreground = m_resourceProvider->fgColor();
    const KoColor background = m_resourceProvider->bgColor();
    m_resourceProvider->setFGColor(background);
    m_resourceProvider->setBGColor(foreground);
}

void QuickColorSelectorWidget::slotShapeChanged()
{
    setShape(storedShape());
}

void QuickColorSelectorWidget::setShape(Shape shape)
{
    const KisColorSelectorConfiguration config = configuration(shape);
    m_selector->setConfiguration(&config);
    if (QAbstractButton *button = m_shapeGroup->button(int(shape)))
        button->setChecked(true);
}

void QuickColorSelectorWidget::updateShapeIcons()
{
    // Rendered previews, as the Wide Gamut Color Selector's shape grid draws
    // them (WGSelectorConfigGrid::generateIcon()).
    const qreal ratio = devicePixelRatioF();
    const int size = 72;
    // A hidden child needs a color space and a color to draw anything.
    const KoColorSpace *rgb = KoColorSpaceRegistry::instance()->rgb8();
    KisVisualColorSelector preview(this);
    preview.setVisible(false);
    preview.setEnabled(false);
    preview.setMinimumSliderWidth(10);
    preview.setSliderPosition(Qt::LeftEdge);
    preview.setGeometry(0, 0, size, size);
    preview.slotSetColorSpace(rgb);
    preview.slotSetColor(KoColor(QColor(255, 0, 0), rgb));
    for (Shape shape : {Shape::Slider, Shape::Ring}) {
        const KisColorSelectorConfiguration config = configuration(shape);
        preview.setConfiguration(&config);
        QPixmap pixmap(QSize(size, size) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        preview.render(&pixmap, QPoint(), QRegion(), RenderFlag::DrawChildren);
        m_shapeGroup->button(int(shape))->setIcon(QIcon(pixmap));
    }
}

void QuickColorSelectorWidget::updateSwatches()
{
    if (!m_resourceProvider)
        return;
    const QColor foreground = m_resourceProvider->fgColor().toQColor();
    const QColor background = m_resourceProvider->bgColor().toQColor();
    m_foreground->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid palette(highlight);")
                                    .arg(foreground.name(QColor::HexArgb)));
    m_background->setStyleSheet(
        QStringLiteral("background-color: %1; border: 1px solid palette(mid);").arg(background.name(QColor::HexArgb)));
    m_foreground->setToolTip(i18nc("@info:tooltip", "Foreground color"));
    m_background->setToolTip(i18nc("@info:tooltip", "Background color"));
}

void QuickColorSelectorWidget::updateRgb(const KoColor &color)
{
    const QColor rgb = color.toQColor();
    const QSignalBlocker hueBlocker(m_hue);
    const QSignalBlocker saturationBlocker(m_saturation);
    const QSignalBlocker valueBlocker(m_value);
    const QSignalBlocker hueSliderBlocker(m_hueSlider);
    const QSignalBlocker saturationSliderBlocker(m_saturationSlider);
    const QSignalBlocker valueSliderBlocker(m_valueSlider);
    const QSignalBlocker redBlocker(m_red);
    const QSignalBlocker greenBlocker(m_green);
    const QSignalBlocker blueBlocker(m_blue);
    const QSignalBlocker redSliderBlocker(m_redSlider);
    const QSignalBlocker greenSliderBlocker(m_greenSlider);
    const QSignalBlocker blueSliderBlocker(m_blueSlider);
    const auto channel = [this](int value) {
        return m_rgbPercentage ? qRound(value * 100.0 / 255.0) : value;
    };
    const int hue = qMax(0, rgb.hsvHue());
    const int saturation = qRound(rgb.hsvSaturation() * 100.0 / 255.0);
    const int value = qRound(rgb.value() * 100.0 / 255.0);
    const int red = channel(rgb.red());
    const int green = channel(rgb.green());
    const int blue = channel(rgb.blue());
    m_hue->setValue(hue);
    m_hueSlider->setValue(hue);
    m_saturation->setValue(saturation);
    m_saturationSlider->setValue(saturation);
    m_value->setValue(value);
    m_valueSlider->setValue(value);
    m_red->setValue(red);
    m_redSlider->setValue(red);
    m_green->setValue(green);
    m_greenSlider->setValue(green);
    m_blue->setValue(blue);
    m_blueSlider->setValue(blue);
    updateChannelGradients(rgb);
}

void QuickColorSelectorWidget::updateChannelGradients(const QColor &color)
{
    const QString handle = QStringLiteral(
        " QSlider::handle:horizontal { width: 9px; margin: -3px 0; background: palette(button-text); "
        "border: 1px solid palette(base); }");
    const auto style = [&handle](const QString &gradient) -> QString {
        return QStringLiteral(
                   "QSlider::groove:horizontal { height: 9px; border: 1px solid palette(mid); "
                   "background: %1; }")
                   .arg(gradient)
            + handle;
    };
    m_hueSlider->setStyleSheet(style(
        QStringLiteral("qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #ff0000, stop:.166 #ffff00, stop:.333 #00ff00, "
                       "stop:.5 #00ffff, stop:.666 #0000ff, stop:.833 #ff00ff, stop:1 #ff0000)")));
    const int hue = qMax(0, color.hsvHue());
    const QColor saturationStart = QColor::fromHsv(hue, 0, color.value());
    const QColor saturationEnd = QColor::fromHsv(hue, 255, color.value());
    const QColor valueEnd = QColor::fromHsv(hue, color.hsvSaturation(), 255);
    m_saturationSlider->setStyleSheet(style(QStringLiteral("qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 %1, stop:1 %2)")
                                                .arg(saturationStart.name(), saturationEnd.name())));
    m_valueSlider->setStyleSheet(
        style(QStringLiteral("qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #000000, stop:1 %1)").arg(valueEnd.name())));
    m_redSlider->setStyleSheet(style(
        QStringLiteral("qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 %1, stop:1 %2)")
            .arg(QColor(0, color.green(), color.blue()).name(), QColor(255, color.green(), color.blue()).name())));
    m_greenSlider->setStyleSheet(
        style(QStringLiteral("qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 %1, stop:1 %2)")
                  .arg(QColor(color.red(), 0, color.blue()).name(), QColor(color.red(), 255, color.blue()).name())));
    m_blueSlider->setStyleSheet(
        style(QStringLiteral("qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 %1, stop:1 %2)")
                  .arg(QColor(color.red(), color.green(), 0).name(), QColor(color.red(), color.green(), 255).name())));
}

QuickColorSelectorDock::QuickColorSelectorDock(QWidget *parent)
    : QDockWidget(parent)
    , m_selector(new QuickColorSelectorWidget(this))
{
    setWindowTitle(i18nc("@title:window", "HueSVC"));
    setWidget(m_selector);
}

QString QuickColorSelectorDock::observerName()
{
    return QStringLiteral("QuickColorSelectorDock");
}

void QuickColorSelectorDock::setCanvas(KoCanvasBase *canvas)
{
    m_selector->setCanvas(dynamic_cast<KisCanvas2 *>(canvas));
}

void QuickColorSelectorDock::unsetCanvas()
{
    m_selector->setCanvas(nullptr);
}

QString QuickColorSelectorDockFactory::id() const
{
    return QStringLiteral("HueSVC");
}

QDockWidget *QuickColorSelectorDockFactory::createDockWidget()
{
    auto *dock = new QuickColorSelectorDock;
    dock->setObjectName(id());
    return dock;
}

KoDockFactoryBase::DockPosition QuickColorSelectorDockFactory::defaultDockPosition() const
{
    return DockRight;
}

#include "QuickColorSelectorDock.moc"
