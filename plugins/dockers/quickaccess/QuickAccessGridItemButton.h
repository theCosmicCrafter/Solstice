/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSGRIDITEMBUTTON_H
#define QUICKACCESSGRIDITEMBUTTON_H

#include "QuickAccessModel.h"
#include <QPushButton>

class QuickAccessGridEditDialog;

class QuickAccessGridItemButton : public QPushButton
{
public:
    QuickAccessGridItemButton(const QuickAccess::Document &document,
                              const QuickAccess::Item &item,
                              int tabIndex,
                              QuickAccessGridEditDialog *dialog,
                              QWidget *parent);

    bool isStrokePreview() const;

    QSize strokePreviewSize() const;

    /// Shows the stroke preview (an empty preview area until it is available).
    void updateStrokePreview();

    QString itemId() const;

    void updateSelectionStyle(const QuickAccess::Document &document);

protected:
    void paintEvent(QPaintEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;

    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    enum DragMode {
        None,
        Move,
        Resize
    };

    bool isResizable() const;

    bool resizeVertically() const;

    bool onResizeHandle(const QPoint &position) const;

    QString iconPath() const;

    void configureAppearance(const QuickAccess::Document &document);

    QuickAccess::Item m_item;
    int m_tabIndex;
    QuickAccessGridEditDialog *m_dialog;
    QPoint m_dragStart;
    DragMode m_dragMode{None};
};

#endif
