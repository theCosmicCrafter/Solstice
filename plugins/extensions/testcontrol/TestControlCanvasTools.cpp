/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestControlCanvasTools.h"

#include <QAction>
#include <QBuffer>
#include <QCryptographicHash>
#include <QElapsedTimer>

#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QKeyEvent>
#include <QKeySequence>
#include <QPixmap>
#include <QPointer>
#include <QSysInfo>
#include <QTimer>
#include <memory>
#include <utility>

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisViewManager.h>
#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorProfile.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoToolManager.h>
#include <KoToolRegistry.h>
#include <KritaVersionWrapper.h>
#include <kactioncollection.h>
#include <kis_canvas2.h>
#include <kis_config.h>
#include <kis_coordinates_converter.h>
#include <kis_image.h>
#include <kis_node.h>
#include <kis_paint_device.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <klocalizedstring.h>
#include <kundo2stack.h>

#ifdef HAVE_KRITA_GPU_ENGINE
#include <KisGpuContext.h>
#include <KisGpuEngineUi.h>
#include <gpu/KisGpuTileBackend.h>
#endif

#include "TestControlImageCompare.h"
#include "TestControlInputInjector.h"
#include "TestControlSpontaneous.h"
#include "TestControlSupport.h"

using namespace TestControlSupport;

namespace
{
constexpr int MaxStrokePoints = 10000;
constexpr int DefaultPointSpacingMs = 10;
constexpr qint64 MaxStrokeDurationMs = 120000;
constexpr qint64 MaxQueriedPixels = qint64(4096) * 4096;
constexpr int MaxFullResolutionPng = 2048;
// wait_idle polls every 20 ms. Three idle polls let work queued to the GUI
// thread run. A vector (shape) selection renders its pixels from timers the
// image does not report as busy (KisSelectionUpdateCompressor, 100 ms, then
// the selection mask's update compressor): the pixels arrived up to 220 ms
// and a last short job ran about 430 ms after the stroke on Windows, so it
// needs 600 ms of idle time. The time is measured with a clock, not counted
// in polls: a busy GUI thread stretches the 20 ms polls (to about 300 ms with
// 36 documents that have selections), and those timers run late by about one
// such gap each, so the wait adds twice the longest gap seen.
constexpr int IdlePolls = 3;
constexpr qint64 ShapeSelectionSettleMs = 600;
// gpu_fault: enough failed read-backs to stand for a lost device for the rest
// of a session (a failed batch is retried tile by tile, each retry one more).
constexpr int MaxInjectedDownloadFailures = 1000000;

QJsonObject environmentInfo()
{
    QJsonObject result;
    for (const char *name : {"KRITA_GPU_PROJECTION", "KRITA_GPU_BRUSH", "KRITA_PAINT_TRACE", "SOLSTICE_PROFILE_ROOT"}) {
        if (qEnvironmentVariableIsSet(name)) {
            result.insert(QString::fromLatin1(name), qEnvironmentVariable(name));
        }
    }
    return result;
}

/// The GPU engine's device once it has started (with the first RGBA float
/// document), or null. The KRITA_GPU_* variables only request it.
QJsonValue gpuEngineInfo()
{
#ifdef HAVE_KRITA_GPU_ENGINE
    KisGpuTileBackend *backend = KisGpuTileBackend::existingInstance();
    if (backend) {
        const KisGpuDeviceInfo &device = backend->context().deviceInfo();
        return QJsonObject{
            {"device", device.name},
            {"driver", device.driverName},
            {"driverInfo", device.driverInfo},
            {"failed", backend->hasFailed()},
            {"lostTiles", double(backend->contentLossCount())},
            {"downloadedTiles", double(backend->downloadedTileCount())},
        };
    }
#endif
    return QJsonValue::Null;
}
/// The process's CPU time so far (all threads) and the current time, both
/// in milliseconds, for timing without the paint trace.
QJsonObject processInfo()
{
    const TestControlTiming::CpuTimes cpu = TestControlTiming::processCpuTimes();
    auto value = [](double ms) {
        return ms < 0 ? QJsonValue() : QJsonValue(ms);
    };
    return {
        {"cpuUserMs", value(cpu.userMs)},
        {"cpuSystemMs", value(cpu.systemMs)},
        {"clockMs", TestControlTiming::clockMs()},
    };
}
} // namespace

TestControlCanvasTools::TestControlCanvasTools(QObject *parent)
    : QObject(parent)
    , m_injector(new TestControlInputInjector(this))
    , m_frames(new TestControlFrameRecorder(this))
{
}

TestControlCanvasTools::~TestControlCanvasTools() = default;

QString TestControlCanvasTools::instructions()
{
    return QStringLiteral(
        "Drives a running Solstice through synthetic input for testing. "
        "Typical flow: session_info, document_new, tool_activate (e.g. KritaShape/KisToolBrush, "
        "KisToolSelectRectangular), input_stroke, wait_idle, query_state. Always call wait_idle after input "
        "or actions before reading state. Coordinates are image pixels unless coordinates is 'widget'. "
        "Calls run one at a time. Results are evidence for a test, not proof of real tablet behavior.");
}

