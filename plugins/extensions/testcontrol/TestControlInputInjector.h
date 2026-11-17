/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TESTCONTROLINPUTINJECTOR_H
#define TESTCONTROLINPUTINJECTOR_H

#include <QElapsedTimer>
#include <QEvent>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <functional>

class QPointingDevice;

/**
 * Plays one synthetic pointer stroke into a widget, so the widget's event
 * filters (Krita's KisInputManager on the canvas) see it like device input.
 * The events are marked spontaneous and delivered through
 * QCoreApplication::notify(), as QTest does: tools cancel a shape on a
 * non-spontaneous release, and sendEvent() clears the flag in Qt 6.
 *
 * Every stroke starts with an Enter at the first point, as when a real
 * pointer reaches the canvas: KisShortcutMatcher suppresses all actions
 * after a Leave, which the canvas gets when another window covers the
 * pointer.
 * With modifiers, ShortcutOverride and KeyPress events for Ctrl, Shift, Alt
 * and Meta follow the Enter, every pointer event carries the modifiers, and
 * KeyRelease events end the stroke. KisInputManager keeps such modifiers only
 * while the canvas has keyboard focus; otherwise it reads the real keyboard.
 * Mouse: a move without buttons to the first point, then, HoverSettleMs
 * later, press, moves, and ReleaseSettleMs after the last move the release
 * (for a double click: press, release, double click, release). The move is what a real mouse does before it presses;
 * tools such as the polygon selection close their shape only when the
 * pointer hovered over the first point before the click.
 * Tablet: TabletEnterProximity (to the application), one hover TabletMove at
 * the first point, TabletPress after HoverSettleMs, a system-synthesized
 * mouse press, TabletMoves, TabletRelease and TabletLeaveProximity. The hover
 * move lets KoToolProxy switch the canvas to the stylus before the press, as
 * a real pen's approach does; the first stylus event on a canvas creates new
 * tool instances, which ends a stroke that has already started. The
 * synthesized press stands for the one Windows generates after a pen press:
 * KisInputManager eats exactly one mouse press after each TabletPress
 * (EventEater::peckish), and without it the next real mouse stroke would lose
 * its press. Leaving proximity restores mouse input.
 *
 * In real-time mode each point is sent at its time offset; otherwise the
 * events are sent from consecutive event-loop iterations. Positions are in
 * the widget's coordinates. Only one stroke plays at a time.
 */
class TestControlInputInjector : public QObject
{
    Q_OBJECT
public:
    enum class Device {
        Mouse,
        Tablet
    };

    /// Time between the hover move and the press. Longer than
    /// KisInputManager's move compression (tabletEventsDelay, 10 ms by
    /// default), which a press would otherwise discard.
    static constexpr qint64 HoverSettleMs = 50;
    /// Default time between the last move and the release of a stroke with
    /// moves, as a hand pauses before letting go. A delay of 0 releases at
    /// the last move's time: that tests that KisInputManager delivers the
    /// move its compression still holds, and, with keyAfterRelease, that the
    /// Transform Tool applies a drag's last position (its Warp, Cage, Puppet
    /// and Mesh modes deliver it from a 40 ms compressor, which Apply now
    /// flushes).
    static constexpr qint64 ReleaseSettleMs = 50;

    struct Point {
        QPointF position;
        qreal pressure = 1.0;
        qreal xTilt = 0.0;
        qreal yTilt = 0.0;
        /// Offset from the first point, in milliseconds.
        qint64 timeMs = 0;
    };

    struct Report {
        int eventsSent = 0;
        bool pressAccepted = false;
        bool releaseAccepted = false;
        qint64 durationMs = 0;
        /// When the first press and the last release were sent, on
        /// TestControlTiming::clockMs(); negative when there was none.
        double pressedAtMs = -1;
        double releasedAtMs = -1;
        /// Empty on success.
        QString error;
    };

    struct Options {
        Qt::KeyboardModifiers modifiers;
        /// Send each point at its time offset; otherwise back to back.
        bool realtime = true;
        /// A one-point mouse click becomes a double click.
        bool doubleClick = false;
        /// Pause between the last move and the release (real time only).
        qint64 releaseSettleMs = ReleaseSettleMs;
        /// A key pressed and released right after the pointer release, in
        /// the same call (for example Return to apply a transform at once).
        /// Needs canvas focus. Qt::Key(0): none.
        Qt::Key keyAfterRelease = Qt::Key(0);
    };

    using Done = std::function<void(const Report &report)>;

    explicit TestControlInputInjector(QObject *parent = nullptr);
    ~TestControlInputInjector() override;

    bool isBusy() const;

    /**
     * Starts the stroke and calls @p done once it ends or fails (target
     * destroyed, invalid points). Point times must not decrease.
     */
    void playStroke(QWidget *target,
                    Device device,
                    Qt::MouseButton button,
                    const QVector<Point> &points,
                    const Options &options,
                    Done done);

private:
    struct Step {
        QEvent::Type type;
        int pointIndex;
        /// Mouse event sent with Qt::MouseEventSynthesizedBySystem.
        bool synthesizedMouse = false;
        /// Move without buttons or pressure to the first point, before the press.
        bool hover = false;
        /// The modifier key of a key event step.
        Qt::Key key = Qt::Key(0);
        /// From the release on, in a stroke with moves: after ReleaseSettleMs.
        bool afterMoves = false;
    };

    /// Time offset of @p step: steps after the hover move are shifted by
    /// HoverSettleMs, the release and what follows it in a stroke with moves
    /// by ReleaseSettleMs more.
    qint64 stepOffsetMs(const Step &step) const;
    void scheduleNext();
    void sendStep(const Step &step);
    void finish(const QString &error);

    QPointingDevice *m_stylus = nullptr;
    QTimer m_timer;
    QElapsedTimer m_clock;

    QPointer<QWidget> m_target;
    Device m_device = Device::Mouse;
    Qt::MouseButton m_button = Qt::LeftButton;
    Qt::KeyboardModifiers m_modifiers;
    /// Modifiers whose KeyPress has been sent and KeyRelease not yet.
    Qt::KeyboardModifiers m_heldModifiers;
    QVector<Point> m_points;
    QVector<Step> m_steps;
    int m_nextStep = 0;
    bool m_realtime = true;
    qint64 m_releaseSettleMs = ReleaseSettleMs;
    bool m_busy = false;
    qint64 m_startMs = 0;
    ulong m_timestampBase = 0;
    Report m_report;
    Done m_done;
};

#endif // TESTCONTROLINPUTINJECTOR_H
