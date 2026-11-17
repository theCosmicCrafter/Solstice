/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSGESTUREPREVIEW_H
#define QUICKACCESSGESTUREPREVIEW_H
#include "QuickAccessModel.h"
#include <QFrame>
class QuickAccessGesturePreview : public QFrame
{
public:
    QuickAccessGesturePreview();
    void showPage(const QuickAccess::Document &document, const QJsonObject &page, const QPoint &position);
};
#endif