void TestControlCanvasTools::registerTools(TestControlMcpServer *server)
{
    using namespace std::placeholders;

    server->addTool("session_info",
                    "Report versions, the active document, canvas widget, active tool and GPU environment.",
                    objectSchema({}),
                    std::bind(&TestControlCanvasTools::sessionInfo, this, _1, _2));

    server->addTool("document_new",
                    "Create an RGBA document with one transparent paint layer on a white canvas and show it in "
                    "the current window.",
                    objectSchema({
                        {"width", schemaProperty("integer", "Width in pixels (1-16384, default 800).")},
                        {"height", schemaProperty("integer", "Height in pixels (1-16384, default 600).")},
                        {"colorDepth", enumProperty({"U8", "U16", "F16", "F32"}, "Channel depth (default U8).")},
                    }),
                    std::bind(&TestControlCanvasTools::documentNew, this, _1, _2));

    server->addTool("tool_activate",
                    "Activate a tool by its factory ID, e.g. KritaShape/KisToolBrush or KisToolSelectRectangular. "
                    "If it is already active it is activated again (restarted: true), so the tool always starts "
                    "fresh, e.g. a new transform. An unknown ID fails and lists the available IDs.",
                    objectSchema({{"id", schemaProperty("string", "Tool factory ID.")}}, {"id"}),
                    std::bind(&TestControlCanvasTools::toolActivate, this, _1, _2));

    const QJsonObject modifiersSchema{
        {"type", "array"},
        {"items", enumProperty({"shift", "ctrl", "alt", "meta"}, "Modifier key.")},
        {"description",
         "Modifier keys held during the input, sent as key events; the canvas gets keyboard focus first "
         "(the window is activated), and the call fails if it cannot."},
    };

    const QJsonObject pointSchema = objectSchema(
        {
            {"x", schemaProperty("number", "X coordinate.")},
            {"y", schemaProperty("number", "Y coordinate.")},
            {"pressure", schemaProperty("number", "Pressure 0-1 (tablet only, default 1).")},
            {"xTilt", schemaProperty("number", "X tilt in degrees, -60 to 60 (tablet only).")},
            {"yTilt", schemaProperty("number", "Y tilt in degrees, -60 to 60 (tablet only).")},
            {"t", schemaProperty("number", "Time offset in ms (0-120000); must not decrease.")},
        },
        {"x", "y"});

    server->addTool(
        "input_stroke",
        "Play one press-move-release stroke into the canvas through the input manager, so the "
        "active tool handles it. Points without t are spaced 10 ms apart. Call wait_idle afterwards.",
        objectSchema(
            {
                {"points",
                 QJsonObject{{"type", "array"},
                             {"items", pointSchema},
                             {"minItems", 1},
                             {"maxItems", MaxStrokePoints},
                             {"description", "Stroke points; the first is the press."}}},
                {"device", enumProperty({"mouse", "tablet"}, "Input device (default mouse).")},
                {"coordinates", enumProperty({"image", "widget"}, "Coordinate space of the points (default image).")},
                {"button", enumProperty({"left", "right", "middle"}, "Button (default left).")},
                {"realtime",
                 schemaProperty("boolean",
                                "Send each point at its time offset (default true); false sends them back "
                                "to back.")},
                {"releaseDelayMs",
                 schemaProperty("number",
                                "Pause between the last move and the release in real time (0-1000, default "
                                "50). 0 releases at once.")},
                {"keyAfterRelease",
                 schemaProperty("string",
                                "A key (QKeySequence portable text, e.g. Return) pressed right after the "
                                "release in the same call; gives the canvas keyboard focus first.")},
                {"modifiers", modifiersSchema},
            },
            {"points"}),
        std::bind(&TestControlCanvasTools::inputStroke, this, _1, _2));

    const QJsonObject positionSchema = objectSchema(
        {{"x", schemaProperty("number", "X coordinate.")}, {"y", schemaProperty("number", "Y coordinate.")}},
        {"x", "y"});

    server->addTool("input_click",
                    "Press and release at one point, through the same path as input_stroke. Call wait_idle afterwards.",
                    objectSchema(
                        {
                            {"x", schemaProperty("number", "X coordinate.")},
                            {"y", schemaProperty("number", "Y coordinate.")},
                            {"device", enumProperty({"mouse", "tablet"}, "Input device (default mouse).")},
                            {"coordinates", enumProperty({"image", "widget"}, "Coordinate space (default image).")},
                            {"button", enumProperty({"left", "right", "middle"}, "Button (default left).")},
                            {"pressure", schemaProperty("number", "Pressure 0-1 (tablet only, default 1).")},
                            {"doubleClick", schemaProperty("boolean", "Double click (mouse only, default false).")},
                            {"modifiers", modifiersSchema},
                        },
                        {"x", "y"}),
                    std::bind(&TestControlCanvasTools::inputClick, this, _1, _2));

    server->addTool(
        "input_drag",
        "Drag in a straight line from one point to another in equal steps, through the same path as "
        "input_stroke. Call wait_idle afterwards.",
        objectSchema(
            {
                {"from", positionSchema},
                {"to", positionSchema},
                {"steps", schemaProperty("integer", "Moves after the press (default 20).")},
                {"durationMs", schemaProperty("number", "Duration of the drag (default 200, max 120000).")},
                {"device", enumProperty({"mouse", "tablet"}, "Input device (default mouse).")},
                {"coordinates", enumProperty({"image", "widget"}, "Coordinate space (default image).")},
                {"button", enumProperty({"left", "right", "middle"}, "Button (default left).")},
                {"pressure", schemaProperty("number", "Constant pressure 0-1 (tablet only, default 1).")},
                {"realtime", schemaProperty("boolean", "Send each point at its time offset (default true).")},
                {"releaseDelayMs",
                 schemaProperty("number",
                                "Pause between the last move and the release in real time (0-1000, default "
                                "50). 0 releases at once.")},
                {"keyAfterRelease",
                 schemaProperty("string",
                                "A key (QKeySequence portable text, e.g. Return) pressed right after the "
                                "release in the same call; gives the canvas keyboard focus first.")},
                {"modifiers", modifiersSchema},
            },
            {"from", "to"}),
        std::bind(&TestControlCanvasTools::inputDrag, this, _1, _2));

    server->addTool("wait_idle",
                    "Wait until the image has no queued or running strokes and stays idle for three polls "
                    "20 ms apart, so follow-up work queued to the GUI thread (such as a new selection mask) "
                    "has run too. While the current selection is a vector selection, it must stay idle for "
                    "600 ms plus twice the longest gap between polls, until the selection's pixels are "
                    "rendered. The result reports that gap (maxPollGapMs).",
                    objectSchema({{"timeoutMs", schemaProperty("integer", "Timeout (default 10000, max 120000).")}}),
                    std::bind(&TestControlCanvasTools::waitIdle, this, _1, _2));

    server->addTool("query_state",
                    "Report the active tool, document, undo stack, selection bounds/pixel count/hash and the "
                    "active layer's exact bounds and SHA-256 pixel hash. Fails while the image is busy.",
                    objectSchema({{"includeLayerHash",
                                   schemaProperty("boolean", "Hash the active layer's pixels (default true).")}}),
                    std::bind(&TestControlCanvasTools::queryState, this, _1, _2));

    server->addTool("action_trigger",
                    "Trigger a main window action by name, e.g. edit_undo, edit_redo, deselect, select_all. "
                    "With async, reply first and trigger afterwards: an action that opens a modal dialog "
                    "would otherwise block every later call; operate the dialog with the ui tools.",
                    objectSchema({{"name", schemaProperty("string", "Action name.")},
                                  {"async",
                                   schemaProperty("boolean",
                                                  "Trigger after replying, for actions that open a modal "
                                                  "dialog (default false).")}},
                                 {"name"}),
                    std::bind(&TestControlCanvasTools::actionTrigger, this, _1, _2));

    server->addTool("input_key",
                    "Press and release a key on the canvas, e.g. Return to apply a transform or Escape to cancel "
                    "it. The canvas gets keyboard focus first (the window is activated). Call wait_idle "
                    "afterwards.",
                    objectSchema(
                        {
                            {"key",
                             schemaProperty("string",
                                            "Key in QKeySequence portable text, e.g. Return, Escape, Delete, "
                                            "Ctrl+Z.")},
                            {"modifiers", modifiersSchema},
                        },
                        {"key"}),
                    std::bind(&TestControlCanvasTools::inputKey, this, _1, _2));

    server->addTool(
        "query_pixels",
        "Hash and measure a rectangle of the active layer or of the image projection, in image pixels: "
        "SHA-256 of the device's pixels, non-transparent and opaque pixel counts, mean alpha and the mean "
        "color of the non-transparent pixels (8-bit sRGB), optionally a small PNG. Fails while the image is "
        "busy.",
        objectSchema({
            {"x", schemaProperty("integer", "Left edge (default 0).")},
            {"y", schemaProperty("integer", "Top edge (default 0).")},
            {"width", schemaProperty("integer", "Width (default image width).")},
            {"height", schemaProperty("integer", "Height (default image height).")},
            {"source", enumProperty({"layer", "projection"}, "Active layer or the whole image (default layer).")},
            {"includePng", schemaProperty("boolean", "Attach a PNG of the rectangle (default false).")},
            {"maxPngSize", schemaProperty("integer", "Longest side of that PNG (default 256, max 1024).")},
            {"fullResolutionPng",
             schemaProperty("boolean",
                            "Attach the PNG at the rectangle's own size, not scaled (rectangle at most "
                            "2048x2048); for recording reference images.")},
            {"reference",
             schemaProperty("string",
                            "Base64 PNG of the same size as the rectangle to compare with (8-bit sRGB, "
                            "straight alpha); the result gets a reference object.")},
            {"maxChannelDiff",
             schemaProperty("integer", "Largest allowed channel difference against the reference (default 0).")},
            {"maxMismatchPixels", schemaProperty("integer", "Pixels allowed above maxChannelDiff (default 0).")},
        }),
        std::bind(&TestControlCanvasTools::queryPixels, this, _1, _2));

    server->addTool(
        "canvas_frames",
        "Record when the canvas presents frames, for timing without the paint trace. start clears the record "
        "and follows the current canvas (frameSwapped of the OpenGL canvas, otherwise the start of each paint "
        "event); read returns the frame times since sinceMs; stop ends the recording. Times are milliseconds "
        "on the clock of input_stroke's pressedAtMs and releasedAtMs; nowMs is the current time.",
        objectSchema({{"action", enumProperty({"start", "read", "stop"}, "What to do (default read).")},
                      {"sinceMs", schemaProperty("number", "read: only frames after this time.")}}),
        std::bind(&TestControlCanvasTools::canvasFrames, this, _1, _2));

    server->addTool("canvas_capture",
                    "Return a PNG image of the canvas widget as displayed (diagnostic; not a pixel reference).",
                    objectSchema({{"maxSize", schemaProperty("integer", "Longest side in pixels (default 1024).")}}),
                    std::bind(&TestControlCanvasTools::canvasCapture, this, _1, _2));

    server->addTool(
        "gpu_fault",
        "Development only: make the GPU engine's next read-backs of image data fail, to check how Solstice "
        "handles lost GPU content (the warning, the stopped engine, the save and export guards). A failed batch "
        "is retried tile by tile and each retry counts, so a small count tests recovery and 1000000 stands for a "
        "lost device. Read-backs happen when the CPU reads painted pixels: query_pixels, a save or export, a CPU "
        "filter. Lost tiles stay lost; use a scratch document. reset leaves the failed state, clears pending "
        "failures and accepts the loss so far for saving. Without arguments it only reports.",
        objectSchema({
            {"downloadFailures",
             schemaProperty("integer",
                            "How many of the next read-back submissions fail (0-1000000); 0 clears pending "
                            "failures. Applied after reset.")},
            {"reset", schemaProperty("boolean", "Leave the failed state first (default false).")},
        }),
        std::bind(&TestControlCanvasTools::gpuFault, this, _1, _2));
}

