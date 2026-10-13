/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAdjustDock.h"

#include "QuickAdjustKeyController.h"

#include <KisAngleSelector.h>
#include <KisMainWindow.h>
#include <KisViewManager.h>
#include <KoColorSpace.h>
#include <KoCompositeOpRegistry.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kconfiggroup.h>
#include <kis_action.h>
#include <kis_action_manager.h>
#include <kis_canvas2.h>
#include <kis_canvas_resource_provider.h>
#include <kis_config.h>
#include <kis_image.h>
#include <kis_image_config.h>
#include <kis_node.h>
#include <kis_node_manager.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>
#include <ksharedconfig.h>

#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
bool colorHistoryResetForSession = false;
}

namespace
{
const QStringList DefaultBlendModes{
    COMPOSITE_OVER,
    COMPOSITE_MULT,
    COMPOSITE_SCREEN,
    COMPOSITE_DODGE,
    COMPOSITE_OVERLAY,
    COMPOSITE_SOFT_LIGHT_SVG,
    COMPOSITE_HARD_LIGHT,
    COMPOSITE_DARKEN,
    COMPOSITE_LIGHTEN,
    COMPOSITE_GREATER,
};

void selectBlendMode(QComboBox *combo, const QString &id)
{
    int index = combo->findData(id);
    if (index < 0 && !id.isEmpty()) {
        const KoID op = KoCompositeOpRegistry::instance().getKoID(id);
        combo->addItem(op.name().isEmpty() ? id : op.name(), id);
        index = combo->count() - 1;
    }
    if (index >= 0)
        combo->setCurrentIndex(index);
}
} // namespace

