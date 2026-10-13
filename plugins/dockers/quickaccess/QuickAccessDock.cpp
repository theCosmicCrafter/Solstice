/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessDock.h"
#include "QuickAccessStrokePreviews.h"

#include "QuickAccessGesture.h"
#include "QuickAccessGridEditDialog.h"
#include "QuickAccessItemExecutor.h"
#include "QuickAccessLayoutEngine.h"
#include "QuickAccessLegacyImporter.h"
#include "QuickAccessProfileRepository.h"
#include "QuickAccessResourcesDialog.h"
#include "QuickAccessSettingsDialog.h"
#include "QuickAdjustDock.h"
#include "QuickColorSelectorDock.h"

#include <KisResourceServerProvider.h>
#include <KisViewManager.h>
#include <KoResource.h>
#include <KoResourcePaths.h>
#include <brushengine/kis_paintop_preset.h>
#include <kconfiggroup.h>
#include <kis_action.h>
#include <kis_action_manager.h>
#include <kis_canvas2.h>
#include <kis_canvas_resource_provider.h>
#include <kis_icon_utils.h>
#include <klocalizedstring.h>
#include <ksharedconfig.h>

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QRubberBand>
#include <QScrollArea>
#include <QShortcut>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <utility>

namespace
{
constexpr int DefaultIconSize = 42;
// The settings dialog's size when none is stored: the legacy Python plugin's
// default, also used when its settings are imported without a size.
constexpr int DefaultSettingsDialogWidth = 550;
constexpr int DefaultSettingsDialogHeight = 650;

/// Brush item display: "stroke" (stroke preview) or "icon" (preset icon).
/// Items saved before the option existed have no value and keep the icon.
const QString BrushDisplayKey = QStringLiteral("display");
const QString BrushDisplayStroke = QStringLiteral("stroke");
const QString BrushDisplayIcon = QStringLiteral("icon");

bool showsStrokePreview(const QuickAccess::Item &item)
{
    return item.type == QuickAccess::ItemType::Brush
        && item.payload.value(BrushDisplayKey).toString() == BrushDisplayStroke;
}

/**
 * Stroke preview brushes take two cells. A profile saved while they were
 * still one cell wide loads with overlaps; each such brush is resized again,
 * which pushes only the overlapping items aside. Returns whether anything
 * changed.
 */
bool repairBrushSpans(QuickAccess::Document *document)
{
    bool changed = false;
    for (QuickAccess::Tab &tab : document->tabs) {
        for (QuickAccess::Grid &grid : tab.grids) {
            const QuickAccess::LayoutEngine engine(grid.columns);
            if (engine.validate(grid.items).isValid())
                continue;
            QStringList strokeBrushes;
            for (const QuickAccess::Item &item : std::as_const(grid.items)) {
                if (showsStrokePreview(item))
                    strokeBrushes << item.id;
            }
            for (const QString &id : std::as_const(strokeBrushes)) {
                const QuickAccess::LayoutResult result = engine.resizeItem(grid.items, id, 1, qMin(2, grid.columns));
                if (result.isValid()) {
                    grid.items = result.items;
                    changed = true;
                    break;
                }
            }
        }
    }
    return changed;
}
constexpr int SeparatorEdgeMargin = 5;

QPoint globalMousePosition(const QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

class PopupDragBar : public QWidget
{
public:
    explicit PopupDragBar(QWidget *target, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_target(target)
    {
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_target) {
            m_dragOffset = globalMousePosition(event) - m_target->frameGeometry().topLeft();
            m_dragging = true;
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragging && (event->buttons() & Qt::LeftButton) && m_target) {
            m_target->move(globalMousePosition(event) - m_dragOffset);
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_dragging) {
            m_dragging = false;
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QPointer<QWidget> m_target;
    QPoint m_dragOffset;
    bool m_dragging{false};
};

class QuickColorPopup : public QFrame
{
public:
    using QFrame::QFrame;

protected:
    void leaveEvent(QEvent *event) override
    {
        QFrame::leaveEvent(event);

        // A combo box opens its list as a separate Qt::Popup window. Moving
        // the pointer into that window sends Leave to this popup even though
        // the user is still interacting with one of its child controls.
        if (QApplication::activePopupWidget())
            return;

        close();
    }
};

template<typename T>
T *findDock(const QString &objectName)
{
    for (QWidget *widget : QApplication::allWidgets()) {
        if (auto *dock = qobject_cast<T *>(widget)) {
            if (dock->objectName() == objectName)
                return dock;
        }
    }
    return nullptr;
}

void moveDockToCursor(QDockWidget *dock)
{
    if (!dock)
        return;
    if (dock->isFloating()) {
        dock->setFloating(false);
        dock->show();
        return;
    }
    dock->show();
    dock->setFloating(true);
    dock->raise();
    dock->move(QCursor::pos() - QPoint(dock->width() / 2, 24));
}

class QuickAccessSeparator : public QWidget
{
public:
    QuickAccessSeparator(bool vertical, int thickness, const QColor &color, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_vertical(vertical)
        , m_thickness(qMax(1, thickness))
        , m_color(color.isValid() ? color : QColor(QStringLiteral("#5a5a5a")))
    {
        setAttribute(Qt::WA_TranslucentBackground);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_color);
        QRectF bar;
        if (m_vertical) {
            bar = QRectF((width() - m_thickness) / 2.0,
                         SeparatorEdgeMargin,
                         m_thickness,
                         qMax(0, height() - SeparatorEdgeMargin * 2));
        } else {
            bar = QRectF(SeparatorEdgeMargin,
                         (height() - m_thickness) / 2.0,
                         qMax(0, width() - SeparatorEdgeMargin * 2),
                         m_thickness);
        }
        const qreal radius = qMax(1.0, m_thickness / 2.0);
        painter.drawRoundedRect(bar, radius, radius);
    }

private:
    bool m_vertical;
    int m_thickness;
    QColor m_color;
};

class ElidingPushButton : public QPushButton
{
public:
    explicit ElidingPushButton(const QString &text, QWidget *parent = nullptr)
        : QPushButton(parent)
        , m_fullText(text)
    {
        setAccessibleName(text);
        setToolTip(text);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    void setFullText(const QString &text)
    {
        m_fullText = text;
        setAccessibleName(text);
        setToolTip(text);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter painter(this);
        QStyleOptionButton option;
        initStyleOption(&option);

        const int iconWidth = option.icon.isNull() ? 0 : option.iconSize.width() + 6;
        const int horizontalPadding = style()->pixelMetric(QStyle::PM_ButtonMargin, &option, this) * 2 + iconWidth;
        option.text = fontMetrics().elidedText(m_fullText, Qt::ElideRight, qMax(0, width() - horizontalPadding));
        painter.drawControl(QStyle::CE_PushButton, option);
    }

private:
    QString m_fullText;
};

QJsonObject aliasFor(const QuickAccess::Document &document, const QString &category, const QString &id)
{
    return document.aliases.value(category).toObject().value(id).toObject();
}

QString compatibleString(const QJsonObject &object,
                         const QString &nativeName,
                         const QString &legacyName,
                         const QString &fallback = QString())
{
    QString value = object.value(nativeName).toString();
    if (value.isEmpty())
        value = object.value(legacyName).toString();
    return value.isEmpty() ? fallback : value;
}

int compatibleInt(const QJsonObject &object, const QString &nativeName, const QString &legacyName, int fallback)
{
    bool ok = false;
    int value = object.value(nativeName).toVariant().toInt(&ok);
    if (!ok)
        value = object.value(legacyName).toVariant().toInt(&ok);
    return ok ? value : fallback;
}

QPushButton *createColorButton(const QColor &initial, QWidget *parent)
{
    auto *button = new QPushButton(parent);
    const auto setColor = [button](const QColor &color) {
        button->setProperty("quickAccessColor", color.name(QColor::HexArgb));
        button->setText(color.name(QColor::HexArgb));
        button->setStyleSheet(
            QStringLiteral("background-color: %1; border: 1px solid #888;").arg(color.name(QColor::HexArgb)));
    };
    setColor(initial.isValid() ? initial : QColor(Qt::white));
    QObject::connect(button, &QPushButton::clicked, button, [button, setColor]() {
        const QColor current(button->property("quickAccessColor").toString());
        const QColor chosen = QColorDialog::getColor(current, button->window(), i18nc("@title:window", "Select Color"));
        if (chosen.isValid())
            setColor(chosen);
    });
    return button;
}

QColor buttonColor(const QPushButton *button)
{
    return QColor(button->property("quickAccessColor").toString());
}

QString defaultIconsDirectory()
{
    return QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
        .filePath(QStringLiteral("resources/default_icons"));
}

QString stripMnemonic(QString text)
{
    const QString escapedAmpersand = QStringLiteral("\x1f");
    text.replace(QStringLiteral("&&"), escapedAmpersand);
    text.remove(QLatin1Char('&'));
    text.replace(escapedAmpersand, QStringLiteral("&"));
    return text;
}

QString bundledIconPath(const QString &category, const QString &iconName)
{
    const QString fileName = QFileInfo(iconName).fileName();
    if (fileName.isEmpty())
        return QString();
    const QString path = QStringLiteral(":/quickaccess/%1/%2").arg(category, fileName);
    return QFileInfo::exists(path) ? path : QString();
}

QString resolveDefaultIcon(const QString &iconName)
{
    if (QFileInfo(iconName).isAbsolute() && QFileInfo::exists(iconName))
        return iconName;
    const QString bundled = bundledIconPath(QStringLiteral("default_icons"), iconName);
    if (!bundled.isEmpty())
        return bundled;
    return QString();
}

void upgradeLegacySettings(const QJsonObject &settings)
{
    KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccess"));
    const QJsonObject defaults = settings.value(QStringLiteral("default")).toObject();
    config.writeEntry("DockerIconSize", defaults.value(QStringLiteral("docker_icon_size")).toInt(DefaultIconSize));
    config.writeEntry("HeaderButtonColor",
                      defaults.value(QStringLiteral("header_button_color")).toString(QStringLiteral("#828282")));
    config.writeEntry("HeaderButtonFontColor",
                      defaults.value(QStringLiteral("header_button_font_color")).toString(QStringLiteral("#ffffff")));
    config.writeEntry("ActiveTabFontSize", defaults.value(QStringLiteral("tab_active_font_size")).toInt(12));
    config.writeEntry("ActiveTabFontColor",
                      defaults.value(QStringLiteral("tab_active_font_color")).toString(QStringLiteral("#ffffff")));
    config.writeEntry(
        "ActiveTabBackgroundColor",
        defaults.value(QStringLiteral("tab_active_background_color")).toString(QStringLiteral("#3f3f3f")));
    config.writeEntry("InactiveTabFontSize", defaults.value(QStringLiteral("tab_inactive_font_size")).toInt(12));
    config.writeEntry("InactiveTabFontColor",
                      defaults.value(QStringLiteral("tab_inactive_font_color")).toString(QStringLiteral("#a0a0a0")));
    config.writeEntry(
        "InactiveTabBackgroundColor",
        defaults.value(QStringLiteral("tab_inactive_background_color")).toString(QStringLiteral("#2b2b2b")));
    config.writeEntry("SettingsDialogWidth",
                      defaults.value(QStringLiteral("config_dialog_width")).toInt(DefaultSettingsDialogWidth));
    config.writeEntry("SettingsDialogHeight",
                      defaults.value(QStringLiteral("config_dialog_height")).toInt(DefaultSettingsDialogHeight));
    config.writeEntry("PopupIconSize",
                      settings.value(QStringLiteral("popup"))
                          .toObject()
                          .value(QStringLiteral("popup_icon_size"))
                          .toInt(DefaultIconSize));
    config.writeEntry("HueSVCEnabled", defaults.value(QStringLiteral("huesvc_enabled")).toBool(true));
    config.writeEntry("QuickAdjustEnabled", defaults.value(QStringLiteral("quick_adjust_enabled")).toBool(true));
    config.writeEntry("MigrationVersion", 2);

    const QJsonObject hue = settings.value(QStringLiteral("huesvc")).toObject();
    KConfigGroup hueConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessHueSVC"));
    hueConfig.writeEntry("RgbDisplayMode",
                         hue.value(QStringLiteral("rgb_display_mode")).toString(QStringLiteral("percentage")));
    hueConfig.writeEntry("PopupWidth", hue.value(QStringLiteral("popup_width")).toInt(350));
    hueConfig.writeEntry("PopupHeight", hue.value(QStringLiteral("popup_height")).toInt(600));
    hueConfig.writeEntry("ControlsPanelWidth", hue.value(QStringLiteral("controls_panel_width")).toInt(220));
    hueConfig.writeEntry("ControlsPanelFontSize", hue.value(QStringLiteral("controls_panel_font_size")).toInt(16));

    const QJsonObject adjust = settings.value(QStringLiteral("quick_adjust")).toObject();
    KConfigGroup adjustConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));
    const QList<QPair<QString, QString>> booleans{
        {QStringLiteral("size_slider_enabled"), QStringLiteral("SizeSliderEnabled")},
        {QStringLiteral("opacity_slider_enabled"), QStringLiteral("OpacitySliderEnabled")},
        {QStringLiteral("flow_slider_enabled"), QStringLiteral("FlowSliderEnabled")},
        {QStringLiteral("layer_opacity_slider_enabled"), QStringLiteral("LayerOpacitySliderEnabled")},
        {QStringLiteral("color_history_enabled"), QStringLiteral("ColorHistoryEnabled")},
        {QStringLiteral("brush_history_enabled"), QStringLiteral("BrushHistoryEnabled")},
        {QStringLiteral("tool_options_enabled"), QStringLiteral("ToolOptionsEnabled")},
        {QStringLiteral("tool_options_start_visible"), QStringLiteral("ToolOptionsStartVisible")}};
    for (const auto &entry : booleans) {
        const bool defaultValue = !entry.first.startsWith(QStringLiteral("tool_options"));
        adjustConfig.writeEntry(entry.second, adjust.value(entry.first).toBool(defaultValue));
    }
    adjustConfig.writeEntry("ColorHistoryTotal", adjust.value(QStringLiteral("color_history_total")).toInt(14));
    adjustConfig.writeEntry("ColorHistoryIconSize", adjust.value(QStringLiteral("color_history_icon_size")).toInt(30));
    adjustConfig.writeEntry("BrushHistoryTotal", adjust.value(QStringLiteral("brush_history_total")).toInt(14));
    adjustConfig.writeEntry("BrushHistoryIconSize", adjust.value(QStringLiteral("brush_history_icon_size")).toInt(34));
    adjustConfig.writeEntry("FontSize", adjust.value(QStringLiteral("font_size")).toString(QStringLiteral("12px")));
    adjustConfig.writeEntry("AltEraseKey", adjust.value(QStringLiteral("alt_erase_key")).toString());
    adjustConfig.writeEntry("PreserveAlphaKey", adjust.value(QStringLiteral("preserve_alpha_key")).toString());
    adjustConfig.writeEntry("SelectOutlineKey", adjust.value(QStringLiteral("select_outline_key")).toString());
    adjustConfig.writeEntry(
        "ToolOptionsPosition",
        adjust.value(QStringLiteral("tool_options_position")).toString(QStringLiteral("left_align_top")));
    adjustConfig.writeEntry(
        "TempBrushSets",
        QString::fromUtf8(
            QJsonDocument(adjust.value(QStringLiteral("temp_brush_sets")).toArray()).toJson(QJsonDocument::Compact)));
    QStringList blendModes;
    for (const QJsonValue &value : adjust.value(QStringLiteral("blender_mode_list")).toArray())
        blendModes.append(value.toString());
    if (!blendModes.isEmpty())
        adjustConfig.writeEntry("BlendModes", blendModes);

    const QJsonObject gesture = settings.value(QStringLiteral("gesture")).toObject();
    KConfigGroup gestureConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessGesture"));
    gestureConfig.writeEntry("Enabled", gesture.value(QStringLiteral("enabled")).toBool(true));
    gestureConfig.writeEntry("MinimumPixelsToMove", gesture.value(QStringLiteral("minimum_pixels_to_move")).toInt(20));
    gestureConfig.writeEntry("ShowPreview", gesture.value(QStringLiteral("show_preview")).toBool(true));
    config.sync();
    hueConfig.sync();
    adjustConfig.sync();
    gestureConfig.sync();
}
} // namespace

QuickAccessDock::QuickAccessDock(QWidget *parent, bool enableGestures)
    : QDockWidget(i18nc("@title:window", "Quick Access Palette"), parent)
    , m_executor(new QuickAccess::ItemExecutor(this))
    , m_strokePreviews(new QuickAccessStrokePreviews(this))
    , m_popupMode(!enableGestures)
{
    connect(m_strokePreviews, &QuickAccessStrokePreviews::changed, this, &QuickAccessDock::updateStrokePreviews);
    setObjectName(QStringLiteral("QuickAccessPalette"));
    auto *root = new QWidget(this);
    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    if (m_popupMode)
        buildPopupHeader(root);
    else
        buildHeader(root);
    m_tabs = new QTabWidget(root);
    layout->addWidget(m_tabs);
    setWidget(root);
    connect(m_tabs, &QTabWidget::currentChanged, this, &QuickAccessDock::slotCurrentTabChanged);
    loadProfile();
    if (enableGestures) {
        m_gestureController = new QuickAccessGestureController(
            &m_document,
            [this](const QuickAccess::Item &item) {
                activateItem(item);
            },
            this);
    }
    rebuildUi();
    setEnabled(false);
}

QuickAccessDock::~QuickAccessDock() = default;

void QuickAccessDock::buildPopupHeader(QWidget *root)
{
    auto *bar = new PopupDragBar(this, root);
    auto *header = new QHBoxLayout(bar);
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(2);
    header->addStretch();

    m_popupPinButton = new QToolButton(bar);
    m_popupPinButton->setCheckable(true);
    m_popupPinButton->setAutoRaise(true);
    const QString unpinnedIcon = bundledIconPath(QStringLiteral("system_icons"), QStringLiteral("pin_unpinned.png"));
    m_popupPinButton->setIcon(unpinnedIcon.isEmpty() ? QIcon::fromTheme(QStringLiteral("window-pin"))
                                                     : QIcon(unpinnedIcon));
    if (m_popupPinButton->icon().isNull())
        m_popupPinButton->setText(QStringLiteral("P"));
    m_popupPinButton->setToolTip(i18nc("@info:tooltip", "Keep popup open after selecting an item"));
    m_popupPinButton->setFixedSize(22, 22);
    m_popupPinButton->setIconSize(QSize(16, 16));
    connect(m_popupPinButton, &QToolButton::toggled, this, &QuickAccessDock::setPopupPinned);
    header->addWidget(m_popupPinButton);

    auto *closeButton = new QToolButton(bar);
    closeButton->setAutoRaise(true);
    const QString closeIcon = bundledIconPath(QStringLiteral("system_icons"), QStringLiteral("circle-xmark.png"));
    closeButton->setIcon(closeIcon.isEmpty() ? QIcon::fromTheme(QStringLiteral("window-close")) : QIcon(closeIcon));
    if (closeButton->icon().isNull())
        closeButton->setText(QStringLiteral("×"));
    closeButton->setToolTip(i18nc("@info:tooltip", "Close"));
    closeButton->setFixedSize(22, 22);
    closeButton->setIconSize(QSize(16, 16));
    connect(closeButton, &QToolButton::clicked, this, &QWidget::close);
    header->addWidget(closeButton);

    if (auto *layout = qobject_cast<QVBoxLayout *>(root->layout()))
        layout->addWidget(bar);
}

void QuickAccessDock::buildHeader(QWidget *root)
{
    auto *header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(3);

    auto *menuButton = new QToolButton(root);
    menuButton->setProperty("quickAccessHeaderButton", true);
    menuButton->setText(i18nc("@action:button", "Menu"));
    menuButton->setFixedHeight(24);
    menuButton->setPopupMode(QToolButton::InstantPopup);
    buildMenu(menuButton);
    header->addWidget(menuButton);

    const auto addToolButton =
        [root,
         header](const QString &assetName, const QString &themeName, const QString &fallback, const QString &toolTip) {
            auto *button = new QToolButton(root);
            button->setProperty("quickAccessHeaderButton", true);
            const QString assetPath = bundledIconPath(QStringLiteral("system_icons"), assetName);
            button->setIcon(assetPath.isEmpty() ? QIcon::fromTheme(themeName) : QIcon(assetPath));
            if (button->icon().isNull())
                button->setText(fallback);
            button->setToolTip(toolTip);
            button->setAutoRaise(false);
            button->setFixedSize(24, 24);
            button->setIconSize(QSize(18, 18));
            header->addWidget(button);
            return button;
        };

    auto *addBrushButton = addToolButton(QStringLiteral("add_brush.png"),
                                         QStringLiteral("list-add"),
                                         QStringLiteral("+"),
                                         i18n("Add Current Brush"));
    connect(addBrushButton, &QToolButton::clicked, this, &QuickAccessDock::addCurrentBrush);

    auto *resourcesButton = addToolButton(QStringLiteral("resources.png"),
                                          QStringLiteral("folder-image"),
                                          QStringLiteral("R"),
                                          i18n("Resources"));
    connect(resourcesButton, &QToolButton::clicked, this, &QuickAccessDock::showResourcesDialog);

    auto *gestureButton = addToolButton(QStringLiteral("gesture.png"),
                                        QStringLiteral("input-touchpad"),
                                        QStringLiteral("G"),
                                        i18n("Gesture Settings"));
    connect(gestureButton, &QToolButton::clicked, this, &QuickAccessDock::showGestureDialog);

    auto *gridEditButton = addToolButton(QStringLiteral("grid_edit.png"),
                                         QStringLiteral("document-edit"),
                                         QStringLiteral("E"),
                                         i18n("Edit Grid"));
    connect(gridEditButton, &QToolButton::clicked, this, &QuickAccessDock::showGridEditDialog);

    auto *configButton = addToolButton(QStringLiteral("setting.png"),
                                       QStringLiteral("configure"),
                                       QStringLiteral("C"),
                                       i18n("Configuration"));
    connect(configButton, &QToolButton::clicked, this, &QuickAccessDock::showSettingsDialog);

    header->addStretch();
    qobject_cast<QVBoxLayout *>(root->layout())->addLayout(header);
}

void QuickAccessDock::buildMenu(QToolButton *button)
{
    auto *menu = new QMenu(button);
    menu->addAction(i18nc("@action", "Add Tab"), this, &QuickAccessDock::addTab);
    menu->addSeparator();
    menu->addAction(i18nc("@action", "Add Current Brush"), this, &QuickAccessDock::addCurrentBrush);
    menu->addAction(i18nc("@action", "Add Label"), this, &QuickAccessDock::addLabel);
    menu->addAction(i18nc("@action", "Add Horizontal Separator"), this, [this]() {
        addSeparator(false);
    });
    menu->addAction(i18nc("@action", "Add Vertical Separator"), this, [this]() {
        addSeparator(true);
    });
    menu->addAction(i18nc("@action", "Add Color Swatch"), this, &QuickAccessDock::addColor);
    menu->addAction(i18nc("@action", "Add Brush Size"), this, &QuickAccessDock::addBrushSize);
    menu->addAction(i18nc("@action", "Add Brush Blending Mode"), this, &QuickAccessDock::addBrushBlendMode);
    menu->addAction(i18nc("@action", "Add Python Script"), this, &QuickAccessDock::addScript);
    button->setMenu(menu);
}

QString QuickAccessDock::observerName()
{
    return QStringLiteral("QuickAccessDock");
}

void QuickAccessDock::setViewManager(KisViewManager *viewManager)
{
    if (!viewManager || m_popupMode)
        return;
    KisActionManager *actionManager = viewManager->actionManager();
    m_palettePopupAction = actionManager->createAction(QStringLiteral("quick_access_palette_popup"));
    connect(m_palettePopupAction, &QAction::triggered, this, &QuickAccessDock::showPalettePopup);
    connect(actionManager->createAction(QStringLiteral("hue_svc_popup")),
            &QAction::triggered,
            this,
            &QuickAccessDock::showColorPopup);
    connect(actionManager->createAction(QStringLiteral("toggle_gesture_recognition")),
            &QAction::triggered,
            this,
            &QuickAccessDock::toggleGestures);
    connect(actionManager->createAction(QStringLiteral("move_quick_access_palette_docker_to_cursor")),
            &QAction::triggered,
            this,
            &QuickAccessDock::movePaletteDockerToCursor);
    connect(actionManager->createAction(QStringLiteral("move_quick_adjust_docker_to_cursor")),
            &QAction::triggered,
            this,
            &QuickAccessDock::moveAdjustDockerToCursor);
}

void QuickAccessDock::showPalettePopup()
{
    if (m_palettePopup) {
        m_palettePopup->close();
        return;
    }
    if (!m_canvas)
        return;
    auto *popup = new QuickAccessDock(nullptr, false);
    popup->setObjectName(QStringLiteral("QuickAccessPalettePopup"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    popup->setCanvas(m_canvas);
    if (m_palettePopupAction) {
        for (const QKeySequence &sequence : m_palettePopupAction->shortcuts()) {
            if (sequence.isEmpty())
                continue;
            auto *shortcut = new QShortcut(sequence, popup);
            connect(shortcut, &QShortcut::activated, popup, &QWidget::close);
        }
    }
    popup->adjustSize();
    popup->resize(popup->sizeHint());
    popup->move(QCursor::pos() - QPoint(popup->width() / 4, popup->height() / 3));
    m_palettePopup = popup;
    popup->show();
}

void QuickAccessDock::showColorPopup()
{
    const KConfigGroup featureConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccess"));
    if (!featureConfig.readEntry("HueSVCEnabled", true))
        return;
    if (m_colorPopup) {
        m_colorPopup->close();
        return;
    }
    if (!m_canvas)
        return;
    auto *popup = new QuickColorPopup(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QHBoxLayout(popup);
    layout->setContentsMargins(6, 6, 6, 6);
    auto *selector = new QuickColorSelectorWidget(popup);
    selector->setCanvas(m_canvas);
    layout->addWidget(selector, 1);

    const KConfigGroup hueConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessHueSVC"));
    QuickAdjustDock *adjust = nullptr;
    if (featureConfig.readEntry("QuickAdjustEnabled", true)) {
        adjust = new QuickAdjustDock(popup, true);
        adjust->setFeatures(QDockWidget::NoDockWidgetFeatures);
        adjust->setTitleBarWidget(new QWidget(adjust));
        adjust->setFixedWidth(qBound(160, hueConfig.readEntry("ControlsPanelWidth", 220), 600));
        adjust->setCanvas(m_canvas);
        layout->addWidget(adjust, 0, Qt::AlignVCenter);
    }
    const int selectorWidth = qBound(200, hueConfig.readEntry("PopupWidth", 350), 1200);
    popup->resize(selectorWidth + (adjust ? adjust->maximumWidth() : 0),
                  qBound(200, hueConfig.readEntry("PopupHeight", 600), 1200));
    popup->move(QCursor::pos() - QPoint(popup->width() / 4, popup->height() / 3));
    m_colorPopup = popup;
    popup->show();
    popup->raise();
    popup->activateWindow();
}

void QuickAccessDock::toggleGestures()
{
    KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessGesture"));
    config.writeEntry("Enabled", !config.readEntry("Enabled", true));
    config.sync();
    for (QWidget *widget : QApplication::allWidgets()) {
        if (auto *dock = qobject_cast<QuickAccessDock *>(widget))
            dock->reloadGestureSettings();
    }
}

void QuickAccessDock::movePaletteDockerToCursor()
{
    moveDockToCursor(this);
}

void QuickAccessDock::moveAdjustDockerToCursor()
{
    moveDockToCursor(findDock<QuickAdjustDock>(QStringLiteral("brush_adjust_docker")));
}

void QuickAccessDock::setPopupPinned(bool pinned)
{
    if (!m_popupMode || m_popupPinned == pinned)
        return;
    m_popupPinned = pinned;
    if (m_popupPinButton) {
        const QString iconName = pinned ? QStringLiteral("pin_pinned.png") : QStringLiteral("pin_unpinned.png");
        const QString iconPath = bundledIconPath(QStringLiteral("system_icons"), iconName);
        m_popupPinButton->setIcon(iconPath.isEmpty() ? QIcon::fromTheme(QStringLiteral("window-pin"))
                                                     : QIcon(iconPath));
        m_popupPinButton->setToolTip(pinned ? i18nc("@info:tooltip", "Allow popup to close after selecting an item")
                                            : i18nc("@info:tooltip", "Keep popup open after selecting an item"));
    }
    const QPoint position = pos();
    setWindowFlag(Qt::Popup, !pinned);
    setWindowFlag(Qt::Tool, pinned);
    setWindowFlag(Qt::FramelessWindowHint, true);
    move(position);
    show();
    raise();
}

void QuickAccessDock::setCanvas(KoCanvasBase *canvas)
{
    m_canvas = dynamic_cast<KisCanvas2 *>(canvas);
    m_executor->setCanvas(m_canvas);
    setEnabled(m_canvas);
    rebuildUi();
}

void QuickAccessDock::unsetCanvas()
{
    m_canvas.clear();
    m_executor->setCanvas(nullptr);
    setEnabled(false);
}

KisCanvas2 *QuickAccessDock::canvas() const
{
    return m_canvas;
}

void QuickAccessDock::reloadGestureSettings()
{
    if (m_gestureController)
        m_gestureController->reloadSettings();
}

void QuickAccessDock::loadProfile()
{
    m_profilePath = profilePath();
    QuickAccess::ProfileRepository repository(m_profilePath);
    QString error;
    if (repository.exists()) {
        if (repository.load(&m_document, &error)) {
            if (repairBrushSpans(&m_document))
                repository.save(m_document, &error);
            KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccess"));
            if (config.readEntry("MigrationVersion", 0) < 2) {
                QuickAccess::LegacyImportResult imported;
                QString migrationError;
                if (QuickAccess::LegacyImporter::importDirectory(legacyConfigPath(), &imported, &migrationError))
                    upgradeLegacySettings(imported.settings);
            }
            return;
        }
        QMessageBox::warning(this,
                             i18nc("@title:window", "Quick Access Profile"),
                             i18n("The Quick Access profile could not be loaded:\n%1", error));
        m_document = QuickAccess::Document::createDefault();
        return;
    }

    const QString legacyPath = legacyConfigPath();
    if (QFileInfo::exists(QDir(legacyPath).filePath(QStringLiteral("quick_access_palette.json")))) {
        QuickAccess::LegacyImportResult imported;
        if (QuickAccess::LegacyImporter::importDirectory(legacyPath, &imported, &error)) {
            m_document = imported.document;
            if (!repository.save(m_document, &error)) {
                QMessageBox::warning(this,
                                     i18nc("@title:window", "Quick Access Migration"),
                                     i18n("The imported Quick Access profile could not be saved:\n%1", error));
            } else {
                KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccess"));
                config.writeEntry("MigrationVersion", 2);
                const QJsonObject defaults = imported.settings.value(QStringLiteral("default")).toObject();
                config.writeEntry("DockerIconSize",
                                  defaults.value(QStringLiteral("docker_icon_size")).toInt(DefaultIconSize));
                config.writeEntry(
                    "HeaderButtonColor",
                    defaults.value(QStringLiteral("header_button_color")).toString(QStringLiteral("#828282")));
                config.writeEntry(
                    "HeaderButtonFontColor",
                    defaults.value(QStringLiteral("header_button_font_color")).toString(QStringLiteral("#ffffff")));
                config.writeEntry("ActiveTabFontSize",
                                  defaults.value(QStringLiteral("tab_active_font_size")).toInt(12));
                config.writeEntry(
                    "ActiveTabFontColor",
                    defaults.value(QStringLiteral("tab_active_font_color")).toString(QStringLiteral("#ffffff")));
                config.writeEntry(
                    "ActiveTabBackgroundColor",
                    defaults.value(QStringLiteral("tab_active_background_color")).toString(QStringLiteral("#3f3f3f")));
                config.writeEntry("InactiveTabFontSize",
                                  defaults.value(QStringLiteral("tab_inactive_font_size")).toInt(12));
                config.writeEntry(
                    "InactiveTabFontColor",
                    defaults.value(QStringLiteral("tab_inactive_font_color")).toString(QStringLiteral("#a0a0a0")));
                config.writeEntry("InactiveTabBackgroundColor",
                                  defaults.value(QStringLiteral("tab_inactive_background_color"))
                                      .toString(QStringLiteral("#2b2b2b")));
                config.writeEntry(
                    "SettingsDialogWidth",
                    defaults.value(QStringLiteral("config_dialog_width")).toInt(DefaultSettingsDialogWidth));
                config.writeEntry(
                    "SettingsDialogHeight",
                    defaults.value(QStringLiteral("config_dialog_height")).toInt(DefaultSettingsDialogHeight));
                config.writeEntry("HueSVCEnabled", defaults.value(QStringLiteral("huesvc_enabled")).toBool(true));
                config.writeEntry("QuickAdjustEnabled",
                                  defaults.value(QStringLiteral("quick_adjust_enabled")).toBool(true));

                const QJsonObject popup = imported.settings.value(QStringLiteral("popup")).toObject();
                config.writeEntry("PopupIconSize",
                                  popup.value(QStringLiteral("popup_icon_size")).toInt(DefaultIconSize));

                const QJsonObject hue = imported.settings.value(QStringLiteral("huesvc")).toObject();
                KConfigGroup hueConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessHueSVC"));
                hueConfig.writeEntry(
                    "RgbDisplayMode",
                    hue.value(QStringLiteral("rgb_display_mode")).toString(QStringLiteral("percentage")));
                hueConfig.writeEntry("PopupWidth", hue.value(QStringLiteral("popup_width")).toInt(350));
                hueConfig.writeEntry("PopupHeight", hue.value(QStringLiteral("popup_height")).toInt(600));
                hueConfig.writeEntry("ControlsPanelWidth",
                                     hue.value(QStringLiteral("controls_panel_width")).toInt(220));
                hueConfig.writeEntry("ControlsPanelFontSize",
                                     hue.value(QStringLiteral("controls_panel_font_size")).toInt(16));

                const QJsonObject adjust = imported.settings.value(QStringLiteral("quick_adjust")).toObject();
                KConfigGroup adjustConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));
                const auto copyBool = [&adjust,
                                       &adjustConfig](const char *jsonKey, const char *configKey, bool fallback) {
                    adjustConfig.writeEntry(configKey, adjust.value(QLatin1String(jsonKey)).toBool(fallback));
                };
                const auto copyInt = [&adjust,
                                      &adjustConfig](const char *jsonKey, const char *configKey, int fallback) {
                    adjustConfig.writeEntry(configKey, adjust.value(QLatin1String(jsonKey)).toInt(fallback));
                };
                const auto copyString =
                    [&adjust, &adjustConfig](const char *jsonKey, const char *configKey, const QString &fallback) {
                        adjustConfig.writeEntry(configKey, adjust.value(QLatin1String(jsonKey)).toString(fallback));
                    };
                copyBool("size_slider_enabled", "SizeSliderEnabled", true);
                copyBool("opacity_slider_enabled", "OpacitySliderEnabled", true);
                copyBool("flow_slider_enabled", "FlowSliderEnabled", true);
                copyBool("layer_opacity_slider_enabled", "LayerOpacitySliderEnabled", true);
                copyBool("color_history_enabled", "ColorHistoryEnabled", true);
                copyInt("color_history_total", "ColorHistoryTotal", 14);
                copyInt("color_history_icon_size", "ColorHistoryIconSize", 30);
                copyBool("brush_history_enabled", "BrushHistoryEnabled", true);
                copyInt("brush_history_total", "BrushHistoryTotal", 14);
                copyInt("brush_history_icon_size", "BrushHistoryIconSize", 34);
                copyString("font_size", "FontSize", QStringLiteral("12px"));
                copyString("alt_erase_key", "AltEraseKey", QString());
                copyString("preserve_alpha_key", "PreserveAlphaKey", QString());
                copyString("select_outline_key", "SelectOutlineKey", QString());
                copyBool("tool_options_enabled", "ToolOptionsEnabled", false);
                copyBool("tool_options_start_visible", "ToolOptionsStartVisible", false);
                copyString("tool_options_position", "ToolOptionsPosition", QStringLiteral("left_align_top"));
                adjustConfig.writeEntry(
                    "TempBrushSets",
                    QString::fromUtf8(QJsonDocument(adjust.value(QStringLiteral("temp_brush_sets")).toArray())
                                          .toJson(QJsonDocument::Compact)));
                QStringList blendModes;
                for (const QJsonValue &value : adjust.value(QStringLiteral("blender_mode_list")).toArray())
                    blendModes.append(value.toString());
                if (!blendModes.isEmpty())
                    adjustConfig.writeEntry("BlendModes", blendModes);
                const QJsonObject gesture = imported.settings.value(QStringLiteral("gesture")).toObject();
                KConfigGroup gestureConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessGesture"));
                gestureConfig.writeEntry("Enabled", gesture.value(QStringLiteral("enabled")).toBool(true));
                gestureConfig.writeEntry("MinimumPixelsToMove",
                                         gesture.value(QStringLiteral("minimum_pixels_to_move")).toInt(20));
                gestureConfig.writeEntry("ShowPreview", gesture.value(QStringLiteral("show_preview")).toBool(true));
                config.sync();
                hueConfig.sync();
                adjustConfig.sync();
                gestureConfig.sync();
            }
            return;
        }
        QMessageBox::warning(this,
                             i18nc("@title:window", "Quick Access Migration"),
                             i18n("The legacy Quick Access configuration could not be imported:\n%1", error));
    }

    m_document = QuickAccess::Document::createDefault();
    if (!repository.save(m_document, &error)) {
        QMessageBox::warning(this,
                             i18nc("@title:window", "Quick Access Profile"),
                             i18n("The default Quick Access profile could not be saved:\n%1", error));
    }
}

bool QuickAccessDock::saveProfile()
{
    QString error;
    QuickAccess::ProfileRepository repository(m_profilePath);
    if (repository.save(m_document, &error))
        return true;

    QMessageBox::warning(this,
                         i18nc("@title:window", "Quick Access Profile"),
                         i18n("The Quick Access profile could not be saved:\n%1", error));
    return false;
}

void QuickAccessDock::rebuildUi()
{
    m_rebuilding = true;
    m_strokePreviewButtons.clear();
    while (m_tabs->count() > 0) {
        QWidget *page = m_tabs->widget(0);
        m_tabs->removeTab(0);
        page->deleteLater();
    }
    int activeIndex = 0;
    for (int index = 0; index < m_document.tabs.size(); ++index) {
        const QuickAccess::Tab &tab = m_document.tabs.at(index);
        m_tabs->addTab(createTabPage(tab), tab.name);
        if (tab.id == m_document.activeTabId)
            activeIndex = index;
    }
    if (m_tabs->count() > 0)
        m_tabs->setCurrentIndex(activeIndex);
    m_rebuilding = false;
    applyAppearanceSettings();

    QStringList names;
    for (const StrokePreviewButton &entry : std::as_const(m_strokePreviewButtons))
        names << entry.name;
    m_strokePreviews->setNames(names);
    updateStrokePreviews();
}

void QuickAccessDock::updateStrokePreviews()
{
    for (StrokePreviewButton &entry : m_strokePreviewButtons) {
        if (entry.shown || !entry.button || !m_strokePreviews->ready(entry.name))
            continue;
        entry.button->setIcon(
            QIcon(m_strokePreviews->pixmap(entry.name, entry.size, entry.button->devicePixelRatioF())));
        entry.shown = true;
    }
}

QWidget *QuickAccessDock::createTabPage(const QuickAccess::Tab &tab)
{
    auto *container = new QWidget(m_tabs);
    auto *containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(4, 4, 4, 4);
    containerLayout->setSpacing(4);

    for (const QuickAccess::Grid &grid : tab.grids) {
        auto *gridWidget = new QWidget(container);
        auto *layout = new QGridLayout(gridWidget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        const KConfigGroup appearance = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccess"));
        const int cellSize = m_popupMode ? appearance.readEntry("PopupIconSize", DefaultIconSize)
                                         : appearance.readEntry("DockerIconSize", DefaultIconSize);
        int rowCount = 0;
        for (const QuickAccess::Item &item : grid.items) {
            layout->addWidget(createItemWidget(item), item.row, item.column, item.rowSpan, item.columnSpan);
            rowCount = qMax(rowCount, item.bottom());
        }
        for (int column = 0; column < grid.columns; ++column) {
            layout->setColumnMinimumWidth(column, cellSize);
            layout->setColumnStretch(column, 0);
        }
        for (int row = 0; row < rowCount; ++row)
            layout->setRowMinimumHeight(row, cellSize);
        gridWidget->setFixedWidth(grid.columns * cellSize + qMax(0, grid.columns - 1) * layout->horizontalSpacing());
        gridWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        containerLayout->addWidget(gridWidget, 0, Qt::AlignLeft);
    }
    if (m_popupMode) {
        containerLayout->setSizeConstraint(QLayout::SetFixedSize);
        return container;
    }

    containerLayout->addStretch();

    auto *scrollArea = new QScrollArea(m_tabs);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(container);
    return scrollArea;
}

QWidget *QuickAccessDock::createItemWidget(const QuickAccess::Item &item)
{
    if (item.type == QuickAccess::ItemType::Label) {
        auto *label = new QLabel(item.payload.value(QStringLiteral("text")).toString(), m_tabs);
        label->setAlignment(Qt::AlignCenter);
        const QColor background(compatibleString(item.payload,
                                                 QStringLiteral("background_color"),
                                                 QStringLiteral("backgroundColor"),
                                                 QStringLiteral("#263746")));
        const QColor foreground(compatibleString(item.payload,
                                                 QStringLiteral("font_color"),
                                                 QStringLiteral("fontColor"),
                                                 QStringLiteral("#4fc3f7")));
        const int fontSize =
            qBound(6, compatibleInt(item.payload, QStringLiteral("font_size"), QStringLiteral("fontSize"), 18), 96);
        label->setStyleSheet(QStringLiteral("background: %1; color: %2; font-size: %3px; padding: 2px 6px;")
                                 .arg(background.name(), foreground.name())
                                 .arg(fontSize));
        if (!m_popupMode)
            attachItemMenu(label, item);
        attachItemDrag(label, item);
        return label;
    }
    if (item.type == QuickAccess::ItemType::Separator) {
        const bool vertical =
            item.payload.value(QStringLiteral("orientation")).toString() == QStringLiteral("vertical");
        bool thicknessOk = false;
        const int configuredThickness = item.payload.value(QStringLiteral("thickness")).toVariant().toInt(&thicknessOk);
        const int thickness = thicknessOk ? qMax(1, configuredThickness) : 2;
        const QColor color(item.payload.value(QStringLiteral("color")).toString(QStringLiteral("#5a5a5a")));
        auto *separator = new QuickAccessSeparator(vertical, thickness, color, m_tabs);
        if (!m_popupMode)
            attachItemMenu(separator, item);
        attachItemDrag(separator, item);
        return separator;
    }

    auto *button = new ElidingPushButton(itemText(item), m_tabs);
    const KConfigGroup appearance = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccess"));
    const int iconSize = m_popupMode ? appearance.readEntry("PopupIconSize", DefaultIconSize)
                                     : appearance.readEntry("DockerIconSize", DefaultIconSize);
    button->setMinimumSize(iconSize, iconSize);

    if (showsStrokePreview(item)) {
        // The stroke preview fills the item's cells (an empty preview area
        // until it is available); the name is only in the tooltip.
        const QString name = item.payload.value(QStringLiteral("brush_name")).toString();
        const int spacing = 2;
        const QSize cells(item.columnSpan * iconSize + (item.columnSpan - 1) * spacing,
                          item.rowSpan * iconSize + (item.rowSpan - 1) * spacing);
        const QSize previewSize = cells - QSize(8, 8);
        button->setFullText(QString());
        button->setToolTip(name);
        button->setIcon(QIcon(m_strokePreviews->pixmap(QString(), previewSize, devicePixelRatioF())));
        button->setIconSize(previewSize);
        m_strokePreviewButtons.append({name, button, previewSize, false});
    } else if (item.type == QuickAccess::ItemType::Brush && m_canvas) {
        const QString name = item.payload.value(QStringLiteral("brush_name")).toString();
        const auto resources =
            KisResourceServerProvider::instance()->paintOpPresetServer()->resourceModel()->resourcesForName(name);
        if (!resources.isEmpty()) {
            button->setIcon(QPixmap::fromImage(resources.constFirst()->image()));
            button->setIconSize(QSize(iconSize - 4, iconSize - 4));
            button->setFullText(QString());
            button->setToolTip(name);
        }
    } else if (item.type == QuickAccess::ItemType::Color) {
        const QColor color(item.payload.value(QStringLiteral("color")).toString());
        if (color.isValid()) {
            button->setFullText(QString());
            button->setStyleSheet(
                QStringLiteral("background-color: %1; border: 2px solid #808080;").arg(color.name(QColor::HexArgb)));
            button->setToolTip(color.name(QColor::HexArgb));
        }
    }

    QJsonObject configuredAlias;
    QString configuredIcon;
    if (item.type == QuickAccess::ItemType::Action) {
        const QString id = item.payload.value(QStringLiteral("action_id")).toString();
        configuredAlias = aliasFor(m_document, QStringLiteral("actions"), id);
        configuredIcon = configuredAlias.value(QStringLiteral("icon_name")).toString();
    } else if (item.type == QuickAccess::ItemType::DockerToggle) {
        const QString id = item.payload.value(QStringLiteral("docker_id")).toString();
        configuredAlias = aliasFor(m_document, QStringLiteral("dockers"), id);
        configuredIcon = configuredAlias.value(QStringLiteral("icon_name")).toString();
    } else if (item.type == QuickAccess::ItemType::Script) {
        configuredIcon = item.payload.value(QStringLiteral("icon_name")).toString();
    }
    const QString configuredIconPath = resolveDefaultIcon(configuredIcon);
    if (!configuredIconPath.isEmpty()) {
        const QString toolTip = itemText(item);
        button->setIcon(QIcon(configuredIconPath));
        button->setIconSize(QSize(iconSize - 4, iconSize - 4));
        button->setFullText(QString());
        button->setToolTip(toolTip);
    }

    if (item.type == QuickAccess::ItemType::Action || item.type == QuickAccess::ItemType::DockerToggle) {
        const bool docker = item.type == QuickAccess::ItemType::DockerToggle;
        QColor background(
            compatibleString(configuredAlias, QStringLiteral("background_color"), QStringLiteral("backgroundColor")));
        QColor foreground(compatibleString(configuredAlias, QStringLiteral("font_color"), QStringLiteral("fontColor")));
        if (!background.isValid())
            background = QColor(docker ? QStringLiteral("#263a2f") : QStringLiteral("#3a263f"));
        if (!foreground.isValid())
            foreground = Qt::white;
        int fontSize = compatibleInt(configuredAlias, QStringLiteral("font_size"), QStringLiteral("fontSize"), 18);
        fontSize = qBound(6, fontSize, 96);
        button->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; color: %2; font-size: %3px; border: 1px solid %4; "
                           "border-radius: 4px; padding: %5; }")
                .arg(background.name(),
                     foreground.name(),
                     QString::number(fontSize),
                     docker ? QStringLiteral("#1c2212") : QStringLiteral("#6b4a73"),
                     configuredIconPath.isEmpty() ? QStringLiteral("2px 6px") : QStringLiteral("0px")));
    }

    if (item.type == QuickAccess::ItemType::BrushSize || item.type == QuickAccess::ItemType::BrushBlendMode) {
        const bool size = item.type == QuickAccess::ItemType::BrushSize;
        QColor background(compatibleString(item.payload,
                                           QStringLiteral("background_color"),
                                           QStringLiteral("backgroundColor"),
                                           size ? QStringLiteral("#3a263f") : QStringLiteral("#263a3a")));
        QColor foreground(compatibleString(item.payload,
                                           QStringLiteral("font_color"),
                                           QStringLiteral("fontColor"),
                                           QStringLiteral("#ffffff")));
        const int fontSize =
            qBound(6, compatibleInt(item.payload, QStringLiteral("font_size"), QStringLiteral("fontSize"), 18), 96);
        button->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; color: %2; font-size: %3px; border: 1px solid %4; "
                           "border-radius: 4px; padding: 2px 6px; }")
                .arg(background.name(), foreground.name())
                .arg(fontSize)
                .arg(size ? QStringLiteral("#6b4a73") : QStringLiteral("#4a8b8b")));
    }

