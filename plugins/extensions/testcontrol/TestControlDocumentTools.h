/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TESTCONTROLDOCUMENTTOOLS_H
#define TESTCONTROLDOCUMENTTOOLS_H

#include <QObject>

#include "TestControlMcpServer.h"

/**
 * The MCP tools that set up a test: opening, exporting and closing documents
 * (files only inside the test root), resetting the view, choosing the brush
 * preset, size, opacity, eraser mode and foreground color, and reading the
 * layer tree and choosing the active and selected layers.
 *
 * Like TestControlCanvasTools, every handler resolves the current window and
 * view when it runs and fails with a message when there is none.
 */
class TestControlDocumentTools : public QObject
{
    Q_OBJECT
public:
    /// @p testRoot is the canonical SOLSTICE_TEST_CONTROL_ROOT, or empty.
    explicit TestControlDocumentTools(const QString &testRoot, QObject *parent = nullptr);
    ~TestControlDocumentTools() override;

    void registerTools(TestControlMcpServer *server);

private:
    using Arguments = QJsonObject;
    using Reply = TestControlMcpServer::ToolReply;

    void documentOpen(const Arguments &arguments, const Reply &reply);
    void documentExportPng(const Arguments &arguments, const Reply &reply);
    void documentClose(const Arguments &arguments, const Reply &reply);
    void viewReset(const Arguments &arguments, const Reply &reply);
    void brushSet(const Arguments &arguments, const Reply &reply);
    void queryLayers(const Arguments &arguments, const Reply &reply);
    void layerActivate(const Arguments &arguments, const Reply &reply);

    QString m_testRoot;
};

#endif // TESTCONTROLDOCUMENTTOOLS_H
