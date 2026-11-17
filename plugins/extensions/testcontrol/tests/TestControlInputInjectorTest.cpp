/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTabletEvent>
#include <QTest>
#include <QWidget>

#include "TestControlInputInjector.h"
#include "TestControlTiming.h"

namespace
{
struct Recorded {
    QEvent::Type type = QEvent::None;
    QPointF position;
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
    qreal pressure = -1.0;
    Qt::MouseEventSource source = Qt::MouseEventNotSynthesized;
    quint64 timestamp = 0;
    bool toApplication = false;
    bool spontaneous = false;
    int key = 0;
    Qt::KeyboardModifiers modifiers;
};

class RecordingWidget : public QWidget
{
public:
    RecordingWidget()
    {
        // As Krita's canvas widgets: QApplication drops button-less moves
        // for widgets without mouse tracking.
        setMouseTracking(true);
    }

    QVector<Recorded> events;

protected:
    bool event(QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::ShortcutOverride:
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            Recorded recorded;
            recorded.type = event->type();
            recorded.key = keyEvent->key();
            recorded.modifiers = keyEvent->modifiers();
            recorded.spontaneous = event->spontaneous();
            events.append(recorded);
            event->accept();
            return true;
        }
        case QEvent::Enter: {
            auto *enter = static_cast<QEnterEvent *>(event);
            Recorded recorded;
            recorded.type = event->type();
            recorded.position = enter->position();
            recorded.spontaneous = event->spontaneous();
            events.append(recorded);
            return QWidget::event(event);
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            events.append({event->type(),
                           mouse->position(),
                           mouse->button(),
                           mouse->buttons(),
                           -1.0,
                           mouse->source(),
                           mouse->timestamp(),
                           false,
                           event->spontaneous(),
                           0,
                           mouse->modifiers()});
            event->accept();
            return true;
        }
        case QEvent::TabletPress:
        case QEvent::TabletMove:
        case QEvent::TabletRelease: {
            auto *tablet = static_cast<QTabletEvent *>(event);
            events.append({event->type(),
                           tablet->position(),
                           tablet->button(),
                           tablet->buttons(),
                           tablet->pressure(),
                           Qt::MouseEventNotSynthesized,
                           tablet->timestamp(),
                           false,
                           event->spontaneous(),
                           0,
                           tablet->modifiers()});
            event->accept();
            return true;
        }
        default:
            return QWidget::event(event);
        }
    }
};

/// Records proximity events, which go to the application object.
class ProximityRecorder : public QObject
{
public:
    explicit ProximityRecorder(QVector<Recorded> *events)
        : m_events(events)
    {
        qApp->installEventFilter(this);
    }
    ~ProximityRecorder() override
    {
        qApp->removeEventFilter(this);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == qApp
            && (event->type() == QEvent::TabletEnterProximity || event->type() == QEvent::TabletLeaveProximity)) {
            Recorded recorded;
            recorded.type = event->type();
            recorded.toApplication = true;
            m_events->append(recorded);
        }
        return false;
    }

private:
    QVector<Recorded> *m_events;
};

TestControlInputInjector::Options options(bool realtime, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    TestControlInputInjector::Options result;
    result.realtime = realtime;
    result.modifiers = modifiers;
    return result;
}

QVector<TestControlInputInjector::Point> threePoints()
{
    TestControlInputInjector::Point a;
    a.position = QPointF(10, 10);
    a.pressure = 0.2;
    a.timeMs = 0;
    TestControlInputInjector::Point b = a;
    b.position = QPointF(20, 15);
    b.pressure = 0.6;
    b.timeMs = 30;
    TestControlInputInjector::Point c = a;
    c.position = QPointF(30, 20);
    c.pressure = 1.0;
    c.timeMs = 60;
    return {a, b, c};
}
} // namespace

class TestControlInputInjectorTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testMouseStroke();
    void testTabletStroke();
    void testModifiers();
    void testDoubleClick();
    void testImmediateRelease();
    void testKeyAfterRelease();
    void testRealtimeTiming();
    void testRejectsInvalidStrokes();
    void testTargetDestroyed();
};