void TestControlCanvasTools::sessionInfo(const Arguments &, const Reply &reply)
{
    KisViewManager *viewManager = currentViewManager();
    reply(TestControlMcpServer::ToolResult::success({
        {"solsticeVersion", KritaVersionWrapper::solsticeVersionString(true)},
        {"kritaCompatibilityVersion", KritaVersionWrapper::versionString(true)},
        {"qtVersion", QString::fromLatin1(qVersion())},
        {"platform", QSysInfo::prettyProductName()},
        {"uiLanguages", QJsonArray::fromStringList(KLocalizedString::languages())},
        {"mainWindows", KisPart::instance()->mainwindowCount()},
        {"activeTool", KoToolManager::instance()->activeToolId()},
        {"document", documentInfo(viewManager)},
        {"canvas", canvasInfo(viewManager)},
        {"environment", environmentInfo()},
        {"gpuEngine", gpuEngineInfo()},
        {"process", processInfo()},
    }));
}

void TestControlCanvasTools::gpuFault(const Arguments &arguments, const Reply &reply)
{
#ifdef HAVE_KRITA_GPU_ENGINE
    const QJsonValue failures = arguments.value("downloadFailures");
    const double count = failures.toDouble(-1);
    if (!failures.isUndefined()
        && (!failures.isDouble() || count < 0 || count > MaxInjectedDownloadFailures || count != qint64(count))) {
        reply(TestControlMcpServer::ToolResult::failure("downloadFailures must be an integer from 0 to 1000000"));
        return;
    }
    KisGpuTileBackend *backend = KisGpuTileBackend::existingInstance();
    if (!backend) {
        reply(TestControlMcpServer::ToolResult::failure(
            "The GPU engine has not started; it starts with the first RGBA float document while it is enabled"));
        return;
    }
    if (arguments.value("reset").toBool(false)) {
        backend->injectDownloadFailuresForTesting(0);
        backend->resetFailureForTesting();
        // Otherwise every later save would be refused for the loss so far.
        KisGpuEngineUi::setAcknowledgedLossCountForTesting(backend->contentLossCount());
    }
    if (!failures.isUndefined()) {
        backend->injectDownloadFailuresForTesting(int(count));
    }
    reply(TestControlMcpServer::ToolResult::success({{"gpuEngine", gpuEngineInfo()}}));
#else
    Q_UNUSED(arguments);
    reply(TestControlMcpServer::ToolResult::failure("This build has no GPU engine"));
#endif
}

