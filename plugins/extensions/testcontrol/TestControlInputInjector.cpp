/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestControlInputInjector.h"
#include "TestControlSpontaneous.h"
#include "TestControlTiming.h"

#include <QCoreApplication>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QTabletEvent>

#include <QKeyEvent>

#include <algorithm>
#include <iterator>
#include <utility>

namespace
{
const std::pair<Qt::KeyboardModifier, Qt::Key> ModifierKeys[] = {
    {Qt::ControlModifier, Qt::Key_Control},
    {Qt::ShiftModifier, Qt::Key_Shift},
    {Qt::AltModifier, Qt::Key_Alt},
    {Qt::MetaModifier, Qt::Key_Meta},
};

Qt::KeyboardModifier modifierForKey(Qt::Key key)
{
    for (const auto &entry : ModifierKeys) {
        if (entry.second == key) {
            return entry.first;
        }
    }
    return Qt::NoModifier;
}

} // namespace

TestControlInputInjector::TestControlInputInjector(QObject *parent)
    : QObject(parent)
{
    // A stylus that exists only for synthetic events; it is not registered
    // with the platform, so it never receives real input.
    m_stylus = new QPointingDevice(QStringLiteral("Solstice test control stylus"),
                                   0x5501,
                                   QInputDevice::DeviceType::Stylus,
                                   QPointingDevice::PointerType::Pen,
                                   QInputDevice::Capability::Position | QInputDevice::Capability::Pressure
                                       | QInputDevice::Capability::XTilt | QInputDevice::Capability::YTilt
                                       | QInputDevice::Capability::Hover,
                                   1,
                                   3,
                                   QString(),
                                   QPointingDeviceUniqueId::fromNumericId(0x5501),
                                   this);

    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        if (!m_busy) {
            return;
        }
        if (!m_target) {
            finish(QStringLiteral("The target widget was destroyed during the stroke"));
            return;
        }
        sendStep(m_steps[m_nextStep++]);
        scheduleNext();
    });

    m_clock.start();
}

TestControlInputInjector::~TestControlInputInjector() = default;

bool TestControlInputInjector::isBusy() const
{
    return m_busy;
}

