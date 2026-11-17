/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAdjustDock.h"

#include "QuickAdjustKeyController.h"
#include "QuickAdjustToolOptionsPad.h"

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
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
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
    m_toolOptions = new QuickAdjustToolOptionsPad(this, m_toolOptionsToggle, compactPopup, [this] {
        QuickAdjustToolOptionsPad::Context context;
        context.canvasAvailable = m_canvas && m_canvas->viewManager();
        if (context.canvasAvailable) {
            context.mainWindow = m_canvas->viewManager()->mainWindow();
            context.toolOptionsInDocker = KisConfig(true).toolOptionsInDocker();
        }
        return context;
    });
    m_toolOptionsToggle->setChecked(adjustConfig.readEntry("ToolOptionsStartVisible", false));
    connect(m_toolOptionsToggle, &QToolButton::toggled, m_toolOptions, &QuickAdjustToolOptionsPad::setVisible);
    m_toolOptions->setVisible(m_toolOptionsToggle->isChecked());
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

    // The controls follow the canvas by polling. The timer runs only while
    // the dock is shown (showEvent()/hideEvent()): every main window has
    // this dock, and it used to poll for the whole session, hidden or not.
    m_syncTimer = new QTimer(this);
    m_syncTimer->setObjectName(QStringLiteral("quickAdjustSyncTimer"));
    m_syncTimer->setInterval(200);
    connect(m_syncTimer, &QTimer::timeout, this, &QuickAdjustDock::syncFromCanvas);
    setControlsEnabled(false);
}

QuickAdjustDock::~QuickAdjustDock()
{
    delete m_toolOptions;
    m_toolOptions = nullptr;
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
        m_toolOptions->ensure();
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

void QuickAdjustDock::moveEvent(QMoveEvent *event)
{
    QDockWidget::moveEvent(event);
    if (m_toolOptions)
        m_toolOptions->position();
}

void QuickAdjustDock::resizeEvent(QResizeEvent *event)
{
    QDockWidget::resizeEvent(event);
    if (m_toolOptions)
        m_toolOptions->position();
}

void QuickAdjustDock::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);
    if (m_toolOptionsToggle && m_toolOptionsToggle->isChecked())
        m_toolOptions->setVisible(true);
    // catch up with what changed while the dock was hidden, then follow
    syncFromCanvas();
    m_syncTimer->start();
}

void QuickAdjustDock::hideEvent(QHideEvent *event)
{
    m_syncTimer->stop();
    if (m_toolOptions)
        m_toolOptions->hide();
    QDockWidget::hideEvent(event);
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
        // This runs on every tick of the sync timer: build and set the icon
        // only when the state it shows changes. A click may have toggled a
        // checkable button already, so the checked state is compared too.
        const QVariant shown = button->property("quickAccessShownState");
        if (shown.isValid() && shown.toBool() == active && (!button->isCheckable() || button->isChecked() == active))
            return;
        button->setProperty("quickAccessShownState", active);
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
    m_toolOptions->resizeToContent();
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
