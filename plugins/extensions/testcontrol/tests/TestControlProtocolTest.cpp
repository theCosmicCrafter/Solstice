/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

#include "TestControlHttpServer.h"
#include "TestControlMcpServer.h"

namespace
{
const QByteArray Token("0123456789abcdef0123");

struct Captured {
    int status = 0;
    QByteArray body;
    int calls = 0;
};

TestControlMcpServer::Reply capture(Captured *captured)
{
    return [captured](int status, const QByteArray &body) {
        captured->status = status;
        captured->body = body;
        captured->calls++;
    };
}

QByteArray request(const QString &method, const QJsonObject &params = QJsonObject(), const QJsonValue &id = 1)
{
    QJsonObject message{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    if (!id.isUndefined()) {
        message.insert("id", id);
    }
    return QJsonDocument(message).toJson(QJsonDocument::Compact);
}

QJsonObject parse(const QByteArray &body)
{
    return QJsonDocument::fromJson(body).object();
}

/// Sends @p parts with a pause between them and returns the raw HTTP response.
QByteArray exchange(quint16 port, const QList<QByteArray> &parts)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
    if (!socket.waitForConnected(2000)) {
        return QByteArray();
    }
    for (const QByteArray &part : parts) {
        socket.write(part);
        socket.flush();
        QTest::qWait(20);
    }

    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    while (socket.state() == QAbstractSocket::ConnectedState && timer.elapsed() < 5000) {
        // The server runs in this thread: process its events instead of
        // blocking in waitForReadyRead(), which starves it on Windows.
        QTest::qWait(20);
        response += socket.readAll();
    }
    response += socket.readAll();
    return response;
}

QByteArray exchange(quint16 port, const QByteArray &raw)
{
    return exchange(port, QList<QByteArray>{raw});
}

QByteArray post(quint16 port, const QByteArray &body, const QByteArray &extraHeaders)
{
    return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n" + extraHeaders
        + "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

int statusOf(const QByteArray &response)
{
    return response.mid(9, 3).toInt();
}
} // namespace

class TestControlProtocolTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testParseRequestHead();
    void testHostAndOrigin();
    void testAuthorization();
    void testInitializeNegotiatesVersion();
    void testNotificationsAreAccepted();
    void testErrors();
    void testToolsListAndCall();
    void testCallsRunInOrder();
    void testHttpEndToEnd();
};

void TestControlProtocolTest::testParseRequestHead()
{
    TestControlHttpServer::Request request;
    int headLength = 0;

    QCOMPARE(TestControlHttpServer::parseRequestHead("POST /mcp HTTP/1.1\r\nHost: x", &request, &headLength),
             TestControlHttpServer::HeadResult::Incomplete);

    const QByteArray head = "POST /mcp?x=1 HTTP/1.1\r\nHost: 127.0.0.1:5\r\nContent-Length:  12 \r\n\r\nbody";
    QCOMPARE(TestControlHttpServer::parseRequestHead(head, &request, &headLength),
             TestControlHttpServer::HeadResult::Complete);
    QCOMPARE(request.method, QByteArray("POST"));
    QCOMPARE(request.path, QByteArray("/mcp"));
    QCOMPARE(request.headers.value("content-length"), QByteArray("12"));
    QCOMPARE(request.headers.value("host"), QByteArray("127.0.0.1:5"));
    QCOMPARE(head.mid(headLength), QByteArray("body"));

    QCOMPARE(TestControlHttpServer::parseRequestHead("GARBAGE\r\n\r\n", &request, &headLength),
             TestControlHttpServer::HeadResult::Invalid);
    QCOMPARE(TestControlHttpServer::parseRequestHead("GET / HTTP/1.1\r\nNoColon\r\n\r\n", &request, &headLength),
             TestControlHttpServer::HeadResult::Invalid);
    QCOMPARE(TestControlHttpServer::parseRequestHead(QByteArray(TestControlHttpServer::MaxHeadBytes + 1, 'a'),
                                                     &request,
                                                     &headLength),
             TestControlHttpServer::HeadResult::Invalid);
}

void TestControlProtocolTest::testHostAndOrigin()
{
    QVERIFY(TestControlHttpServer::isAllowedHost("127.0.0.1:8765", 8765));
    QVERIFY(TestControlHttpServer::isAllowedHost("LOCALHOST:8765", 8765));
    QVERIFY(!TestControlHttpServer::isAllowedHost("127.0.0.1:8766", 8765));
    QVERIFY(!TestControlHttpServer::isAllowedHost("evil.example:8765", 8765));
    QVERIFY(!TestControlHttpServer::isAllowedHost("", 8765));

    QVERIFY(TestControlHttpServer::isAllowedOrigin("http://localhost:8765", 8765));
    QVERIFY(!TestControlHttpServer::isAllowedOrigin("http://evil.example:8765", 8765));
    QVERIFY(!TestControlHttpServer::isAllowedOrigin("null", 8765));
}