    connect(button, &QPushButton::clicked, this, [this, item]() {
        activateItem(item);
    });
    if (!m_popupMode)
        attachItemMenu(button, item);
    attachItemDrag(button, item);
    return button;
}

namespace
{
/// Gap between palette cells; matches the grid layout in createTabPage().
constexpr int GridCellSpacing = 2;
} // namespace

int QuickAccessDock::cellSize() const
{
    const KConfigGroup appearance = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccess"));
    return m_popupMode ? appearance.readEntry("PopupIconSize", DefaultIconSize)
                       : appearance.readEntry("DockerIconSize", DefaultIconSize);
}

void QuickAccessDock::attachItemDrag(QWidget *widget, const QuickAccess::Item &item)
{
    widget->setProperty("quickaccess_item_id", item.id);
    widget->installEventFilter(this);
}

QPoint QuickAccessDock::dragTarget(const QuickAccess::Item &item, const QPoint &globalPosition) const
{
    const QuickAccess::Grid *grid = const_cast<QuickAccessDock *>(this)->activeGrid();
    const int step = cellSize() + GridCellSpacing;
    const QPoint delta = globalPosition - m_dragStart;
    const int row = qMax(0, item.row + qRound(delta.y() / double(step)));
    const int column = qBound(0,
                              item.column + qRound(delta.x() / double(step)),
                              qMax(0, (grid ? grid->columns : 1) - item.columnSpan));
    return QPoint(column, row);
}