void TestControlInputInjectorTest::testMouseStroke()
{
    RecordingWidget widget;
    widget.resize(100, 100);
    TestControlInputInjector injector;

    bool finished = false;
    TestControlInputInjector::Report report;
    injector.playStroke(&widget,
                        TestControlInputInjector::Device::Mouse,
                        Qt::LeftButton,
                        threePoints(),
                        options(false),
                        [&](const TestControlInputInjector::Report &r) {
                            report = r;
                            finished = true;
                        });
    QVERIFY(injector.isBusy());
    QTRY_VERIFY(finished);

    QVERIFY(report.error.isEmpty());
    QCOMPARE(report.eventsSent, 6);
    QVERIFY(report.pressAccepted);
    QVERIFY(report.releaseAccepted);
    QVERIFY(report.pressedAtMs > 0);
    QVERIFY(report.releasedAtMs >= report.pressedAtMs);

    QCOMPARE(widget.events.size(), 6);
    // The pointer enters the canvas and moves to the first point before the
    // press, as a real mouse does.
    QCOMPARE(widget.events[0].type, QEvent::Enter);
    QCOMPARE(widget.events[0].position, QPointF(10, 10));
    QVERIFY(widget.events[0].spontaneous);
    QCOMPARE(widget.events[1].type, QEvent::MouseMove);
    QCOMPARE(widget.events[1].position, QPointF(10, 10));
    QCOMPARE(widget.events[1].buttons, Qt::MouseButtons(Qt::NoButton));
    QCOMPARE(widget.events[2].timestamp - widget.events[1].timestamp, quint64(TestControlInputInjector::HoverSettleMs));
    const QVector<Recorded> e = widget.events.mid(2);
    QCOMPARE(e[0].type, QEvent::MouseButtonPress);
    QCOMPARE(e[0].button, Qt::LeftButton);
    QCOMPARE(e[0].buttons, Qt::MouseButtons(Qt::LeftButton));
    QCOMPARE(e[0].position, QPointF(10, 10));
    QCOMPARE(e[1].type, QEvent::MouseMove);
    QCOMPARE(e[1].button, Qt::NoButton);
    QCOMPARE(e[1].buttons, Qt::MouseButtons(Qt::LeftButton));
    QCOMPARE(e[2].position, QPointF(30, 20));
    QCOMPARE(e[3].type, QEvent::MouseButtonRelease);
    QCOMPARE(e[3].button, Qt::LeftButton);
    QCOMPARE(e[3].buttons, Qt::MouseButtons(Qt::NoButton));
    QCOMPARE(e[3].position, QPointF(30, 20));
    for (const Recorded &recorded : e) {
        QCOMPARE(recorded.source, Qt::MouseEventNotSynthesized);
        // Tools cancel shapes on non-spontaneous releases.
        QVERIFY(recorded.spontaneous);
    }
    QCOMPARE(e[1].timestamp - e[0].timestamp, quint64(30));
    QCOMPARE(e[2].timestamp - e[0].timestamp, quint64(60));
    // The release waits, so the move compression delivers the last move first.
    QCOMPARE(e[3].timestamp - e[2].timestamp, quint64(TestControlInputInjector::ReleaseSettleMs));
}

void TestControlInputInjectorTest::testImmediateRelease()
{
    RecordingWidget widget;
    widget.resize(100, 100);
    TestControlInputInjector injector;
    TestControlInputInjector::Options immediate = options(false);
    immediate.releaseSettleMs = 0;

    bool finished = false;
    injector.playStroke(&widget,
                        TestControlInputInjector::Device::Mouse,
                        Qt::LeftButton,
                        threePoints(),
                        immediate,
                        [&](const TestControlInputInjector::Report &) {
                            finished = true;
                        });
    QTRY_VERIFY(finished);

    // Enter, hover, press, two moves, release: the release carries the last
    // move's time, as a quick real release does.
    QCOMPARE(widget.events.size(), 6);
    QCOMPARE(widget.events[5].type, QEvent::MouseButtonRelease);
    QCOMPARE(widget.events[5].timestamp, widget.events[4].timestamp);
}