void TestControlProtocolTest::testAuthorization()
{
    QVERIFY(TestControlHttpServer::authorizationMatches("Bearer " + Token, Token));
    QVERIFY(!TestControlHttpServer::authorizationMatches("Bearer " + Token + "x", Token));
    QVERIFY(!TestControlHttpServer::authorizationMatches("Bearer " + Token.left(Token.size() - 1), Token));
    QVERIFY(!TestControlHttpServer::authorizationMatches("Basic " + Token, Token));
    QVERIFY(!TestControlHttpServer::authorizationMatches("", Token));
    QVERIFY(!TestControlHttpServer::authorizationMatches("Bearer ", QByteArray()));
}

void TestControlProtocolTest::testInitializeNegotiatesVersion()
{
    TestControlMcpServer server("solstice-test", "1.0");
    server.setInstructions("Use wait_idle.");
    Captured captured;

    server.handleMessage(request("initialize", {{"protocolVersion", "2025-03-26"}}), capture(&captured));
    QCOMPARE(captured.status, 200);
    QJsonObject result = parse(captured.body).value("result").toObject();
    QCOMPARE(result.value("protocolVersion").toString(), QString("2025-03-26"));
    QCOMPARE(result.value("serverInfo").toObject().value("name").toString(), QString("solstice-test"));
    QVERIFY(result.value("capabilities").toObject().contains("tools"));
    QCOMPARE(result.value("instructions").toString(), QString("Use wait_idle."));

    server.handleMessage(request("initialize", {{"protocolVersion", "1999-01-01"}}), capture(&captured));
    result = parse(captured.body).value("result").toObject();
    QCOMPARE(result.value("protocolVersion").toString(), TestControlMcpServer::supportedProtocolVersions().first());
}

void TestControlProtocolTest::testNotificationsAreAccepted()
{
    TestControlMcpServer server("solstice-test", "1.0");
    Captured captured;

    server.handleMessage(request("notifications/initialized", {}, QJsonValue::Undefined), capture(&captured));
    QCOMPARE(captured.status, 202);
    QVERIFY(captured.body.isEmpty());

    server.handleMessage(R"({"jsonrpc":"2.0","id":7,"result":{}})", capture(&captured));
    QCOMPARE(captured.status, 202);
}

void TestControlProtocolTest::testErrors()
{
    TestControlMcpServer server("solstice-test", "1.0");
    Captured captured;

    server.handleMessage("{not json", capture(&captured));
    QCOMPARE(captured.status, 400);
    QCOMPARE(parse(captured.body).value("error").toObject().value("code").toInt(), -32700);

    server.handleMessage("[]", capture(&captured));
    QCOMPARE(parse(captured.body).value("error").toObject().value("code").toInt(), -32600);

    server.handleMessage(request("no/such/method"), capture(&captured));
    QCOMPARE(captured.status, 200);
    QCOMPARE(parse(captured.body).value("error").toObject().value("code").toInt(), -32601);

    server.handleMessage(request("tools/call", {{"name", "missing"}}), capture(&captured));
    QCOMPARE(parse(captured.body).value("error").toObject().value("code").toInt(), -32602);
}

void TestControlProtocolTest::testToolsListAndCall()
{
    TestControlMcpServer server("solstice-test", "1.0");
    const QJsonObject schema{{"type", "object"}};
    server.addTool("echo", "Echo the arguments", schema, [](const QJsonObject &arguments, const auto &reply) {
        reply(TestControlMcpServer::ToolResult::success(arguments));
        reply(TestControlMcpServer::ToolResult::failure("second reply must be ignored"));
    });
    server.addTool("fail", "Always fails", schema, [](const QJsonObject &, const auto &reply) {
        reply(TestControlMcpServer::ToolResult::failure("broken", {{"detail", 3}}));
    });

    Captured captured;
    server.handleMessage(request("tools/list"), capture(&captured));
    const QJsonArray tools = parse(captured.body).value("result").toObject().value("tools").toArray();
    QCOMPARE(tools.size(), 2);
    QCOMPARE(tools.at(0).toObject().value("name").toString(), QString("echo"));
    QCOMPARE(tools.at(0).toObject().value("inputSchema").toObject(), schema);

    server.handleMessage(request("tools/call", {{"name", "echo"}, {"arguments", QJsonObject{{"x", 5}}}}),
                         capture(&captured));
    QTRY_COMPARE(captured.calls, 2);
    QJsonObject result = parse(captured.body).value("result").toObject();
    QCOMPARE(result.value("isError").toBool(), false);
    QCOMPARE(result.value("structuredContent").toObject().value("x").toInt(), 5);
    QCOMPARE(result.value("content").toArray().at(0).toObject().value("text").toString(), QString(R"({"x":5})"));

    QTest::qWait(20);
    QCOMPARE(captured.calls, 2);

    server.handleMessage(request("tools/call", {{"name", "fail"}}), capture(&captured));
    QTRY_COMPARE(captured.calls, 3);
    result = parse(captured.body).value("result").toObject();
    QCOMPARE(result.value("isError").toBool(), true);
    QCOMPARE(result.value("structuredContent").toObject().value("error").toString(), QString("broken"));
    QCOMPARE(result.value("structuredContent").toObject().value("detail").toInt(), 3);

    server.handleMessage(request("tools/call", {{"name", "echo"}, {"arguments", 5}}), capture(&captured));
    QCOMPARE(parse(captured.body).value("error").toObject().value("code").toInt(), -32602);
}

