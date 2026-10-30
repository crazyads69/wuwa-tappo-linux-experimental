// End-to-end test of the bridge client against a local RFC 6455 server (plain ws:// is test-only).
#include "wsclient.h"
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtEndian>
#include <QtTest>
using namespace ww;

namespace {
struct Server : QTcpServer {
    QTcpSocket *peer = nullptr;
    QByteArray request, in;
    bool upgraded = false;
    QByteArray subprotocol;
    explicit Server(QObject *p = nullptr) : QTcpServer(p) {
        connect(this, &QTcpServer::newConnection, this, [this] {
            peer = nextPendingConnection();
            connect(peer, &QTcpSocket::readyRead, this, [this] {
                in += peer->readAll();
                if (!upgraded) {
                    const int e = in.indexOf("\r\n\r\n");
                    if (e < 0) return;
                    request = in.left(e);
                    in.remove(0, e + 4);
                    QByteArray key;
                    for (const auto &l : request.split('\n')) if (l.toLower().startsWith("sec-websocket-key:")) key = l.mid(18).trimmed();
                    peer->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                                + ws::acceptKey(key) + "\r\n" + (subprotocol.isEmpty() ? "" : "Sec-WebSocket-Protocol: " + subprotocol + "\r\n") + "\r\n");
                    upgraded = true;
                }
            });
        });
    }
    // Decode one masked client frame from `in`.
    bool clientFrame(quint8 &op, QByteArray &payload) {
        if (in.size() < 6) return false;
        const auto *b = reinterpret_cast<const uchar *>(in.constData());
        if (!(b[1] & 0x80)) return false;
        qsizetype len = b[1] & 0x7F, hdr = 2;
        if (len == 126) { len = qFromBigEndian<quint16>(b + 2); hdr = 4; }
        if (in.size() < hdr + 4 + len) return false;
        const QByteArray m = in.mid(hdr, 4);
        payload = in.mid(hdr + 4, len);
        for (qsizetype i = 0; i < len; ++i) payload[i] = payload[i] ^ m[i & 3];
        op = b[0] & 0x0F;
        in.remove(0, hdr + 4 + len);
        return true;
    }
    // QTRY_VERIFY evaluates its expression twice, and clientFrame() consumes: poll by hand.
    bool waitFrame(quint8 &op, QByteArray &payload) {
        for (int i = 0; i < 100; ++i) { if (clientFrame(op, payload)) return true; QTest::qWait(50); }
        return false;
    }
    void send(quint8 op, const QByteArray &p) { peer->write(QByteArray(1, char(0x80 | op)) + char(p.size()) + p); }
};
}  // namespace