void TestControlInputInjectorTest::testKeyAfterRelease()
{
    RecordingWidget widget;
    widget.resize(100, 100);
    TestControlInputInjector injector;
    TestControlInputInjector::Options withKey = options(false);
    withKey.releaseSettleMs = 0;
    withKey.keyAfterRelease = Qt::Key_Return;

    bool finished = false;
    injector.playStroke(&widget,
                        TestControlInputInjector::Device::Mouse,
                        Qt::LeftButton,
                        threePoints(),
                        withKey,
                        [&](const TestControlInputInjector::Report &) {
                            finished = true;
                        });
    QTRY_VERIFY(finished);

    // Enter, hover, press, two moves, release, then the key in the same call.
    QCOMPARE(widget.events.size(), 9);
    QCOMPARE(widget.events[5].type, QEvent::MouseButtonRelease);
    const QEvent::Type keyTypes[] = {QEvent::ShortcutOverride, QEvent::KeyPress, QEvent::KeyRelease};
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(widget.events[6 + i].type, keyTypes[i]);
        QCOMPARE(widget.events[6 + i].key, int(Qt::Key_Return));
        QCOMPARE(widget.events[6 + i].modifiers, Qt::KeyboardModifiers(Qt::NoModifier));
        QVERIFY(widget.events[6 + i].spontaneous);
    }
}

void TestControlInputInjectorTest::testTabletStroke()
{
    RecordingWidget widget;
    widget.resize(100, 100);
    QVector<Recorded> proximity;
    ProximityRecorder recorder(&proximity);
    TestControlInputInjector injector;

    bool finished = false;
    TestControlInputInjector::Report report;
    injector.playStroke(&widget,
                        TestControlInputInjector::Device::Tablet,
                        Qt::LeftButton,
                        threePoints(),
                        options(false),
                        [&](const TestControlInputInjector::Report &r) {
                            report = r;
                            finished = true;
                        });
    QTRY_VERIFY(finished);
    QVERIFY(report.error.isEmpty());
    QCOMPARE(report.eventsSent, 10);
    QVERIFY2(report.durationMs >= TestControlInputInjector::HoverSettleMs,
             qPrintable(QString::number(report.durationMs)));

    QCOMPARE(proximity.size(), 2);
    QCOMPARE(proximity[0].type, QEvent::TabletEnterProximity);
    QCOMPARE(proximity[1].type, QEvent::TabletLeaveProximity);

    QCOMPARE(widget.events.size(), 8);
    QCOMPARE(widget.events[0].type, QEvent::Enter);
    QVERIFY(widget.events[0].spontaneous);
    const QVector<Recorded> e = widget.events.mid(1);
    QCOMPARE(e.size(), 7);
    // The hover move switches Krita to the stylus before the press.
    QCOMPARE(e[0].type, QEvent::TabletMove);
    QCOMPARE(e[0].position, QPointF(10, 10));
    QCOMPARE(e[0].pressure, 0.0);
    QCOMPARE(e[0].buttons, Qt::MouseButtons(Qt::NoButton));
    QCOMPARE(e[1].type, QEvent::TabletPress);
    QCOMPARE(e[1].pressure, 0.2);
    QCOMPARE(e[1].timestamp - e[0].timestamp, quint64(TestControlInputInjector::HoverSettleMs));
    QCOMPARE(e[2].type, QEvent::MouseButtonPress);
    QCOMPARE(e[2].source, Qt::MouseEventSynthesizedBySystem);
    QCOMPARE(e[3].type, QEvent::TabletMove);
    QCOMPARE(e[3].pressure, 0.6);
    QCOMPARE(e[3].buttons, Qt::MouseButtons(Qt::LeftButton));
    QCOMPARE(e[4].type, QEvent::TabletMove);
    QCOMPARE(e[4].pressure, 1.0);
    QCOMPARE(e[5].type, QEvent::TabletRelease);
    QCOMPARE(e[5].pressure, 0.0);
    QCOMPARE(e[5].timestamp - e[4].timestamp, quint64(TestControlInputInjector::ReleaseSettleMs));
    QCOMPARE(e[5].buttons, Qt::MouseButtons(Qt::NoButton));
    QCOMPARE(e[6].type, QEvent::MouseButtonRelease);
    QCOMPARE(e[6].source, Qt::MouseEventSynthesizedBySystem);
    for (const Recorded &recorded : e) {
        QVERIFY(recorded.spontaneous);
    }
}