void QuickAccessDock::cancelItemDrag()
{
    if (m_dragHighlight)
        m_dragHighlight->deleteLater();
    m_dragHighlight = nullptr;
    m_dragItemId.clear();
}

bool QuickAccessDock::eventFilter(QObject *watched, QEvent *event)
{
    // Ctrl + left-drag moves an item to another cell of its grid, as in the
    // original plugin. A plain click still activates the item; the Ctrl press
    // is swallowed so the item does not fire. Any other button cancels.
    auto *widget = qobject_cast<QWidget *>(watched);
    const QString itemId = widget ? widget->property("quickaccess_item_id").toString() : QString();
    if (itemId.isEmpty())
        return QDockWidget::eventFilter(watched, event);
    auto findItem = [this](const QString &id) -> const QuickAccess::Item * {
        const QuickAccess::Grid *grid = activeGrid();
        if (!grid)
            return nullptr;
        for (const QuickAccess::Item &item : grid->items) {
            if (item.id == id)
                return &item;
        }
        return nullptr;
    };
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (!m_dragItemId.isEmpty()) {
            cancelItemDrag();
            return true;
        }
        if (mouse->button() != Qt::LeftButton || !(mouse->modifiers() & Qt::ControlModifier) || !findItem(itemId))
            return false;
        m_dragItemId = itemId;
        m_dragStart = mouse->globalPosition().toPoint();
        return true;
    }
    case QEvent::MouseMove: {
        if (m_dragItemId.isEmpty())
            return false;
        const QuickAccess::Item *item = findItem(m_dragItemId);
        QWidget *gridWidget = widget->parentWidget();
        if (!item || !gridWidget)
            return true;
        const QPoint target = dragTarget(*item, static_cast<QMouseEvent *>(event)->globalPosition().toPoint());
        if (!m_dragHighlight || m_dragHighlight->parentWidget() != gridWidget) {
            if (m_dragHighlight)
                m_dragHighlight->deleteLater();
            m_dragHighlight = new QRubberBand(QRubberBand::Rectangle, gridWidget);
        }
        const int size = cellSize();
        const int step = size + GridCellSpacing;
        m_dragHighlight->setGeometry(target.x() * step,
                                     target.y() * step,
                                     item->columnSpan * size + (item->columnSpan - 1) * GridCellSpacing,
                                     item->rowSpan * size + (item->rowSpan - 1) * GridCellSpacing);
        m_dragHighlight->show();
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (m_dragItemId.isEmpty())
            return false;
        const QString id = m_dragItemId;
        const QuickAccess::Item *item = findItem(id);
        const QPoint target =
            item ? dragTarget(*item, static_cast<QMouseEvent *>(event)->globalPosition().toPoint()) : QPoint();
        const bool moved = item && (target.y() != item->row || target.x() != item->column);
        cancelItemDrag();
        if (moved) {
            // Deferred: the move rebuilds the pages, deleting this widget
            // while its release is still being delivered.
            QTimer::singleShot(0, this, [this, id, target]() {
                QuickAccess::Grid *grid = activeGrid();
                if (!grid)
                    return;
                const QuickAccess::LayoutResult result =
                    QuickAccess::LayoutEngine(grid->columns).moveItem(grid->items, id, target.y(), target.x());
                if (!result.isValid())
                    return;
                grid->items = result.items;
                saveProfile();
                rebuildUi();
            });
        }
        return true;
    }
    default:
        break;
    }
    return QDockWidget::eventFilter(watched, event);
}

