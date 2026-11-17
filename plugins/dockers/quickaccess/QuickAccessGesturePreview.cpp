/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessGesturePreview.h"
#include "QuickAccessGesturePresentation.h"
#include <QGridLayout>
#include <QToolButton>
#include <QTimer>

using namespace QuickAccessGesturePresentation;
QuickAccessGesturePreview::QuickAccessGesturePreview()
    : QFrame(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus)
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setStyleSheet(
        QStringLiteral("QFrame { background: rgba(35,35,35,225); border: 1px solid #666; "
                       "border-radius: 5px; } QToolButton { color: white; padding: 3px; }"));
}
void QuickAccessGesturePreview::showPage(const QuickAccess::Document &document,
                                         const QJsonObject &page,
                                         const QPoint &position)
{
    delete this->layout();
    const auto cells = this->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly);
    qDeleteAll(cells);
    auto *layout = new QGridLayout(this);
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(2);
    for (int index = 0; index < Directions.size(); ++index) {
        const QJsonObject config = page.value(Directions.at(index)).toObject();
        const QString value = configuredValue(config);
        const QIcon icon = gestureIcon(document, config);
        auto *cell = new QToolButton(this);
        cell->setFocusPolicy(Qt::NoFocus);
        cell->setAttribute(Qt::WA_TransparentForMouseEvents);
        cell->setMinimumSize(72, 42);
        cell->setMaximumWidth(110);
        cell->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        cell->setToolTip(value);
        if (icon.isNull()) {
            cell->setText(value.isEmpty() ? QStringLiteral("·") : value);
            cell->setToolButtonStyle(Qt::ToolButtonTextOnly);
        } else {
            cell->setIcon(icon);
            cell->setIconSize(QSize(36, 36));
            cell->setToolButtonStyle(Qt::ToolButtonIconOnly);
        }
        layout->addWidget(cell, index / 3, index % 3);
    }
    layout->activate();
    const QSize previewSize = layout->sizeHint();
    const QPoint topLeft = position - QPoint(previewSize.width() / 2, previewSize.height() / 2);
    this->setGeometry(QRect(topLeft, previewSize));
    this->show();
    this->move(topLeft);
    this->raise();
    QTimer::singleShot(0, this, [this, topLeft] {
        if (this->isVisible())
            this->move(topLeft);
    });
}
