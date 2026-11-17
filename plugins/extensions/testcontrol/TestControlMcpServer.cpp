/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestControlMcpServer.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace
{
// JSON-RPC 2.0 error codes.
constexpr int ParseError = -32700;
constexpr int InvalidRequest = -32600;
constexpr int MethodNotFound = -32601;
constexpr int InvalidParams = -32602;
} // namespace

TestControlMcpServer::ToolResult TestControlMcpServer::ToolResult::success(const QJsonObject &structured)
{
    ToolResult result;
    result.structured = structured;
    return result;
}

TestControlMcpServer::ToolResult TestControlMcpServer::ToolResult::failure(const QString &message,
                                                                           const QJsonObject &details)
{
    ToolResult result;
    result.structured = details;
    result.structured.insert(QStringLiteral("error"), message);
    result.isError = true;
    return result;
}

QStringList TestControlMcpServer::supportedProtocolVersions()
{
    // Newest first; the first entry is offered when the client asks for an
    // unknown version.
    return {QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26"), QStringLiteral("2024-11-05")};
}

TestControlMcpServer::TestControlMcpServer(const QString &serverName, const QString &serverVersion, QObject *parent)
    : QObject(parent)
    , m_serverName(serverName)
    , m_serverVersion(serverVersion)
{
}

TestControlMcpServer::~TestControlMcpServer() = default;

void TestControlMcpServer::setInstructions(const QString &instructions)
{
    m_instructions = instructions;
}

void TestControlMcpServer::addTool(const QString &name,
                                   const QString &description,
                                   const QJsonObject &inputSchema,
                                   ToolHandler handler)
{
    m_tools.append({name, description, inputSchema, std::move(handler)});
}

QStringList TestControlMcpServer::toolNames() const
{
    QStringList names;
    for (const Tool &tool : m_tools) {
        names << tool.name;
    }
    return names;
}

void TestControlMcpServer::handleMessage(const QByteArray &body, const Reply &reply)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        reply(400, errorMessage(QJsonValue::Null, ParseError, parseError.errorString()));
        return;
    }
    if (!document.isObject()) {
        reply(400, errorMessage(QJsonValue::Null, InvalidRequest, QStringLiteral("Batch requests are not supported")));
        return;
    }

    const QJsonObject message = document.object();
    if (!message.contains(QStringLiteral("method"))) {
        if (message.contains(QStringLiteral("result")) || message.contains(QStringLiteral("error"))) {
            // A response to a server request; this server sends none.
            reply(202, QByteArray());
        } else {
            reply(400, errorMessage(QJsonValue::Null, InvalidRequest, QStringLiteral("Missing method")));
        }
        return;
    }

    const QString method = message.value(QStringLiteral("method")).toString();
    if (!message.contains(QStringLiteral("id"))) {
        // Notifications (initialized, cancelled) need no answer.
        reply(202, QByteArray());
        return;
    }

    const QJsonValue id = message.value(QStringLiteral("id"));
    const QJsonObject params = message.value(QStringLiteral("params")).toObject();

    if (method == QLatin1String("initialize")) {
        reply(200, resultMessage(id, initializeResult(params)));
    } else if (method == QLatin1String("ping")) {
        reply(200, resultMessage(id, QJsonObject()));
    } else if (method == QLatin1String("tools/list")) {
        reply(200, resultMessage(id, toolsListResult()));
    } else if (method == QLatin1String("tools/call")) {
        callTool(id, params, reply);
    } else {
        reply(200, errorMessage(id, MethodNotFound, QStringLiteral("Method not found: %1").arg(method)));
    }
}

QJsonObject TestControlMcpServer::initializeResult(const QJsonObject &params) const
{
    const QStringList versions = supportedProtocolVersions();
    QString version = params.value(QStringLiteral("protocolVersion")).toString();
    if (!versions.contains(version)) {
        version = versions.first();
    }

    QJsonObject result{
        {QStringLiteral("protocolVersion"), version},
        {QStringLiteral("capabilities"),
         QJsonObject{{QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
        {QStringLiteral("serverInfo"),
         QJsonObject{{QStringLiteral("name"), m_serverName}, {QStringLiteral("version"), m_serverVersion}}},
    };
    if (!m_instructions.isEmpty()) {
        result.insert(QStringLiteral("instructions"), m_instructions);
    }
    return result;
}

QJsonObject TestControlMcpServer::toolsListResult() const
{
    QJsonArray tools;
    for (const Tool &tool : m_tools) {
        tools.append(QJsonObject{
            {QStringLiteral("name"), tool.name},
            {QStringLiteral("description"), tool.description},
            {QStringLiteral("inputSchema"), tool.inputSchema},
        });
    }
    return QJsonObject{{QStringLiteral("tools"), tools}};
}

void TestControlMcpServer::callTool(const QJsonValue &id, const QJsonObject &params, const Reply &reply)
{
    const QString name = params.value(QStringLiteral("name")).toString();
    auto it = std::find_if(m_tools.cbegin(), m_tools.cend(), [&name](const Tool &tool) {
        return tool.name == name;
    });
    if (it == m_tools.cend()) {
        reply(200, errorMessage(id, InvalidParams, QStringLiteral("Unknown tool: %1").arg(name)));
        return;
    }

    const QJsonValue argumentsValue = params.value(QStringLiteral("arguments"));
    if (!argumentsValue.isUndefined() && !argumentsValue.isNull() && !argumentsValue.isObject()) {
        reply(200, errorMessage(id, InvalidParams, QStringLiteral("Tool arguments must be an object")));
        return;
    }

    const ToolHandler handler = it->handler;
    const QJsonObject arguments = argumentsValue.toObject();

    enqueue([this, id, arguments, handler, reply]() {
        auto replied = std::make_shared<bool>(false);
        handler(arguments, [this, id, reply, replied](const ToolResult &result) {
            if (*replied) {
                return;
            }
            *replied = true;

            QJsonArray content{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"),
                 QString::fromUtf8(QJsonDocument(result.structured).toJson(QJsonDocument::Compact))},
            }};
            for (const QJsonValue &extra : result.extraContent) {
                content.append(extra);
            }

            reply(200,
                  resultMessage(id,
                                QJsonObject{
                                    {QStringLiteral("content"), content},
                                    {QStringLiteral("structuredContent"), result.structured},
                                    {QStringLiteral("isError"), result.isError},
                                }));
            finishJob();
        });
    });
}

void TestControlMcpServer::enqueue(std::function<void()> job)
{
    m_queue.push_back(std::move(job));
    if (!m_busy) {
        finishJob();
    }
}

void TestControlMcpServer::finishJob()
{
    m_busy = false;
    if (m_queue.empty()) {
        return;
    }

    // Start the next call from the event loop, so a handler that replies
    // synchronously does not recurse into the next one.
    m_busy = true;
    QTimer::singleShot(0, this, [this]() {
        std::function<void()> job = std::move(m_queue.front());
        m_queue.pop_front();
        job();
    });
}

QByteArray TestControlMcpServer::resultMessage(const QJsonValue &id, const QJsonObject &result)
{
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                             {QStringLiteral("id"), id},
                             {QStringLiteral("result"), result},
                         })
        .toJson(QJsonDocument::Compact);
}

QByteArray TestControlMcpServer::errorMessage(const QJsonValue &id, int code, const QString &message)
{
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                             {QStringLiteral("id"), id},
                             {QStringLiteral("error"),
                              QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}},
                         })
        .toJson(QJsonDocument::Compact);
}
