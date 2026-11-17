/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestControlDocumentTools.h"

#include <QColor>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisResourceModel.h>
#include <KisResourceServerProvider.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <KoColor.h>
#include <brushengine/kis_paintop_preset.h>
#include <canvas/kis_canvas_controller.h>
#include <kis_canvas2.h>
#include <kis_canvas_resource_provider.h>
#include <kis_coordinates_converter.h>
#include <kis_image.h>
#include <kis_node.h>
#include <kis_node_manager.h>
#include <kis_paint_layer.h>
#include <kis_paintop_box.h>

#include "TestControlPaths.h"
#include "TestControlSupport.h"

using namespace TestControlSupport;

namespace
{
constexpr int MaxListedPresets = 200;

QStringList presetNames()
{
    QStringList names;
    KisResourceModel *model = KisResourceServerProvider::instance()->paintOpPresetServer()->resourceModel();
    for (int row = 0; row < model->rowCount(); ++row) {
        names << model->data(model->index(row, KisAbstractResourceModel::Name)).toString();
    }
    names.sort();
    names.removeDuplicates();
    return names.mid(0, MaxListedPresets);
}

QJsonObject brushInfo(KisViewManager *viewManager)
{
    KisCanvasResourceProvider *provider = viewManager->canvasResourceProvider();
    const KisPaintOpPresetSP preset = provider->currentPreset();
    const QColor color = provider->fgColor().toQColor();
    return {
        {"preset", preset ? preset->name() : QString()},
        {"size", provider->size()},
        {"opacity", provider->opacity()},
        {"flow", provider->flow()},
        {"compositeOp", provider->currentCompositeOp()},
        {"eraser", provider->eraserMode()},
        {"paintMode",
         preset && preset->settings() ? QJsonValue(preset->settings()->paintIncremental() ? "buildup" : "wash")
                                      : QJsonValue()},
        {"preserveAlpha", provider->globalAlphaLock()},
        {"color", color.name(QColor::HexRgb)},
        // Channels as numbers, for expectations with a tolerance: a color set
        // in another color space can come back a step or two off.
        {"colorRgb", QJsonArray{color.red(), color.green(), color.blue()}},
    };
}
/// The first node named @p name, searching from the top as the Layers docker
/// lists them, parents before their children.
KisNodeSP findNode(KisNodeSP parent, const QString &name)
{
    for (KisNodeSP child = parent->lastChild(); child; child = child->prevSibling()) {
        if (child->name() == name) {
            return child;
        }
        if (KisNodeSP found = findNode(child, name)) {
            return found;
        }
    }
    return KisNodeSP();
}

/// @p node's children from top to bottom, as the Layers docker lists them.
QJsonArray childNodes(KisNodeSP node, KisNodeSP activeNode)
{
    QJsonArray children;
    for (KisNodeSP child = node->lastChild(); child; child = child->prevSibling()) {
        QJsonObject info{
            {"name", child->name()},
            {"class", QString::fromLatin1(child->metaObject()->className())},
            {"visible", child->visible()},
            {"opacity", child->opacity()},
            {"compositeOp", child->compositeOpId()},
            {"colorLabel", child->colorLabelIndex()},
            {"locked", child->userLocked()},
            {"active", child == activeNode},
        };
        if (auto *paintLayer = dynamic_cast<KisPaintLayer *>(child.data())) {
            info.insert("alphaLocked", paintLayer->alphaLocked());
        }
        if (child->childCount() > 0) {
            info.insert("children", childNodes(child, activeNode));
        }
        children.append(info);
    }
    return children;
}
} // namespace

TestControlDocumentTools::TestControlDocumentTools(const QString &testRoot, QObject *parent)
    : QObject(parent)
    , m_testRoot(testRoot)
{
}

TestControlDocumentTools::~TestControlDocumentTools() = default;