void QuickAccessDock::attachItemMenu(QWidget *widget, const QuickAccess::Item &item)
{
    widget->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(widget, &QWidget::customContextMenuRequested, this, [this, widget, item](const QPoint &position) {
        QMenu menu(this);
        const QString removeIcon = bundledIconPath(QStringLiteral("system_icons"), QStringLiteral("circle-xmark.png"));
        QAction *removeAction =
            menu.addAction(removeIcon.isEmpty() ? QIcon::fromTheme(QStringLiteral("edit-delete")) : QIcon(removeIcon),
                           i18nc("@action", "Remove"));
        QAction *propertyAction = menu.addAction(KisIconUtils::loadIcon(QStringLiteral("document-properties")),
                                                 i18nc("@action", "Properties"));
        QAction *selected = menu.exec(widget->mapToGlobal(position));
        if (selected == removeAction) {
            removeItem(item.id);
        } else if (selected == propertyAction) {
            if (item.type == QuickAccess::ItemType::Brush)
                editBrushProperties(item.id);
            else
                editItemProperties(item.id);
        }
    });
}

QString QuickAccessDock::itemText(const QuickAccess::Item &item) const
{
    switch (item.type) {
    case QuickAccess::ItemType::Brush:
        return item.payload.value(QStringLiteral("brush_name")).toString();
    case QuickAccess::ItemType::Action: {
        const QString id = item.payload.value(QStringLiteral("action_id")).toString();
        const QJsonObject alias = aliasFor(m_document, QStringLiteral("actions"), id);
        const QString customName = compatibleString(alias, QStringLiteral("custom_name"), QStringLiteral("customName"));
        if (!customName.isEmpty())
            return customName;
        if (m_canvas && m_canvas->viewManager()) {
            if (QAction *action = m_canvas->viewManager()->actionManager()->actionByName(id))
                return stripMnemonic(action->text());
        }
        return id;
    }
    case QuickAccess::ItemType::DockerToggle: {
        const QString id = item.payload.value(QStringLiteral("docker_id")).toString();
        const QJsonObject alias = aliasFor(m_document, QStringLiteral("dockers"), id);
        return compatibleString(alias, QStringLiteral("custom_name"), QStringLiteral("customName"), id);
    }
    case QuickAccess::ItemType::BrushSize:
    case QuickAccess::ItemType::BrushBlendMode:
    case QuickAccess::ItemType::Label:
        return item.payload.value(QStringLiteral("text")).toString();
    case QuickAccess::ItemType::Color:
        return item.payload.value(QStringLiteral("color")).toString();
    case QuickAccess::ItemType::Script:
        return compatibleString(item.payload,
                                QStringLiteral("custom_name"),
                                QStringLiteral("customName"),
                                i18n("Script"));
    case QuickAccess::ItemType::Separator:
        return QString();
    }
    return QString();
}

