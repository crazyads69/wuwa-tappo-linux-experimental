#include "httpserver.h"
#include <QFile>
#include <QFileInfo>
#include <QTcpSocket>
#include <QUrl>

namespace ww {

static QByteArray mimeFor(const QString &name) {
    static const QHash<QString, QByteArray> m{
        {"html", "text/html; charset=utf-8"}, {"js", "text/javascript; charset=utf-8"}, {"mjs", "text/javascript; charset=utf-8"},
        {"css", "text/css; charset=utf-8"}, {"json", "application/json"}, {"png", "image/png"}, {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"}, {"webp", "image/webp"}, {"svg", "image/svg+xml"}, {"woff2", "font/woff2"}, {"woff", "font/woff"},
        {"ttf", "font/ttf"}, {"otf", "font/otf"}, {"ogg", "audio/ogg"}, {"mp3", "audio/mpeg"}, {"mp4", "video/mp4"},
        {"wasm", "application/wasm"}, {"txt", "text/plain; charset=utf-8"}};
    return m.value(QFileInfo(name).suffix().toLower(), "application/octet-stream");
}

bool HttpServer::start(quint16 preferred) {
    return listen(QHostAddress::LocalHost, preferred) || listen(QHostAddress::LocalHost, 0);
}

HttpServer::Reply HttpServer::route(const QString &root, const QByteArray &method, const QByteArray &target, const QByteArray &host,
                                    quint16 port) {
    if (method != "GET" && method != "HEAD") return {405, {}, {}};
    // DNS-rebinding guard: a web page that resolves its own name to 127.0.0.1 must not be able to read the frontend.
    if (host != QByteArray("127.0.0.1:") + QByteArray::number(port)) return {403, {}, {}};
    QString path = QUrl::fromPercentEncoding(target.left(target.indexOf('?') < 0 ? target.size() : target.indexOf('?')));
    if (path.contains(QChar(0)) || path.contains('\\') || !path.startsWith('/')) return {400, {}, {}};
    if (path.endsWith('/')) path += "index.html";
    const QFileInfo fi(root + path);
    const QString canon = fi.canonicalFilePath();
    if (canon.isEmpty() || !fi.isFile()) return {404, {}, {}};
    if (!canon.startsWith(root + '/')) return {403, {}, {}};  // ../ and symlink escapes
    return {200, mimeFor(canon), canon};
}

void HttpServer::incomingConnection(qintptr fd) {
    auto *s = new QTcpSocket(this);
    if (s->setSocketDescriptor(fd)) handle(s); else delete s;
}

void HttpServer::handle(QTcpSocket *s) {
    connect(s, &QTcpSocket::readyRead, s, [this, s] {
        const QByteArray in = s->peek(16384);
        const int end = in.indexOf("\r\n\r\n");
        if (end < 0) { if (in.size() >= 16384) s->disconnectFromHost(); return; }
        s->read(end + 4);
        const auto lines = in.left(end).split('\n');
        const auto req = lines.value(0).trimmed().split(' ');
        QByteArray host;
        for (const auto &l : lines) if (l.toLower().startsWith("host:")) host = l.mid(5).trimmed();
        const Reply r = route(root_, req.value(0), req.value(1), host, serverPort());
        QByteArray body;
        if (r.status == 200) { QFile f(r.file); if (f.open(QIODevice::ReadOnly)) body = f.readAll(); }
        const QByteArray head = "HTTP/1.1 " + QByteArray::number(r.status) + (r.status == 200 ? " OK" : " Error")
                                + "\r\nContent-Type: " + (r.status == 200 ? r.type : "text/plain") + "\r\nContent-Length: "
                                + QByteArray::number(r.status == 200 ? body.size() : 0)
                                + "\r\nCache-Control: no-cache\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
        s->write(head);
        if (req.value(0) != "HEAD") s->write(body);
        s->disconnectFromHost();
    });
    connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
}

}  // namespace ww