void TestControlDocumentTools::registerTools(TestControlMcpServer *server)
{
    using namespace std::placeholders;

    const QString rootNote = QStringLiteral(
        " Paths are relative to the test root (SOLSTICE_TEST_CONTROL_ROOT); "
        "without it the tool fails.");

    server->addTool(
        "document_open",
        "Open an image file from the test root without dialogs and show it in the current window." + rootNote,
        objectSchema({{"path", schemaProperty("string", "File path relative to the test root.")}}, {"path"}),
        std::bind(&TestControlDocumentTools::documentOpen, this, _1, _2));

    server->addTool(
        "document_export_png",
        "Export the active document's image as PNG into the test root without dialogs, replacing an "
        "existing file. Call wait_idle first."
            + rootNote,
        objectSchema({{"path", schemaProperty("string", "Target .png path relative to the test root.")}}, {"path"}),
        std::bind(&TestControlDocumentTools::documentExportPng, this, _1, _2));

    server->addTool("document_close",
                    "Close the active document, or every document, discarding unsaved changes without asking. "
                    "Close documents a scenario no longer needs: each open document with a selection costs GUI "
                    "time.",
                    objectSchema({{"all", schemaProperty("boolean", "Close every document (default false).")}}),
                    std::bind(&TestControlDocumentTools::documentClose, this, _1, _2));

    server->addTool("view_reset",
                    "Make the view deterministic: show the window normally at the given size, zoom 100 %, no "
                    "rotation or mirroring, image centered in the canvas.",
                    objectSchema({
                        {"width", schemaProperty("integer", "Window width (default 1280, 640-7680).")},
                        {"height", schemaProperty("integer", "Window height (default 900, 480-4320).")},
                    }),
                    std::bind(&TestControlDocumentTools::viewReset, this, _1, _2));

    server->addTool("brush_set",
                    "Choose the paintop preset by name and set brush size, opacity, eraser mode and foreground "
                    "color. Omitted values stay as they are; without arguments it only reports the brush "
                    "(also flow, blending mode and preserve alpha). An unknown preset fails and lists preset names.",
                    objectSchema({
                        {"preset", schemaProperty("string", "Paintop preset name, e.g. \"b) Basic-5 Size Opacity\".")},
                        {"size", schemaProperty("number", "Brush size in pixels (1-1000).")},
                        {"opacity", schemaProperty("number", "Opacity 0-1.")},
                        {"eraser", schemaProperty("boolean", "Eraser mode.")},
                        {"color", schemaProperty("string", "Foreground color as #RRGGBB.")},
                        {"paintMode",
                         enumProperty({"buildup", "wash"},
                                      "Painting mode of the current preset (Build-up or Wash), as an unsaved "
                                      "preset change.")},
                    }),
                    std::bind(&TestControlDocumentTools::brushSet, this, _1, _2));

    server->addTool("query_layers",
                    "Report the active document's layer tree from top to bottom, as the Layers docker shows "
                    "it: name, class, visibility, opacity (0-255), blending mode, color label, lock, alpha "
                    "lock (paint layers) and which node is active, with the children of groups. Fails while "
                    "the image is busy.",
                    objectSchema({}),
                    std::bind(&TestControlDocumentTools::queryLayers, this, _1, _2));

    QJsonObject namesProperty = schemaProperty("array", "Names of the layers to select (default: the active one).");
    namesProperty.insert("items", QJsonObject{{"type", "string"}});
    server->addTool("layer_activate",
                    "Make the layer or mask with this name the active node and select it, or select several "
                    "nodes, as clicking in the Layers docker does. Names are matched from the top, parents "
                    "before their children; the first match counts.",
                    objectSchema(
                        {
                            {"name", schemaProperty("string", "Name of the node to activate.")},
                            {"select", namesProperty},
                        },
                        {"name"}),
                    std::bind(&TestControlDocumentTools::layerActivate, this, _1, _2));
}