QuickAdjustDock::QuickAdjustDock(QWidget *parent, bool compactPopup)
    : QDockWidget(parent)
    , m_compactPopup(compactPopup)
{
    if (!compactPopup)
        new QuickAdjustKeyController(this);
    const KConfigGroup adjustConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));
    setWindowTitle(i18nc("@title:window", "Quick Brush Adjustments"));
    setMinimumSize(100, 100);

    auto *root = new QWidget(this);
    auto *outerLayout = new QHBoxLayout(root);
    outerLayout->setContentsMargins(5, 5, 5, 5);
    outerLayout->setSpacing(4);
    auto *content = new QWidget(root);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *brushGroup = new QWidget(root);
    auto *brushLayout = new QVBoxLayout(brushGroup);
    brushLayout->setContentsMargins(0, 0, 0, 0);
    brushLayout->setSpacing(4);
    // the same controls as Tool Options: the brush tip's diameter slider,
    // value bars and the angle selector
    m_brushSize = new KisDoubleSliderSpinBox(this);
    m_brushSize->setRange(1.0, KisImageConfig(true).maxBrushSize(), 2);
    m_brushSize->setExponentRatio(3.0);
    m_brushSize->setSingleStep(1);
    m_brushSize->setPrefix(i18nc("@label:slider, followed by the brush size", "Size: "));
    m_brushSize->setSuffix(i18n(" px"));
    m_brushOpacity = createPercentSlider(i18nc("@label:slider, followed by a percentage", "Opacity: "));
    m_brushFlow = createPercentSlider(i18nc("@label:slider, followed by a percentage", "Flow: "));
    m_brushRotationRow = new QWidget(this);
    auto *rotationLayout = new QHBoxLayout(m_brushRotationRow);
    rotationLayout->setContentsMargins(0, 0, 0, 0);
    rotationLayout->setSpacing(4);
    m_brushRotation = new KisAngleSelector(m_brushRotationRow);
    m_brushRotation->setRange(0.0, 360.0);
    m_brushRotation->setDecimals(0);
    // one flip menu button instead of three keeps the row within the popup's
    // width
    m_brushRotation->setFlipOptionsMode(KisAngleSelector::FlipOptionsMode_MenuButton);
    m_brushRotation->setToolTip(i18nc("@info:tooltip", "Brush rotation"));
    rotationLayout->addWidget(m_brushRotation, 1);
    m_brushSize->setVisible(adjustConfig.readEntry("SizeSliderEnabled", true));
    m_brushOpacity->setVisible(adjustConfig.readEntry("OpacitySliderEnabled", true));
    m_brushFlow->setVisible(adjustConfig.readEntry("FlowSliderEnabled", true));

    m_brushBlend = new QComboBox(brushGroup);
    populateBlendModes(m_brushBlend);
    auto *reset = new QPushButton(QIcon(QStringLiteral(":/quickaccess/system_icons/reset.png")), QString(), brushGroup);
    reset->setToolTip(i18nc("@info:tooltip", "Reload the current brush preset"));
    reset->setFixedSize(24, 24);
    auto *layerGroup = new QWidget(root);
    auto *layerLayout = new QVBoxLayout(layerGroup);
    layerLayout->setContentsMargins(0, 0, 0, 0);
    layerLayout->setSpacing(4);
    m_layerOpacity = createPercentSlider(i18nc("@label:slider, followed by a percentage", "Layer Opacity: "));
    m_layerOpacity->setVisible(adjustConfig.readEntry("LayerOpacitySliderEnabled", true));
    m_layerBlend = new QComboBox(layerGroup);
    populateBlendModes(m_layerBlend);
    if (compactPopup) {
        // the docker's brush and layer columns are not used here; left shown,
        // they would cover the top of the panel and catch its mouse events
        brushGroup->hide();
        layerGroup->hide();
        layout->addWidget(m_brushSize);
        layout->addWidget(m_brushOpacity);
        layout->addWidget(m_brushFlow);
        layout->addWidget(m_brushBlend);
        rotationLayout->addWidget(reset);
        layout->addWidget(m_brushRotationRow);
        layout->addWidget(m_layerOpacity);
        layout->addWidget(m_layerBlend);
    } else {
        layout->addWidget(m_brushSize);
        brushLayout->addWidget(m_brushOpacity);
        brushLayout->addWidget(m_brushFlow);
        auto *brushFooter = new QHBoxLayout;
        brushFooter->addWidget(m_brushBlend, 1);
        brushFooter->addWidget(reset);
        brushLayout->addLayout(brushFooter);
        layerLayout->addWidget(m_layerOpacity);
        layerLayout->addWidget(m_layerBlend);
        auto *brushAndLayer = new QHBoxLayout;
        brushAndLayer->setSpacing(8);
        brushAndLayer->addWidget(brushGroup, 1);
        brushAndLayer->addWidget(layerGroup, 1);
        layout->addLayout(brushAndLayer);
        layout->addWidget(m_brushRotationRow);
    }
    m_colorHistoryGroup = createColorHistoryWidget();
    m_brushHistoryGroup = createBrushHistoryWidget();
    m_colorHistoryGroup->setVisible(adjustConfig.readEntry("ColorHistoryEnabled", true));
    m_brushHistoryGroup->setVisible(adjustConfig.readEntry("BrushHistoryEnabled", true));
    layout->addWidget(m_colorHistoryGroup);
    layout->addWidget(m_brushHistoryGroup);
    layout->addStretch();
    outerLayout->addWidget(content, 1);

    QVBoxLayout *statusLayout = nullptr;
    if (!compactPopup) {
        statusLayout = new QVBoxLayout;
        statusLayout->setContentsMargins(0, 0, 0, 0);
        statusLayout->setSpacing(2);
    }
    m_toolOptionsToggle =
        createStatusButton(i18nc("@info:tooltip", "Show Tool Options"), QStringLiteral("tool_options"));
    m_toolOptionsToggle->setVisible(!compactPopup && adjustConfig.readEntry("ToolOptionsEnabled", false));
    m_eraseToggle = createStatusButton(i18nc("@info:tooltip", "Toggle eraser mode"), QStringLiteral("erase_mode"));
    m_alphaToggle =
        createStatusButton(i18nc("@info:tooltip", "Toggle preserve alpha"), QStringLiteral("preserve_alpha"));
    m_selectionToggle =
        createStatusButton(i18nc("@info:tooltip", "Toggle selection outline"), QStringLiteral("selection"));
    m_gestureToggle =
        createStatusButton(i18nc("@info:tooltip", "Toggle Quick Access gestures"), QStringLiteral("gesture"));
    for (QToolButton *button :
         {m_toolOptionsToggle, m_eraseToggle, m_alphaToggle, m_selectionToggle, m_gestureToggle}) {
        if (compactPopup) {
            button->hide();
            continue;
        }
        statusLayout->addWidget(button, 0, Qt::AlignHCenter);
        if (button != m_gestureToggle) {
            auto *line = new QFrame(root);
            line->setFrameShape(QFrame::HLine);
            line->setFixedWidth(14);
            statusLayout->addWidget(line, 0, Qt::AlignHCenter);
        }
    }
    if (statusLayout) {
        statusLayout->addStretch();
        outerLayout->addLayout(statusLayout);
    }
    setWidget(root);

    if (compactPopup) {
        m_colorHistoryGroup->hide();
        m_brushHistoryGroup->hide();
        layout->insertWidget(layout->count() - 1, createBrushToggleWidget());
    } else {
        m_brushRotationRow->hide();
    }
    bool fontOk = false;
    const int configuredFontSize = adjustConfig.readEntry("FontSize", QStringLiteral("12px"))
                                       .remove(QStringLiteral("px"), Qt::CaseInsensitive)
                                       .toInt(&fontOk);
    if (fontOk)
        content->setStyleSheet(QStringLiteral("font-size: %1px;").arg(qBound(6, configuredFontSize, 36)));
    m_toolOptionsToggle->setChecked(adjustConfig.readEntry("ToolOptionsStartVisible", false));
    connect(m_toolOptionsToggle, &QToolButton::toggled, this, &QuickAdjustDock::setToolOptionsPadVisible);
    setToolOptionsPadVisible(m_toolOptionsToggle->isChecked());
    connect(m_eraseToggle, &QToolButton::clicked, this, [this] {
        if (m_resourceProvider)
            m_resourceProvider->setEraserMode(!m_resourceProvider->eraserMode());
    });
    connect(m_alphaToggle, &QToolButton::clicked, this, [this] {
        if (m_resourceProvider)
            m_resourceProvider->setGlobalAlphaLock(!m_resourceProvider->globalAlphaLock());
    });
    m_selectionToggle->setCheckable(false);
    connect(m_gestureToggle, &QToolButton::clicked, this, [this] {
        triggerAction(QStringLiteral("toggle_gesture_recognition"));
    });

    connect(m_brushSize,
            qOverload<double>(&KisDoubleSliderSpinBox::valueChanged),
            this,
            &QuickAdjustDock::slotBrushSizeChanged);
    connect(m_brushOpacity,
            qOverload<int>(&KisSliderSpinBox::valueChanged),
            this,
            &QuickAdjustDock::slotBrushOpacityChanged);
    connect(m_brushFlow, qOverload<int>(&KisSliderSpinBox::valueChanged), this, &QuickAdjustDock::slotBrushFlowChanged);
    connect(m_brushRotation, &KisAngleSelector::angleChanged, this, &QuickAdjustDock::slotBrushRotationChanged);
    connect(m_brushBlend,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            &QuickAdjustDock::slotBrushBlendChanged);
    connect(m_layerOpacity,
            qOverload<int>(&KisSliderSpinBox::valueChanged),
            this,
            &QuickAdjustDock::slotLayerOpacityChanged);
    connect(m_layerBlend,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            &QuickAdjustDock::slotLayerBlendChanged);
    connect(reset, &QPushButton::clicked, this, &QuickAdjustDock::slotResetBrush);

    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(200);
    connect(m_syncTimer, &QTimer::timeout, this, &QuickAdjustDock::syncFromCanvas);
    m_syncTimer->start();
    setControlsEnabled(false);
}

