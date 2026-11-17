/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestControlTiming.h"

#include <QEvent>

#include <chrono>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace TestControlTiming
{
double clockMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return double(std::chrono::duration_cast<std::chrono::microseconds>(now).count()) / 1000.0;
}

CpuTimes processCpuTimes()
{
    CpuTimes times;
#ifdef Q_OS_WIN
    FILETIME creation, exitTime, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user)) {
        // FILETIME counts 100 ns intervals.
        auto toMs = [](const FILETIME &time) {
            return double((quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime) / 10000.0;
        };
        times.userMs = toMs(user);
        times.systemMs = toMs(kernel);
    }
#else
    rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        auto toMs = [](const timeval &time) {
            return double(time.tv_sec) * 1000.0 + double(time.tv_usec) / 1000.0;
        };
        times.userMs = toMs(usage.ru_utime);
        times.systemMs = toMs(usage.ru_stime);
    }
#endif
    return times;
}
} // namespace TestControlTiming

TestControlFrameRecorder::TestControlFrameRecorder(QObject *parent)
    : QObject(parent)
{
}

TestControlFrameRecorder::~TestControlFrameRecorder()
{
    stop();
}

void TestControlFrameRecorder::start(QWidget *widget)
{
    stop();
    m_frames.clear();
    m_overflowed = false;
    m_kind.clear();
    if (!widget) {
        return;
    }
    m_widget = widget;
    // By name: QOpenGLWidget lives in QtOpenGLWidgets, which the plugin
    // does not link.
    if (widget->metaObject()->indexOfSignal("frameSwapped()") >= 0) {
        m_connection = connect(widget, SIGNAL(frameSwapped()), this, SLOT(recordFrame()));
        m_kind = QStringLiteral("frameSwapped");
    } else {
        widget->installEventFilter(this);
        m_kind = QStringLiteral("paint");
    }
}

void TestControlFrameRecorder::stop()
{
    if (m_connection) {
        disconnect(m_connection);
        m_connection = QMetaObject::Connection();
    }
    if (m_widget) {
        m_widget->removeEventFilter(this);
    }
    m_widget.clear();
}

bool TestControlFrameRecorder::isRecording() const
{
    return m_widget;
}

QString TestControlFrameRecorder::kind() const
{
    return m_kind;
}

QVector<double> TestControlFrameRecorder::frames() const
{
    return m_frames;
}

bool TestControlFrameRecorder::overflowed() const
{
    return m_overflowed;
}

bool TestControlFrameRecorder::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_widget && event->type() == QEvent::Paint) {
        recordFrame();
    }
    return QObject::eventFilter(watched, event);
}

void TestControlFrameRecorder::recordFrame()
{
    if (m_frames.size() >= MaxFrames) {
        m_overflowed = true;
        return;
    }
    m_frames.append(TestControlTiming::clockMs());
}