void TestControlInputInjector::playStroke(QWidget *target,
                                          Device device,
                                          Qt::MouseButton button,
                                          const QVector<Point> &points,
                                          const Options &options,
                                          Done done)
{
    Report report;
    if (m_busy) {
        report.error = QStringLiteral("Another stroke is still playing");
    } else if (!target) {
        report.error = QStringLiteral("No target widget");
    } else if (points.isEmpty()) {
        report.error = QStringLiteral("A stroke needs at least one point");
    } else if (!std::is_sorted(points.cbegin(), points.cend(), [](const Point &a, const Point &b) {
                   return a.timeMs < b.timeMs;
               })) {
        report.error = QStringLiteral("Point times must not decrease");
    } else if (options.doubleClick && (device != Device::Mouse || points.size() != 1)) {
        report.error = QStringLiteral("A double click needs the mouse and exactly one point");
    }
    if (!report.error.isEmpty()) {
        done(report);
        return;
    }

    m_target = target;
    m_device = device;
    m_button = button;
    m_modifiers = options.modifiers;
    m_heldModifiers = Qt::NoModifier;
    m_points = points;
    m_realtime = options.realtime;
    m_releaseSettleMs = options.releaseSettleMs;
    m_done = std::move(done);
    m_report = Report();
    m_nextStep = 0;
    m_busy = true;

    const int last = points.size() - 1;
    m_steps.clear();
    // The pointer enters the canvas first, as with a real device. The
    // shortcut matcher suppresses every action after a Leave (for example
    // when another window covered the pointer), until the next Enter.
    m_steps.append({QEvent::Enter, 0});
    for (const auto &entry : ModifierKeys) {
        if (m_modifiers.testFlag(entry.first)) {
            m_steps.append({QEvent::ShortcutOverride, 0, false, false, entry.second});
            m_steps.append({QEvent::KeyPress, 0, false, false, entry.second});
        }
    }
    if (device == Device::Mouse) {
        m_steps.append({QEvent::MouseMove, 0, false, true});
        m_steps.append({QEvent::MouseButtonPress, 0});
        for (int i = 1; i <= last; ++i) {
            m_steps.append({QEvent::MouseMove, i});
        }
        m_steps.append({QEvent::MouseButtonRelease, last});
        if (options.doubleClick) {
            m_steps.append({QEvent::MouseButtonDblClick, 0});
            m_steps.append({QEvent::MouseButtonRelease, 0});
        }
    } else {
        m_steps.append({QEvent::TabletEnterProximity, 0});
        m_steps.append({QEvent::TabletMove, 0, false, true});
        m_steps.append({QEvent::TabletPress, 0});
        m_steps.append({QEvent::MouseButtonPress, 0, true});
        for (int i = 1; i <= last; ++i) {
            m_steps.append({QEvent::TabletMove, i});
        }
        m_steps.append({QEvent::TabletRelease, last});
        m_steps.append({QEvent::MouseButtonRelease, last, true});
        m_steps.append({QEvent::TabletLeaveProximity, last});
    }
    if (options.keyAfterRelease != Qt::Key(0)) {
        m_steps.append({QEvent::ShortcutOverride, last, false, false, options.keyAfterRelease});
        m_steps.append({QEvent::KeyPress, last, false, false, options.keyAfterRelease});
        m_steps.append({QEvent::KeyRelease, last, false, false, options.keyAfterRelease});
    }
    for (auto it = std::rbegin(ModifierKeys); it != std::rend(ModifierKeys); ++it) {
        if (m_modifiers.testFlag(it->first)) {
            m_steps.append({QEvent::KeyRelease, last, false, false, it->second});
        }
    }
    if (last > 0) {
        bool released = false;
        for (Step &step : m_steps) {
            released = released || step.type == QEvent::MouseButtonRelease || step.type == QEvent::TabletRelease;
            step.afterMoves = released;
        }
    }

    m_startMs = m_clock.elapsed();
    // Event timestamps are milliseconds on a monotonic clock; keep them
    // away from zero, which some code treats as "unknown".
    m_timestampBase = ulong(m_startMs) + 1000;
    scheduleNext();
}

qint64 TestControlInputInjector::stepOffsetMs(const Step &step) const
{
    if (step.hover || step.type == QEvent::Enter || step.type == QEvent::TabletEnterProximity
        || step.type == QEvent::ShortcutOverride || step.type == QEvent::KeyPress) {
        return 0;
    }
    return step.afterMoves ? HoverSettleMs + m_releaseSettleMs : HoverSettleMs;
}

void TestControlInputInjector::scheduleNext()
{
    if (m_nextStep >= m_steps.size()) {
        finish(QString());
        return;
    }

    const Step &step = m_steps[m_nextStep];
    qint64 due = -1;
    if (m_realtime) {
        due = m_startMs + stepOffsetMs(step) + m_points[step.pointIndex].timeMs;
    } else if (m_nextStep > 0 && m_steps[m_nextStep - 1].hover) {
        // Without real time, only the press after the hover move waits.
        due = m_startMs + HoverSettleMs;
    }
    const qint64 delay = due < 0 ? 0 : std::max<qint64>(0, due - m_clock.elapsed());
    m_timer.start(int(delay));
}