void TestControlDocumentTools::layerActivate(const Arguments &arguments, const Reply &reply)
{
    KisViewManager *viewManager = currentViewManager();
    KisImageSP image = viewManager ? KisImageSP(viewManager->image()) : KisImageSP();
    if (!image) {
        reply(TestControlMcpServer::ToolResult::failure("No document; create or open one first"));
        return;
    }
    const QString name = arguments.value("name").toString();
    KisNodeSP node = findNode(image->root(), name);
    if (!node) {
        reply(TestControlMcpServer::ToolResult::failure("No layer named " + name));
        return;
    }
    KisNodeList selection;
    for (const QJsonValue &value : arguments.value("select").toArray()) {
        KisNodeSP selected = findNode(image->root(), value.toString());
        if (!selected) {
            reply(TestControlMcpServer::ToolResult::failure("No layer named " + value.toString()));
            return;
        }
        selection << selected;
    }
    if (selection.isEmpty()) {
        selection << node;
    }

    viewManager->nodeManager()->slotImageRequestNodeReselection(node, selection);

    QPointer<KisViewManager> guard(viewManager);
    QTimer::singleShot(0, this, [guard, reply]() {
        if (!guard) {
            reply(TestControlMcpServer::ToolResult::failure("The view was closed"));
            return;
        }
        QJsonArray selected;
        for (KisNodeSP selectedNode : guard->nodeManager()->selectedNodes()) {
            selected.append(selectedNode->name());
        }
        KisNodeSP active = guard->activeNode();
        reply(TestControlMcpServer::ToolResult::success({
            {"active", active ? QJsonValue(active->name()) : QJsonValue()},
            {"selected", selected},
        }));
    });
}

void TestControlDocumentTools::queryLayers(const Arguments &arguments, const Reply &reply)
{
    Q_UNUSED(arguments);
    KisViewManager *viewManager = currentViewManager();
    KisImageSP image = viewManager ? KisImageSP(viewManager->image()) : KisImageSP();
    if (!image) {
        reply(TestControlMcpServer::ToolResult::failure("No document; create or open one first"));
        return;
    }
    if (!image->tryBarrierLock(true)) {
        reply(TestControlMcpServer::ToolResult::failure("The image is busy; call wait_idle first"));
        return;
    }
    const KisNodeSP activeNode = viewManager->activeNode();
    const QJsonArray layers = childNodes(image->root(), activeNode);
    image->unlock();
    reply(TestControlMcpServer::ToolResult::success({
        {"layers", layers},
        {"active", activeNode ? QJsonValue(activeNode->name()) : QJsonValue()},
    }));
}

void TestControlDocumentTools::documentOpen(const Arguments &arguments, const Reply &reply)
{
    const QString relative = arguments.value("path").toString();
    const TestControlPaths::Resolved resolved = TestControlPaths::resolve(m_testRoot, relative, true);
    if (!resolved.error.isEmpty()) {
        reply(TestControlMcpServer::ToolResult::failure(resolved.error));
        return;
    }
    KisMainWindow *window = currentWindow();
    if (!window) {
        reply(TestControlMcpServer::ToolResult::failure("No main window"));
        return;
    }

    if (!window->openDocument(resolved.path, KisMainWindow::BatchMode)) {
        reply(TestControlMcpServer::ToolResult::failure("Could not open " + relative));
        return;
    }

    const QString path = resolved.path;
    pollUntil(
        this,
        [path]() {
            KisViewManager *viewManager = currentViewManager();
            KisDocument *document = viewManager ? viewManager->document() : nullptr;
            return document && QFileInfo(document->path()) == QFileInfo(path) && viewManager->canvasBase()
                && viewManager->image();
        },
        10000,
        [reply, relative](bool shown, qint64) {
            if (!shown) {
                reply(TestControlMcpServer::ToolResult::failure("The opened document did not become the active view"));
                return;
            }
            KisViewManager *viewManager = currentViewManager();
            reply(TestControlMcpServer::ToolResult::success({
                {"path", relative},
                {"document", documentInfo(viewManager)},
                {"canvas", canvasInfo(viewManager)},
            }));
        });
}

