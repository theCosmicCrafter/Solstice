/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessGridEditOperations.h"

#include <algorithm>
#include <utility>

namespace
{
bool overlaps(const QuickAccess::Item &first, const QuickAccess::Item &second)
{
    return !(first.right() <= second.column || second.right() <= first.column || first.bottom() <= second.row
             || second.bottom() <= first.row);
}

bool overlapsAny(const QuickAccess::Item &item, const QList<QuickAccess::Item> &placed)
{
    return std::any_of(placed.cbegin(), placed.cend(), [&item](const QuickAccess::Item &other) {
        return overlaps(item, other);
    });
}

} // namespace

QList<QuickAccess::Item> QuickAccessGridEditOperations::moveGroup(const QList<QuickAccess::Item> &items,
                                                                  const QSet<QString> &selection,
                                                                  int rowDelta,
                                                                  int columnDelta,
                                                                  int columns)
{
    QList<QuickAccess::Item> moving;
    QList<QuickAccess::Item> rest;
    for (const QuickAccess::Item &item : items) {
        (selection.contains(item.id) ? moving : rest).append(item);
    }
    if (moving.isEmpty())
        return items;

    int minimumRow = moving.first().row;
    int minimumColumn = moving.first().column;
    int maximumRight = moving.first().right();
    for (const QuickAccess::Item &item : std::as_const(moving)) {
        minimumRow = qMin(minimumRow, item.row);
        minimumColumn = qMin(minimumColumn, item.column);
        maximumRight = qMax(maximumRight, item.right());
    }
    rowDelta = qMax(rowDelta, -minimumRow);
    columnDelta = qBound(-minimumColumn, columnDelta, columns - maximumRight);
    for (QuickAccess::Item &item : moving) {
        item.row += rowDelta;
        item.column += columnDelta;
    }

    std::sort(rest.begin(), rest.end(), [columns](const QuickAccess::Item &a, const QuickAccess::Item &b) {
        const int aIndex = a.row * columns + a.column;
        const int bIndex = b.row * columns + b.column;
        return aIndex == bIndex ? a.id < b.id : aIndex < bIndex;
    });
    QList<QuickAccess::Item> placed = moving;
    for (QuickAccess::Item item : std::as_const(rest)) {
        int cursor = qMax(0, item.row * columns + item.column);
        while (item.column + item.columnSpan > columns || overlapsAny(item, placed)) {
            ++cursor;
            item.row = cursor / columns;
            item.column = cursor % columns;
        }
        placed.append(item);
    }
    return placed;
}