void TestControlInputInjectorTest::testModifiers()
{
    RecordingWidget widget;
    widget.resize(100, 100);
    TestControlInputInjector injector;

    bool finished = false;
    TestControlInputInjector::Report report;
    injector.playStroke(&widget,
                        TestControlInputInjector::Device::Mouse,
                        Qt::LeftButton,
                        threePoints(),
                        options(false, Qt::ShiftModifier | Qt::ControlModifier),
                        [&](const TestControlInputInjector::Report &r) {
                            report = r;
                            finished = true;
                        });
    QTRY_VERIFY(finished);
    QVERIFY(report.error.isEmpty());

    const Qt::KeyboardModifiers both = Qt::ShiftModifier | Qt::ControlModifier;
    const QVector<Recorded> &e = widget.events;
    // Enter, Ctrl and Shift (ShortcutOverride + KeyPress each), hover move,
    // press, two moves, release, Shift and Ctrl KeyRelease.
    QCOMPARE(e.size(), 12);
    QCOMPARE(report.eventsSent, 12);
    QCOMPARE(e[0].type, QEvent::Enter);
    QCOMPARE(e[1].type, QEvent::ShortcutOverride);
    QCOMPARE(e[1].key, int(Qt::Key_Control));
    QCOMPARE(e[1].modifiers, Qt::KeyboardModifiers(Qt::ControlModifier));
    QCOMPARE(e[2].type, QEvent::KeyPress);
    QCOMPARE(e[2].key, int(Qt::Key_Control));
    QCOMPARE(e[3].type, QEvent::ShortcutOverride);
    QCOMPARE(e[3].key, int(Qt::Key_Shift));
    QCOMPARE(e[4].type, QEvent::KeyPress);
    QCOMPARE(e[4].key, int(Qt::Key_Shift));
    QCOMPARE(e[4].modifiers, both);
    for (int i = 5; i <= 9; ++i) {
        QCOMPARE(e[i].modifiers, both);
    }
    QCOMPARE(e[5].type, QEvent::MouseMove);
    QCOMPARE(e[6].type, QEvent::MouseButtonPress);
    QCOMPARE(e[9].type, QEvent::MouseButtonRelease);
    QCOMPARE(e[10].type, QEvent::KeyRelease);
    QCOMPARE(e[10].key, int(Qt::Key_Shift));
    QCOMPARE(e[10].modifiers, Qt::KeyboardModifiers(Qt::ControlModifier));
    QCOMPARE(e[11].type, QEvent::KeyRelease);
    QCOMPARE(e[11].key, int(Qt::Key_Control));
    QCOMPARE(e[11].modifiers, Qt::KeyboardModifiers(Qt::NoModifier));
    for (const Recorded &recorded : e) {
        QVERIFY(recorded.spontaneous);
    }
}

void TestControlInputInjectorTest::testDoubleClick()
{
    RecordingWidget widget;
    widget.resize(100, 100);
    TestControlInputInjector injector;

    TestControlInputInjector::Options doubleClick = options(false);
    doubleClick.doubleClick = true;

    QString error;
    auto record = [&error](const TestControlInputInjector::Report &r) {
        error = r.error;
    };
    injector.playStroke(&widget,
                        TestControlInputInjector::Device::Mouse,
                        Qt::LeftButton,
                        threePoints(),
                        doubleClick,
                        record);
    QVERIFY(error.contains("double click"));
    injector.playStroke(&widget,
                        TestControlInputInjector::Device::Tablet,
                        Qt::LeftButton,
                        {threePoints()[0]},
                        doubleClick,
                        record);
    QVERIFY(error.contains("double click"));

    bool finished = false;
    TestControlInputInjector::Report report;
    injector.playStroke(&widget,
                        TestControlInputInjector::Device::Mouse,
                        Qt::LeftButton,
                        {threePoints()[0]},
                        doubleClick,
                        [&](const TestControlInputInjector::Report &r) {
                            report = r;
                            finished = true;
                        });
    QTRY_VERIFY(finished);
    QVERIFY(report.error.isEmpty());

    // Enter, hover move, then Qt's sequence: press, release, double click, release.
    const QVector<Recorded> &e = widget.events;
    QCOMPARE(e.size(), 6);
    QCOMPARE(e[1].type, QEvent::MouseMove);
    QCOMPARE(e[2].type, QEvent::MouseButtonPress);
    QCOMPARE(e[3].type, QEvent::MouseButtonRelease);
    QCOMPARE(e[4].type, QEvent::MouseButtonDblClick);
    QCOMPARE(e[4].button, Qt::LeftButton);
    QCOMPARE(e[4].buttons, Qt::MouseButtons(Qt::LeftButton));
    QCOMPARE(e[5].type, QEvent::MouseButtonRelease);
    QCOMPARE(e[5].buttons, Qt::MouseButtons(Qt::NoButton));
    for (const Recorded &recorded : e) {
        QCOMPARE(recorded.position, QPointF(10, 10));
    }
}

