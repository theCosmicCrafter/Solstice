/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessDock.h"
#include "QuickAccessItemPresentation.h"
#include "QuickAdjustDock.h"
#include "QuickColorSelectorDock.h"
#include <kis_canvas2.h>
#include <kconfiggroup.h>
#include <ksharedconfig.h>
#include <klocalizedstring.h>
#include <QAction>
#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QShortcut>
#include <QToolButton>
#include <QVBoxLayout>

using namespace QuickAccessItemPresentation;
namespace
{
QPoint globalMousePosition(const QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

class PopupDragBar : public QWidget
{
public:
    explicit PopupDragBar(QWidget *target, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_target(target)
    {
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_target) {
            m_dragOffset = globalMousePosition(event) - m_target->frameGeometry().topLeft();
            m_dragging = true;
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragging && (event->buttons() & Qt::LeftButton) && m_target) {
            m_target->move(globalMousePosition(event) - m_dragOffset);
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_dragging) {
            m_dragging = false;
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QPointer<QWidget> m_target;
    QPoint m_dragOffset;
    bool m_dragging{false};
};

class QuickColorPopup : public QFrame
{
public:
    using QFrame::QFrame;

protected:
    void leaveEvent(QEvent *event) override
    {
        QFrame::leaveEvent(event);

        // A combo box opens its list as a separate Qt::Popup window. Moving
        // the pointer into that window sends Leave to this popup even though
        // the user is still interacting with one of its child controls.
        if (QApplication::activePopupWidget())
            return;

        close();
    }
};

template<typename T>
T *findDock(const QString &objectName)
{
    for (QWidget *widget : QApplication::allWidgets()) {
        if (auto *dock = qobject_cast<T *>(widget)) {
            if (dock->objectName() == objectName)
                return dock;
        }
    }
    return nullptr;
}

void moveDockToCursor(QDockWidget *dock)
{
    if (!dock)
        return;
    if (dock->isFloating()) {
        dock->setFloating(false);
        dock->show();
        return;
    }
    dock->show();
    dock->setFloating(true);
    dock->raise();
    dock->move(QCursor::pos() - QPoint(dock->width() / 2, 24));
}

} // namespace

void QuickAccessDock::buildPopupHeader(QWidget *root)
{
    auto *bar = new PopupDragBar(this, root);
    auto *header = new QHBoxLayout(bar);
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(2);
    header->addStretch();

    m_popupPinButton = new QToolButton(bar);
    m_popupPinButton->setCheckable(true);
    m_popupPinButton->setAutoRaise(true);
    const QString unpinnedIcon = bundledIconPath(QStringLiteral("system_icons"), QStringLiteral("pin_unpinned.png"));
    m_popupPinButton->setIcon(unpinnedIcon.isEmpty() ? QIcon::fromTheme(QStringLiteral("window-pin"))
                                                     : QIcon(unpinnedIcon));
    if (m_popupPinButton->icon().isNull())
        m_popupPinButton->setText(QStringLiteral("P"));
    m_popupPinButton->setToolTip(i18nc("@info:tooltip", "Keep popup open after selecting an item"));
    m_popupPinButton->setFixedSize(22, 22);
    m_popupPinButton->setIconSize(QSize(16, 16));
    connect(m_popupPinButton, &QToolButton::toggled, this, &QuickAccessDock::setPopupPinned);
    header->addWidget(m_popupPinButton);

    auto *closeButton = new QToolButton(bar);
    closeButton->setAutoRaise(true);
    const QString closeIcon = bundledIconPath(QStringLiteral("system_icons"), QStringLiteral("circle-xmark.png"));
    closeButton->setIcon(closeIcon.isEmpty() ? QIcon::fromTheme(QStringLiteral("window-close")) : QIcon(closeIcon));
    if (closeButton->icon().isNull())
        closeButton->setText(QStringLiteral("×"));
    closeButton->setToolTip(i18nc("@info:tooltip", "Close"));
    closeButton->setFixedSize(22, 22);
    closeButton->setIconSize(QSize(16, 16));
    connect(closeButton, &QToolButton::clicked, this, &QWidget::close);
    header->addWidget(closeButton);

    if (auto *layout = qobject_cast<QVBoxLayout *>(root->layout()))
        layout->addWidget(bar);
}

void QuickAccessDock::showPalettePopup()
{
    if (m_palettePopup) {
        m_palettePopup->close();
        return;
    }
    if (!m_canvas)
        return;
    auto *popup = new QuickAccessDock(nullptr, false);
    popup->setObjectName(QStringLiteral("QuickAccessPalettePopup"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    popup->setCanvas(m_canvas);
    if (m_palettePopupAction) {
        for (const QKeySequence &sequence : m_palettePopupAction->shortcuts()) {
            if (sequence.isEmpty())
                continue;
            auto *shortcut = new QShortcut(sequence, popup);
            connect(shortcut, &QShortcut::activated, popup, &QWidget::close);
        }
    }
    popup->adjustSize();
    popup->resize(popup->sizeHint());
    popup->move(QCursor::pos() - QPoint(popup->width() / 4, popup->height() / 3));
    m_palettePopup = popup;
    popup->show();
}

void QuickAccessDock::showColorPopup()
{
    const KConfigGroup featureConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccess"));
    if (!featureConfig.readEntry("HueSVCEnabled", true))
        return;
    if (m_colorPopup) {
        m_colorPopup->close();
        return;
    }
    if (!m_canvas)
        return;
    auto *popup = new QuickColorPopup(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QHBoxLayout(popup);
    layout->setContentsMargins(6, 6, 6, 6);
    auto *selector = new QuickColorSelectorWidget(popup);
    selector->setCanvas(m_canvas);
    layout->addWidget(selector, 1);

    const KConfigGroup hueConfig = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessHueSVC"));
    QuickAdjustDock *adjust = nullptr;
    if (featureConfig.readEntry("QuickAdjustEnabled", true)) {
        adjust = new QuickAdjustDock(popup, true);
        adjust->setFeatures(QDockWidget::NoDockWidgetFeatures);
        adjust->setTitleBarWidget(new QWidget(adjust));
        adjust->setFixedWidth(qBound(160, hueConfig.readEntry("ControlsPanelWidth", 220), 600));
        adjust->setCanvas(m_canvas);
        layout->addWidget(adjust, 0, Qt::AlignVCenter);
    }
    const int selectorWidth = qBound(200, hueConfig.readEntry("PopupWidth", 350), 1200);
    popup->resize(selectorWidth + (adjust ? adjust->maximumWidth() : 0),
                  qBound(200, hueConfig.readEntry("PopupHeight", 600), 1200));
    popup->move(QCursor::pos() - QPoint(popup->width() / 4, popup->height() / 3));
    m_colorPopup = popup;
    popup->show();
    popup->raise();
    popup->activateWindow();
}

void QuickAccessDock::movePaletteDockerToCursor()
{
    moveDockToCursor(this);
}

void QuickAccessDock::moveAdjustDockerToCursor()
{
    moveDockToCursor(findDock<QuickAdjustDock>(QStringLiteral("brush_adjust_docker")));
}

void QuickAccessDock::setPopupPinned(bool pinned)
{
    if (!m_popupMode || m_popupPinned == pinned)
        return;
    m_popupPinned = pinned;
    if (m_popupPinButton) {
        const QString iconName = pinned ? QStringLiteral("pin_pinned.png") : QStringLiteral("pin_unpinned.png");
        const QString iconPath = bundledIconPath(QStringLiteral("system_icons"), iconName);
        m_popupPinButton->setIcon(iconPath.isEmpty() ? QIcon::fromTheme(QStringLiteral("window-pin"))
                                                     : QIcon(iconPath));
        m_popupPinButton->setToolTip(pinned ? i18nc("@info:tooltip", "Allow popup to close after selecting an item")
                                            : i18nc("@info:tooltip", "Keep popup open after selecting an item"));
    }
    const QPoint position = pos();
    setWindowFlag(Qt::Popup, !pinned);
    setWindowFlag(Qt::Tool, pinned);
    setWindowFlag(Qt::FramelessWindowHint, true);
    move(position);
    show();
    raise();
}
