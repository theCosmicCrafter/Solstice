/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSGRIDCANVAS_H
#define QUICKACCESSGRIDCANVAS_H

#include "QuickAccessModel.h"
#include <QWidget>
#include <QSet>

class QuickAccessGridEditDialog;
class QRubberBand;
class QFrame;

class QuickAccessGridCanvas : public QWidget
{
public:
    QuickAccessGridCanvas(QuickAccess::Document *document,
                          QuickAccess::Grid *grid,
                          int tabIndex,
                          QuickAccessGridEditDialog *dialog,
                          QWidget *parent = nullptr);

    void rebuild();

    void showDropHighlight(const QuickAccess::Item &item);

    void clearDropHighlight();

    void updateSelectionStyles();
    void updateStrokePreviews();
    QSet<QString> itemsInRect(const QRect &rect) const;

protected:
    void paintEvent(QPaintEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;

    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QuickAccess::Document *m_document;
    QuickAccess::Grid *m_grid;
    int m_tabIndex;
    QuickAccessGridEditDialog *m_dialog;
    QRubberBand *m_rubberBand{nullptr};
    QFrame *m_dropHighlight{nullptr};
    QPoint m_origin;
};

#endif
