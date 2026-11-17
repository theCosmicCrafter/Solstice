/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessItemDragController.h"
#include <QMouseEvent>
#include <QRubberBand>
#include <QTimer>
#include <QWidget>
#include <utility>

namespace
{
// Matches the palette's grid layout spacing.
constexpr int GridCellSpacing = 2;
} // namespace
QuickAccessItemDragController::QuickAccessItemDragController(
    std::function<QuickAccess::Grid *()> activeGrid,
    std::function<int()> cellSize,
    std::function<void(const QString &, const QPoint &)> commitMove,
    QObject *parent)
    : QObject(parent)
    , m_activeGrid(std::move(activeGrid))
    , m_cellSize(std::move(cellSize))
    , m_commitMove(std::move(commitMove))
{
}
QuickAccessItemDragController::~QuickAccessItemDragController()
{
    cancel();
}

void QuickAccessItemDragController::attach(QWidget *widget, const QuickAccess::Item &item)
{
    widget->setProperty("quickaccess_item_id", item.id);
    widget->installEventFilter(this);
}

QPoint QuickAccessItemDragController::dragTarget(const QuickAccess::Item &item, const QPoint &globalPosition) const
{
    const QuickAccess::Grid *grid = m_activeGrid();
    const int step = m_cellSize() + GridCellSpacing;
    const QPoint delta = globalPosition - m_dragStart;
    const int row = qMax(0, item.row + qRound(delta.y() / double(step)));
    const int column = qBound(0,
                              item.column + qRound(delta.x() / double(step)),
                              qMax(0, (grid ? grid->columns : 1) - item.columnSpan));
    return QPoint(column, row);
}

void QuickAccessItemDragController::cancel()
{
    if (m_dragHighlight)
        m_dragHighlight->deleteLater();
    m_dragHighlight = nullptr;
    m_dragItemId.clear();
}

bool QuickAccessItemDragController::eventFilter(QObject *watched, QEvent *event)
{
    // Ctrl + left-drag moves an item to another cell of its grid, as in the
    // original plugin. A plain click still activates the item; the Ctrl press
    // is swallowed so the item does not fire. Any other button cancels.
    auto *widget = qobject_cast<QWidget *>(watched);
    const QString itemId = widget ? widget->property("quickaccess_item_id").toString() : QString();
    if (itemId.isEmpty())
        return QObject::eventFilter(watched, event);
    auto findItem = [this](const QString &id) -> const QuickAccess::Item * {
        const QuickAccess::Grid *grid = m_activeGrid();
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
            cancel();
            return true;
        }
        if (mouse->button() != Qt::LeftButton || !(mouse->modifiers() & Qt::ControlModifier) || !findItem(itemId))
            return false;
        m_dragItemId = itemId;
        m_dragStart = mouse->globalPos();
        return true;
    }
    case QEvent::MouseMove: {
        if (m_dragItemId.isEmpty())
            return false;
        const QuickAccess::Item *item = findItem(m_dragItemId);
        QWidget *gridWidget = widget->parentWidget();
        if (!item || !gridWidget)
            return true;
        const QPoint target = dragTarget(*item, static_cast<QMouseEvent *>(event)->globalPos());
        if (!m_dragHighlight || m_dragHighlight->parentWidget() != gridWidget) {
            if (m_dragHighlight)
                m_dragHighlight->deleteLater();
            m_dragHighlight = new QRubberBand(QRubberBand::Rectangle, gridWidget);
        }
        const int size = m_cellSize();
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
            item ? dragTarget(*item, static_cast<QMouseEvent *>(event)->globalPos()) : QPoint();
        const bool moved = item && (target.y() != item->row || target.x() != item->column);
        cancel();
        if (moved) {
            // Deferred: the move rebuilds the pages, deleting this widget
            // while its release is still being delivered.
            QTimer::singleShot(0, this, [this, id, target]() {
                m_commitMove(id, target);
            });
        }
        return true;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}
