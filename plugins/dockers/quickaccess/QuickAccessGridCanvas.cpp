/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessGridCanvas.h"
#include "QuickAccessGridEditDialog.h"
#include "QuickAccessGridItemButton.h"
#include "QuickAccessGridEditMetrics.h"

#include <QFrame>
#include <QMouseEvent>
#include <QPainter>
#include <QRubberBand>
#include <utility>

using namespace QuickAccessGridEditMetrics;

QuickAccessGridCanvas::QuickAccessGridCanvas(QuickAccess::Document *document,
                                             QuickAccess::Grid *grid,
                                             int tabIndex,
                                             QuickAccessGridEditDialog *dialog,
                                             QWidget *parent)
    : QWidget(parent)
    , m_document(document)
    , m_grid(grid)
    , m_tabIndex(tabIndex)
    , m_dialog(dialog)
{
    setAutoFillBackground(true);
    rebuild();
}

void QuickAccessGridCanvas::rebuild()
{
    const QObjectList directChildren = children();
    for (QObject *child : directChildren) {
        if (auto *button = dynamic_cast<QuickAccessGridItemButton *>(child))
            delete button;
    }
    int rows = VisibleRows;
    for (const QuickAccess::Item &item : std::as_const(m_grid->items))
        rows = qMax(rows, item.bottom() + 2);
    setMinimumSize(CanvasMargin * 2 + m_grid->columns * CellStep, CanvasMargin * 2 + rows * CellStep);
    resize(minimumSize());
    for (const QuickAccess::Item &item : std::as_const(m_grid->items)) {
        auto *button = new QuickAccessGridItemButton(*m_document, item, m_tabIndex, m_dialog, this);
        button->setGeometry(CanvasMargin + item.column * CellStep,
                            CanvasMargin + item.row * CellStep,
                            item.columnSpan * CellSize + qMax(0, item.columnSpan - 1) * CellSpacing,
                            item.rowSpan * CellSize + qMax(0, item.rowSpan - 1) * CellSpacing);
        button->show();
    }
    if (m_dropHighlight)
        m_dropHighlight->raise();
    update();
}

void QuickAccessGridCanvas::showDropHighlight(const QuickAccess::Item &item)
{
    if (!m_dropHighlight) {
        m_dropHighlight = new QFrame(this);
        m_dropHighlight->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_dropHighlight->setStyleSheet(
            QStringLiteral("QFrame { border: 2px solid #4fc3f7; background-color: rgba(79, 195, 247, 60); "
                           "border-radius: 3px; }"));
    }
    m_dropHighlight->setGeometry(CanvasMargin + item.column * CellStep,
                                 CanvasMargin + item.row * CellStep,
                                 item.columnSpan * CellSize + qMax(0, item.columnSpan - 1) * CellSpacing,
                                 item.rowSpan * CellSize + qMax(0, item.rowSpan - 1) * CellSpacing);
    m_dropHighlight->raise();
    m_dropHighlight->show();
}

void QuickAccessGridCanvas::clearDropHighlight()
{
    if (m_dropHighlight)
        m_dropHighlight->hide();
}

void QuickAccessGridCanvas::updateSelectionStyles()
{
    const QObjectList directChildren = children();
    for (QObject *child : directChildren) {
        if (auto *button = dynamic_cast<QuickAccessGridItemButton *>(child))
            button->updateSelectionStyle(*m_document);
    }
}

void QuickAccessGridCanvas::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);
    QPainter painter(this);
    painter.setPen(QPen(QColor(QStringLiteral("#3f3f3f")), 1));
    painter.setBrush(Qt::NoBrush);
    const int rows = qMax(VisibleRows, (height() - CanvasMargin * 2) / CellStep);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < m_grid->columns; ++column) {
            painter.drawRect(CanvasMargin + column * CellStep,
                             CanvasMargin + row * CellStep,
                             CellSize - 1,
                             CellSize - 1);
        }
    }
}

void QuickAccessGridCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_origin = event->pos();
    if (!m_rubberBand)
        m_rubberBand = new QRubberBand(QRubberBand::Rectangle, this);
    m_rubberBand->setGeometry(QRect(m_origin, QSize()));
    m_rubberBand->show();
}

void QuickAccessGridCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_rubberBand && !m_origin.isNull()) {
        m_rubberBand->setGeometry(QRect(m_origin, event->pos()).normalized());
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void QuickAccessGridCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_rubberBand) {
        const QRect selection = m_rubberBand->geometry();
        m_rubberBand->hide();
        m_origin = QPoint();
        m_dialog->selectItemsInRect(m_tabIndex, selection, event->modifiers().testFlag(Qt::ControlModifier));
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void QuickAccessGridCanvas::updateStrokePreviews()
{
    for (QObject *child : children()) {
        if (auto *button = dynamic_cast<QuickAccessGridItemButton *>(child))
            button->updateStrokePreview();
    }
}

QSet<QString> QuickAccessGridCanvas::itemsInRect(const QRect &rect) const
{
    QSet<QString> selected;
    const QObjectList directChildren = children();
    for (QObject *child : directChildren) {
        if (auto *button = dynamic_cast<QuickAccessGridItemButton *>(child)) {
            if (rect.intersects(button->geometry()))
                selected.insert(button->itemId());
        }
    }
    return selected;
}