void QuickAccessDock::activateItem(const QuickAccess::Item &item)
{
    QString error;
    if (!m_executor->execute(item, &error) && !error.isEmpty()) {
        QMessageBox::warning(this, i18nc("@title:window", "Quick Access"), error);
    }
    if (m_popupMode && !m_popupPinned)
        close();
}

void QuickAccessDock::slotCurrentTabChanged(int index)
{
    if (m_rebuilding || index < 0 || index >= m_document.tabs.size())
        return;
    m_document.activeTabId = m_document.tabs.at(index).id;
    saveProfile();
}

QuickAccess::Tab *QuickAccessDock::activeTab()
{
    for (QuickAccess::Tab &tab : m_document.tabs) {
        if (tab.id == m_document.activeTabId)
            return &tab;
    }
    return m_document.tabs.isEmpty() ? nullptr : &m_document.tabs.first();
}

QuickAccess::Grid *QuickAccessDock::activeGrid()
{
    QuickAccess::Tab *tab = activeTab();
    return !tab || tab->grids.isEmpty() ? nullptr : &tab->grids.first();
}

void QuickAccessDock::addItem(QuickAccess::Item item)
{
    QuickAccess::Grid *grid = activeGrid();
    if (!grid)
        return;

    item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.normalize();
    item.columnSpan = qMin(item.columnSpan, grid->columns);
    const bool sequential = m_sequentialPlacement && m_sequentialGridId == grid->id;
    if (sequential) {
        // Fill the row left to right; wrap when the item no longer fits.
        if (m_sequentialColumn + item.columnSpan > grid->columns) {
            ++m_sequentialRow;
            m_sequentialColumn = 0;
        }
        item.row = m_sequentialRow;
        item.column = m_sequentialColumn;
    } else {
        int bottom = 0;
        for (const QuickAccess::Item &existing : std::as_const(grid->items))
            bottom = qMax(bottom, existing.bottom());
        item.row = bottom;
        item.column = 0;
    }

    const QuickAccess::LayoutResult result = QuickAccess::LayoutEngine(grid->columns).addItem(grid->items, item);
    if (!result.isValid()) {
        QMessageBox::warning(this,
                             i18nc("@title:window", "Quick Access Layout"),
                             i18n("The item could not be placed in the active grid."));
        return;
    }
    grid->items = result.items;
    if (sequential) {
        // Continue just right of where the item landed.
        for (const QuickAccess::Item &placed : std::as_const(grid->items)) {
            if (placed.id != item.id)
                continue;
            m_sequentialRow = placed.row;
            m_sequentialColumn = placed.column + placed.columnSpan;
            if (m_sequentialColumn >= grid->columns) {
                m_sequentialRow = placed.row + placed.rowSpan;
                m_sequentialColumn = 0;
            }
        }
    }
    saveProfile();
    rebuildUi();
}