void TestControlCanvasTools::documentNew(const Arguments &arguments, const Reply &reply)
{
    const int width = arguments.value("width").toInt(800);
    const int height = arguments.value("height").toInt(600);
    if (width < 1 || width > 16384 || height < 1 || height > 16384) {
        reply(TestControlMcpServer::ToolResult::failure("width and height must be between 1 and 16384"));
        return;
    }

    const QString depth = arguments.value("colorDepth").toString("U8");
    KoID depthId;
    if (depth == QLatin1String("U8")) {
        depthId = Integer8BitsColorDepthID;
    } else if (depth == QLatin1String("U16")) {
        depthId = Integer16BitsColorDepthID;
    } else if (depth == QLatin1String("F16")) {
        depthId = Float16BitsColorDepthID;
    } else if (depth == QLatin1String("F32")) {
        depthId = Float32BitsColorDepthID;
    } else {
        reply(TestControlMcpServer::ToolResult::failure("Unknown colorDepth: " + depth));
        return;
    }

    if (!currentWindow()) {
        reply(TestControlMcpServer::ToolResult::failure("No main window"));
        return;
    }

    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(), depthId.id());
    if (!colorSpace) {
        reply(TestControlMcpServer::ToolResult::failure("Color space RGBA/" + depth + " is not available"));
        return;
    }

    KisDocument *document = KisPart::instance()->createDocument();
    // An empty name avoids a new translatable string; the view shows the
    // usual unnamed-document title.
    const bool created = document->newImage(QString(),
                                            width,
                                            height,
                                            colorSpace,
                                            KoColor(QColor(Qt::white), colorSpace),
                                            KisConfig::CANVAS_COLOR,
                                            1,
                                            QString(),
                                            300.0 / 72.0);
    if (!created) {
        delete document;
        reply(TestControlMcpServer::ToolResult::failure("KisDocument::newImage() failed"));
        return;
    }

    QPointer<KisDocument> guard(document);
    KisPart::instance()->startCustomDocument(document);

    TestControlSupport::pollUntil(
        this,
        [guard]() {
            KisViewManager *viewManager = currentViewManager();
            return guard && viewManager && viewManager->document() == guard.data() && viewManager->canvasBase();
        },
        5000,
        [reply](bool shown, qint64) {
            KisViewManager *viewManager = currentViewManager();
            if (!shown) {
                reply(TestControlMcpServer::ToolResult::failure("The new document did not become the active view"));
                return;
            }
            reply(TestControlMcpServer::ToolResult::success({
                {"document", documentInfo(viewManager)},
                {"canvas", canvasInfo(viewManager)},
                {"activeTool", KoToolManager::instance()->activeToolId()},
            }));
        });
}

void TestControlCanvasTools::toolActivate(const Arguments &arguments, const Reply &reply)
{
    const QString id = arguments.value("id").toString();
    if (!KoToolRegistry::instance()->contains(id)) {
        QStringList ids = KoToolRegistry::instance()->keys();
        ids.sort();
        reply(TestControlMcpServer::ToolResult::failure("Unknown tool ID: " + id,
                                                        {{"availableTools", QJsonArray::fromStringList(ids)}}));
        return;
    }
    if (!currentViewManager() || !currentViewManager()->canvasBase()) {
        reply(TestControlMcpServer::ToolResult::failure("No canvas; create or open a document first"));
        return;
    }

    // Activating gives a freshly activated tool: requesting the active tool
    // again is a no-op in KoToolManager, which would leave, for example, an
    // applied transform without a new one, so switch away first.
    const bool restart = KoToolManager::instance()->activeToolId() == id;
    if (restart) {
        KoToolManager::instance()->switchToolRequested(id == QLatin1String("KisToolSelectRectangular")
                                                           ? QStringLiteral("KritaShape/KisToolBrush")
                                                           : QStringLiteral("KisToolSelectRectangular"));
    }
    KoToolManager::instance()->switchToolRequested(id);

    TestControlSupport::pollUntil(
        this,
        [id]() {
            return KoToolManager::instance()->activeToolId() == id;
        },
        2000,
        [reply, id, restart](bool active, qint64) {
            const QString activeTool = KoToolManager::instance()->activeToolId();
            if (!active) {
                reply(TestControlMcpServer::ToolResult::failure("The tool did not become active",
                                                                {{"requested", id}, {"activeTool", activeTool}}));
                return;
            }
            reply(TestControlMcpServer::ToolResult::success({{"activeTool", activeTool}, {"restarted", restart}}));
        });
}

bool TestControlCanvasTools::parseStrokeOptions(const Arguments &arguments, StrokeOptions *options, QString *error)
{
    const QString deviceName = arguments.value("device").toString("mouse");
    const QString coordinates = arguments.value("coordinates").toString("image");
    const QString buttonName = arguments.value("button").toString("left");
    options->realtime = arguments.value("realtime").toBool(true);
    options->releaseDelayMs =
        qint64(arguments.value("releaseDelayMs").toDouble(TestControlInputInjector::ReleaseSettleMs));
    if (options->releaseDelayMs < 0 || options->releaseDelayMs > 1000) {
        *error = "releaseDelayMs must be between 0 and 1000";
        return false;
    }
    if (arguments.contains("keyAfterRelease")) {
        const QString keyName = arguments.value("keyAfterRelease").toString();
        const QKeySequence sequence(keyName, QKeySequence::PortableText);
        if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown
            || sequence[0].keyboardModifiers() != Qt::NoModifier) {
            *error = "keyAfterRelease must be one key without modifiers: " + keyName;
            return false;
        }
        options->keyAfterRelease = sequence[0].key();
    }

    if (deviceName == QLatin1String("mouse")) {
        options->device = TestControlInputInjector::Device::Mouse;
    } else if (deviceName == QLatin1String("tablet")) {
        options->device = TestControlInputInjector::Device::Tablet;
    } else {
        *error = "Unknown device: " + deviceName;
        return false;
    }

    if (coordinates != QLatin1String("image") && coordinates != QLatin1String("widget")) {
        *error = "Unknown coordinates: " + coordinates;
        return false;
    }
    options->imageCoordinates = coordinates == QLatin1String("image");

    if (buttonName == QLatin1String("left")) {
        options->button = Qt::LeftButton;
    } else if (buttonName == QLatin1String("right")) {
        options->button = Qt::RightButton;
    } else if (buttonName == QLatin1String("middle")) {
        options->button = Qt::MiddleButton;
    } else {
        *error = "Unknown button: " + buttonName;
        return false;
    }

    options->modifiers = Qt::NoModifier;
    for (const QJsonValue &value : arguments.value("modifiers").toArray()) {
        const QString name = value.toString();
        if (name == QLatin1String("shift")) {
            options->modifiers |= Qt::ShiftModifier;
        } else if (name == QLatin1String("ctrl")) {
            options->modifiers |= Qt::ControlModifier;
        } else if (name == QLatin1String("alt")) {
            options->modifiers |= Qt::AltModifier;
        } else if (name == QLatin1String("meta")) {
            options->modifiers |= Qt::MetaModifier;
        } else {
            *error = "Unknown modifier: " + name;
            return false;
        }
    }
    return true;
}

