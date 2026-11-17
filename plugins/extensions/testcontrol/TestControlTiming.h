/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TESTCONTROLTIMING_H
#define TESTCONTROLTIMING_H

#include <QObject>
#include <QPointer>
#include <QVector>
#include <QWidget>

/**
 * Timing that does not depend on the paint trace, so a run with the trace
 * disabled can be compared with one that has it: one monotonic clock for
 * the injected input and the canvas frames, and the process's CPU time.
 */
namespace TestControlTiming
{
/// Milliseconds on a monotonic clock with an arbitrary origin, the same in
/// every call of this process (microsecond resolution).
double clockMs();

struct CpuTimes {
    double userMs = -1;
    double systemMs = -1;
};
/// The CPU time of the whole process so far, all threads; negative values
/// when the platform does not report it.
CpuTimes processCpuTimes();
} // namespace TestControlTiming

/**
 * Records when a canvas widget presents a frame, on TestControlTiming's
 * clock. An OpenGL canvas (QOpenGLWidget) reports frameSwapped(), the
 * signal the paint trace's command-swap endpoint also uses; any other
 * widget is timed at the start of each paint event, which is earlier than
 * its presentation.
 *
 * Recording stops when the widget is destroyed; at most MaxFrames frames
 * are kept, later ones only set overflowed().
 */
class TestControlFrameRecorder : public QObject
{
    Q_OBJECT
public:
    static constexpr int MaxFrames = 100000;

    explicit TestControlFrameRecorder(QObject *parent = nullptr);
    ~TestControlFrameRecorder() override;

    /// Starts recording @p widget's frames and clears the earlier record.
    void start(QWidget *widget);
    void stop();

    bool isRecording() const;
    /// "frameSwapped", "paint", or empty when nothing was started.
    QString kind() const;
    QVector<double> frames() const;
    bool overflowed() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    void recordFrame();

private:
    QPointer<QWidget> m_widget;
    QMetaObject::Connection m_connection;
    QString m_kind;
    QVector<double> m_frames;
    bool m_overflowed = false;
};

#endif // TESTCONTROLTIMING_H