QuickAdjustDock::~QuickAdjustDock()
{
    returnToolOptionsDocker();
}

QString QuickAdjustDock::observerName()
{
    return QStringLiteral("QuickAdjustDock");
}

void QuickAdjustDock::setCanvas(KoCanvasBase *canvas)
{
    if (m_resourceProvider)
        disconnect(m_resourceProvider, nullptr, this, nullptr);
    m_canvas = dynamic_cast<KisCanvas2 *>(canvas);
    m_resourceProvider =
        m_canvas && m_canvas->viewManager() ? m_canvas->viewManager()->canvasResourceProvider() : nullptr;
    if (m_resourceProvider) {
        ensureToolOptionsPad();
        if (!colorHistoryResetForSession) {
            m_resourceProvider->setColorHistory({});
            colorHistoryResetForSession = true;
        }
        connect(m_resourceProvider,
                &KisCanvasResourceProvider::sigFGColorUsed,
                this,
                &QuickAdjustDock::slotForegroundColorChanged);
        connect(m_resourceProvider,
                &KisCanvasResourceProvider::sigPaintOpPresetChanged,
                this,
                &QuickAdjustDock::slotBrushPresetChanged);
    }
    setControlsEnabled(m_canvas);
    loadColorHistory();
    slotBrushPresetChanged(m_resourceProvider ? m_resourceProvider->currentPreset() : KisPaintOpPresetSP());
    syncFromCanvas();
}

void QuickAdjustDock::unsetCanvas()
{
    if (m_resourceProvider)
        disconnect(m_resourceProvider, nullptr, this, nullptr);
    m_resourceProvider = nullptr;
    m_canvas.clear();
    m_colorHistory.clear();
    updateColorHistoryButtons();
    updateBrushHistoryButtons();
    setControlsEnabled(false);
}

KisSliderSpinBox *QuickAdjustDock::createPercentSlider(const QString &prefix)
{
    auto *slider = new KisSliderSpinBox(this);
    // the width comes from the layout, not the text, so that the brush and
    // layer columns of the docker are equally wide
    slider->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    slider->setRange(0, 100);
    slider->setPrefix(prefix);
    slider->setSuffix(QStringLiteral("%"));
    return slider;
}

