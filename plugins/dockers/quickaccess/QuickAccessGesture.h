/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSGESTURE_H
#define QUICKACCESSGESTURE_H

#include "QuickAccessModel.h"

#include <QObject>
#include <QPoint>

#include <functional>

class QuickAccessGesturePreview;
class QKeyEvent;

class QuickAccessGestureController : public QObject
{
public:
    QuickAccessGestureController(QuickAccess::Document *document,
                                 std::function<void(const QuickAccess::Item &)> execute,
                                 QObject *parent = nullptr);
    ~QuickAccessGestureController() override;

    void reloadSettings();
    void cancelGesture();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QString eventKey(QKeyEvent *event) const;
    QJsonObject pageForKey(const QString &key) const;
    QString directionForDelta(const QPoint &delta) const;
    void executeConfig(const QJsonObject &config);

    QuickAccess::Document *m_document;
    std::function<void(const QuickAccess::Item &)> m_execute;
    bool m_enabled{true};
    bool m_showPreview{true};
    int m_threshold{20};
    QString m_activeKey;
    QJsonObject m_activePage;
    QPoint m_startPosition;
    QPoint m_lastPosition;
    QuickAccessGesturePreview *m_preview{nullptr};
};

#endif