void QuickAccessDock::addCurrentBrush()
{
    if (!m_canvas || !m_canvas->viewManager())
        return;
    const KisPaintOpPresetSP preset = m_canvas->viewManager()->canvasResourceProvider()->currentPreset();
    if (!preset) {
        QMessageBox::information(this, i18nc("@title:window", "Quick Access"), i18n("No brush preset is active."));
        return;
    }
    QuickAccess::Item item;
    item.type = QuickAccess::ItemType::Brush;
    item.payload.insert(QStringLiteral("brush_name"), preset->name());
    item.payload.insert(BrushDisplayKey, BrushDisplayStroke);
    item.columnSpan = qMin(2, activeGrid() ? activeGrid()->columns : 2);
    addItem(item);
}

void QuickAccessDock::addLabel()
{
    bool accepted = false;
    const QString text = QInputDialog::getText(this,
                                               i18nc("@title:window", "Add Label"),
                                               i18nc("@label:textbox", "Label text:"),
                                               QLineEdit::Normal,
                                               QString(),
                                               &accepted)
                             .trimmed();
    if (!accepted || text.isEmpty())
        return;
    QuickAccess::Item item;
    item.type = QuickAccess::ItemType::Label;
    item.columnSpan = qMin(2, activeGrid() ? activeGrid()->columns : 2);
    item.payload.insert(QStringLiteral("text"), text);
    addItem(item);
}

void QuickAccessDock::addSeparator(bool vertical)
{
    QuickAccess::Item item;
    item.type = QuickAccess::ItemType::Separator;
    item.rowSpan = vertical ? 2 : 1;
    item.columnSpan = vertical ? 1 : qMin(2, activeGrid() ? activeGrid()->columns : 2);
    item.payload.insert(QStringLiteral("orientation"),
                        vertical ? QStringLiteral("vertical") : QStringLiteral("horizontal"));
    addItem(item);
}

void QuickAccessDock::addColor()
{
    QColor initial(Qt::white);
    if (m_canvas && m_canvas->resourceManager())
        initial = m_canvas->resourceManager()->foregroundColor().toQColor();
    const QColor color = QColorDialog::getColor(initial, this, i18nc("@title:window", "Add Color Swatch"));
    if (!color.isValid())
        return;
    QuickAccess::Item item;
    item.type = QuickAccess::ItemType::Color;
    item.payload.insert(QStringLiteral("color"), color.name(QColor::HexArgb));
    addItem(item);
}

void QuickAccessDock::addBrushSize()
{
    const double initial =
        m_canvas && m_canvas->viewManager() ? m_canvas->viewManager()->canvasResourceProvider()->size() : 10.0;
    bool accepted = false;
    const double size = QInputDialog::getDouble(this,
                                                i18nc("@title:window", "Add Brush Size"),
                                                i18nc("@label:spinbox", "Brush size:"),
                                                initial,
                                                0.01,
                                                10000.0,
                                                2,
                                                &accepted);
    if (!accepted)
        return;
    QuickAccess::Item item;
    item.type = QuickAccess::ItemType::BrushSize;
    item.payload.insert(QStringLiteral("text"), QString::number(size));
    addItem(item);
}

void QuickAccessDock::addBrushBlendMode()
{
    const QString initial = m_canvas && m_canvas->viewManager()
        ? m_canvas->viewManager()->canvasResourceProvider()->currentCompositeOp()
        : QStringLiteral("normal");
    bool accepted = false;
    const QString id = QInputDialog::getText(this,
                                             i18nc("@title:window", "Add Brush Blending Mode"),
                                             i18nc("@label:textbox", "Blending mode ID:"),
                                             QLineEdit::Normal,
                                             initial,
                                             &accepted)
                           .trimmed();
    if (!accepted || id.isEmpty())
        return;
    QuickAccess::Item item;
    item.type = QuickAccess::ItemType::BrushBlendMode;
    item.columnSpan = qMin(2, activeGrid() ? activeGrid()->columns : 2);
    item.payload.insert(QStringLiteral("text"), id);
    addItem(item);
}

void QuickAccessDock::addScript()
{
    const QString path = QFileDialog::getOpenFileName(this,
                                                      i18nc("@title:window", "Add Python Script"),
                                                      QDir::homePath(),
                                                      i18nc("@item:inlistbox", "Python scripts (*.py)"));
    if (path.isEmpty())
        return;
    QuickAccess::Item item;
    item.type = QuickAccess::ItemType::Script;
    item.columnSpan = qMin(2, activeGrid() ? activeGrid()->columns : 2);
    item.payload.insert(QStringLiteral("script_path"), path);
    item.payload.insert(QStringLiteral("custom_name"), QFileInfo(path).completeBaseName());
    addItem(item);
}

void QuickAccessDock::addTab()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               i18nc("@title:window", "Add Quick Access Tab"),
                                               i18nc("@label:textbox", "Tab name:"),
                                               QLineEdit::Normal,
                                               i18n("New Tab"),
                                               &accepted)
                             .trimmed();
    if (!accepted || name.isEmpty())
        return;

    QuickAccess::Grid grid;
    grid.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    grid.name = name;
    QuickAccess::Tab tab;
    tab.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    tab.name = name;
    tab.grids.append(grid);
    m_document.tabs.append(tab);
    m_document.activeTabId = tab.id;
    saveProfile();
    rebuildUi();
}

void QuickAccessDock::showResourcesDialog()
{
    if (!m_canvas)
        return;
    QuickAccessResourcesDialog dialog(m_canvas, m_document, this);
    // Items added from the dialog fill the grid's last empty row from the left
    // and wrap to the next row, instead of each starting a new row.
    if (QuickAccess::Grid *grid = activeGrid()) {
        int bottom = 0;
        for (const QuickAccess::Item &existing : std::as_const(grid->items))
            bottom = qMax(bottom, existing.bottom());
        m_sequentialPlacement = true;
        m_sequentialGridId = grid->id;
        m_sequentialRow = bottom;
        m_sequentialColumn = 0;
    }
    connect(&dialog, &QuickAccessResourcesDialog::brushRequested, this, [this](const QString &name) {
        QuickAccess::Item item;
        item.type = QuickAccess::ItemType::Brush;
        item.payload.insert(QStringLiteral("brush_name"), name);
        item.payload.insert(BrushDisplayKey, BrushDisplayStroke);
        item.columnSpan = qMin(2, activeGrid() ? activeGrid()->columns : 2);
        addItem(item);
    });
    connect(&dialog, &QuickAccessResourcesDialog::actionRequested, this, [this](const QString &id) {
        QuickAccess::Item item;
        item.type = QuickAccess::ItemType::Action;
        item.payload.insert(QStringLiteral("action_id"), id);
        const QJsonObject alias = aliasFor(m_document, QStringLiteral("actions"), id);
        item.columnSpan = alias.value(QStringLiteral("icon_name")).toString().isEmpty() ? 2 : 1;
        addItem(item);
    });
    connect(&dialog, &QuickAccessResourcesDialog::dockerRequested, this, [this](const QString &id) {
        QuickAccess::Item item;
        item.type = QuickAccess::ItemType::DockerToggle;
        item.payload.insert(QStringLiteral("docker_id"), id);
        const QJsonObject alias = aliasFor(m_document, QStringLiteral("dockers"), id);
        item.columnSpan = alias.value(QStringLiteral("icon_name")).toString().isEmpty() ? 2 : 1;
        addItem(item);
    });
    connect(&dialog,
            &QuickAccessResourcesDialog::aliasChanged,
            this,
            [this](const QString &category, const QString &id, const QJsonObject &alias) {
                QJsonObject aliases = m_document.aliases;
                QJsonObject categoryAliases = aliases.value(category).toObject();
                categoryAliases.insert(id, alias);
                aliases.insert(category, categoryAliases);
                m_document.aliases = aliases;
                saveProfile();
                rebuildUi();
            });
    dialog.exec();
    m_sequentialPlacement = false;
}