void QuickAdjustDock::populateBlendModes(QComboBox *combo)
{
    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));
    const QStringList configured = config.readEntry("BlendModes", QStringList());
    const QStringList modes = configured.isEmpty() ? DefaultBlendModes : configured;
    for (const QString &id : modes) {
        const KoID op = KoCompositeOpRegistry::instance().getKoID(id);
        combo->addItem(op.name().isEmpty() ? id : op.name(), id);
    }
    combo->setMinimumWidth(72);
    combo->setMinimumContentsLength(8);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}

void QuickAdjustDock::ensureToolOptionsPad()
{
    if (m_compactPopup || m_toolOptionsPad || !m_canvas || !m_canvas->viewManager()
        || !m_toolOptionsToggle->isVisible())
        return;
    if (!KisConfig(true).toolOptionsInDocker()) {
        m_toolOptionsToggle->hide();
        return;
    }

    QWidget *mainWindow = m_canvas->viewManager()->mainWindow();
    QDockWidget *docker =
        mainWindow ? mainWindow->findChild<QDockWidget *>(QStringLiteral("sharedtooldocker")) : nullptr;
    if (!docker || !docker->widget()) {
        m_toolOptionsToggle->hide();
        return;
    }

    m_toolOptionsDocker = docker;
    m_toolOptionsDockerWasVisible = docker->isVisible();
    m_borrowedToolOptions = docker->widget();
    m_toolOptionsPlaceholder = new QLabel(i18nc("@info", "Tool Options are open beside Quick Brush Adjustments."));
    m_toolOptionsPlaceholder->hide();

    m_toolOptionsPad = new QWidget(mainWindow, Qt::Tool | Qt::FramelessWindowHint);
    m_toolOptionsPad->setObjectName(QStringLiteral("quickAccessToolOptionsPad"));
    m_toolOptionsPad->setAutoFillBackground(true);
    auto *padLayout = new QVBoxLayout(m_toolOptionsPad);
    padLayout->setContentsMargins(4, 4, 4, 4);
    padLayout->addWidget(m_borrowedToolOptions);
    docker->setWidget(m_toolOptionsPlaceholder);
    docker->hide();
    docker->toggleViewAction()->setEnabled(false);

    padLayout->invalidate();
    padLayout->activate();
    resizeToolOptionsPad();
    setToolOptionsPadVisible(m_toolOptionsToggle->isChecked());

    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (!m_toolOptionsPad)
            return;
        if (state != Qt::ApplicationActive) {
            m_toolOptionsPad->hide();
        } else if (m_toolOptionsToggle && m_toolOptionsToggle->isChecked() && isVisible()) {
            resizeToolOptionsPad();
            m_toolOptionsPad->show();
            positionToolOptionsPad();
        }
    });
}

void QuickAdjustDock::setToolOptionsPadVisible(bool visible)
{
    if (!m_compactPopup && visible && !m_toolOptionsPad)
        ensureToolOptionsPad();
    if (m_toolOptionsPad) {
        resizeToolOptionsPad();
        m_toolOptionsPad->setVisible(visible && isVisible()
                                     && QGuiApplication::applicationState() == Qt::ApplicationActive);
        if (visible) {
            positionToolOptionsPad();
            QTimer::singleShot(0, this, [this] {
                resizeToolOptionsPad();
                positionToolOptionsPad();
            });
        }
    }
    if (m_toolOptionsToggle) {
        m_toolOptionsToggle->setIcon(QIcon(QStringLiteral(":/quickaccess/quick_adjust/tool_options_%1.png")
                                               .arg(visible ? QStringLiteral("on") : QStringLiteral("off"))));
    }
    KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));
    config.writeEntry("ToolOptionsStartVisible", visible);
    config.sync();
}

void QuickAdjustDock::resizeToolOptionsPad()
{
    if (!m_toolOptionsPad || !m_borrowedToolOptions)
        return;

    QSize contentSize = m_borrowedToolOptions->sizeHint();
    QScrollArea *scrollArea = qobject_cast<QScrollArea *>(m_borrowedToolOptions.data());
    if (scrollArea && scrollArea->widget()) {
        QWidget *contents = scrollArea->widget();
        contents->ensurePolished();
        if (contents->layout()) {
            contents->layout()->invalidate();
            contents->layout()->activate();
        }
        contentSize = contents->sizeHint();
        const int frame = scrollArea->frameWidth() * 2;
        contentSize += QSize(frame, frame);
    }
    if (!contentSize.isValid() || contentSize.isEmpty())
        contentSize = m_borrowedToolOptions->minimumSizeHint();
    contentSize = contentSize.expandedTo(QSize(1, 1));

    QSize maximumSize;
    if (m_canvas && m_canvas->viewManager() && m_canvas->viewManager()->mainWindow())
        maximumSize = m_canvas->viewManager()->mainWindow()->centralWidget()->size();
    if ((!maximumSize.isValid() || maximumSize.isEmpty()) && m_toolOptionsPad->screen())
        maximumSize = m_toolOptionsPad->screen()->availableGeometry().size();

    const QMargins margins = m_toolOptionsPad->layout()->contentsMargins();
    const QSize marginSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    if (maximumSize.isValid() && !maximumSize.isEmpty()) {
        const QSize maximumContent = (maximumSize - marginSize - QSize(8, 8)).expandedTo(QSize(1, 1));
        if (scrollArea) {
            const int scrollBarExtent = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
            if (contentSize.height() > maximumContent.height())
                contentSize.rwidth() += scrollBarExtent;
            if (contentSize.width() > maximumContent.width())
                contentSize.rheight() += scrollBarExtent;
        }
        contentSize = contentSize.boundedTo(maximumContent);
    }

    const QSize wanted = contentSize + marginSize;
    if (wanted != m_toolOptionsPad->size()) {
        m_toolOptionsPad->resize(wanted);
        positionToolOptionsPad();
    }
}

