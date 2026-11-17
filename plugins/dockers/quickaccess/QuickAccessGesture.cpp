/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessGesture.h"
#include "QuickAccessGesturePreview.h"
#include <kconfiggroup.h>
#include <ksharedconfig.h>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTextEdit>
#include <QtMath>
#include <cmath>
#include <utility>

namespace
{
bool textInputHasFocus()
{
    QWidget *focus = QApplication::focusWidget();
    return qobject_cast<QLineEdit *>(focus) || qobject_cast<QTextEdit *>(focus) || qobject_cast<QPlainTextEdit *>(focus)
        || qobject_cast<QSpinBox *>(focus) || qobject_cast<QComboBox *>(focus);
}

} // namespace

QuickAccessGestureController::QuickAccessGestureController(QuickAccess::Document *document,
                                                           std::function<void(const QuickAccess::Item &)> execute,
                                                           QObject *parent)
    : QObject(parent)
    , m_document(document)
    , m_execute(std::move(execute))
{
    reloadSettings();
    qApp->installEventFilter(this);
}

QuickAccessGestureController::~QuickAccessGestureController()
{
    qApp->removeEventFilter(this);
    delete m_preview;
}

void QuickAccessGestureController::reloadSettings()
{
    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessGesture"));
    m_enabled = config.readEntry("Enabled", true);
    m_showPreview = config.readEntry("ShowPreview", true);
    m_threshold = qBound(1, config.readEntry("MinimumPixelsToMove", 20), 200);
    if (!m_enabled)
        cancelGesture();
}

void QuickAccessGestureController::cancelGesture()
{
    m_activeKey.clear();
    m_activePage = QJsonObject();
    if (m_preview)
        m_preview->hide();
}

bool QuickAccessGestureController::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched)
    if (!m_enabled || !m_document || QApplication::activeModalWidget())
        return false;
    if (event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->isAutoRepeat() || textInputHasFocus())
            return false;
        const QString key = eventKey(keyEvent);
        const QJsonObject page = pageForKey(key);
        if (!page.isEmpty()) {
            m_activeKey = key;
            m_activePage = page;
            m_startPosition = QCursor::pos();
            m_lastPosition = m_startPosition;
            if (m_showPreview) {
                if (!m_preview)
                    m_preview = new QuickAccessGesturePreview;
                m_preview->showPage(*m_document, page, m_startPosition);
            }
        }
    } else if (event->type() == QEvent::MouseMove && !m_activeKey.isEmpty()) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        m_lastPosition = mouseEvent->globalPos();
    } else if (event->type() == QEvent::KeyRelease && !m_activeKey.isEmpty()) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->isAutoRepeat() || eventKey(keyEvent) != m_activeKey)
            return false;
        const QPoint delta = m_lastPosition - m_startPosition;
        const qreal distance = std::hypot(qreal(delta.x()), qreal(delta.y()));
        const QString direction = distance < m_threshold ? QStringLiteral("center") : directionForDelta(delta);
        executeConfig(m_activePage.value(direction).toObject());
        cancelGesture();
    }
    return false;
}

QString QuickAccessGestureController::eventKey(QKeyEvent *event) const
{
    QString key = event->text().toUpper();
    if (key.isEmpty() && event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F12)
        key = QStringLiteral("F%1").arg(event->key() - Qt::Key_F1 + 1);
    return key;
}

QJsonObject QuickAccessGestureController::pageForKey(const QString &key) const
{
    if (key.isEmpty())
        return {};
    for (const QJsonValue &value : m_document->gesturePages) {
        const QJsonObject page = value.toObject();
        if (page.value(QStringLiteral("gesture_key")).toString().compare(key, Qt::CaseInsensitive) == 0)
            return page;
    }
    return {};
}

QString QuickAccessGestureController::directionForDelta(const QPoint &delta) const
{
    qreal angle = qRadiansToDegrees(std::atan2(qreal(-delta.y()), qreal(delta.x())));
    if (angle < 0)
        angle += 360;
    if (angle >= 337.5 || angle < 22.5)
        return QStringLiteral("right");
    if (angle < 67.5)
        return QStringLiteral("right_up");
    if (angle < 112.5)
        return QStringLiteral("up");
    if (angle < 157.5)
        return QStringLiteral("left_up");
    if (angle < 202.5)
        return QStringLiteral("left");
    if (angle < 247.5)
        return QStringLiteral("left_down");
    if (angle < 292.5)
        return QStringLiteral("down");
    return QStringLiteral("right_down");
}

void QuickAccessGestureController::executeConfig(const QJsonObject &config)
{
    if (config.isEmpty())
        return;
    QuickAccess::Item item;
    item.id = QStringLiteral("gesture");
    const QString type = config.value(QStringLiteral("gesture_type")).toString();
    const QJsonObject parameters = config.value(QStringLiteral("parameters")).toObject();
    if (type == QStringLiteral("action")) {
        item.type = QuickAccess::ItemType::Action;
        item.payload.insert(QStringLiteral("action_id"), parameters.value(QStringLiteral("action_id")));
    } else if (type == QStringLiteral("brush")) {
        item.type = QuickAccess::ItemType::Brush;
        item.payload.insert(QStringLiteral("brush_name"), parameters.value(QStringLiteral("brush_name")));
    } else if (type == QStringLiteral("docker_toggle")) {
        item.type = QuickAccess::ItemType::DockerToggle;
        item.payload.insert(QStringLiteral("docker_id"), parameters.value(QStringLiteral("docker_name")));
    } else {
        return;
    }
    m_execute(item);
}