void QuickAccessDock::showGridEditDialog()
{
    QuickAccessGridEditDialog dialog(m_document, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_document = dialog.document();
    saveProfile();
    rebuildUi();
}

void QuickAccessDock::showGestureDialog()
{
    if (m_gestureController)
        m_gestureController->cancelGesture();
    QuickAccessGestureDialog dialog(m_document, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_document.gesturePages = dialog.pages();
    KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessGesture"));
    config.writeEntry("Enabled", dialog.gesturesEnabled());
    config.writeEntry("MinimumPixelsToMove", dialog.movementThreshold());
    config.writeEntry("ShowPreview", dialog.showPreview());
    config.sync();
    saveProfile();
    if (m_gestureController)
        m_gestureController->reloadSettings();
}

void QuickAccessDock::showSettingsDialog()
{
    KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccess"));
    QuickAccessSettingsDialog::Settings current;
    if (QuickAccess::Grid *grid = activeGrid())
        current.columns = grid->columns;
    current.dockerIconSize = config.readEntry("DockerIconSize", DefaultIconSize);
    current.headerButtonColor = QColor(config.readEntry("HeaderButtonColor", QStringLiteral("#828282")));
    current.headerButtonFontColor = QColor(config.readEntry("HeaderButtonFontColor", QStringLiteral("#ffffff")));
    current.activeTabFontSize = config.readEntry("ActiveTabFontSize", 12);
    current.activeTabFontColor = QColor(config.readEntry("ActiveTabFontColor", QStringLiteral("#ffffff")));
    current.activeTabBackgroundColor = QColor(config.readEntry("ActiveTabBackgroundColor", QStringLiteral("#3f3f3f")));
    current.inactiveTabFontSize = config.readEntry("InactiveTabFontSize", 12);
    current.inactiveTabFontColor = QColor(config.readEntry("InactiveTabFontColor", QStringLiteral("#a0a0a0")));
    current.inactiveTabBackgroundColor =
        QColor(config.readEntry("InactiveTabBackgroundColor", QStringLiteral("#2b2b2b")));
    current.dialogWidth = config.readEntry("SettingsDialogWidth", DefaultSettingsDialogWidth);
    current.dialogHeight = config.readEntry("SettingsDialogHeight", DefaultSettingsDialogHeight);
    const KConfigGroup gestureConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessGesture"));
    current.gestureEnabled = gestureConfig.readEntry("Enabled", true);
    current.hueSvcEnabled = config.readEntry("HueSVCEnabled", true);
    current.quickAdjustEnabled = config.readEntry("QuickAdjustEnabled", true);
    current.popupIconSize = config.readEntry("PopupIconSize", DefaultIconSize);
    const KConfigGroup hueConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessHueSVC"));
    current.rgbDisplayMode = hueConfig.readEntry("RgbDisplayMode", QStringLiteral("percentage"));
    current.huePopupWidth = hueConfig.readEntry("PopupWidth", 350);
    current.huePopupHeight = hueConfig.readEntry("PopupHeight", 600);
    current.hueControlsWidth = hueConfig.readEntry("ControlsPanelWidth", 220);
    current.hueControlsFontSize = hueConfig.readEntry("ControlsPanelFontSize", 16);
    const KConfigGroup adjustConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));
    current.sizeSliderEnabled = adjustConfig.readEntry("SizeSliderEnabled", true);
    current.opacitySliderEnabled = adjustConfig.readEntry("OpacitySliderEnabled", true);
    current.flowSliderEnabled = adjustConfig.readEntry("FlowSliderEnabled", true);
    current.layerOpacitySliderEnabled = adjustConfig.readEntry("LayerOpacitySliderEnabled", true);
    current.colorHistoryEnabled = adjustConfig.readEntry("ColorHistoryEnabled", true);
    current.colorHistoryTotal = adjustConfig.readEntry("ColorHistoryTotal", 14);
    current.colorHistoryIconSize = adjustConfig.readEntry("ColorHistoryIconSize", 30);
    current.brushHistoryEnabled = adjustConfig.readEntry("BrushHistoryEnabled", true);
    current.brushHistoryTotal = adjustConfig.readEntry("BrushHistoryTotal", 14);
    current.brushHistoryIconSize = adjustConfig.readEntry("BrushHistoryIconSize", 34);
    current.altEraseKey = adjustConfig.readEntry("AltEraseKey", QString());
    current.preserveAlphaKey = adjustConfig.readEntry("PreserveAlphaKey", QString());
    current.selectOutlineKey = adjustConfig.readEntry("SelectOutlineKey", QString());
    current.toolOptionsEnabled = adjustConfig.readEntry("ToolOptionsEnabled", false);
    current.toolOptionsPosition = adjustConfig.readEntry("ToolOptionsPosition", QStringLiteral("left_align_top"));
    current.blendModes = adjustConfig.readEntry("BlendModes", current.blendModes);
    const QJsonDocument brushSets =
        QJsonDocument::fromJson(adjustConfig.readEntry("TempBrushSets", QStringLiteral("[]")).toUtf8());
    current.tempBrushSets = brushSets.isArray() ? brushSets.array() : QJsonArray();

    QuickAccessSettingsDialog dialog(current, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QuickAccessSettingsDialog::Settings updated = dialog.settings();

    if (updated.columns != current.columns) {
        for (QuickAccess::Tab &tab : m_document.tabs) {
            for (QuickAccess::Grid &grid : tab.grids) {
                grid.columns = updated.columns;
                for (QuickAccess::Item &item : grid.items)
                    item.columnSpan = qMin(item.columnSpan, grid.columns);
                QuickAccess::LayoutEngine engine(grid.columns);
                if (!engine.validate(grid.items).isValid()) {
                    const QuickAccess::LayoutResult compacted = engine.compact(grid.items);
                    if (compacted.isValid())
                        grid.items = compacted.items;
                }
            }
        }
    }

    config.writeEntry("DockerIconSize", updated.dockerIconSize);
    config.writeEntry("HeaderButtonColor", updated.headerButtonColor.name(QColor::HexArgb));
    config.writeEntry("HeaderButtonFontColor", updated.headerButtonFontColor.name(QColor::HexArgb));
    config.writeEntry("ActiveTabFontSize", updated.activeTabFontSize);
    config.writeEntry("ActiveTabFontColor", updated.activeTabFontColor.name(QColor::HexArgb));
    config.writeEntry("ActiveTabBackgroundColor", updated.activeTabBackgroundColor.name(QColor::HexArgb));
    config.writeEntry("InactiveTabFontSize", updated.inactiveTabFontSize);
    config.writeEntry("InactiveTabFontColor", updated.inactiveTabFontColor.name(QColor::HexArgb));
    config.writeEntry("InactiveTabBackgroundColor", updated.inactiveTabBackgroundColor.name(QColor::HexArgb));
    config.writeEntry("SettingsDialogWidth", updated.dialogWidth);
    config.writeEntry("SettingsDialogHeight", updated.dialogHeight);
    config.writeEntry("PopupIconSize", updated.popupIconSize);
    config.writeEntry("HueSVCEnabled", updated.hueSvcEnabled);
    config.writeEntry("QuickAdjustEnabled", updated.quickAdjustEnabled);
    KConfigGroup gestureWrite = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessGesture"));
    gestureWrite.writeEntry("Enabled", updated.gestureEnabled);
    gestureWrite.sync();
    KConfigGroup hueWrite = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessHueSVC"));
    hueWrite.writeEntry("RgbDisplayMode", updated.rgbDisplayMode);
    hueWrite.writeEntry("PopupWidth", updated.huePopupWidth);
    hueWrite.writeEntry("PopupHeight", updated.huePopupHeight);
    hueWrite.writeEntry("ControlsPanelWidth", updated.hueControlsWidth);
    hueWrite.writeEntry("ControlsPanelFontSize", updated.hueControlsFontSize);
    hueWrite.sync();
    KConfigGroup adjustWrite = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));
    adjustWrite.writeEntry("SizeSliderEnabled", updated.sizeSliderEnabled);
    adjustWrite.writeEntry("OpacitySliderEnabled", updated.opacitySliderEnabled);
    adjustWrite.writeEntry("FlowSliderEnabled", updated.flowSliderEnabled);
    adjustWrite.writeEntry("LayerOpacitySliderEnabled", updated.layerOpacitySliderEnabled);
    adjustWrite.writeEntry("ColorHistoryEnabled", updated.colorHistoryEnabled);
    adjustWrite.writeEntry("ColorHistoryTotal", updated.colorHistoryTotal);
    adjustWrite.writeEntry("ColorHistoryIconSize", updated.colorHistoryIconSize);
    adjustWrite.writeEntry("BrushHistoryEnabled", updated.brushHistoryEnabled);
    adjustWrite.writeEntry("BrushHistoryTotal", updated.brushHistoryTotal);
    adjustWrite.writeEntry("BrushHistoryIconSize", updated.brushHistoryIconSize);
    adjustWrite.writeEntry("AltEraseKey", updated.altEraseKey);
    adjustWrite.writeEntry("PreserveAlphaKey", updated.preserveAlphaKey);
    adjustWrite.writeEntry("SelectOutlineKey", updated.selectOutlineKey);
    adjustWrite.writeEntry("ToolOptionsEnabled", updated.toolOptionsEnabled);
    adjustWrite.writeEntry("ToolOptionsPosition", updated.toolOptionsPosition);
    adjustWrite.writeEntry("BlendModes", updated.blendModes);
    adjustWrite.writeEntry("TempBrushSets",
                           QString::fromUtf8(QJsonDocument(updated.tempBrushSets).toJson(QJsonDocument::Compact)));
    adjustWrite.sync();
    config.sync();
    saveProfile();
    rebuildUi();
}

void QuickAccessDock::applyAppearanceSettings()
{
    if (!widget() || !m_tabs)
        return;
    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccess"));
    QColor headerColor(config.readEntry("HeaderButtonColor", QStringLiteral("#828282")));
    if (!headerColor.isValid())
        headerColor = QColor(QStringLiteral("#828282"));
    QColor headerFontColor(config.readEntry("HeaderButtonFontColor", QStringLiteral("#ffffff")));
    if (!headerFontColor.isValid())
        headerFontColor = QColor(Qt::white);
    const QString headerStyle =
        QStringLiteral(
            "QToolButton { background-color: %1; color: %2; border: 1px solid palette(mid); border-radius: 2px; } "
            "QToolButton:hover { border-color: palette(highlight); }")
            .arg(headerColor.name(QColor::HexArgb), headerFontColor.name(QColor::HexArgb));
    for (QToolButton *button : widget()->findChildren<QToolButton *>()) {
        if (button->property("quickAccessHeaderButton").toBool())
            button->setStyleSheet(headerStyle);
    }

    const int activeSize = qBound(6, config.readEntry("ActiveTabFontSize", 12), 24);
    const int inactiveSize = qBound(6, config.readEntry("InactiveTabFontSize", 12), 24);
    const QColor activeText(config.readEntry("ActiveTabFontColor", QStringLiteral("#ffffff")));
    const QColor activeBackground(config.readEntry("ActiveTabBackgroundColor", QStringLiteral("#3f3f3f")));
    const QColor inactiveText(config.readEntry("InactiveTabFontColor", QStringLiteral("#a0a0a0")));
    const QColor inactiveBackground(config.readEntry("InactiveTabBackgroundColor", QStringLiteral("#2b2b2b")));
    m_tabs->setStyleSheet(
        QStringLiteral("QTabBar::tab { color: %1; background: %2; font-size: %3px; padding: 5px 12px; } "
                       "QTabBar::tab:selected { color: %4; background: %5; font-size: %6px; }")
            .arg(inactiveText.name(QColor::HexArgb), inactiveBackground.name(QColor::HexArgb))
            .arg(inactiveSize)
            .arg(activeText.name(QColor::HexArgb), activeBackground.name(QColor::HexArgb))
            .arg(activeSize));
}