void TestControlCanvasTools::inputStroke(const Arguments &arguments, const Reply &reply)
{
    StrokeOptions options;
    QString error;
    if (!parseStrokeOptions(arguments, &options, &error)) {
        reply(TestControlMcpServer::ToolResult::failure(error));
        return;
    }

    const QJsonArray jsonPoints = arguments.value("points").toArray();
    if (jsonPoints.isEmpty() || jsonPoints.size() > MaxStrokePoints) {
        reply(TestControlMcpServer::ToolResult::failure(
            QStringLiteral("points must hold 1 to %1 points").arg(MaxStrokePoints)));
        return;
    }

    bool anyTime = false;
    for (const QJsonValue &value : jsonPoints) {
        anyTime |= value.toObject().contains("t");
    }

    QVector<SourcePoint> points;
    for (int i = 0; i < jsonPoints.size(); ++i) {
        const QJsonObject object = jsonPoints[i].toObject();
        if (!object.value("x").isDouble() || !object.value("y").isDouble()) {
            reply(TestControlMcpServer::ToolResult::failure(QStringLiteral("Point %1 needs numeric x and y").arg(i)));
            return;
        }
        if (anyTime && !object.value("t").isDouble()) {
            reply(TestControlMcpServer::ToolResult::failure(
                QStringLiteral("Point %1 has no t; give t for every point or for none").arg(i)));
            return;
        }
        SourcePoint point;
        point.position = QPointF(object.value("x").toDouble(), object.value("y").toDouble());
        point.pressure = object.value("pressure").toDouble(1.0);
        point.xTilt = object.value("xTilt").toDouble(0.0);
        point.yTilt = object.value("yTilt").toDouble(0.0);
        point.timeMs = anyTime ? qint64(object.value("t").toDouble()) : qint64(i) * DefaultPointSpacingMs;
        points.append(point);
    }

    playPoints(points, options, reply);
}

void TestControlCanvasTools::inputClick(const Arguments &arguments, const Reply &reply)
{
    StrokeOptions options;
    QString error;
    if (!parseStrokeOptions(arguments, &options, &error)) {
        reply(TestControlMcpServer::ToolResult::failure(error));
        return;
    }
    if (!arguments.value("x").isDouble() || !arguments.value("y").isDouble()) {
        reply(TestControlMcpServer::ToolResult::failure("input_click needs numeric x and y"));
        return;
    }
    options.doubleClick = arguments.value("doubleClick").toBool(false);
    SourcePoint point;
    point.position = QPointF(arguments.value("x").toDouble(), arguments.value("y").toDouble());
    point.pressure = arguments.value("pressure").toDouble(1.0);
    playPoints({point}, options, reply);
}

void TestControlCanvasTools::inputDrag(const Arguments &arguments, const Reply &reply)
{
    StrokeOptions options;
    QString error;
    if (!parseStrokeOptions(arguments, &options, &error)) {
        reply(TestControlMcpServer::ToolResult::failure(error));
        return;
    }
    const QJsonObject from = arguments.value("from").toObject();
    const QJsonObject to = arguments.value("to").toObject();
    if (!from.value("x").isDouble() || !from.value("y").isDouble() || !to.value("x").isDouble()
        || !to.value("y").isDouble()) {
        reply(TestControlMcpServer::ToolResult::failure("input_drag needs from and to with numeric x and y"));
        return;
    }
    const int steps = arguments.value("steps").toInt(20);
    const qint64 durationMs = qint64(arguments.value("durationMs").toDouble(200));
    if (steps < 1 || steps >= MaxStrokePoints) {
        reply(TestControlMcpServer::ToolResult::failure(
            QStringLiteral("steps must be between 1 and %1").arg(MaxStrokePoints - 1)));
        return;
    }
    if (durationMs < 0 || durationMs > MaxStrokeDurationMs) {
        reply(TestControlMcpServer::ToolResult::failure(
            QStringLiteral("durationMs must be between 0 and %1").arg(MaxStrokeDurationMs)));
        return;
    }

    const QPointF start(from.value("x").toDouble(), from.value("y").toDouble());
    const QPointF end(to.value("x").toDouble(), to.value("y").toDouble());
    const qreal pressure = arguments.value("pressure").toDouble(1.0);
    QVector<SourcePoint> points;
    for (int i = 0; i <= steps; ++i) {
        const qreal f = qreal(i) / steps;
        SourcePoint point;
        point.position = start + (end - start) * f;
        point.pressure = pressure;
        point.timeMs = qint64(durationMs * f);
        points.append(point);
    }
    playPoints(points, options, reply);
}

void TestControlCanvasTools::playPoints(const QVector<SourcePoint> &sourcePoints,
                                        const StrokeOptions &options,
                                        const Reply &reply)
{
    KisViewManager *viewManager = currentViewManager();
    KisCanvas2 *canvas = viewManager ? viewManager->canvasBase() : nullptr;
    QWidget *widget = canvas ? canvas->canvasWidget() : nullptr;
    if (!widget) {
        reply(TestControlMcpServer::ToolResult::failure("No canvas; create or open a document first"));
        return;
    }

    const KisCoordinatesConverter *converter = canvas->coordinatesConverter();
    const QRectF widgetRect(widget->rect());
    QVector<TestControlInputInjector::Point> points;
    int outside = 0;
    for (int i = 0; i < sourcePoints.size(); ++i) {
        const SourcePoint &source = sourcePoints[i];
        TestControlInputInjector::Point point;
        point.position = options.imageCoordinates ? converter->imageToWidget(source.position) : source.position;
        point.pressure = qBound(0.0, source.pressure, 1.0);
        point.xTilt = qBound(-60.0, source.xTilt, 60.0);
        point.yTilt = qBound(-60.0, source.yTilt, 60.0);
        point.timeMs = source.timeMs;
        if (point.timeMs < 0 || point.timeMs > MaxStrokeDurationMs) {
            reply(TestControlMcpServer::ToolResult::failure(
                QStringLiteral("Point %1: t must be between 0 and %2 ms").arg(i).arg(MaxStrokeDurationMs)));
            return;
        }
        if (!widgetRect.contains(point.position)) {
            outside++;
        }
        points.append(point);
    }

    QStringList modifierNames;
    for (const auto &entry : {std::make_pair(Qt::ShiftModifier, "shift"),
                              std::make_pair(Qt::ControlModifier, "ctrl"),
                              std::make_pair(Qt::AltModifier, "alt"),
                              std::make_pair(Qt::MetaModifier, "meta")}) {
        if (options.modifiers.testFlag(entry.first)) {
            modifierNames << QString::fromLatin1(entry.second);
        }
    }

    QPointer<QWidget> target(widget);
    auto play = [this, target, options, points, outside, modifierNames, reply]() {
        if (!target) {
            reply(TestControlMcpServer::ToolResult::failure("The canvas closed before the stroke"));
            return;
        }
        TestControlInputInjector::Options injectorOptions;
        injectorOptions.modifiers = options.modifiers;
        injectorOptions.realtime = options.realtime;
        injectorOptions.doubleClick = options.doubleClick;
        injectorOptions.releaseSettleMs = options.releaseDelayMs;
        injectorOptions.keyAfterRelease = options.keyAfterRelease;
        m_injector->playStroke(
            target,
            options.device,
            options.button,
            points,
            injectorOptions,
            [reply, outside, modifierNames](const TestControlInputInjector::Report &report) {
                const QJsonObject details{
                    {"eventsSent", report.eventsSent},
                    {"pressAccepted", report.pressAccepted},
                    {"releaseAccepted", report.releaseAccepted},
                    {"durationMs", report.durationMs},
                    {"pressedAtMs", report.pressedAtMs < 0 ? QJsonValue() : QJsonValue(report.pressedAtMs)},
                    {"releasedAtMs", report.releasedAtMs < 0 ? QJsonValue() : QJsonValue(report.releasedAtMs)},
                    {"pointsOutsideCanvas", outside},
                    {"modifiers", QJsonArray::fromStringList(modifierNames)},
                    {"activeTool", KoToolManager::instance()->activeToolId()},
                };
                if (!report.error.isEmpty()) {
                    reply(TestControlMcpServer::ToolResult::failure(report.error, details));
                } else {
                    reply(TestControlMcpServer::ToolResult::success(details));
                }
            });
    };

    if (options.modifiers == Qt::NoModifier && options.keyAfterRelease == Qt::Key(0)) {
        play();
        return;
    }

    // KisInputManager keeps synthetic modifiers only while the canvas has
    // keyboard focus; without it, it reads the real keyboard instead. Keys
    // need the focus too.
    withCanvasFocus(widget, reply, play);
}