void QuickAdjustDock::positionToolOptionsPad()
{
    if (!m_toolOptionsPad)
        return;
    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));
    const QString position = config.readEntry("ToolOptionsPosition", QStringLiteral("left_align_top"));
    const QPoint dockTopLeft = mapToGlobal(QPoint(0, 0));
    QPoint target;
    if (position == QStringLiteral("right_align_top")) {
        target = QPoint(dockTopLeft.x() + width() + 5, dockTopLeft.y());
    } else if (position == QStringLiteral("bottom_left")) {
        target = QPoint(dockTopLeft.x(), dockTopLeft.y() + height() + 5);
    } else {
        target = QPoint(dockTopLeft.x() - m_toolOptionsPad->width() - 5, dockTopLeft.y());
    }

    if (QScreen *screen = QGuiApplication::screenAt(dockTopLeft)) {
        const QRect bounds = screen->availableGeometry();
        target.setX(qBound(bounds.left(), target.x(), bounds.right() - m_toolOptionsPad->width() + 1));
        target.setY(qBound(bounds.top(), target.y(), bounds.bottom() - m_toolOptionsPad->height() + 1));
    }
    m_toolOptionsPad->move(target);
}

void QuickAdjustDock::returnToolOptionsDocker()
{
    if (m_toolOptionsDocker && m_borrowedToolOptions) {
        if (m_toolOptionsPad && m_toolOptionsPad->layout())
            m_toolOptionsPad->layout()->removeWidget(m_borrowedToolOptions);
        m_toolOptionsDocker->setWidget(m_borrowedToolOptions);
        m_borrowedToolOptions->setParent(m_toolOptionsDocker);
        m_toolOptionsDocker->toggleViewAction()->setEnabled(true);
        m_toolOptionsDocker->setVisible(m_toolOptionsDockerWasVisible);
    }
    if (m_toolOptionsPlaceholder)
        m_toolOptionsPlaceholder->deleteLater();
    if (m_toolOptionsPad)
        m_toolOptionsPad->deleteLater();
    m_toolOptionsDocker.clear();
    m_borrowedToolOptions.clear();
    m_toolOptionsPlaceholder.clear();
    m_toolOptionsPad.clear();
}

void QuickAdjustDock::moveEvent(QMoveEvent *event)
{
    QDockWidget::moveEvent(event);
    positionToolOptionsPad();
}

void QuickAdjustDock::resizeEvent(QResizeEvent *event)
{
    QDockWidget::resizeEvent(event);
    positionToolOptionsPad();
}

void QuickAdjustDock::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);
    if (m_toolOptionsToggle && m_toolOptionsToggle->isChecked())
        setToolOptionsPadVisible(true);
}

void QuickAdjustDock::hideEvent(QHideEvent *event)
{
    if (m_toolOptionsPad)
        m_toolOptionsPad->hide();
    QDockWidget::hideEvent(event);
}

QWidget *QuickAdjustDock::createColorHistoryWidget()
{
    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));
    const int colorCount = qBound(2, config.readEntry("ColorHistoryTotal", 14), 40);
    const int columns = (colorCount + 1) / 2;
    const int buttonSize = qBound(16, config.readEntry("ColorHistoryIconSize", 30), 64);

    auto *group = new QWidget(this);
    auto *grid = new QGridLayout(group);
    grid->setContentsMargins(4, 4, 4, 4);
    grid->setHorizontalSpacing(1);
    grid->setVerticalSpacing(1);
    for (int i = 0; i < colorCount; ++i) {
        auto *button = new QToolButton(group);
        button->setFixedSize(buttonSize, buttonSize);
        button->setAutoRaise(false);
        connect(button, &QToolButton::clicked, this, [this, i] {
            selectHistoryColor(i);
        });
        grid->addWidget(button, i / columns, i % columns);
        m_colorButtons.append(button);
    }
    grid->setColumnStretch(columns, 1);
    updateColorHistoryButtons();
    return group;
}

