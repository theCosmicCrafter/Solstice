/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessDock.h"
#include "QuickAccessStrokePreviews.h"

#include "QuickAccessGesture.h"
#include "QuickAccessGestureDialog.h"
#include "QuickAccessItemDragController.h"
#include "QuickAccessGridEditDialog.h"
#include "QuickAccessItemExecutor.h"
#include "QuickAccessLayoutEngine.h"
#include "QuickAccessSettingsStore.h"
#include "QuickAccessProfileLoader.h"
#include "QuickAccessItemPresentation.h"
#include "QuickAccessItemProperties.h"
#include "QuickAccessProfileRepository.h"
#include "QuickAccessResourcesDialog.h"
#include "QuickAccessSettingsDialog.h"
#include "QuickAdjustDock.h"

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
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <utility>

using namespace QuickAccessItemPresentation;

QuickAccessDock::QuickAccessDock(QWidget *parent, bool enableGestures)
    : QDockWidget(i18nc("@title:window", "Quick Access Palette"), parent)
    , m_executor(new QuickAccess::ItemExecutor(this))
    , m_strokePreviews(new QuickAccessStrokePreviews(this))
    , m_popupMode(!enableGestures)
{
    m_itemDrag = new QuickAccessItemDragController(
        [this] {
            return activeGrid();
        },
        [this] {
            return cellSize();
        },
        [this](const QString &id, const QPoint &target) {
            moveItem(id, target);
        },
        this);
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

QuickAccessDock::~QuickAccessDock()
{
    // Cancel queued moves before document members and item widgets go away.
    delete m_itemDrag;
}

void QuickAccessDock::moveItem(const QString &id, const QPoint &target)
{
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
    QuickAccessSettingsStore settings;
    QuickAccessProfileLoader::load(&m_document,
                                   m_profilePath,
                                   legacyConfigPath(),
                                   settings,
                                   [this](const QString &title, const QString &message) {
                                       QMessageBox::warning(this, title, message);
                                   });
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
                normalizeIconItemSpans(&m_document);
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
    QuickAccessSettingsStore settings;
    const QuickAccess::Grid *grid = activeGrid();
    const auto current = settings.load(grid ? grid->columns : 8);

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

    settings.save(updated);
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
    const auto item = std::find_if(grid->items.cbegin(), grid->items.cend(), [&itemId](const QuickAccess::Item &item) {
        return item.id == itemId;
    });
    if (item == grid->items.cend())
        return;
    if (QuickAccessItemProperties::edit(m_document, *grid, itemId, itemText(*item), this)) {
        saveProfile();
        rebuildUi();
    }
}

void QuickAccessDock::editBrushProperties(const QString &itemId)
{
    QuickAccess::Grid *grid = activeGrid();
    if (grid && QuickAccessItemProperties::editBrush(*grid, itemId, this)) {
        saveProfile();
        rebuildUi();
    }
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
