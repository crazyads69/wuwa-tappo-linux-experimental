// RFC 6455 framing + the destination allow-list of the multiplayer bridge (ported from native_ws.rs).
// QtWebSockets headers are not guaranteed to be installed, and the bridge needs a client that sends no Origin, so
// the few hundred lines of protocol live here and are unit-tested.
#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <optional>

namespace ww::ws {

inline constexpr auto kHost = "api-wutheringwaves-wuwatappo.kurogames-global.com";
inline constexpr int kMaxMessage = 4 << 20;
inline constexpr int kMaxSockets = 4;

enum Opcode : quint8 { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

// Only wss://<kHost>/socket.io[/] with no port, credentials or fragment.
bool urlAllowed(const QString &raw);
bool isToken(const QString &s);  // RFC 7230 token (subprotocol names)
bool idValid(const QString &id);
QByteArray acceptKey(const QByteArray &clientKey);
QByteArray encodeFrame(quint8 opcode, const QByteArray &payload, quint32 maskKey, bool fin = true);
QByteArray closePayload(quint16 code, const QString &reason);

struct Message {
    quint8 opcode = 0;  // Text, Binary, Close, Ping, Pong
    QByteArray payload;
};

// Incremental server->client parser. Fragmented messages are reassembled; control frames may interleave.
class Parser {
public:
    void feed(const QByteArray &data) { buf_ += data; }
    // true: `out` filled. false: need more data, or error() is set.
    bool next(Message &out);
    bool error() const { return error_; }

private:
    QByteArray buf_, partial_;
    quint8 partialOp_ = 0;
    bool error_ = false;
};

}  // namespace ww::ws
