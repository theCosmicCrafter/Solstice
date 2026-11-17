/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestControlHttpServer.h"

#include <QHostAddress>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

struct TestControlHttpServer::Connection : public QObject {
    explicit Connection(QObject *parent)
        : QObject(parent)
        , idleTimer(this)
    {
    }

    QByteArray buffer;
    Request request;
    int headLength = 0;
    qint64 contentLength = -1;
    bool dispatched = false;
    QTimer idleTimer;
};

namespace
{
QByteArray loopbackAuthority(const QByteArray &host, quint16 port)
{
    return host + ':' + QByteArray::number(port);
}
} // namespace

TestControlHttpServer::HeadResult
TestControlHttpServer::parseRequestHead(const QByteArray &data, Request *request, int *headLength)
{
    const int end = data.indexOf("\r\n\r\n");
    if (end < 0) {
        return data.size() > MaxHeadBytes ? HeadResult::Invalid : HeadResult::Incomplete;
    }
    if (end + 4 > MaxHeadBytes) {
        return HeadResult::Invalid;
    }

    const QList<QByteArray> lines = data.left(end).split('\n');
    const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
    if (requestLine.size() != 3 || !requestLine[2].startsWith("HTTP/1.")) {
        return HeadResult::Invalid;
    }

    Request result;
    result.method = requestLine[0];
    result.path = requestLine[1];
    const int query = result.path.indexOf('?');
    if (query >= 0) {
        result.path.truncate(query);
    }

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0) {
            return HeadResult::Invalid;
        }
        result.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
    }

    *request = result;
    *headLength = end + 4;
    return HeadResult::Complete;
}

bool TestControlHttpServer::isAllowedHost(const QByteArray &host, quint16 port)
{
    const QByteArray value = host.toLower();
    return value == loopbackAuthority("127.0.0.1", port) || value == loopbackAuthority("localhost", port);
}

bool TestControlHttpServer::isAllowedOrigin(const QByteArray &origin, quint16 port)
{
    const QByteArray value = origin.toLower();
    return value == "http://" + loopbackAuthority("127.0.0.1", port)
        || value == "http://" + loopbackAuthority("localhost", port);
}

bool TestControlHttpServer::authorizationMatches(const QByteArray &authorization, const QByteArray &token)
{
    static const QByteArray prefix("Bearer ");
    if (token.isEmpty() || !authorization.startsWith(prefix)) {
        return false;
    }

    const QByteArray given = authorization.mid(prefix.size());
    // Compare every byte of the token regardless of where the first
    // difference is, so the response time does not reveal a prefix.
    quint8 difference = given.size() == token.size() ? 0 : 1;
    for (int i = 0; i < token.size(); ++i) {
        const char g = i < given.size() ? given[i] : 0;
        difference |= quint8(g ^ token[i]);
    }
    return difference == 0;
}

QByteArray TestControlHttpServer::reasonPhrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 411:
        return "Length Required";
    case 413:
        return "Content Too Large";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    default:
        return "Unknown";
    }
}

TestControlHttpServer::TestControlHttpServer(const QByteArray &token, Handler handler, QObject *parent)
    : QObject(parent)
    , m_token(token)
    , m_handler(std::move(handler))
{
    connect(&m_server, &QTcpServer::newConnection, this, &TestControlHttpServer::acceptConnections);
}

TestControlHttpServer::~TestControlHttpServer()
{
    close();
}

bool TestControlHttpServer::listen(quint16 port)
{
    return m_server.listen(QHostAddress(QHostAddress::LocalHost), port);
}

quint16 TestControlHttpServer::port() const
{
    return m_server.serverPort();
}

QString TestControlHttpServer::errorString() const
{
    return m_server.errorString();
}

void TestControlHttpServer::close()
{
    m_server.close();
}

