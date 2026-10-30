#include "httpserver.h"
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ww;

class TstHttp : public QObject {
    Q_OBJECT
private slots:
    void routing() {
        QTemporaryDir d;
        QDir(d.path()).mkpath("assets");
        for (const char *f : {"index.html", "assets/a.js", "assets/a b.css"}) { QFile x(d.filePath(f)); QVERIFY(x.open(QIODevice::WriteOnly)); x.write("x"); }
        QTemporaryDir outside;
        { QFile x(outside.filePath("secret.txt")); QVERIFY(x.open(QIODevice::WriteOnly)); x.write("s"); }
        QVERIFY(QFile::link(outside.filePath("secret.txt"), d.filePath("link.txt")));
        const QString root = QDir(d.path()).canonicalPath();
        const QByteArray host = "127.0.0.1:5000";
        auto r = [&](const char *m, const char *t, const QByteArray &h = "127.0.0.1:5000") { return HttpServer::route(root, m, t, h, 5000); };
        QCOMPARE(r("GET", "/").status, 200);
        QCOMPARE(r("GET", "/index.html?x=1").status, 200);
        QCOMPARE(r("GET", "/assets/a.js").type, QByteArray("text/javascript; charset=utf-8"));  // modules need a JS MIME type
        QCOMPARE(r("GET", "/assets/a%20b.css").status, 200);
        QCOMPARE(r("GET", "/nope").status, 404);
        QCOMPARE(r("POST", "/").status, 405);
        QCOMPARE(r("GET", "/", "evil.example:5000").status, 403);   // DNS rebinding
        QCOMPARE(r("GET", "/", "127.0.0.1:5001").status, 403);
        QVERIFY(r("GET", "/../../etc/passwd").status != 200);
        QVERIFY(r("GET", "/%2e%2e/%2e%2e/etc/passwd").status != 200);
        QCOMPARE(r("GET", "/link.txt").status, 403);                // symlink escape
        QCOMPARE(r("GET", "/a\\b").status, 400);
    }
    void servesOverTcp() {
        QTemporaryDir d;
        QFile x(d.filePath("index.html")); QVERIFY(x.open(QIODevice::WriteOnly)); x.write("<h1>hi</h1>"); x.close();
        HttpServer s(d.path());
        QVERIFY(s.start(0));
        QTcpSocket c;
        c.connectToHost("127.0.0.1", s.serverPort());
        QByteArray all;  // client and server share this thread: wait with the event loop, not waitFor*()
        connect(&c, &QTcpSocket::connected, &c, [&] { c.write("GET / HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(s.serverPort()) + "\r\n\r\n"); });
        connect(&c, &QTcpSocket::readyRead, &c, [&] { all += c.readAll(); });
        QTRY_VERIFY(all.endsWith("<h1>hi</h1>"));
        QVERIFY(all.startsWith("HTTP/1.1 200"));
        QVERIFY(all.endsWith("<h1>hi</h1>"));
    }
};
QTEST_MAIN(TstHttp)
#include "tst_httpserver.moc"