QWidget *QuickAdjustDock::createBrushHistoryWidget()
{
    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));
    const int brushCount = qBound(2, config.readEntry("BrushHistoryTotal", 14), 40);
    const int columns = (brushCount + 1) / 2;
    const int buttonSize = qBound(16, config.readEntry("BrushHistoryIconSize", 34), 64);

    auto *group = new QWidget(this);
    auto *grid = new QGridLayout(group);
    grid->setContentsMargins(4, 4, 4, 4);
    grid->setHorizontalSpacing(1);
    grid->setVerticalSpacing(1);
    for (int i = 0; i < brushCount; ++i) {
        auto *button = new QToolButton(group);
        button->setFixedSize(buttonSize, buttonSize);
        button->setIconSize(QSize(buttonSize - 4, buttonSize - 4));
        button->setStyleSheet(
            QStringLiteral("QToolButton { background-color: #b0b0b0; border: 1px solid #888; border-radius: 4px; }"));
        connect(button, &QToolButton::clicked, this, [this, i] {
            selectHistoryBrush(i);
        });
        grid->addWidget(button, i / columns, i % columns);
        m_brushButtons.append(button);
    }
    grid->setColumnStretch(columns, 1);
    updateBrushHistoryButtons();
    return group;
}

QWidget *QuickAdjustDock::createBrushToggleWidget()
{
    auto *widget = new QWidget(this);
    auto *grid = new QGridLayout(widget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);
    const QList<QPair<QString, QString>> properties{
        {QStringLiteral("PressureSize"), QStringLiteral("SizeUseCurve")},
        {QStringLiteral("OpacityUseCurve"), QString()},
        {QStringLiteral("FlowUseCurve"), QString()},
        {QStringLiteral("PressureRotation"), QStringLiteral("RotationUseCurve")},
    };
    const QStringList labels{i18nc("@action:button", "Size"),
                             i18nc("@action:button", "Opacity"),
                             i18nc("@action:button", "Flow"),
                             i18nc("@action:button", "Rotation")};
    for (int i = 0; i < properties.size(); ++i) {
        const QString name = properties.at(i).first;
        const QString pairedName = properties.at(i).second;
        auto *button = new QToolButton(widget);
        button->setText(labels.at(i));
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        connect(button, &QToolButton::toggled, this, [this, name, pairedName](bool checked) {
            if (m_syncing || !m_resourceProvider || !m_resourceProvider->currentPreset())
                return;
            KisPaintOpSettingsSP settings = m_resourceProvider->currentPreset()->settings();
            if (!settings || !settings->hasProperty(name))
                return;
            settings->setProperty(name, checked);
            if (!pairedName.isEmpty() && settings->hasProperty(pairedName))
                settings->setProperty(pairedName, checked);
            m_resourceProvider->setPaintOpPreset(m_resourceProvider->currentPreset());
        });
        grid->addWidget(button, i / 2, i % 2);
        m_brushToggleButtons.insert(name, button);
    }
    return widget;
}

void QuickAdjustDock::loadColorHistory()
{
    m_colorHistory = m_resourceProvider ? m_resourceProvider->colorHistory() : QList<KoColor>();
    while (m_colorHistory.size() > m_colorButtons.size())
        m_colorHistory.removeLast();
    updateColorHistoryButtons();
}

void QuickAdjustDock::updateColorHistoryButtons()
{
    for (int i = 0; i < m_colorButtons.size(); ++i) {
        QToolButton *button = m_colorButtons.at(i);
        if (i < m_colorHistory.size()) {
            const QColor color = m_colorHistory.at(i).toQColor();
            button->setEnabled(true);
            button->setToolTip(i18nc("@info:tooltip", "RGB: %1, %2, %3", color.red(), color.green(), color.blue()));
            button->setStyleSheet(
                QStringLiteral("QToolButton { background-color: %1; border: 1px solid #888; border-radius: 4px; }")
                    .arg(color.name(QColor::HexArgb)));
        } else {
            button->setEnabled(false);
            button->setToolTip(QString());
            button->setStyleSheet(QStringLiteral(
                "QToolButton { background-color: #b0b0b0; border: 1px solid #888; border-radius: 4px; }"));
        }
    }
}

void QuickAdjustDock::selectHistoryColor(int index)
{
    if (m_resourceProvider && index >= 0 && index < m_colorHistory.size())
        m_resourceProvider->setFGColor(m_colorHistory.at(index));
}

