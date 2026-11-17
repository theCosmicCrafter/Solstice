/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TESTCONTROLPLUGIN_H
#define TESTCONTROLPLUGIN_H

#include <QObject>
#include <QVariant>

class TestControlCanvasTools;
class TestControlDocumentTools;
class TestControlUiTools;
class TestControlHttpServer;
class TestControlMcpServer;

/**
 * Application plugin that serves the test control MCP endpoint.
 *
 * Inert unless SOLSTICE_TEST_CONTROL_PORT is set. It then also requires
 * SOLSTICE_TEST_CONTROL_TOKEN (at least 16 characters) and listens on
 * http://127.0.0.1:<port>/mcp. Loaded once per process, with the first
 * main window ("Krita/ApplicationPlugin").
 */
class TestControlPlugin : public QObject
{
    Q_OBJECT
public:
    TestControlPlugin(QObject *parent, const QVariantList &);
    ~TestControlPlugin() override;

private:
    TestControlCanvasTools *m_tools = nullptr;
    TestControlDocumentTools *m_documentTools = nullptr;
    TestControlUiTools *m_uiTools = nullptr;
    TestControlMcpServer *m_mcp = nullptr;
    TestControlHttpServer *m_http = nullptr;
};

#endif // TESTCONTROLPLUGIN_H
