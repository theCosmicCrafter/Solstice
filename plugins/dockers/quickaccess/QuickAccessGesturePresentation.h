/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSGESTUREPRESENTATION_H
#define QUICKACCESSGESTUREPRESENTATION_H
#include "QuickAccessModel.h"
#include <QIcon>
namespace QuickAccessGesturePresentation
{
extern const QStringList Directions;
QString configuredValue(const QJsonObject &config);
QIcon gestureIcon(const QuickAccess::Document &document, const QJsonObject &config);
} // namespace QuickAccessGesturePresentation
#endif