void TestControlCanvasTools::withCanvasFocus(QWidget *widget, const Reply &reply, std::function<void()> then)
{
    QPointer<QWidget> target(widget);
    widget->window()->activateWindow();
    widget->setFocus(Qt::OtherFocusReason);
    pollUntil(
        this,
        [target]() {
            return target && target->hasFocus();
        },
        2000,
        [then, reply](bool focused, qint64) {
            if (!focused) {
                reply(TestControlMcpServer::ToolResult::failure(
                    "Keys and modifiers need keyboard focus on the canvas, and the window could not be activated",
                    {{"canvas", canvasInfo(currentViewManager())}}));
                return;
            }
            then();
        });
}

void TestControlCanvasTools::inputKey(const Arguments &arguments, const Reply &reply)
{
    KisViewManager *viewManager = currentViewManager();
    KisCanvas2 *canvas = viewManager ? viewManager->canvasBase() : nullptr;
    QWidget *widget = canvas ? canvas->canvasWidget() : nullptr;
    if (!widget) {
        reply(TestControlMcpServer::ToolResult::failure("No canvas; create or open a document first"));
        return;
    }

    const QString keyName = arguments.value("key").toString();
    const QKeySequence sequence(keyName, QKeySequence::PortableText);
    if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown) {
        reply(TestControlMcpServer::ToolResult::failure("Unknown key: " + keyName));
        return;
    }
    const Qt::Key key = sequence[0].key();
    Qt::KeyboardModifiers modifiers = sequence[0].keyboardModifiers();
    StrokeOptions options;
    QString error;
    if (!parseStrokeOptions(QJsonObject{{"modifiers", arguments.value("modifiers")}}, &options, &error)) {
        reply(TestControlMcpServer::ToolResult::failure(error));
        return;
    }
    modifiers |= options.modifiers;

    QPointer<QWidget> target(widget);
    withCanvasFocus(widget, reply, [this, target, key, modifiers, keyName, reply]() {
        if (!target) {
            reply(TestControlMcpServer::ToolResult::failure("The canvas closed before the key"));
            return;
        }
        // Modifier keys first, each carrying the state before it, as from the
        // platform; then the key itself, then the releases in reverse.
        const std::pair<Qt::KeyboardModifier, Qt::Key> modifierKeys[] = {
            {Qt::ControlModifier, Qt::Key_Control},
            {Qt::ShiftModifier, Qt::Key_Shift},
            {Qt::AltModifier, Qt::Key_Alt},
            {Qt::MetaModifier, Qt::Key_Meta},
        };
        Qt::KeyboardModifiers held;
        bool accepted = false;
        for (const auto &entry : modifierKeys) {
            if (modifiers.testFlag(entry.first)) {
                QKeyEvent shortcut(QEvent::ShortcutOverride, entry.second, held);
                TestControlSpontaneous::send(target, &shortcut);
                QKeyEvent press(QEvent::KeyPress, entry.second, held);
                TestControlSpontaneous::send(target, &press);
                held |= entry.first;
            }
        }
        QKeyEvent shortcut(QEvent::ShortcutOverride, key, held);
        TestControlSpontaneous::send(target, &shortcut);
        QKeyEvent press(QEvent::KeyPress, key, held);
        TestControlSpontaneous::send(target, &press);
        accepted = shortcut.isAccepted() || press.isAccepted();
        QKeyEvent release(QEvent::KeyRelease, key, held);
        TestControlSpontaneous::send(target, &release);
        for (auto it = std::rbegin(modifierKeys); it != std::rend(modifierKeys); ++it) {
            if (modifiers.testFlag(it->first)) {
                QKeyEvent releaseModifier(QEvent::KeyRelease, it->second, held);
                TestControlSpontaneous::send(target, &releaseModifier);
                held &= ~it->first;
            }
        }
        QTimer::singleShot(0, this, [reply, keyName, accepted]() {
            reply(TestControlMcpServer::ToolResult::success({
                {"key", keyName},
                {"accepted", accepted},
                {"activeTool", KoToolManager::instance()->activeToolId()},
            }));
        });
    });
}