class TstWsClient : public QObject {
    Q_OBJECT
private slots:
    void handshakeHasNoOriginAndMessagesFlow() {
        Server s;
        s.subprotocol = "v1";
        QVERIFY(s.listen(QHostAddress::LocalHost));
        WsClient c(QUrl(QString("ws://127.0.0.1:%1/socket.io/").arg(s.serverPort())), {"v1"}, true);
        QList<QJsonObject> ev;
        connect(&c, &WsClient::event, this, [&](const QJsonObject &e) { ev.append(e); });
        QTRY_VERIFY(!ev.isEmpty());
        QCOMPARE(ev[0]["event"].toString(), QString("open"));
        QCOMPARE(ev[0]["protocol"].toString(), QString("v1"));
        QVERIFY(!s.request.toLower().contains("origin:"));    // the whole point of the native bridge
        QVERIFY(!s.request.toLower().contains("cookie:"));
        QVERIFY(s.request.contains("Sec-WebSocket-Protocol: v1"));
        QVERIFY(s.request.contains("unofficial"));

        c.sendText("40");
        c.sendBinary(QByteArray("\x01\x02", 2));
        quint8 op; QByteArray p;
        QTRY_VERIFY(s.in.size() > 0);
        QVERIFY(s.clientFrame(op, p));
        QCOMPARE(op, quint8(ws::Text)); QCOMPARE(p, QByteArray("40"));
        QVERIFY(s.waitFrame(op, p));
        QCOMPARE(op, quint8(ws::Binary)); QCOMPARE(p, QByteArray("\x01\x02", 2));

        s.send(ws::Text, "42[\"hi\"]");
        s.send(ws::Binary, QByteArray("\xff\x00", 2));
        s.send(ws::Ping, "pp");
        QTRY_COMPARE(ev.size(), 3);
        QCOMPARE(ev[1]["data"].toString(), QString("42[\"hi\"]"));
        QCOMPARE(ev[2]["kind"].toString(), QString("binary"));
        QCOMPARE(QByteArray::fromBase64(ev[2]["data"].toString().toLatin1()), QByteArray("\xff\x00", 2));
        QVERIFY(s.waitFrame(op, p));
        QCOMPARE(op, quint8(ws::Pong)); QCOMPARE(p, QByteArray("pp"));

        QSignalSpy done(&c, &WsClient::finished);
        s.send(ws::Close, ws::closePayload(1000, "bye"));
        QTRY_VERIFY(done.count() == 1);
        QCOMPARE(ev.last()["event"].toString(), QString("close"));
        QCOMPARE(ev.last()["code"].toInt(), 1000);
        QVERIFY(ev.last()["clean"].toBool());
    }
    void rejectedHandshakeReportsHttpStatus() {
        QTcpServer s;
        QVERIFY(s.listen(QHostAddress::LocalHost));
        connect(&s, &QTcpServer::newConnection, &s, [&] {
            auto *p = s.nextPendingConnection();
            connect(p, &QTcpSocket::readyRead, p, [p] { p->readAll(); p->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"); });
        });
        WsClient c(QUrl(QString("ws://127.0.0.1:%1/socket.io/").arg(s.serverPort())), {}, true);
        QList<QJsonObject> ev;
        connect(&c, &WsClient::event, this, [&](const QJsonObject &e) { ev.append(e); });
        QTRY_COMPARE(ev.size(), 2);
        QCOMPARE(ev[0]["event"].toString(), QString("error"));
        QVERIFY(ev[0]["message"].toString().contains("403"));
        QCOMPARE(ev[1]["code"].toInt(), 1006);
        QVERIFY(!ev[1]["clean"].toBool());
    }
    void clientInitiatedCloseCompletes() {
        Server s;
        QVERIFY(s.listen(QHostAddress::LocalHost));
        WsClient c(QUrl(QString("ws://127.0.0.1:%1/socket.io/").arg(s.serverPort())), {}, true);
        QList<QJsonObject> ev;
        connect(&c, &WsClient::event, this, [&](const QJsonObject &e) { ev.append(e); });
        QTRY_VERIFY(!ev.isEmpty());
        c.close(1000, "done");
        quint8 op; QByteArray p;
        QVERIFY(s.waitFrame(op, p));
        QCOMPARE(op, quint8(ws::Close));
        s.send(ws::Close, p.left(2));
        QTRY_COMPARE(ev.last()["event"].toString(), QString("close"));
        QVERIFY(ev.last()["clean"].toBool());
    }
    void hubEnforcesTheAllowList() {
        WsHub hub;
        auto sink = [](const QJsonObject &) {};
        QVERIFY(!hub.connectSocket("main", "../bad", "wss://x", {}, sink).isEmpty());
        QVERIFY(!hub.connectSocket("main", "ok-1", "wss://evil.example/socket.io/", {}, sink).isEmpty());
        QVERIFY(!hub.connectSocket("main", "ok-1", QString("wss://") + ws::kHost + "/socket.io/", {"bad proto"}, sink).isEmpty());
        QVERIFY(!hub.send("nope", "text", "x").isEmpty());
    }
};
QTEST_MAIN(TstWsClient)
#include "tst_wsclient.moc"
