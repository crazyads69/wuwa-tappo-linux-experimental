#include "wsclient.h"
#include <QRandomGenerator>
#include <QtEndian>

namespace ww {

WsClient::WsClient(QUrl url, QStringList protocols, bool plain, QObject *parent)
    : QObject(parent), url_(std::move(url)), protocols_(std::move(protocols)) {
    key_ = [] { QByteArray k; for (int i = 0; i < 16; ++i) k.append(char(QRandomGenerator::system()->bounded(256))); return k.toBase64(); }();
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] { fail(open_ ? "WebSocket close timed out" : "WebSocket connection timed out"); });
    timeout_.start(20000);
    connect(&sock_, &QSslSocket::readyRead, this, &WsClient::onReadyRead);
    connect(&sock_, &QSslSocket::errorOccurred, this, [this] {
        fail(sock_.error() == QAbstractSocket::SslHandshakeFailedError ? "WebSocket TLS connection failed" : "WebSocket network connection failed");
    });
    connect(&sock_, &QSslSocket::disconnected, this, [this] { if (!done_) finish(1006, {}, false); });
    if (plain) {
        connect(&sock_, &QSslSocket::connected, this, &WsClient::onConnected);
        sock_.connectToHost(url_.host(), quint16(url_.port(80)));
    } else {
        connect(&sock_, &QSslSocket::encrypted, this, &WsClient::onConnected);
        sock_.connectToHostEncrypted(url_.host(), quint16(url_.port(443)));
    }
}

void WsClient::onConnected() {
    QByteArray r = "GET " + url_.path(QUrl::FullyEncoded).toUtf8() + (url_.hasQuery() ? "?" + url_.query(QUrl::FullyEncoded).toUtf8() : "")
                   + " HTTP/1.1\r\nHost: " + url_.host().toUtf8() + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: " + key_ + "\r\nSec-WebSocket-Version: 13\r\n"
                     "User-Agent: wuwatappo-qt/1.0.1 (unofficial)\r\n";
    if (!protocols_.isEmpty()) r += "Sec-WebSocket-Protocol: " + protocols_.join(", ").toUtf8() + "\r\n";
    sock_.write(r + "\r\n");  // deliberately no Origin and no cookies
}

void WsClient::onReadyRead() {
    if (done_) return;
    if (!open_) {
        head_ += sock_.readAll();
        const int end = head_.indexOf("\r\n\r\n");
        if (end < 0) { if (head_.size() > 16384) fail("WebSocket connection failed"); return; }
        const QList<QByteArray> lines = head_.left(end).split('\n');
        QHash<QByteArray, QByteArray> h;
        for (int i = 1; i < lines.size(); ++i) {
            const int c = lines[i].indexOf(':');
            if (c > 0) h[lines[i].left(c).trimmed().toLower()] = lines[i].mid(c + 1).trimmed();
        }
        const auto status = lines.value(0).split(' ').value(1);
        if (status != "101") return fail(QString("WebSocket handshake rejected (HTTP %1)").arg(QString::fromLatin1(status)));
        if (h["sec-websocket-accept"] != ws::acceptKey(key_) || h["upgrade"].toLower() != "websocket") return fail("WebSocket connection failed");
        const QString proto = QString::fromLatin1(h["sec-websocket-protocol"]);
        if (!proto.isEmpty() && !protocols_.contains(proto)) return fail("WebSocket connection failed");
        open_ = true;
        timeout_.stop();
        emit event({{"event", "open"}, {"protocol", proto}, {"extensions", QString::fromLatin1(h["sec-websocket-extensions"])}});
        parser_.feed(head_.mid(end + 4));
        head_.clear();
    } else {
        parser_.feed(sock_.readAll());
    }
    ws::Message m;
    while (!done_ && parser_.next(m)) {
        switch (m.opcode) {
        case ws::Text: emit event({{"event", "message"}, {"kind", "text"}, {"data", QString::fromUtf8(m.payload)}}); break;
        case ws::Binary: emit event({{"event", "message"}, {"kind", "binary"}, {"data", QString::fromLatin1(m.payload.toBase64())}}); break;
        case ws::Ping: sendFrame(ws::Pong, m.payload); break;
        case ws::Close: {
            const quint16 code = m.payload.size() >= 2 ? qFromBigEndian<quint16>(m.payload.constData()) : 1005;
            const QString reason = m.payload.size() > 2 ? QString::fromUtf8(m.payload.mid(2)) : QString();
            if (!closing_) sock_.write(ws::encodeFrame(ws::Close, m.payload.left(2), QRandomGenerator::system()->generate()));
            finish(code, reason, true);
            return;
        }
        default: break;
        }
    }
    if (parser_.error()) fail("WebSocket connection failed");
}