void QuickAdjustDock::updateBrushHistoryButtons()
{
    for (int i = 0; i < m_brushButtons.size(); ++i) {
        QToolButton *button = m_brushButtons.at(i);
        if (i < m_brushHistory.size() && m_brushHistory.at(i)) {
            const KisPaintOpPresetSP preset = m_brushHistory.at(i);
            button->setEnabled(true);
            button->setIcon(QIcon(QPixmap::fromImage(preset->image())));
            button->setToolTip(i18nc("@info:tooltip", "Brush: %1", preset->name()));
        } else {
            button->setEnabled(false);
            button->setIcon(QIcon());
            button->setToolTip(QString());
        }
    }
}

void QuickAdjustDock::selectHistoryBrush(int index)
{
    if (m_resourceProvider && index >= 0 && index < m_brushHistory.size())
        m_resourceProvider->setPaintOpPreset(m_brushHistory.at(index));
}

QToolButton *QuickAdjustDock::createStatusButton(const QString &toolTip, const QString &iconName)
{
    auto *button = new QToolButton(this);
    button->setCheckable(true);
    button->setAutoRaise(false);
    button->setFixedSize(18, 18);
    button->setIconSize(QSize(16, 16));
    button->setProperty("quickAccessIconName", iconName);
    button->setToolTip(toolTip);
    return button;
}

void QuickAdjustDock::updateStatusButtons()
{
    auto setStateIcon = [](QToolButton *button, bool active) {
        const QString name = button->property("quickAccessIconName").toString();
        const QString separator = name == QStringLiteral("rotate") ? QStringLiteral("-") : QStringLiteral("_");
        button->setIcon(QIcon(QStringLiteral(":/quickaccess/quick_adjust/%1%2%3.png")
                                  .arg(name, separator, active ? QStringLiteral("on") : QStringLiteral("off"))));
        const QSignalBlocker blocker(button);
        button->setChecked(active);
    };

    setStateIcon(m_eraseToggle, m_resourceProvider && m_resourceProvider->eraserMode());
    setStateIcon(m_alphaToggle, m_resourceProvider && m_resourceProvider->globalAlphaLock());

    bool selectionVisible = false;
    if (m_canvas && m_canvas->viewManager() && m_canvas->viewManager()->actionManager()) {
        if (KisAction *action =
                m_canvas->viewManager()->actionManager()->actionByName(QStringLiteral("toggle_display_selection"))) {
            selectionVisible = action->isChecked();
        }
    }
    setStateIcon(m_selectionToggle, selectionVisible);
    const KConfigGroup gestures = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessGesture"));
    setStateIcon(m_gestureToggle, gestures.readEntry("Enabled", true));
}

void QuickAdjustDock::triggerAction(const QString &name)
{
    if (!m_canvas || !m_canvas->viewManager() || !m_canvas->viewManager()->actionManager())
        return;
    if (KisAction *action = m_canvas->viewManager()->actionManager()->actionByName(name))
        action->trigger();
}

void QuickAdjustDock::setControlsEnabled(bool enabled)
{
    if (widget())
        widget()->setEnabled(enabled);
}

void QuickAdjustDock::syncFromCanvas()
{
    if (!m_canvas || !m_canvas->viewManager())
        return;
    KisCanvasResourceProvider *provider = m_canvas->viewManager()->canvasResourceProvider();
    if (!provider)
        return;

    m_syncing = true;
    {
        const QSignalBlocker sizeBlocker(m_brushSize);
        const QSignalBlocker opacityBlocker(m_brushOpacity);
        const QSignalBlocker flowBlocker(m_brushFlow);
        const QSignalBlocker rotationBlocker(m_brushRotation);
        const QSignalBlocker brushBlendBlocker(m_brushBlend);
        // set only changed values: the timer must not reset a value being typed
        if (qAbs(m_brushSize->value() - provider->size()) >= 0.005)
            m_brushSize->setValue(provider->size());
        if (m_brushOpacity->value() != qRound(provider->opacity() * 100.0))
            m_brushOpacity->setValue(qRound(provider->opacity() * 100.0));
        if (m_brushFlow->value() != qRound(provider->flow() * 100.0))
            m_brushFlow->setValue(qRound(provider->flow() * 100.0));
        if (qRound(m_brushRotation->angle()) != qRound(provider->brushRotation()))
            m_brushRotation->setAngle(qRound(provider->brushRotation()));
        selectBlendMode(m_brushBlend, provider->currentCompositeOp());
    }

    const KisNodeSP node = m_canvas->viewManager()->activeNode();
    m_layerOpacity->setEnabled(bool(node));
    m_layerBlend->setEnabled(bool(node));
    if (node) {
        const QSignalBlocker opacityBlocker(m_layerOpacity);
        const QSignalBlocker blendBlocker(m_layerBlend);
        if (m_layerOpacity->value() != qRound(node->opacity() * 100.0 / 255.0))
            m_layerOpacity->setValue(qRound(node->opacity() * 100.0 / 255.0));
        selectBlendMode(m_layerBlend, node->compositeOpId());
    }
    updateStatusButtons();
    resizeToolOptionsPad();
    m_syncing = false;
}

