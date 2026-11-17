/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QTest>
#include <QWidget>

#include "TestControlTiming.h"

namespace
{
/// Stands for the OpenGL canvas: QOpenGLWidget's frameSwapped() signal,
/// without an OpenGL context.
class SwappingWidget : public QWidget
{
    Q_OBJECT
Q_SIGNALS:
    void frameSwapped();

public:
    void swap()
    {
        Q_EMIT frameSwapped();
    }
};
} // namespace

class TestControlTimingTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testClockAndCpuTime();
    void testFrameSwapped();
    void testPaintEvents();
    void testWidgetDestroyed();
    void testOverflow();
};

void TestControlTimingTest::testClockAndCpuTime()
{
    const double start = TestControlTiming::clockMs();
    const TestControlTiming::CpuTimes before = TestControlTiming::processCpuTimes();
    QVERIFY(before.userMs >= 0);
    QVERIFY(before.systemMs >= 0);

    // Busy for about 50 ms of CPU time.
    volatile double sink = 0;
    while (TestControlTiming::clockMs() - start < 50) {
        for (int i = 0; i < 10000; ++i) {
            sink = sink + i;
        }
    }
    const double elapsed = TestControlTiming::clockMs() - start;
    QVERIFY(elapsed >= 50);
    QVERIFY(elapsed < 10000);
    const TestControlTiming::CpuTimes after = TestControlTiming::processCpuTimes();
    const double used = (after.userMs + after.systemMs) - (before.userMs + before.systemMs);
    // The tick of the platform's accounting can be 15.6 ms (Windows).
    QVERIFY2(used >= 20, qPrintable(QString::number(used)));
}

void TestControlTimingTest::testFrameSwapped()
{
    SwappingWidget widget;
    TestControlFrameRecorder recorder;
    QVERIFY(recorder.kind().isEmpty());
    QVERIFY(!recorder.isRecording());

    recorder.start(&widget);
    QCOMPARE(recorder.kind(), QStringLiteral("frameSwapped"));
    QVERIFY(recorder.isRecording());
    const double before = TestControlTiming::clockMs();
    widget.swap();
    widget.swap();
    QCOMPARE(recorder.frames().size(), 2);
    QVERIFY(recorder.frames()[0] >= before);
    QVERIFY(recorder.frames()[1] >= recorder.frames()[0]);

    // Paint events of a widget with frameSwapped() are not frames.
    widget.resize(20, 20);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    widget.repaint();
    QCOMPARE(recorder.frames().size(), 2);

    // A new start clears the record; stop keeps it.
    recorder.start(&widget);
    QVERIFY(recorder.frames().isEmpty());
    widget.swap();
    recorder.stop();
    QVERIFY(!recorder.isRecording());
    widget.swap();
    QCOMPARE(recorder.frames().size(), 1);
}

void TestControlTimingTest::testPaintEvents()
{
    QWidget widget;
    widget.resize(20, 20);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    TestControlFrameRecorder recorder;
    recorder.start(&widget);
    QCOMPARE(recorder.kind(), QStringLiteral("paint"));
    widget.repaint();
    QVERIFY(recorder.frames().size() >= 1);

    const int count = recorder.frames().size();
    recorder.stop();
    widget.repaint();
    QCOMPARE(recorder.frames().size(), count);
}

void TestControlTimingTest::testWidgetDestroyed()
{
    TestControlFrameRecorder recorder;
    auto *widget = new SwappingWidget;
    recorder.start(widget);
    widget->swap();
    delete widget;
    QVERIFY(!recorder.isRecording());
    QCOMPARE(recorder.frames().size(), 1);
    recorder.stop();
}

void TestControlTimingTest::testOverflow()
{
    SwappingWidget widget;
    TestControlFrameRecorder recorder;
    recorder.start(&widget);
    for (int i = 0; i < TestControlFrameRecorder::MaxFrames; ++i) {
        widget.swap();
    }
    QVERIFY(!recorder.overflowed());
    widget.swap();
    QVERIFY(recorder.overflowed());
    QCOMPARE(recorder.frames().size(), TestControlFrameRecorder::MaxFrames);
}

QTEST_MAIN(TestControlTimingTest)

#include "TestControlTimingTest.moc"