void WsClient::sendFrame(quint8 op, const QByteArray &payload) {
    if (!open_ || done_ || closing_ || payload.size() > ws::kMaxMessage) return;
    if (sock_.bytesToWrite() > (8 << 20)) return fail("WebSocket is closed or backed up");
    sock_.write(ws::encodeFrame(op, payload, QRandomGenerator::system()->generate()));
}

void WsClient::close(quint16 code, const QString &reason) {
    if (done_) return;
    if (!open_) return fail("WebSocket closed before it opened");
    if (closing_) return;
    closing_ = true;
    sock_.write(ws::encodeFrame(ws::Close, ws::closePayload(code, reason), QRandomGenerator::system()->generate()));
    timeout_.start(3000);  // close grace; then drop the connection
}

void WsClient::fail(const QString &msg) {
    if (done_) return;
    if (!closing_) emit event({{"event", "error"}, {"message", msg}});
    finish(1006, {}, false);
}

void WsClient::finish(quint16 code, const QString &reason, bool clean) {
    if (done_) return;
    done_ = true;
    timeout_.stop();
    emit event({{"event", "close"}, {"code", code}, {"reason", reason}, {"clean", clean}});
    sock_.abort();
    emit finished();
}

QString WsHub::connectSocket(const QString &label, const QString &id, const QString &url, const QStringList &protocols,
                             std::function<void(const QJsonObject &)> sink) {
    if (!ws::idValid(id)) return "invalid socket id";
    if (!ws::urlAllowed(url)) return "WebSocket destination is not allowed";
    if (protocols.size() > 8) return "invalid WebSocket subprotocol";
    for (const auto &p : protocols) if (!ws::isToken(p)) return "invalid WebSocket subprotocol";
    if (map_.size() >= ws::kMaxSockets || map_.contains(id)) return "too many WebSockets";
    auto *c = new WsClient(QUrl(url), protocols, false, this);
    map_.insert(id, {label, c});
    connect(c, &WsClient::event, this, [sink](const QJsonObject &e) { sink(e); });
    connect(c, &WsClient::finished, this, [this, id, c] { map_.remove(id); c->deleteLater(); });
    return {};
}

QString WsHub::send(const QString &id, const QString &kind, const QString &data) {
    auto it = map_.find(id);
    if (it == map_.end()) return "unknown WebSocket";
    if (kind == "text") it->client->sendText(data);
    else if (kind == "binary") {
        const auto raw = QByteArray::fromBase64Encoding(data.toLatin1());
        if (!raw) return "invalid binary payload";
        it->client->sendBinary(*raw);
    } else return "invalid payload kind";
    return {};
}

QString WsHub::close(const QString &id, int code, const QString &reason) {
    auto it = map_.find(id);
    if (it == map_.end()) return "unknown WebSocket";
    it->client->close(quint16(code <= 0 ? 1000 : code), reason);
    return {};
}

void WsHub::closeWindow(const QString &label) {
    QList<WsClient *> mine;  // close() may finish a socket synchronously and mutate map_
    for (const auto &e : std::as_const(map_))
        if (e.label == label) mine.append(e.client);
    for (auto *c : mine) c->close(1001, {});
}

}  // namespace ww
