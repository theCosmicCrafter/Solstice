/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSGRIDEDITOPERATIONS_H
#define QUICKACCESSGRIDEDITOPERATIONS_H

#include "QuickAccessModel.h"
#include <QSet>

namespace QuickAccessGridEditOperations
{
/// Move a selection as one bounded group, then place displaced items in visual
/// order. Input items must already fit the grid, as in the editor's document.
QList<QuickAccess::Item> moveGroup(const QList<QuickAccess::Item> &items,
                                   const QSet<QString> &selection,
                                   int rowDelta,
                                   int columnDelta,
                                   int columns);
} // namespace QuickAccessGridEditOperations

#endif