void TestControlProtocolTest::testCallsRunInOrder()
{
    TestControlMcpServer server("solstice-test", "1.0");
    QStringList events;

    server.addTool("slow", "Replies after a delay", {}, [&events](const QJsonObject &arguments, const auto &reply) {
        const QString tag = arguments.value("tag").toString();
        events << "start " + tag;
        QTimer::singleShot(30, [&events, tag, reply]() {
            events << "end " + tag;
            reply(TestControlMcpServer::ToolResult::success({}));
        });
    });

    Captured first;
    Captured second;
    server.handleMessage(request("tools/call", {{"name", "slow"}, {"arguments", QJsonObject{{"tag", "a"}}}}),
                         capture(&first));
    server.handleMessage(request("tools/call", {{"name", "slow"}, {"arguments", QJsonObject{{"tag", "b"}}}}, 2),
                         capture(&second));

    QTRY_COMPARE(second.calls, 1);
    QCOMPARE(first.calls, 1);
    QCOMPARE(events, QStringList({"start a", "end a", "start b", "end b"}));
    QCOMPARE(parse(second.body).value("id").toInt(), 2);
}

void TestControlProtocolTest::testHttpEndToEnd()
{
    TestControlMcpServer mcp("solstice-test", "1.0");
    mcp.addTool("later", "Replies asynchronously", {}, [](const QJsonObject &, const auto &reply) {
        QTimer::singleShot(10, [reply]() {
            reply(TestControlMcpServer::ToolResult::success({{"done", true}}));
        });
    });

    TestControlHttpServer http(Token, [&mcp](const QByteArray &body, const TestControlHttpServer::Reply &reply) {
        mcp.handleMessage(body, reply);
    });
    QVERIFY(http.listen(0));
    const quint16 port = http.port();
    const QByteArray auth = "Authorization: Bearer " + Token + "\r\n";

    QByteArray response = exchange(port, post(port, request("ping"), auth));
    QCOMPARE(statusOf(response), 200);
    QVERIFY(response.contains("Content-Type: application/json"));
    QVERIFY(response.endsWith(R"({"id":1,"jsonrpc":"2.0","result":{}})"));

    response = exchange(port, post(port, request("tools/call", {{"name", "later"}}), auth));
    QCOMPARE(statusOf(response), 200);
    QVERIFY(response.contains(R"("done":true)"));

    // The head and body may arrive in several segments.
    const QByteArray raw = post(port, request("ping"), auth);
    QCOMPARE(statusOf(exchange(port, {raw.left(10), raw.mid(10, raw.size() - 15), raw.right(5)})), 200);

    // "Expect: 100-continue" gets an interim response before the body.
    {
        const QByteArray full = post(port, request("ping"), auth + "Expect: 100-continue\r\n");
        const int split = full.indexOf("\r\n\r\n") + 4;
        const QByteArray response = exchange(port, {full.left(split), full.mid(split)});
        QVERIFY(response.startsWith("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK"));
    }

    QCOMPARE(statusOf(exchange(port, post(port, request("ping"), QByteArray()))), 401);
    QCOMPARE(statusOf(exchange(port, post(port, request("ping"), "Authorization: Bearer wrong\r\n"))), 401);
    QCOMPARE(statusOf(exchange(port, post(port, request("ping"), auth + "Origin: http://evil.example\r\n"))), 403);

    QByteArray wrongHost = post(port, request("ping"), auth);
    wrongHost.replace("Host: 127.0.0.1:", "Host: evil.example:");
    QCOMPARE(statusOf(exchange(port, wrongHost)), 403);

    QByteArray wrongPath = post(port, request("ping"), auth);
    wrongPath.replace("POST /mcp", "POST /other");
    QCOMPARE(statusOf(exchange(port, wrongPath)), 404);

    const QByteArray get = "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n" + auth + "\r\n";
    response = exchange(port, get);
    QCOMPARE(statusOf(response), 405);
    QVERIFY(response.contains("Allow: POST"));

    const QByteArray noLength =
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n" + auth + "\r\n";
    QCOMPARE(statusOf(exchange(port, noLength)), 411);

    const QByteArray tooLarge = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n" + auth
        + "Content-Length: " + QByteArray::number(TestControlHttpServer::MaxBodyBytes + 1) + "\r\n\r\n";
    QCOMPARE(statusOf(exchange(port, tooLarge)), 413);

    QCOMPARE(
        statusOf(exchange(port, post(port, request("notifications/initialized", {}, QJsonValue::Undefined), auth))),
        202);
}

QTEST_GUILESS_MAIN(TestControlProtocolTest)

#include "TestControlProtocolTest.moc"