void TestControlHttpServer::acceptConnections()
{
    while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        Connection *connection = new Connection(socket);

        connection->idleTimer.setSingleShot(true);
        connection->idleTimer.setInterval(IdleTimeoutMs);
        connect(&connection->idleTimer, &QTimer::timeout, socket, &QTcpSocket::abort);
        connection->idleTimer.start();

        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, connection]() {
            readFromConnection(socket, connection);
        });
    }
}

void TestControlHttpServer::readFromConnection(QTcpSocket *socket, Connection *connection)
{
    if (connection->dispatched) {
        // One request per connection; ignore pipelined data.
        socket->readAll();
        return;
    }

    connection->buffer += socket->readAll();

    if (!connection->headLength) {
        const HeadResult result = parseRequestHead(connection->buffer, &connection->request, &connection->headLength);
        if (result == HeadResult::Incomplete) {
            return;
        }
        if (result == HeadResult::Invalid) {
            connection->dispatched = true;
            const int status = connection->buffer.size() > MaxHeadBytes ? 431 : 400;
            writeResponse(socket, status, QByteArray());
            return;
        }
        if (!validateRequest(socket, connection->request)) {
            connection->dispatched = true;
            return;
        }

        bool ok = false;
        const QByteArray length = connection->request.headers.value("content-length");
        connection->contentLength = length.toLongLong(&ok);
        if (!ok || connection->contentLength < 0) {
            connection->dispatched = true;
            writeResponse(socket, 411, QByteArray());
            return;
        }
        if (connection->contentLength > MaxBodyBytes) {
            connection->dispatched = true;
            writeResponse(socket, 413, QByteArray());
            return;
        }
        // curl waits for this before sending a larger body.
        if (connection->request.headers.value("expect").toLower() == "100-continue"
            && connection->buffer.size() - connection->headLength < connection->contentLength) {
            socket->write("HTTP/1.1 100 Continue\r\n\r\n");
        }
    }

    if (connection->buffer.size() - connection->headLength < connection->contentLength) {
        return;
    }

    connection->dispatched = true;
    connection->idleTimer.stop();

    const QByteArray body = connection->buffer.mid(connection->headLength, connection->contentLength);
    connection->buffer.clear();

    QPointer<QTcpSocket> guard(socket);
    m_handler(body, [guard](int status, const QByteArray &responseBody) {
        if (guard) {
            writeResponse(guard, status, responseBody);
        }
    });
}

bool TestControlHttpServer::validateRequest(QTcpSocket *socket, const Request &request)
{
    const quint16 serverPort = port();

    if (!isAllowedHost(request.headers.value("host"), serverPort)) {
        writeResponse(socket, 403, QByteArray());
        return false;
    }
    if (request.headers.contains("origin") && !isAllowedOrigin(request.headers.value("origin"), serverPort)) {
        writeResponse(socket, 403, QByteArray());
        return false;
    }
    if (request.path != "/mcp") {
        writeResponse(socket, 404, QByteArray());
        return false;
    }
    if (!authorizationMatches(request.headers.value("authorization"), m_token)) {
        writeResponse(socket, 401, QByteArray(), {{"WWW-Authenticate", "Bearer"}});
        return false;
    }
    if (request.method != "POST") {
        // No server-initiated SSE stream and no session termination.
        writeResponse(socket, 405, QByteArray(), {{"Allow", "POST"}});
        return false;
    }
    if (request.headers.contains("transfer-encoding")) {
        writeResponse(socket, 501, QByteArray());
        return false;
    }
    return true;
}

void TestControlHttpServer::writeResponse(QTcpSocket *socket,
                                          int status,
                                          const QByteArray &body,
                                          const QList<QPair<QByteArray, QByteArray>> &extraHeaders)
{
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reasonPhrase(status) + "\r\n";
    if (!body.isEmpty()) {
        response += "Content-Type: application/json\r\n";
    }
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\n";
    response += "Connection: close\r\n";
    for (const auto &header : extraHeaders) {
        response += header.first + ": " + header.second + "\r\n";
    }
    response += "\r\n";
    response += body;

    socket->write(response);
    socket->disconnectFromHost();
}
