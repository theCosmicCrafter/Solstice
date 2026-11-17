/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSITEMDRAGCONTROLLER_H
#define QUICKACCESSITEMDRAGCONTROLLER_H
#include "QuickAccessModel.h"
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <functional>
class QWidget;
class QRubberBand;
// Per-palette mouse state. The dock owns the document and commits moves.
class QuickAccessItemDragController : public QObject
{
public:
    QuickAccessItemDragController(std::function<QuickAccess::Grid *()> activeGrid,
                                  std::function<int()> cellSize,
                                  std::function<void(const QString &, const QPoint &)> commitMove,
                                  QObject *parent);
    ~QuickAccessItemDragController() override;
    void attach(QWidget *widget, const QuickAccess::Item &item);
    void cancel();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QPoint dragTarget(const QuickAccess::Item &item, const QPoint &globalPosition) const;
    std::function<QuickAccess::Grid *()> m_activeGrid;
    std::function<int()> m_cellSize;
    std::function<void(const QString &, const QPoint &)> m_commitMove;
    QString m_dragItemId;
    QPoint m_dragStart;
    QPointer<QRubberBand> m_dragHighlight;
};
#endif
