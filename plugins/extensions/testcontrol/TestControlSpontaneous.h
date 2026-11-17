/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TESTCONTROLSPONTANEOUS_H
#define TESTCONTROLSPONTANEOUS_H

#include <QCoreApplication>
#include <QtTest/qtestspontaneevent.h>

namespace TestControlSpontaneous
{
/**
 * Delivers @p event to @p receiver as if it came from the window system.
 *
 * Tools treat a non-spontaneous release as a cancellation (for example
 * KisToolRectangleBase and KisToolOutlineBase, which cannot tell it from the
 * release sent when the canvas loses focus). The event is marked as QTest
 * does; the header is inline only, so the plugin does not link QtTest. It is
 * delivered through notify() like QTest: QCoreApplication::sendEvent() clears
 * the spontaneous flag in Qt 6.
 */
inline bool send(QObject *receiver, QEvent *event)
{
    QSpontaneKeyEvent::setSpontaneous(event);
    return QCoreApplication::instance()->notify(receiver, event);
}
} // namespace TestControlSpontaneous

#endif // TESTCONTROLSPONTANEOUS_H