void TestControlInputInjector::sendStep(const Step &step)
{
    const Point &point = m_points[step.pointIndex];
    const QPointF global = m_target->mapToGlobal(point.position);
    const QPointF scene = m_target->window() ? m_target->window()->mapFromGlobal(global) : point.position;
    const ulong timestamp = m_timestampBase + ulong(stepOffsetMs(step) + point.timeMs);

    const bool isPress = step.type == QEvent::MouseButtonPress || step.type == QEvent::TabletPress
        || step.type == QEvent::MouseButtonDblClick;
    const bool isRelease = step.type == QEvent::MouseButtonRelease || step.type == QEvent::TabletRelease;
    const Qt::MouseButton button = (isPress || isRelease) ? m_button : Qt::NoButton;
    const Qt::MouseButtons buttons =
        (isRelease || step.hover) ? Qt::MouseButtons(Qt::NoButton) : Qt::MouseButtons(m_button);
    if (!step.synthesizedMouse) {
        if (isPress && m_report.pressedAtMs < 0) {
            m_report.pressedAtMs = TestControlTiming::clockMs();
        } else if (isRelease) {
            m_report.releasedAtMs = TestControlTiming::clockMs();
        }
    }

    switch (step.type) {
    case QEvent::ShortcutOverride:
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        // As from the platform, a modifier key's own event carries the state
        // before it; QKeyEvent::modifiers() toggles that key's flag, so
        // receivers see the state after the event.
        const Qt::KeyboardModifier modifier = modifierForKey(step.key);
        const bool isKeyRelease = step.type == QEvent::KeyRelease;
        const Qt::KeyboardModifiers before =
            isKeyRelease ? (m_heldModifiers | modifier) : (m_heldModifiers & ~modifier);
        QKeyEvent event(step.type, step.key, before);
        event.setTimestamp(timestamp);
        TestControlSpontaneous::send(m_target, &event);
        if (isKeyRelease) {
            m_heldModifiers &= ~modifier;
        } else if (step.type == QEvent::KeyPress) {
            m_heldModifiers |= modifier;
        }
        break;
    }
    case QEvent::Enter: {
        QEnterEvent event(point.position, scene, global);
        TestControlSpontaneous::send(m_target, &event);
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease: {
        QMouseEvent event(step.type,
                          point.position,
                          scene,
                          global,
                          button,
                          buttons,
                          m_modifiers,
                          step.synthesizedMouse ? Qt::MouseEventSynthesizedBySystem : Qt::MouseEventNotSynthesized);
        event.setTimestamp(timestamp);
        TestControlSpontaneous::send(m_target, &event);
        if (!step.synthesizedMouse) {
            if (step.type == QEvent::MouseButtonPress) {
                m_report.pressAccepted = event.isAccepted();
            } else if (isRelease) {
                m_report.releaseAccepted = event.isAccepted();
            }
        }
        break;
    }
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease: {
        QTabletEvent event(step.type,
                           m_stylus,
                           point.position,
                           global,
                           (isRelease || step.hover) ? 0.0 : point.pressure,
                           float(point.xTilt),
                           float(point.yTilt),
                           0.0f,
                           0.0,
                           0.0f,
                           m_modifiers,
                           button,
                           buttons);
        event.setTimestamp(timestamp);
        TestControlSpontaneous::send(m_target, &event);
        if (isPress) {
            m_report.pressAccepted = event.isAccepted();
        } else if (isRelease) {
            m_report.releaseAccepted = event.isAccepted();
        }
        break;
    }
    case QEvent::TabletEnterProximity:
    case QEvent::TabletLeaveProximity: {
        // Qt delivers proximity events to the application object.
        QTabletEvent event(step.type,
                           m_stylus,
                           point.position,
                           global,
                           0.0,
                           0.0f,
                           0.0f,
                           0.0f,
                           0.0,
                           0.0f,
                           Qt::NoModifier,
                           Qt::NoButton,
                           Qt::NoButton);
        event.setTimestamp(timestamp);
        TestControlSpontaneous::send(QCoreApplication::instance(), &event);
        break;
    }
    default:
        break;
    }

    m_report.eventsSent++;
}

void TestControlInputInjector::finish(const QString &error)
{
    m_timer.stop();
    m_busy = false;
    m_report.error = error;
    m_report.durationMs = m_clock.elapsed() - m_startMs;

    Done done = std::move(m_done);
    m_done = Done();
    const Report report = m_report;
    if (done) {
        done(report);
    }
}