void TestControlInputInjectorTest::testRealtimeTiming()
{
    RecordingWidget widget;
    TestControlInputInjector injector;

    bool finished = false;
    TestControlInputInjector::Report report;
    injector.playStroke(&widget,
                        TestControlInputInjector::Device::Mouse,
                        Qt::LeftButton,
                        threePoints(),
                        options(true),
                        [&](const TestControlInputInjector::Report &r) {
                            report = r;
                            finished = true;
                        });
    QTRY_VERIFY(finished);
    QVERIFY(report.error.isEmpty());
    QVERIFY2(report.durationMs >= 60, qPrintable(QString::number(report.durationMs)));
    // The press and release times use TestControlTiming's clock: the release
    // follows the last point (60 ms) and ReleaseSettleMs; QTimer may fire a
    // little early.
    QVERIFY(report.pressedAtMs > 0);
    const double held = report.releasedAtMs - report.pressedAtMs;
    QVERIFY2(held >= 0.9 * (60 + TestControlInputInjector::ReleaseSettleMs), qPrintable(QString::number(held)));
    QVERIFY(report.releasedAtMs <= TestControlTiming::clockMs());
}

void TestControlInputInjectorTest::testRejectsInvalidStrokes()
{
    RecordingWidget widget;
    TestControlInputInjector injector;
    QString error;
    auto record = [&error](const TestControlInputInjector::Report &r) {
        error = r.error;
    };

    injector.playStroke(&widget, TestControlInputInjector::Device::Mouse, Qt::LeftButton, {}, options(false), record);
    QVERIFY(!error.isEmpty());

    QVector<TestControlInputInjector::Point> points = threePoints();
    std::swap(points[1].timeMs, points[2].timeMs);
    error.clear();
    injector
        .playStroke(&widget, TestControlInputInjector::Device::Mouse, Qt::LeftButton, points, options(false), record);
    QVERIFY(!error.isEmpty());

    error.clear();
    injector.playStroke(nullptr,
                        TestControlInputInjector::Device::Mouse,
                        Qt::LeftButton,
                        threePoints(),
                        options(false),
                        record);
    QVERIFY(!error.isEmpty());

    bool finished = false;
    injector.playStroke(&widget,
                        TestControlInputInjector::Device::Mouse,
                        Qt::LeftButton,
                        threePoints(),
                        options(true),
                        [&finished](const TestControlInputInjector::Report &) {
                            finished = true;
                        });
    error.clear();
    injector.playStroke(&widget,
                        TestControlInputInjector::Device::Mouse,
                        Qt::LeftButton,
                        threePoints(),
                        options(false),
                        record);
    QVERIFY(error.contains("still playing"));
    QTRY_VERIFY(finished);
}

void TestControlInputInjectorTest::testTargetDestroyed()
{
    auto *widget = new RecordingWidget;
    TestControlInputInjector injector;

    QString error;
    bool finished = false;
    injector.playStroke(widget,
                        TestControlInputInjector::Device::Mouse,
                        Qt::LeftButton,
                        threePoints(),
                        options(true),
                        [&](const TestControlInputInjector::Report &r) {
                            error = r.error;
                            finished = true;
                        });
    QTest::qWait(10);
    delete widget;
    QTRY_VERIFY(finished);
    QVERIFY(error.contains("destroyed"));
    QVERIFY(!injector.isBusy());
}

QTEST_MAIN(TestControlInputInjectorTest)

#include "TestControlInputInjectorTest.moc"
