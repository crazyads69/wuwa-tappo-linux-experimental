#include "wsframe.h"
#include <QtEndian>
#include <QtTest>
using namespace ww::ws;

// server->client frames are unmasked
static QByteArray serverFrame(quint8 op, const QByteArray &p, bool fin = true) {
    QByteArray f;
    f.append(char((fin ? 0x80 : 0) | op));
    if (p.size() < 126) f.append(char(p.size()));
    else if (p.size() <= 0xFFFF) { f.append(char(126)); f.append(char(p.size() >> 8)); f.append(char(p.size() & 0xFF)); }
    else { f.append(char(127)); char b[8]; qToBigEndian<quint64>(p.size(), b); f.append(b, 8); }
    return f + p;
}

class TstWsFrame : public QObject {
    Q_OBJECT
private slots:
    void rfcAcceptKey() { QCOMPARE(acceptKey("dGhlIHNhbXBsZSBub25jZQ=="), QByteArray("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")); }  // RFC 6455 section 1.3
    void clientFrameIsMaskedAndRoundTrips() {
        const QByteArray f = encodeFrame(Text, "Hello", 0x37fa213d);  // RFC 6455 section 5.7 example
        QCOMPARE(f.toHex(), QByteArray("818537fa213d7f9f4d5158"));
    }
    void lengthEncodings() {
        QCOMPARE(encodeFrame(Binary, QByteArray(125, 'x'), 0).at(1) & 0x7F, 125);
        QCOMPARE(encodeFrame(Binary, QByteArray(126, 'x'), 0).at(1) & 0x7F, 126);
        QCOMPARE(encodeFrame(Binary, QByteArray(70000, 'x'), 0).size(), 70000 + 10 + 4);
        QCOMPARE(encodeFrame(Binary, QByteArray(70000, 'x'), 0).at(1) & 0x7F, 127);
    }
    void parsesAcrossArbitrarySplits() {
        const QByteArray wire = serverFrame(Text, "héllo") + serverFrame(Binary, QByteArray(300, 'b')) + serverFrame(Ping, "p");
        Parser p;
        QList<Message> got;
        for (char c : wire) {  // one byte at a time
            p.feed(QByteArray(1, c));
            Message m;
            while (p.next(m)) got.append(m);
        }
        QVERIFY(!p.error());
        QCOMPARE(got.size(), 3);
        QCOMPARE(QString::fromUtf8(got[0].payload), QString("héllo"));
        QCOMPARE(got[1].payload.size(), 300);
        QCOMPARE(got[2].opcode, quint8(Ping));
    }
    void reassemblesFragmentsWithInterleavedPing() {
        Parser p;
        p.feed(serverFrame(Text, "ab", false) + serverFrame(Ping, "x") + serverFrame(Continuation, "cd", true));
        Message m;
        QVERIFY(p.next(m)); QCOMPARE(m.opcode, quint8(Ping));
        QVERIFY(p.next(m)); QCOMPARE(m.payload, QByteArray("abcd")); QCOMPARE(m.opcode, quint8(Text));
        QVERIFY(!p.error());
    }
    void protocolViolationsAreErrors_data() {
        QTest::addColumn<QByteArray>("wire");
        QTest::newRow("masked server frame") << QByteArray("\x81\x81\0\0\0\0x", 7);
        QTest::newRow("reserved bit") << QByteArray("\xC1\x00", 2);
        QTest::newRow("fragmented control") << QByteArray("\x09\x00", 2);
        QTest::newRow("orphan continuation") << serverFrame(Continuation, "x");
        QTest::newRow("reserved opcode") << QByteArray("\x83\x00", 2);
        QTest::newRow("oversized") << QByteArray("\x82\x7F\0\0\0\0\x01\x00\0\0\0", 11);
        QTest::newRow("invalid utf8 text") << serverFrame(Text, QByteArray("\xff\xfe", 2));
        QTest::newRow("new data frame mid-message") << (serverFrame(Text, "a", false) + serverFrame(Text, "b"));
    }
    void protocolViolationsAreErrors() {
        QFETCH(QByteArray, wire);
        Parser p;
        p.feed(wire);
        Message m;
        bool ok = true;
        while (ok && !p.error()) ok = p.next(m);
        QVERIFY(p.error());
    }
    void closePayloadIsCodePlusReason() { QCOMPARE(closePayload(1000, "bye").toHex(), QByteArray("03e8627965")); }
    void destinationAllowList() {
        const QString ok = QString("wss://") + kHost + "/socket.io/?EIO=4&transport=websocket";
        QVERIFY(urlAllowed(ok));  // socket.io appends a query; only host, path, scheme and port are pinned
        QVERIFY(urlAllowed(QString("wss://") + kHost + "/socket.io/"));
        QVERIFY(urlAllowed(QString("wss://") + kHost + "/socket.io"));
        for (const QString &bad : {QString("ws://") + kHost + "/socket.io/", QString("wss://") + kHost + ":8443/socket.io/",
                                   QString("wss://evil.example/socket.io/"), QString("wss://") + kHost + "/other",
                                   QString("wss://u:p@") + kHost + "/socket.io/", QString("wss://") + kHost + "/socket.io/#x",
                                   QString("wss://") + kHost + ".evil.example/socket.io/", QString()})
            QVERIFY2(!urlAllowed(bad), qPrintable(bad));
    }
    void tokensAndIds() {
        QVERIFY(isToken("socket.io-v4"));
        QVERIFY(!isToken("a b"));
        QVERIFY(!isToken(""));
        QVERIFY(idValid("ws-abc123-1"));
        QVERIFY(!idValid("../x"));
        QVERIFY(!idValid(QString(65, 'a')));
    }
};
QTEST_APPLESS_MAIN(TstWsFrame)
#include "tst_wsframe.moc"