void TestControlCanvasTools::queryPixels(const Arguments &arguments, const Reply &reply)
{
    KisViewManager *viewManager = currentViewManager();
    KisImageSP image;
    if (viewManager) {
        image = viewManager->image();
    }
    if (!image) {
        reply(TestControlMcpServer::ToolResult::failure("No image; create or open a document first"));
        return;
    }

    const QRect rect(arguments.value("x").toInt(0),
                     arguments.value("y").toInt(0),
                     arguments.value("width").toInt(image->width()),
                     arguments.value("height").toInt(image->height()));
    if (rect.isEmpty() || qint64(rect.width()) * rect.height() > MaxQueriedPixels) {
        reply(TestControlMcpServer::ToolResult::failure(
            QStringLiteral("The rectangle must be non-empty and at most %1 pixels").arg(MaxQueriedPixels)));
        return;
    }
    const QString source = arguments.value("source").toString("layer");
    if (source != QLatin1String("layer") && source != QLatin1String("projection")) {
        reply(TestControlMcpServer::ToolResult::failure("Unknown source: " + source));
        return;
    }

    if (!image->tryBarrierLock(true)) {
        reply(TestControlMcpServer::ToolResult::failure("The image is busy; call wait_idle first"));
        return;
    }

    KisPaintDeviceSP device;
    if (source == QLatin1String("projection")) {
        device = image->projection();
    } else if (KisNodeSP node = viewManager->activeNode()) {
        device = node->paintDevice();
    }
    if (!device) {
        image->unlock();
        reply(TestControlMcpServer::ToolResult::failure("The active node has no pixels"));
        return;
    }

    QByteArray bytes;
    readDevice(device, rect, &bytes);
    const QImage pixels = device->convertToQImage(nullptr, rect.x(), rect.y(), rect.width(), rect.height());
    image->unlock();

    // Counts and the average are taken from 8-bit sRGB; the hash is of the
    // device's own pixels.
    qint64 opaque = 0;
    qint64 visible = 0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 0.0;
    const QImage argb = pixels.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            const QRgb pixel = line[x];
            alpha += qAlpha(pixel);
            if (qAlpha(pixel) == 0) {
                continue;
            }
            visible++;
            opaque += qAlpha(pixel) == 255;
            red += qRed(pixel);
            green += qGreen(pixel);
            blue += qBlue(pixel);
        }
    }
    const qint64 total = qint64(rect.width()) * rect.height();
    QJsonObject result{
        {"rect", rectToJson(rect)},
        {"source", source},
        {"colorSpace", device->colorSpace()->id()},
        {"sha256", sha256(bytes)},
        {"nonTransparentPixels", visible},
        {"opaquePixels", opaque},
        {"meanAlpha", alpha / total},
    };
    if (visible > 0) {
        result.insert("meanColor",
                      QColor(qRound(red / visible), qRound(green / visible), qRound(blue / visible)).name());
    }

    if (arguments.contains("reference")) {
        const QImage reference =
            QImage::fromData(QByteArray::fromBase64(arguments.value("reference").toString().toLatin1()), "PNG");
        TestControlImageCompare::Tolerance tolerance;
        tolerance.maxChannelDiff = qBound(0, arguments.value("maxChannelDiff").toInt(0), 255);
        tolerance.maxMismatchPixels = qMax<qint64>(0, qint64(arguments.value("maxMismatchPixels").toDouble(0)));
        const TestControlImageCompare::Result comparison = TestControlImageCompare::compare(argb, reference, tolerance);
        if (!comparison.error.isEmpty()) {
            reply(TestControlMcpServer::ToolResult::failure("Reference comparison: " + comparison.error, result));
            return;
        }
        result.insert("reference",
                      QJsonObject{
                          {"matches", comparison.matches},
                          {"maxChannelDiff", comparison.maxChannelDiff},
                          {"mismatchPixels", comparison.mismatchPixels},
                          {"tolerance",
                           QJsonObject{{"maxChannelDiff", tolerance.maxChannelDiff},
                                       {"maxMismatchPixels", tolerance.maxMismatchPixels}}},
                      });
    }

    TestControlMcpServer::ToolResult toolResult = TestControlMcpServer::ToolResult::success(result);
    const bool fullResolution = arguments.value("fullResolutionPng").toBool(false);
    if (fullResolution && (rect.width() > MaxFullResolutionPng || rect.height() > MaxFullResolutionPng)) {
        reply(TestControlMcpServer::ToolResult::failure(
            QStringLiteral("fullResolutionPng needs a rectangle of at most %1x%1").arg(MaxFullResolutionPng)));
        return;
    }
    if (arguments.value("includePng").toBool(false) || fullResolution) {
        const int maxSize = qBound(16, arguments.value("maxPngSize").toInt(256), 1024);
        QImage scaled = argb;
        if (!fullResolution && (scaled.width() > maxSize || scaled.height() > maxSize)) {
            scaled = scaled.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (scaled.save(&buffer, "PNG")) {
            toolResult.extraContent.append(QJsonObject{
                {"type", "image"},
                {"data", QString::fromLatin1(png.toBase64())},
                {"mimeType", "image/png"},
            });
        }
    }
    reply(toolResult);
}

void TestControlCanvasTools::waitIdle(const Arguments &arguments, const Reply &reply)
{
    KisViewManager *viewManager = currentViewManager();
    KisImageSP image;
    if (viewManager) {
        image = viewManager->image();
    }
    if (!image) {
        reply(TestControlMcpServer::ToolResult::failure("No image; create or open a document first"));
        return;
    }

    const int timeoutMs = qBound(0, arguments.value("timeoutMs").toInt(10000), 120000);
    struct IdleState {
        QElapsedTimer clock;
        qint64 lastPollMs = -1;
        qint64 maxGapMs = 0;
        qint64 idleSinceMs = -1;
        int idlePolls = 0;
    };
    auto state = std::make_shared<IdleState>();
    state->clock.start();
    TestControlSupport::pollUntil(
        this,
        [image, state]() mutable {
            const qint64 now = state->clock.elapsed();
            if (state->lastPollMs >= 0) {
                state->maxGapMs = std::max(state->maxGapMs, now - state->lastPollMs);
            }
            state->lastPollMs = now;
            if (!image->isIdle()) {
                state->idlePolls = 0;
                state->idleSinceMs = -1;
                return false;
            }
            if (state->idleSinceMs < 0) {
                state->idleSinceMs = now;
            }
            ++state->idlePolls;
            if (state->idlePolls < IdlePolls) {
                return false;
            }
            KisViewManager *viewManager = currentViewManager();
            const KisSelectionSP selection = viewManager ? viewManager->selection() : KisSelectionSP();
            if (!selection || !selection->hasShapeSelection()) {
                return true;
            }
            return now - state->idleSinceMs >= ShapeSelectionSettleMs + 2 * state->maxGapMs;
        },
        timeoutMs,
        [reply, state](bool idle, qint64 waitedMs) {
            const QJsonObject result{{"idle", idle}, {"waitedMs", waitedMs}, {"maxPollGapMs", state->maxGapMs}};
            if (idle) {
                reply(TestControlMcpServer::ToolResult::success(result));
            } else {
                reply(TestControlMcpServer::ToolResult::failure("The image did not become idle", result));
            }
        },
        // The condition tracks its own idle polls and time (see IdlePolls and
        // ShapeSelectionSettleMs). A vector selection read right after three
        // polls was empty in 13 of 15 runs on Windows and in about one of 25
        // on Linux.
        1);
}

