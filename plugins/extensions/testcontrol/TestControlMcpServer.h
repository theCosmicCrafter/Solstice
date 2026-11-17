/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TESTCONTROLMCPSERVER_H
#define TESTCONTROLMCPSERVER_H

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <deque>
#include <functional>

/**
 * JSON-RPC 2.0 / Model Context Protocol message handling for the test control
 * server: initialize, ping, tools/list and tools/call.
 *
 * Tool calls run one at a time in arrival order, because they drive the
 * same canvas; a handler may reply asynchronously (strokes, waits). Every
 * handler must call its reply exactly once; later calls are ignored.
 *
 * This class depends on QtCore only.
 */
class TestControlMcpServer : public QObject
{
    Q_OBJECT
public:
    struct ToolResult {
        QJsonObject structured;
        /// Additional content blocks, e.g. {"type": "image", ...}.
        QJsonArray extraContent;
        bool isError = false;

        static ToolResult success(const QJsonObject &structured);
        static ToolResult failure(const QString &message, const QJsonObject &details = QJsonObject());
    };

    using ToolReply = std::function<void(const ToolResult &result)>;
    using ToolHandler = std::function<void(const QJsonObject &arguments, const ToolReply &reply)>;
    using Reply = std::function<void(int httpStatus, const QByteArray &body)>;

    static QStringList supportedProtocolVersions();

    TestControlMcpServer(const QString &serverName, const QString &serverVersion, QObject *parent = nullptr);
    ~TestControlMcpServer() override;

    void setInstructions(const QString &instructions);
    void addTool(const QString &name, const QString &description, const QJsonObject &inputSchema, ToolHandler handler);
    QStringList toolNames() const;

    /// Handles one HTTP POST body and replies with the HTTP status and body.
    void handleMessage(const QByteArray &body, const Reply &reply);

private:
    struct Tool {
        QString name;
        QString description;
        QJsonObject inputSchema;
        ToolHandler handler;
    };

    QJsonObject initializeResult(const QJsonObject &params) const;
    QJsonObject toolsListResult() const;
    void callTool(const QJsonValue &id, const QJsonObject &params, const Reply &reply);
    void enqueue(std::function<void()> job);
    void finishJob();

    static QByteArray resultMessage(const QJsonValue &id, const QJsonObject &result);
    static QByteArray errorMessage(const QJsonValue &id, int code, const QString &message);

    QString m_serverName;
    QString m_serverVersion;
    QString m_instructions;
    QList<Tool> m_tools;
    std::deque<std::function<void()>> m_queue;
    bool m_busy = false;
};

#endif // TESTCONTROLMCPSERVER_H