void TestControlDocumentTools::documentExportPng(const Arguments &arguments, const Reply &reply)
{
    const QString relative = arguments.value("path").toString();
    if (!relative.endsWith(QLatin1String(".png"), Qt::CaseInsensitive)) {
        reply(TestControlMcpServer::ToolResult::failure("The path must end in .png"));
        return;
    }
    const TestControlPaths::Resolved resolved = TestControlPaths::resolve(m_testRoot, relative, false);
    if (!resolved.error.isEmpty()) {
        reply(TestControlMcpServer::ToolResult::failure(resolved.error));
        return;
    }
    KisViewManager *viewManager = currentViewManager();
    KisDocument *document = viewManager ? viewManager->document() : nullptr;
    if (!document || !viewManager->image()) {
        reply(TestControlMcpServer::ToolResult::failure("No document; create or open one first"));
        return;
    }

    // Batch mode suppresses the export options dialog and error message boxes.
    const bool batchMode = document->fileBatchMode();
    document->setFileBatchMode(true);
    const bool exported = document->exportDocumentSync(resolved.path, "image/png");
    document->setFileBatchMode(batchMode);

    if (!exported) {
        reply(TestControlMcpServer::ToolResult::failure("The export failed: " + relative));
        return;
    }
    reply(TestControlMcpServer::ToolResult::success({
        {"path", relative},
        {"bytes", QFileInfo(resolved.path).size()},
        {"width", viewManager->image()->width()},
        {"height", viewManager->image()->height()},
    }));
}

void TestControlDocumentTools::documentClose(const Arguments &arguments, const Reply &reply)
{
    const bool all = arguments.value("all").toBool(false);

    QList<QPointer<KisDocument>> documents;
    if (all) {
        for (const QPointer<KisDocument> &document : KisPart::instance()->documents()) {
            documents << document;
        }
    } else {
        KisViewManager *viewManager = currentViewManager();
        KisDocument *document = viewManager ? viewManager->document() : nullptr;
        if (!document) {
            reply(TestControlMcpServer::ToolResult::failure("No document to close"));
            return;
        }
        documents << document;
    }

    // As libkis Document::close(), without asking to save: the views are
    // closed first, then the part removes and deletes the document.
    int closed = 0;
    for (const QPointer<KisDocument> &document : documents) {
        if (!document) {
            continue;
        }
        document->setModified(false);
        document->closePath(false);
        for (const QPointer<KisView> &view : KisPart::instance()->views()) {
            if (view && view->document() == document) {
                view->close();
                view->closeView();
                view->deleteLater();
            }
        }
        KisPart::instance()->removeDocument(document, true);
        closed++;
    }

    QTimer::singleShot(0, this, [reply, closed]() {
        reply(TestControlMcpServer::ToolResult::success({
            {"closed", closed},
            {"documentsLeft", KisPart::instance()->documentCount()},
            {"document", documentInfo(currentViewManager())},
        }));
    });
}

void TestControlDocumentTools::viewReset(const Arguments &arguments, const Reply &reply)
{
    KisMainWindow *window = currentWindow();
    KisViewManager *viewManager = currentViewManager();
    KisCanvas2 *canvas = viewManager ? viewManager->canvasBase() : nullptr;
    auto *controller = canvas ? dynamic_cast<KisCanvasController *>(canvas->canvasController()) : nullptr;
    if (!window || !controller) {
        reply(TestControlMcpServer::ToolResult::failure("No canvas; create or open a document first"));
        return;
    }

    const int width = qBound(640, arguments.value("width").toInt(1280), 7680);
    const int height = qBound(480, arguments.value("height").toInt(900), 4320);

    window->showNormal();
    window->resize(width, height);

    // Let the window and canvas take their new size before fitting the view.
    QPointer<KisCanvas2> guard(canvas);
    QTimer::singleShot(100, this, [reply, guard, viewManager]() {
        if (!guard || currentViewManager() != viewManager) {
            reply(TestControlMcpServer::ToolResult::failure("The view changed during view_reset"));
            return;
        }
        auto *controller = dynamic_cast<KisCanvasController *>(guard->canvasController());
        controller->mirrorCanvas(false);
        controller->resetCanvasRotation();
        controller->setZoom(KoZoomMode::ZOOM_CONSTANT, 1.0); // as KisZoomManager::zoomTo100()

        const KisCoordinatesConverter *converter = guard->coordinatesConverter();
        const QRectF imageRect = converter->imageRectInWidgetPixels();
        controller->setPreferredCenter(QPointF(imageRect.width() / 2.0, imageRect.height() / 2.0));

        QTimer::singleShot(0, guard.data(), [reply, guard, viewManager]() {
            const KisCoordinatesConverter *converter = guard->coordinatesConverter();
            const QRectF imageRect = converter->imageRectInWidgetPixels();
            reply(TestControlMcpServer::ToolResult::success({
                {"zoom", converter->zoom()},
                {"effectiveZoom", converter->effectiveZoom()},
                {"rotation", converter->rotationAngle()},
                {"mirrored", converter->xAxisMirrored() || converter->yAxisMirrored()},
                {"imageRectInWidget", rectToJson(imageRect.toAlignedRect())},
                {"canvas", canvasInfo(viewManager)},
            }));
        });
    });
}