void TestControlCanvasTools::queryState(const Arguments &arguments, const Reply &reply)
{
    const bool includeLayerHash = arguments.value("includeLayerHash").toBool(true);

    QJsonObject state{{"activeTool", KoToolManager::instance()->activeToolId()}};
    KisViewManager *viewManager = currentViewManager();
    KisImageSP image;
    if (viewManager) {
        image = viewManager->image();
    }
    state.insert("document", documentInfo(viewManager));
    if (!image) {
        reply(TestControlMcpServer::ToolResult::success(state));
        return;
    }

    KisDocument *document = viewManager->document();
    if (document && document->undoStack()) {
        KUndo2Stack *stack = document->undoStack();
        const int index = stack->index();
        state.insert("undo",
                     QJsonObject{
                         {"count", stack->count()},
                         {"index", index},
                         {"lastCommand", index > 0 ? stack->text(index - 1) : QString()},
                     });
    }

    if (!image->tryBarrierLock(true)) {
        reply(TestControlMcpServer::ToolResult::failure("The image is busy; call wait_idle first", state));
        return;
    }

    KisSelectionSP selection = viewManager->selection();
    if (selection) {
        const QRect bounds = selection->selectedExactRect();
        QJsonObject selectionInfo{{"active", true}, {"bounds", rectToJson(bounds)}};
        KisPixelSelectionSP pixels = selection->projection();
        QByteArray bytes;
        if (pixels && !bounds.isEmpty() && readDevice(pixels, bounds, &bytes)) {
            const int pixelSize = pixels->pixelSize();
            qint64 selected = 0;
            for (int i = 0; i < bytes.size(); i += pixelSize) {
                bool any = false;
                for (int c = 0; c < pixelSize; ++c) {
                    any |= bytes[i + c] != 0;
                }
                selected += any;
            }
            selectionInfo.insert("selectedPixels", selected);
            selectionInfo.insert("sha256", sha256(bytes));
        }
        state.insert("selection", selectionInfo);
    } else {
        state.insert("selection", QJsonObject{{"active", false}});
    }

    KisNodeSP node = viewManager->activeNode();
    if (node) {
        QJsonObject layer{
            {"name", node->name()},
            {"class", QString::fromLatin1(node->metaObject()->className())},
        };
        KisPaintDeviceSP device = node->paintDevice();
        if (device) {
            const QRect bounds = device->exactBounds();
            layer.insert("exactBounds", rectToJson(bounds));
            layer.insert("colorSpace", device->colorSpace()->id());
            QByteArray bytes;
            if (includeLayerHash && !bounds.isEmpty() && readDevice(device, bounds, &bytes)) {
                layer.insert("sha256", sha256(bytes));
            }
        }
        state.insert("activeLayer", layer);
    }

    image->unlock();
    reply(TestControlMcpServer::ToolResult::success(state));
}

void TestControlCanvasTools::actionTrigger(const Arguments &arguments, const Reply &reply)
{
    const QString name = arguments.value("name").toString();
    KisMainWindow *window = currentWindow();
    QAction *action = window ? window->actionCollection()->action(name) : nullptr;
    if (!action) {
        reply(TestControlMcpServer::ToolResult::failure("Unknown action: " + name));
        return;
    }
    if (!action->isEnabled()) {
        reply(TestControlMcpServer::ToolResult::failure("The action is disabled: " + name));
        return;
    }

    QPointer<QAction> guard(action);
    if (arguments.value("async").toBool(false)) {
        // The trigger runs from the event loop after the reply is sent, so a
        // modal dialog's own event loop keeps serving later calls.
        reply(TestControlMcpServer::ToolResult::success({{"triggered", name}, {"async", true}}));
        QTimer::singleShot(0, this, [guard]() {
            if (guard && guard->isEnabled()) {
                guard->trigger();
            }
        });
        return;
    }

    action->trigger();

    QTimer::singleShot(0, this, [reply, guard, name]() {
        QJsonObject result{{"triggered", name}};
        if (guard && guard->isCheckable()) {
            result.insert("checked", guard->isChecked());
        }
        reply(TestControlMcpServer::ToolResult::success(result));
    });
}

void TestControlCanvasTools::canvasCapture(const Arguments &arguments, const Reply &reply)
{
    KisViewManager *viewManager = currentViewManager();
    KisCanvas2 *canvas = viewManager ? viewManager->canvasBase() : nullptr;
    QWidget *widget = canvas ? canvas->canvasWidget() : nullptr;
    if (!widget) {
        reply(TestControlMcpServer::ToolResult::failure("No canvas; create or open a document first"));
        return;
    }

    QImage image = widget->grab().toImage();
    const int maxSize = qBound(16, arguments.value("maxSize").toInt(1024), 4096);
    if (image.width() > maxSize || image.height() > maxSize) {
        image = image.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (image.isNull() || !image.save(&buffer, "PNG")) {
        reply(TestControlMcpServer::ToolResult::failure("Could not capture the canvas widget"));
        return;
    }

    TestControlMcpServer::ToolResult result = TestControlMcpServer::ToolResult::success({
        {"width", image.width()},
        {"height", image.height()},
        {"widgetWidth", widget->width()},
        {"widgetHeight", widget->height()},
    });
    result.extraContent.append(QJsonObject{
        {"type", "image"},
        {"data", QString::fromLatin1(png.toBase64())},
        {"mimeType", "image/png"},
    });
    reply(result);
}

void TestControlCanvasTools::canvasFrames(const Arguments &arguments, const Reply &reply)
{
    const QString action = arguments.value("action").toString(QStringLiteral("read"));
    if (action == QLatin1String("start")) {
        KisViewManager *viewManager = currentViewManager();
        KisCanvas2 *canvas = viewManager ? viewManager->canvasBase() : nullptr;
        QWidget *widget = canvas ? canvas->canvasWidget() : nullptr;
        if (!widget) {
            reply(TestControlMcpServer::ToolResult::failure("No canvas; create or open a document first"));
            return;
        }
        m_frames->start(widget);
        reply(TestControlMcpServer::ToolResult::success(
            {{"kind", m_frames->kind()}, {"nowMs", TestControlTiming::clockMs()}}));
        return;
    }
    if (action == QLatin1String("stop")) {
        m_frames->stop();
    } else if (action != QLatin1String("read")) {
        reply(TestControlMcpServer::ToolResult::failure("action must be start, read or stop"));
        return;
    }

    const double sinceMs = arguments.value("sinceMs").toDouble(-1);
    QJsonArray frames;
    for (double frame : m_frames->frames()) {
        if (frame > sinceMs) {
            frames.append(frame);
        }
    }
    reply(TestControlMcpServer::ToolResult::success({
        {"kind", m_frames->kind()},
        {"recording", m_frames->isRecording()},
        {"overflowed", m_frames->overflowed()},
        {"frames", frames},
        {"nowMs", TestControlTiming::clockMs()},
    }));
}
