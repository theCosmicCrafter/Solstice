/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TESTCONTROLHTTPSERVER_H
#define TESTCONTROLHTTPSERVER_H

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QTcpServer>

#include <functional>

class QTcpSocket;

/**
 * Minimal HTTP/1.1 endpoint for the MCP Streamable HTTP transport.
 *
 * It listens on 127.0.0.1 only and accepts POST /mcp with a Content-Length
 * body. Every request must carry "Authorization: Bearer <token>", a loopback
 * Host header and, when present, a loopback Origin (DNS rebinding guard).
 * One request is served per connection ("Connection: close"). The handler may
 * reply asynchronously; a reply after the client disconnected is dropped.
 *
 * This class depends on QtNetwork only, so the protocol tests run without
 * a Krita build.
 */
class TestControlHttpServer : public QObject
{
    Q_OBJECT
public:
    using Reply = std::function<void(int status, const QByteArray &body)>;
    using Handler = std::function<void(const QByteArray &body, const Reply &reply)>;

    struct Request {
        QByteArray method;
        QByteArray path;
        /// Header names are lower case; repeated headers keep the last value.
        QMap<QByteArray, QByteArray> headers;
    };

    enum class HeadResult {
        Incomplete,
        Complete,
        Invalid
    };

    static constexpr int MaxHeadBytes = 32 * 1024;
    static constexpr qint64 MaxBodyBytes = 8 * 1024 * 1024;
    static constexpr int IdleTimeoutMs = 30000;

    /**
     * Parses the request line and headers at the start of @p data.
     * On Complete, @p headLength is the byte count including the empty line.
     */
    static HeadResult parseRequestHead(const QByteArray &data, Request *request, int *headLength);
    static bool isAllowedHost(const QByteArray &host, quint16 port);
    static bool isAllowedOrigin(const QByteArray &origin, quint16 port);
    /// Constant-time comparison of an Authorization header with the token.
    static bool authorizationMatches(const QByteArray &authorization, const QByteArray &token);
    static QByteArray reasonPhrase(int status);

    TestControlHttpServer(const QByteArray &token, Handler handler, QObject *parent = nullptr);
    ~TestControlHttpServer() override;

    /// Listens on 127.0.0.1:@p port; port 0 picks a free port (tests).
    bool listen(quint16 port);
    quint16 port() const;
    QString errorString() const;
    void close();

private:
    struct Connection;

    void acceptConnections();
    void readFromConnection(QTcpSocket *socket, Connection *connection);
    /// Returns false and writes an error response if the request is rejected.
    bool validateRequest(QTcpSocket *socket, const Request &request);

    static void writeResponse(QTcpSocket *socket,
                              int status,
                              const QByteArray &body,
                              const QList<QPair<QByteArray, QByteArray>> &extraHeaders = {});

    QTcpServer m_server;
    QByteArray m_token;
    Handler m_handler;
};

#endif // TESTCONTROLHTTPSERVER_H
