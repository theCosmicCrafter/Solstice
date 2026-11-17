/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TESTCONTROLCANVASTOOLS_H
#define TESTCONTROLCANVASTOOLS_H

#include <QObject>

#include <functional>

#include "TestControlInputInjector.h"
#include "TestControlMcpServer.h"
#include "TestControlTiming.h"

/**
 * The MCP tools that act on the running application: documents, tools,
 * synthetic strokes, idle waits and state/pixel queries.
 *
 * Every handler resolves the current main window, view and image when it
 * runs, never at registration, and fails with a message when there is none.
 * Handlers run on the GUI thread, one at a time (TestControlMcpServer).
 */
class TestControlCanvasTools : public QObject
{
    Q_OBJECT
public:
    explicit TestControlCanvasTools(QObject *parent = nullptr);
    ~TestControlCanvasTools() override;

    void registerTools(TestControlMcpServer *server);

    static QString instructions();

private:
    using Arguments = QJsonObject;
    using Reply = TestControlMcpServer::ToolReply;

    void sessionInfo(const Arguments &arguments, const Reply &reply);
    void documentNew(const Arguments &arguments, const Reply &reply);
    void toolActivate(const Arguments &arguments, const Reply &reply);
    void inputStroke(const Arguments &arguments, const Reply &reply);
    void inputClick(const Arguments &arguments, const Reply &reply);
    void inputDrag(const Arguments &arguments, const Reply &reply);
    void inputKey(const Arguments &arguments, const Reply &reply);
    void waitIdle(const Arguments &arguments, const Reply &reply);
    void queryState(const Arguments &arguments, const Reply &reply);
    void actionTrigger(const Arguments &arguments, const Reply &reply);
    void canvasCapture(const Arguments &arguments, const Reply &reply);
    void queryPixels(const Arguments &arguments, const Reply &reply);
    void canvasFrames(const Arguments &arguments, const Reply &reply);
    void gpuFault(const Arguments &arguments, const Reply &reply);

    /// Options shared by the input tools.
    struct StrokeOptions {
        TestControlInputInjector::Device device = TestControlInputInjector::Device::Mouse;
        Qt::MouseButton button = Qt::LeftButton;
        Qt::KeyboardModifiers modifiers;
        bool imageCoordinates = true;
        bool realtime = true;
        bool doubleClick = false;
        qint64 releaseDelayMs = TestControlInputInjector::ReleaseSettleMs;
        Qt::Key keyAfterRelease = Qt::Key(0);
    };
    /// A point as given by a tool call, before conversion to widget pixels.
    struct SourcePoint {
        QPointF position;
        qreal pressure = 1.0;
        qreal xTilt = 0.0;
        qreal yTilt = 0.0;
        qint64 timeMs = 0;
    };

    static bool parseStrokeOptions(const Arguments &arguments, StrokeOptions *options, QString *error);
    /// Converts the points to the canvas widget and plays them; with
    /// modifiers, gives the canvas keyboard focus first.
    void playPoints(const QVector<SourcePoint> &points, const StrokeOptions &options, const Reply &reply);
    /// Gives @p widget keyboard focus (activating its window), then calls
    /// @p then; fails the call if focus does not arrive within 2 s.
    void withCanvasFocus(QWidget *widget, const Reply &reply, std::function<void()> then);

    TestControlInputInjector *m_injector;
    TestControlFrameRecorder *m_frames;
};

#endif // TESTCONTROLCANVASTOOLS_H