void QuickAdjustDock::slotBrushSizeChanged(qreal size)
{
    if (!m_syncing && m_canvas)
        m_canvas->viewManager()->canvasResourceProvider()->setSize(size);
}

void QuickAdjustDock::slotBrushOpacityChanged(int value)
{
    if (!m_syncing && m_canvas)
        m_canvas->viewManager()->canvasResourceProvider()->setOpacity(value / 100.0);
}

void QuickAdjustDock::slotBrushFlowChanged(int value)
{
    if (!m_syncing && m_canvas)
        m_canvas->viewManager()->canvasResourceProvider()->setFlow(value / 100.0);
}

void QuickAdjustDock::slotBrushRotationChanged(qreal angle)
{
    if (!m_syncing && m_canvas)
        m_canvas->viewManager()->canvasResourceProvider()->setBrushRotation(qRound(angle));
}

void QuickAdjustDock::slotBrushBlendChanged(int index)
{
    if (!m_syncing && m_canvas && index >= 0)
        m_canvas->viewManager()->canvasResourceProvider()->setCurrentCompositeOp(
            m_brushBlend->itemData(index).toString());
}

void QuickAdjustDock::slotLayerOpacityChanged(int value)
{
    if (!m_syncing && m_canvas && m_canvas->viewManager()->activeNode())
        m_canvas->viewManager()->nodeManager()->setNodeOpacity(m_canvas->viewManager()->activeNode(),
                                                               qRound(value * 255.0 / 100.0));
}

void QuickAdjustDock::slotLayerBlendChanged(int index)
{
    if (m_syncing || !m_canvas || index < 0)
        return;
    const KisNodeSP node = m_canvas->viewManager()->activeNode();
    if (!node || !node->paintDevice())
        return;
    const QString id = m_layerBlend->itemData(index).toString();
    const KoColorSpace *colorSpace = node->paintDevice()->compositionSourceColorSpace();
    const KoCompositeOp *op = colorSpace ? colorSpace->compositeOp(id) : nullptr;
    if (op)
        m_canvas->viewManager()->nodeManager()->setNodeCompositeOp(node, op);
}

void QuickAdjustDock::slotResetBrush()
{
    if (!m_canvas || !m_canvas->viewManager()->actionManager())
        return;
    if (KisAction *action =
            m_canvas->viewManager()->actionManager()->actionByName(QStringLiteral("reload_preset_action"))) {
        action->trigger();
        QTimer::singleShot(150, this, &QuickAdjustDock::syncFromCanvas);
    }
}

void QuickAdjustDock::slotForegroundColorChanged(const KoColor &color)
{
    if (!m_resourceProvider || m_resourceProvider->eraserMode())
        return;
    m_colorHistory.removeAll(color);
    m_colorHistory.prepend(color);
    while (m_colorHistory.size() > m_colorButtons.size())
        m_colorHistory.removeLast();
    m_resourceProvider->setColorHistory(m_colorHistory);
    updateColorHistoryButtons();
}

void QuickAdjustDock::slotBrushPresetChanged(const KisPaintOpPresetSP preset)
{
    if (!preset)
        return;
    if (!m_brushToggleButtons.isEmpty()) {
        const KisPaintOpSettingsSP settings = preset->settings();
        for (auto it = m_brushToggleButtons.begin(); it != m_brushToggleButtons.end(); ++it) {
            const QSignalBlocker blocker(it.value());
            const bool supported = settings && settings->hasProperty(it.key());
            it.value()->setEnabled(supported);
            it.value()->setChecked(supported && settings->getBool(it.key()));
        }
    }
    for (auto it = m_brushHistory.begin(); it != m_brushHistory.end();) {
        if (*it && (*it)->name() == preset->name())
            it = m_brushHistory.erase(it);
        else
            ++it;
    }
    m_brushHistory.prepend(preset);
    while (m_brushHistory.size() > m_brushButtons.size())
        m_brushHistory.removeLast();
    updateBrushHistoryButtons();
}

QString QuickAdjustDockFactory::id() const
{
    return QStringLiteral("brush_adjust_docker");
}

QDockWidget *QuickAdjustDockFactory::createDockWidget()
{
    auto *dock = new QuickAdjustDock;
    dock->setObjectName(id());
    return dock;
}

KoDockFactoryBase::DockPosition QuickAdjustDockFactory::defaultDockPosition() const
{
    return DockRight;
}
