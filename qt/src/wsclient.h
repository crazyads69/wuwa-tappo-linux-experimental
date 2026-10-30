// Native WebSocket client for the multiplayer socket (socket.io over wss). It sends no Origin header: Kuro's server
// answers a handshake with HTTP 403 unless Origin is absent or http(s)://tauri.localhost, so the browser's own
// WebSocket (Origin http://127.0.0.1:port) can never connect. See docs/qt-port/FEATURES.md.
#pragma once
#include "wsframe.h"
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSslSocket>
#include <QTimer>
#include <functional>

namespace ww {

class WsClient : public QObject {
    Q_OBJECT
public:
    // `plainForTests` allows ws://127.0.0.1 so the protocol can be tested without TLS; the app never sets it.
    WsClient(QUrl url, QStringList protocols, bool plainForTests = false, QObject *parent = nullptr);
    void sendText(const QString &s) { sendFrame(ws::Text, s.toUtf8()); }
    void sendBinary(const QByteArray &b) { sendFrame(ws::Binary, b); }
    void close(quint16 code, const QString &reason);

signals:
    // {event:"open"|"message"|"close"|"error", ...}: the exact JSON the page-side ws.js expects.
    void event(const QJsonObject &e);
    void finished();

private:
    void onConnected();
    void onReadyRead();
    void sendFrame(quint8 op, const QByteArray &payload);
    void fail(const QString &msg);
    void finish(quint16 code, const QString &reason, bool clean);

    QSslSocket sock_;
    QUrl url_;
    QStringList protocols_;
    QByteArray key_, head_;
    ws::Parser parser_;
    QTimer timeout_;
    bool open_ = false, done_ = false, closing_ = false;
};

// All sockets of the app; closing a window closes its sockets.
class WsHub : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    QString connectSocket(const QString &label, const QString &id, const QString &url, const QStringList &protocols,
                          std::function<void(const QJsonObject &)> sink);  // returns error text or empty
    QString send(const QString &id, const QString &kind, const QString &data);
    QString close(const QString &id, int code, const QString &reason);
    void closeWindow(const QString &label);

private:
    struct Entry { QString label; WsClient *client; };
    QHash<QString, Entry> map_;
};

}  // namespace ww