void QuickAccessDock::editItemProperties(const QString &itemId)
{
    QuickAccess::Grid *grid = activeGrid();
    if (!grid)
        return;
    auto itemIt = std::find_if(grid->items.begin(), grid->items.end(), [&itemId](const QuickAccess::Item &item) {
        return item.id == itemId;
    });
    if (itemIt == grid->items.end() || itemIt->type == QuickAccess::ItemType::Brush)
        return;

    QuickAccess::Item &item = *itemIt;
    QDialog dialog(this);
    dialog.setWindowTitle(i18nc("@title:window", "Quick Access Item Properties"));
    dialog.setMinimumWidth(320);
    auto *root = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    root->addLayout(form);

    QLineEdit *textEdit = nullptr;
    QSpinBox *valueSpin = nullptr;
    QSpinBox *fontSize = nullptr;
    QPushButton *background = nullptr;
    QPushButton *foreground = nullptr;
    QPushButton *swatch = nullptr;
    QLineEdit *iconEdit = nullptr;
    QLineEdit *scriptEdit = nullptr;
    QString aliasCategory;
    QString aliasId;
    QJsonObject values = item.payload;

    if (item.type == QuickAccess::ItemType::Action) {
        aliasCategory = QStringLiteral("actions");
        aliasId = item.payload.value(QStringLiteral("action_id")).toString();
        values = aliasFor(m_document, aliasCategory, aliasId);
    } else if (item.type == QuickAccess::ItemType::DockerToggle) {
        aliasCategory = QStringLiteral("dockers");
        aliasId = item.payload.value(QStringLiteral("docker_id")).toString();
        values = aliasFor(m_document, aliasCategory, aliasId);
    }

    const auto addText = [&](const QString &label, const QString &value) {
        textEdit = new QLineEdit(value, &dialog);
        form->addRow(label, textEdit);
    };
    const auto addAppearance = [&](const QString &defaultBackground, const QString &defaultForeground) {
        fontSize = new QSpinBox(&dialog);
        fontSize->setRange(6, 96);
        fontSize->setValue(compatibleInt(values, QStringLiteral("font_size"), QStringLiteral("fontSize"), 18));
        form->addRow(i18nc("@label:spinbox", "Font size:"), fontSize);
        background = createColorButton(QColor(compatibleString(values,
                                                               QStringLiteral("background_color"),
                                                               QStringLiteral("backgroundColor"),
                                                               defaultBackground)),
                                       &dialog);
        foreground = createColorButton(
            QColor(
                compatibleString(values, QStringLiteral("font_color"), QStringLiteral("fontColor"), defaultForeground)),
            &dialog);
        form->addRow(i18nc("@label", "Background color:"), background);
        form->addRow(i18nc("@label", "Text color:"), foreground);
    };
    const auto addPath = [&](const QString &label,
                             const QString &buttonText,
                             QLineEdit **field,
                             const QString &value,
                             const QString &filter,
                             bool browseBundledIcons = false) {
        auto *container = new QWidget(&dialog);
        auto *layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        *field = new QLineEdit(value, container);
        auto *browse = new QPushButton(buttonText, container);
        layout->addWidget(*field);
        layout->addWidget(browse);
        form->addRow(label, container);
        connect(browse, &QPushButton::clicked, &dialog, [field, filter, browseBundledIcons, &dialog]() {
            QString initialPath = (*field)->text();
            if (browseBundledIcons && !QFileInfo(initialPath).isAbsolute()) {
                const QString bundledPath = QDir(defaultIconsDirectory()).filePath(QFileInfo(initialPath).fileName());
                initialPath = QFileInfo::exists(bundledPath) ? bundledPath : defaultIconsDirectory();
            }
            const QString selected =
                QFileDialog::getOpenFileName(&dialog, i18nc("@title:window", "Select File"), initialPath, filter);
            if (selected.isEmpty())
                return;
            if (browseBundledIcons
                && QDir::cleanPath(QFileInfo(selected).absolutePath())
                    == QDir::cleanPath(QFileInfo(defaultIconsDirectory()).absoluteFilePath())) {
                (*field)->setText(QFileInfo(selected).fileName());
            } else {
                (*field)->setText(selected);
            }
        });
    };

    switch (item.type) {
    case QuickAccess::ItemType::Action:
    case QuickAccess::ItemType::DockerToggle:
        form->addRow(i18nc("@label", "Identifier:"), new QLabel(aliasId, &dialog));
        addText(i18nc("@label:textbox", "Button name:"),
                compatibleString(values, QStringLiteral("custom_name"), QStringLiteral("customName"), itemText(item)));
        addAppearance(item.type == QuickAccess::ItemType::Action ? QStringLiteral("#3a263f")
                                                                 : QStringLiteral("#263a2f"),
                      QStringLiteral("#ffffff"));
        addPath(i18nc("@label:textbox", "Icon:"),
                i18nc("@action:button", "Browse…"),
                &iconEdit,
                values.value(QStringLiteral("icon_name")).toString(),
                i18nc("@item:inlistbox", "Images (*.png *.jpg *.jpeg *.bmp *.gif);;All Files (*)"),
                true);
        break;
    case QuickAccess::ItemType::Label:
        addText(i18nc("@label:textbox", "Label name:"),
                item.payload.value(QStringLiteral("text")).toString(QStringLiteral("Label")));
        addAppearance(QStringLiteral("#263746"), QStringLiteral("#4fc3f7"));
        break;
    case QuickAccess::ItemType::Separator:
        valueSpin = new QSpinBox(&dialog);
        valueSpin->setRange(1, 50);
        valueSpin->setValue(qMax(1, item.payload.value(QStringLiteral("thickness")).toVariant().toInt()));
        if (!item.payload.contains(QStringLiteral("thickness")))
            valueSpin->setValue(2);
        form->addRow(i18nc("@label:spinbox", "Thickness:"), valueSpin);
        swatch =
            createColorButton(QColor(item.payload.value(QStringLiteral("color")).toString(QStringLiteral("#5a5a5a"))),
                              &dialog);
        form->addRow(i18nc("@label", "Color:"), swatch);
        break;
    case QuickAccess::ItemType::Color:
        swatch =
            createColorButton(QColor(item.payload.value(QStringLiteral("color")).toString(QStringLiteral("#ffffff"))),
                              &dialog);
        form->addRow(i18nc("@label", "Color:"), swatch);
        break;
    case QuickAccess::ItemType::BrushSize:
        valueSpin = new QSpinBox(&dialog);
        valueSpin->setRange(1, 10000);
        valueSpin->setValue(qMax(1, item.payload.value(QStringLiteral("text")).toString().toInt()));
        form->addRow(i18nc("@label:spinbox", "Size:"), valueSpin);
        addAppearance(QStringLiteral("#3a263f"), QStringLiteral("#ffffff"));
        break;
    case QuickAccess::ItemType::BrushBlendMode:
        addText(i18nc("@label:textbox", "Blending mode:"), item.payload.value(QStringLiteral("text")).toString());
        addAppearance(QStringLiteral("#263a3a"), QStringLiteral("#ffffff"));
        break;
    case QuickAccess::ItemType::Script:
        addText(i18nc("@label:textbox", "Button name:"),
                compatibleString(item.payload,
                                 QStringLiteral("custom_name"),
                                 QStringLiteral("customName"),
                                 i18n("Script")));
        addPath(i18nc("@label:textbox", "Script:"),
                i18nc("@action:button", "Browse…"),
                &scriptEdit,
                item.payload.value(QStringLiteral("script_path")).toString(),
                i18nc("@item:inlistbox", "Python scripts (*.py)"));
        addPath(i18nc("@label:textbox", "Icon:"),
                i18nc("@action:button", "Browse…"),
                &iconEdit,
                item.payload.value(QStringLiteral("icon_name")).toString(),
                i18nc("@item:inlistbox", "Images (*.png *.jpg *.jpeg *.bmp *.gif);;All Files (*)"),
                true);
        break;
    case QuickAccess::ItemType::Brush:
        return;
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    if (!aliasCategory.isEmpty()) {
        QJsonObject alias = values;
        alias.insert(QStringLiteral("custom_name"), textEdit->text().trimmed());
        alias.insert(QStringLiteral("font_size"), QString::number(fontSize->value()));
        alias.insert(QStringLiteral("background_color"), buttonColor(background).name(QColor::HexArgb));
        alias.insert(QStringLiteral("font_color"), buttonColor(foreground).name(QColor::HexArgb));
        alias.insert(QStringLiteral("icon_name"), iconEdit->text().trimmed());
        QJsonObject aliases = m_document.aliases.value(aliasCategory).toObject();
        aliases.insert(aliasId, alias);
        m_document.aliases.insert(aliasCategory, aliases);
    } else {
        if (textEdit)
            item.payload.insert(item.type == QuickAccess::ItemType::Script ? QStringLiteral("custom_name")
                                                                           : QStringLiteral("text"),
                                textEdit->text().trimmed());
        if (fontSize) {
            item.payload.insert(QStringLiteral("fontSize"), QString::number(fontSize->value()));
            item.payload.insert(QStringLiteral("backgroundColor"), buttonColor(background).name(QColor::HexArgb));
            item.payload.insert(QStringLiteral("fontColor"), buttonColor(foreground).name(QColor::HexArgb));
        }
        if (item.type == QuickAccess::ItemType::Separator) {
            item.payload.insert(QStringLiteral("thickness"), QString::number(valueSpin->value()));
            item.payload.insert(QStringLiteral("color"), buttonColor(swatch).name(QColor::HexArgb));
        } else if (item.type == QuickAccess::ItemType::Color) {
            item.payload.insert(QStringLiteral("color"), buttonColor(swatch).name(QColor::HexArgb));
        } else if (item.type == QuickAccess::ItemType::BrushSize) {
            item.payload.insert(QStringLiteral("text"), QString::number(valueSpin->value()));
        } else if (item.type == QuickAccess::ItemType::Script) {
            item.payload.insert(QStringLiteral("script_path"), scriptEdit->text().trimmed());
            item.payload.insert(QStringLiteral("icon_name"), iconEdit->text().trimmed());
        }
    }
    saveProfile();
    rebuildUi();
}

void QuickAccessDock::editBrushProperties(const QString &itemId)
{
    QuickAccess::Grid *grid = activeGrid();
    if (!grid)
        return;
    auto itemIt = std::find_if(grid->items.begin(), grid->items.end(), [&itemId](const QuickAccess::Item &item) {
        return item.id == itemId;
    });
    if (itemIt == grid->items.end() || itemIt->type != QuickAccess::ItemType::Brush)
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(i18nc("@title:window", "Quick Access Item Properties"));
    dialog.setMinimumWidth(320);
    auto *root = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    root->addLayout(form);
    auto *nameLabel = new QLabel(itemIt->payload.value(QStringLiteral("brush_name")).toString(), &dialog);
    form->addRow(i18nc("@label", "Brush:"), nameLabel);
    auto *display = new QComboBox(&dialog);
    display->addItem(i18nc("@item:inlistbox", "Stroke Preview"), BrushDisplayStroke);
    display->addItem(i18nc("@item:inlistbox", "Icon"), BrushDisplayIcon);
    const bool stroke = showsStrokePreview(*itemIt);
    display->setCurrentIndex(stroke ? 0 : 1);
    display->setToolTip(i18n("A stroke preview uses two cells; an icon uses one."));
    form->addRow(i18nc("@label:listbox", "Display:"), display);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const bool newStroke = display->currentData().toString() == BrushDisplayStroke;
    if (newStroke == stroke)
        return;
    // A stroke preview takes two cells, an icon one; neighbours are pushed
    // aside as when an item is resized in the grid editor.
    const int columnSpan = newStroke ? qMin(2, grid->columns) : 1;
    // The display decides the span when items are normalized, so it is set
    // before resizing.
    QList<QuickAccess::Item> items = grid->items;
    for (QuickAccess::Item &item : items) {
        if (item.id == itemId)
            item.payload.insert(BrushDisplayKey, newStroke ? BrushDisplayStroke : BrushDisplayIcon);
    }
    const QuickAccess::LayoutResult result =
        QuickAccess::LayoutEngine(grid->columns).resizeItem(items, itemId, 1, columnSpan);
    if (!result.isValid()) {
        QMessageBox::warning(this,
                             i18nc("@title:window", "Quick Access Layout"),
                             i18n("The item could not be resized in the active grid."));
        return;
    }
    grid->items = result.items;
    saveProfile();
    rebuildUi();
}

void QuickAccessDock::removeItem(const QString &itemId)
{
    if (itemId.isEmpty())
        return;
    QuickAccess::Tab *tab = activeTab();
    if (!tab)
        return;

    bool removed = false;
    for (QuickAccess::Grid &grid : tab->grids) {
        for (auto it = grid.items.begin(); it != grid.items.end(); ++it) {
            if (it->id == itemId) {
                grid.items.erase(it);
                removed = true;
                break;
            }
        }
        if (removed)
            break;
    }
    if (!removed)
        return;
    saveProfile();
    rebuildUi();
}

QString QuickAccessDock::profilePath() const
{
    return QDir(KoResourcePaths::saveLocation("data", "quickaccess/", true)).filePath(QStringLiteral("default.kqap"));
}

QString QuickAccessDock::legacyConfigPath() const
{
    return QDir(KoResourcePaths::getAppDataLocation()).filePath(QStringLiteral("quick_access_manager/remaster"));
}

QString QuickAccessDockFactory::id() const
{
    return QStringLiteral("QuickAccessPalette");
}

QDockWidget *QuickAccessDockFactory::createDockWidget()
{
    auto *dock = new QuickAccessDock();
    dock->setObjectName(id());
    return dock;
}

KoDockFactoryBase::DockPosition QuickAccessDockFactory::defaultDockPosition() const
{
    return DockMinimized;
}
