/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessGridEditDialog.h"
#include "QuickAccessGridCanvas.h"
#include "QuickAccessGridEditOperations.h"
#include "QuickAccessLayoutEngine.h"
#include "QuickAccessStrokePreviews.h"

#include <klocalizedstring.h>
#include <QDialogButtonBox>
#include <QMenu>
#include <QScrollArea>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

using QuickAccessGridEditOperations::moveGroup;

namespace
{
constexpr int MaximumHistory = 20;
}

QuickAccessGridEditDialog::QuickAccessGridEditDialog(const QuickAccess::Document &document, QWidget *parent)
    : QDialog(parent)
    , m_document(document)
    , m_strokePreviews(new QuickAccessStrokePreviews(this))
{
    QStringList strokeBrushes;
    for (const QuickAccess::Tab &tab : std::as_const(m_document.tabs)) {
        for (const QuickAccess::Grid &grid : tab.grids) {
            for (const QuickAccess::Item &item : grid.items) {
                if (item.type == QuickAccess::ItemType::Brush
                    && item.payload.value(QStringLiteral("display")).toString() == QStringLiteral("stroke"))
                    strokeBrushes << item.payload.value(QStringLiteral("brush_name")).toString();
            }
        }
    }
    m_strokePreviews->setNames(strokeBrushes);
    connect(m_strokePreviews, &QuickAccessStrokePreviews::changed, this, [this]() {
        for (QuickAccessGridCanvas *canvas : std::as_const(m_canvases)) {
            canvas->updateStrokePreviews();
        }
    });
    setWindowTitle(i18nc("@title:window", "Edit Grid"));
    resize(850, 620);
    auto *layout = new QVBoxLayout(this);

    m_undoButton = new QToolButton(this);
    m_undoButton->setIcon(QIcon(QStringLiteral(":/quickaccess/system_icons/undo.png")));
    if (m_undoButton->icon().isNull())
        m_undoButton->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    m_undoButton->setToolTip(i18nc("@action", "Undo"));
    m_undoButton->setFixedSize(32, 32);
    layout->addWidget(m_undoButton, 0, Qt::AlignLeft);

    m_tabs = new QTabWidget(this);
    m_selection.resize(m_document.tabs.size());
    m_history.resize(m_document.tabs.size());
    for (int tabIndex = 0; tabIndex < m_document.tabs.size(); ++tabIndex) {
        QuickAccess::Grid *grid = gridForTab(tabIndex);
        if (!grid)
            continue;
        auto *scrollArea = new QScrollArea(m_tabs);
        scrollArea->setWidgetResizable(false);
        scrollArea->setFrameShape(QFrame::NoFrame);
        auto *canvas = new QuickAccessGridCanvas(&m_document, grid, tabIndex, this, scrollArea);
        scrollArea->setWidget(canvas);
        m_canvases.append(canvas);
        m_tabs->addTab(scrollArea, m_document.tabs.at(tabIndex).name);
        if (m_document.tabs.at(tabIndex).id == m_document.activeTabId)
            m_tabs->setCurrentIndex(tabIndex);
    }
    layout->addWidget(m_tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(m_undoButton, &QToolButton::clicked, this, &QuickAccessGridEditDialog::undoCurrentTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, &QuickAccessGridEditDialog::currentTabChanged);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateUndoButton();
}

QuickAccess::Document QuickAccessGridEditDialog::document() const
{
    return m_document;
}

QuickAccess::Grid *QuickAccessGridEditDialog::gridForTab(int tabIndex)
{
    if (tabIndex < 0 || tabIndex >= m_document.tabs.size() || m_document.tabs[tabIndex].grids.isEmpty())
        return nullptr;
    return &m_document.tabs[tabIndex].grids.first();
}

const QuickAccess::Item *QuickAccessGridEditDialog::itemForId(int tabIndex, const QString &itemId) const
{
    if (tabIndex < 0 || tabIndex >= m_document.tabs.size() || m_document.tabs.at(tabIndex).grids.isEmpty())
        return nullptr;
    const auto &items = m_document.tabs.at(tabIndex).grids.first().items;
    for (const QuickAccess::Item &item : items) {
        if (item.id == itemId)
            return &item;
    }
    return nullptr;
}

void QuickAccessGridEditDialog::currentTabChanged(int index)
{
    if (index >= 0 && index < m_document.tabs.size())
        m_document.activeTabId = m_document.tabs.at(index).id;
    updateUndoButton();
}

void QuickAccessGridEditDialog::selectItem(int tabIndex, const QString &itemId, bool toggle, bool preserveGroup)
{
    if (tabIndex < 0 || tabIndex >= m_selection.size())
        return;
    QSet<QString> &selection = m_selection[tabIndex];
    if (toggle) {
        if (selection.contains(itemId))
            selection.remove(itemId);
        else
            selection.insert(itemId);
    } else if (!preserveGroup || !selection.contains(itemId)) {
        selection = {itemId};
    }
    if (tabIndex < m_canvases.size())
        m_canvases.at(tabIndex)->updateSelectionStyles();
}

void QuickAccessGridEditDialog::selectItemsInRect(int tabIndex, const QRect &rect, bool additive)
{
    if (tabIndex < 0 || tabIndex >= m_selection.size() || tabIndex >= m_canvases.size())
        return;
    QSet<QString> selected = additive ? m_selection.at(tabIndex) : QSet<QString>();
    selected.unite(m_canvases.at(tabIndex)->itemsInRect(rect));
    m_selection[tabIndex] = selected;
    if (tabIndex < m_canvases.size())
        m_canvases.at(tabIndex)->updateSelectionStyles();
}

bool QuickAccessGridEditDialog::isSelected(int tabIndex, const QString &itemId) const
{
    return tabIndex >= 0 && tabIndex < m_selection.size() && m_selection.at(tabIndex).contains(itemId);
}

void QuickAccessGridEditDialog::showMovePreview(int tabIndex, const QString &itemId, int rowDelta, int columnDelta)
{
    const QuickAccess::Item *source = itemForId(tabIndex, itemId);
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    if (!source || !grid || tabIndex < 0 || tabIndex >= m_canvases.size())
        return;
    QuickAccess::Item preview = *source;
    preview.row = qMax(0, source->row + rowDelta);
    preview.column = qBound(0, source->column + columnDelta, grid->columns - source->columnSpan);
    m_canvases.at(tabIndex)->showDropHighlight(preview);
}

void QuickAccessGridEditDialog::showResizePreview(int tabIndex, const QString &itemId, int rowDelta, int columnDelta)
{
    const QuickAccess::Item *source = itemForId(tabIndex, itemId);
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    if (!source || !grid || tabIndex < 0 || tabIndex >= m_canvases.size())
        return;
    if (!rowDelta && !columnDelta) {
        clearPreview(tabIndex);
        return;
    }
    QuickAccess::Item preview = *source;
    preview.rowSpan = qMax(1, source->rowSpan + rowDelta);
    preview.columnSpan = qBound(1, source->columnSpan + columnDelta, grid->columns - source->column);
    m_canvases.at(tabIndex)->showDropHighlight(preview);
}

void QuickAccessGridEditDialog::clearPreview(int tabIndex)
{
    if (tabIndex >= 0 && tabIndex < m_canvases.size())
        m_canvases.at(tabIndex)->clearDropHighlight();
}

void QuickAccessGridEditDialog::moveSelected(int tabIndex, int rowDelta, int columnDelta)
{
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    if (!grid || m_selection.value(tabIndex).isEmpty() || (!rowDelta && !columnDelta))
        return;
    pushUndo(tabIndex);
    grid->items = moveGroup(grid->items, m_selection.at(tabIndex), rowDelta, columnDelta, grid->columns);
    refreshCanvas(tabIndex);
}

void QuickAccessGridEditDialog::resizeItem(int tabIndex, const QString &itemId, int rowDelta, int columnDelta)
{
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    const QuickAccess::Item *item = itemForId(tabIndex, itemId);
    if (!grid || !item || (!rowDelta && !columnDelta))
        return;
    pushUndo(tabIndex);
    const QuickAccess::LayoutResult result = QuickAccess::LayoutEngine(grid->columns)
                                                 .resizeItem(grid->items,
                                                             itemId,
                                                             qMax(1, item->rowSpan + rowDelta),
                                                             qMax(1, item->columnSpan + columnDelta));
    if (result.isValid()) {
        grid->items = result.items;
    } else {
        m_history[tabIndex].removeLast();
    }
    refreshCanvas(tabIndex);
    updateUndoButton();
}

void QuickAccessGridEditDialog::showItemContextMenu(int tabIndex, const QString &itemId, const QPoint &globalPosition)
{
    if (!isSelected(tabIndex, itemId))
        selectItem(tabIndex, itemId, false, false);
    QMenu menu(this);
    QAction *remove = menu.addAction(QIcon(QStringLiteral(":/quickaccess/system_icons/circle-xmark.png")),
                                     i18nc("@action", "Remove"));
    QMenu *copyMenu = menu.addMenu(i18nc("@title:menu", "Copy to Tab"));
    QMenu *moveMenu = menu.addMenu(i18nc("@title:menu", "Move to Tab"));
    for (int target = 0; target < m_document.tabs.size(); ++target) {
        if (target == tabIndex)
            continue;
        QAction *copy = copyMenu->addAction(m_document.tabs.at(target).name);
        connect(copy, &QAction::triggered, this, [this, tabIndex, target]() {
            copyOrMoveSelected(tabIndex, target, false);
        });
        QAction *move = moveMenu->addAction(m_document.tabs.at(target).name);
        connect(move, &QAction::triggered, this, [this, tabIndex, target]() {
            copyOrMoveSelected(tabIndex, target, true);
        });
    }
    copyMenu->setEnabled(!copyMenu->actions().isEmpty());
    moveMenu->setEnabled(!moveMenu->actions().isEmpty());
    QAction *chosen = menu.exec(globalPosition);
    if (chosen == remove)
        removeSelected(tabIndex);
}

void QuickAccessGridEditDialog::removeSelected(int tabIndex)
{
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    if (!grid || m_selection.value(tabIndex).isEmpty())
        return;
    pushUndo(tabIndex);
    const QSet<QString> selected = m_selection.at(tabIndex);
    grid->items.erase(std::remove_if(grid->items.begin(),
                                     grid->items.end(),
                                     [&selected](const QuickAccess::Item &item) {
                                         return selected.contains(item.id);
                                     }),
                      grid->items.end());
    m_selection[tabIndex].clear();
    refreshCanvas(tabIndex);
}

void QuickAccessGridEditDialog::copyOrMoveSelected(int sourceTab, int targetTab, bool move)
{
    QuickAccess::Grid *source = gridForTab(sourceTab);
    QuickAccess::Grid *target = gridForTab(targetTab);
    if (!source || !target || m_selection.value(sourceTab).isEmpty())
        return;

    QList<QuickAccess::Item> copied;
    for (const QuickAccess::Item &item : std::as_const(source->items)) {
        if (m_selection.at(sourceTab).contains(item.id))
            copied.append(item);
    }
    if (copied.isEmpty())
        return;
    pushUndo(targetTab);
    if (move)
        pushUndo(sourceTab);

    int minimumRow = copied.first().row;
    int minimumColumn = copied.first().column;
    int targetRow = 0;
    for (const QuickAccess::Item &item : std::as_const(copied)) {
        minimumRow = qMin(minimumRow, item.row);
        minimumColumn = qMin(minimumColumn, item.column);
    }
    for (const QuickAccess::Item &item : std::as_const(target->items))
        targetRow = qMax(targetRow, item.bottom());

    QSet<QString> newSelection;
    for (QuickAccess::Item &item : copied) {
        item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        item.row = targetRow + item.row - minimumRow;
        item.column = item.column - minimumColumn;
        const QuickAccess::LayoutResult result =
            QuickAccess::LayoutEngine(target->columns).addItem(target->items, item);
        if (result.isValid()) {
            target->items = result.items;
            newSelection.insert(item.id);
        }
    }
    if (move) {
        const QSet<QString> oldSelection = m_selection.at(sourceTab);
        source->items.erase(std::remove_if(source->items.begin(),
                                           source->items.end(),
                                           [&oldSelection](const QuickAccess::Item &item) {
                                               return oldSelection.contains(item.id);
                                           }),
                            source->items.end());
        m_selection[sourceTab].clear();
        refreshCanvas(sourceTab);
    }
    m_selection[targetTab] = newSelection;
    refreshCanvas(targetTab);
    updateUndoButton();
}

void QuickAccessGridEditDialog::pushUndo(int tabIndex)
{
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    if (!grid || tabIndex < 0 || tabIndex >= m_history.size())
        return;
    m_history[tabIndex].append(grid->items);
    while (m_history[tabIndex].size() > MaximumHistory)
        m_history[tabIndex].removeFirst();
    updateUndoButton();
}

void QuickAccessGridEditDialog::undoCurrentTab()
{
    const int tabIndex = m_tabs->currentIndex();
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    if (!grid || tabIndex < 0 || tabIndex >= m_history.size() || m_history[tabIndex].isEmpty())
        return;
    grid->items = m_history[tabIndex].takeLast();
    m_selection[tabIndex].clear();
    refreshCanvas(tabIndex);
    updateUndoButton();
}

void QuickAccessGridEditDialog::refreshCanvas(int tabIndex)
{
    if (tabIndex >= 0 && tabIndex < m_canvases.size()) {
        QuickAccessGridCanvas *canvas = m_canvases.at(tabIndex);
        QTimer::singleShot(0, canvas, [canvas]() {
            canvas->rebuild();
        });
    }
}

void QuickAccessGridEditDialog::updateUndoButton()
{
    const int tabIndex = m_tabs ? m_tabs->currentIndex() : -1;
    m_undoButton->setEnabled(tabIndex >= 0 && tabIndex < m_history.size() && !m_history.at(tabIndex).isEmpty());
}