void TestControlDocumentTools::brushSet(const Arguments &arguments, const Reply &reply)
{
    KisViewManager *viewManager = currentViewManager();
    if (!viewManager || !viewManager->canvasBase() || !viewManager->image()) {
        reply(TestControlMcpServer::ToolResult::failure("No canvas; create or open a document first"));
        return;
    }
    KisCanvasResourceProvider *provider = viewManager->canvasResourceProvider();

    if (arguments.contains("preset")) {
        const QString name = arguments.value("preset").toString();
        KisPaintOpPresetResourceServer *server = KisResourceServerProvider::instance()->paintOpPresetServer();
        const KisPaintOpPresetSP preset = server->resource("", "", name);
        if (!preset) {
            reply(TestControlMcpServer::ToolResult::failure("Unknown preset: " + name,
                                                            {{"presets", QJsonArray::fromStringList(presetNames())}}));
            return;
        }
        viewManager->paintOpBox()->resourceSelected(preset);
    }
    if (arguments.contains("size")) {
        provider->setSize(qBound(1.0, arguments.value("size").toDouble(), 1000.0));
    }
    if (arguments.contains("opacity")) {
        provider->setOpacity(qBound(0.0, arguments.value("opacity").toDouble(), 1.0));
    }
    if (arguments.contains("eraser")) {
        provider->setEraserMode(arguments.value("eraser").toBool());
    }
    if (arguments.contains("color")) {
        const QColor color(arguments.value("color").toString());
        if (!color.isValid()) {
            reply(TestControlMcpServer::ToolResult::failure("color must be #RRGGBB"));
            return;
        }
        provider->setFGColor(KoColor(color, viewManager->image()->colorSpace()));
    }
    if (arguments.contains("paintMode")) {
        const QString mode = arguments.value("paintMode").toString();
        const KisPaintOpPresetSP preset = provider->currentPreset();
        if (mode != QLatin1String("buildup") && mode != QLatin1String("wash")) {
            reply(TestControlMcpServer::ToolResult::failure("paintMode must be buildup or wash"));
            return;
        }
        if (!preset || !preset->settings()) {
            reply(TestControlMcpServer::ToolResult::failure("No current preset"));
            return;
        }
        // The Painting Mode option's property (KisPaintingModeOptionData): 1 Build-up, 2 Wash.
        preset->settings()->setProperty("PaintOpAction", mode == QLatin1String("buildup") ? 1 : 2);
    }

    // The paintop box applies a preset through queued updates; report after them.
    QTimer::singleShot(0, this, [reply]() {
        KisViewManager *viewManager = currentViewManager();
        if (!viewManager) {
            reply(TestControlMcpServer::ToolResult::failure("The view closed during brush_set"));
            return;
        }
        reply(TestControlMcpServer::ToolResult::success(brushInfo(viewManager)));
    });
}
