#include "wsframe.h"
#include <QCryptographicHash>
#include <QStringConverter>
#include <QtEndian>

namespace ww::ws {

bool urlAllowed(const QString &raw) {
    const QUrl u(raw, QUrl::StrictMode);
    return raw.size() <= 4096 && u.isValid() && u.scheme() == "wss" && u.host() == kHost && u.port() == -1
           && (u.path() == "/socket.io/" || u.path() == "/socket.io") && u.userName().isEmpty() && u.password().isEmpty()
           && !u.hasFragment();
}

bool isToken(const QString &s) {
    if (s.isEmpty()) return false;
    for (QChar c : s)
        if (!(c.unicode() < 128 && (c.isLetterOrNumber() || QStringLiteral("!#$%&'*+-.^_`|~").contains(c)))) return false;
    return true;
}

bool idValid(const QString &id) {
    if (id.isEmpty() || id.size() > 64) return false;
    for (QChar c : id)
        if (!(c.unicode() < 128 && (c.isLetterOrNumber() || c == '-' || c == '_'))) return false;
    return true;
}

QByteArray acceptKey(const QByteArray &k) {
    return QCryptographicHash::hash(k + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
}

QByteArray encodeFrame(quint8 opcode, const QByteArray &p, quint32 maskKey, bool fin) {
    QByteArray f;
    f.append(char((fin ? 0x80 : 0) | opcode));
    const qsizetype n = p.size();
    if (n < 126) f.append(char(0x80 | n));
    else if (n <= 0xFFFF) { f.append(char(0x80 | 126)); f.append(char(n >> 8)); f.append(char(n & 0xFF)); }
    else { f.append(char(0x80 | 127)); char b[8]; qToBigEndian<quint64>(n, b); f.append(b, 8); }
    char m[4]; qToBigEndian<quint32>(maskKey, m);
    f.append(m, 4);
    for (qsizetype i = 0; i < n; ++i) f.append(char(p[i] ^ m[i & 3]));
    return f;
}

QByteArray closePayload(quint16 code, const QString &reason) {
    char c[2]; qToBigEndian<quint16>(code, c);
    return QByteArray(c, 2) + reason.toUtf8();
}

bool Parser::next(Message &out) {
    while (!error_) {
        if (buf_.size() < 2) return false;
        const auto *b = reinterpret_cast<const uchar *>(buf_.constData());
        const bool fin = b[0] & 0x80, masked = b[1] & 0x80;
        const quint8 op = b[0] & 0x0F;
        quint64 len = b[1] & 0x7F;
        qsizetype hdr = 2;
        if (b[0] & 0x70 || masked) { error_ = true; return false; }  // no extensions negotiated; servers never mask
        if (len == 126) { if (buf_.size() < 4) return false; len = qFromBigEndian<quint16>(b + 2); hdr = 4; }
        else if (len == 127) { if (buf_.size() < 10) return false; len = qFromBigEndian<quint64>(b + 2); hdr = 10; }
        const bool control = op >= 8;
        if (len > quint64(kMaxMessage) || (control && (!fin || len > 125)) || (op > 2 && op < 8) || op > 10) { error_ = true; return false; }
        if (quint64(buf_.size() - hdr) < len) return false;
        const QByteArray payload = buf_.mid(hdr, qsizetype(len));
        buf_.remove(0, hdr + qsizetype(len));
        if (control) { out = {op, payload}; return true; }
        if (op == Continuation) {
            if (!partialOp_) { error_ = true; return false; }
        } else {
            if (partialOp_) { error_ = true; return false; }
            partialOp_ = op;
        }
        partial_ += payload;
        if (partial_.size() > kMaxMessage) { error_ = true; return false; }
        if (fin) {
            out = {partialOp_, std::exchange(partial_, {})};
            partialOp_ = 0;
            if (out.opcode == Text) {
                QStringDecoder dec(QStringDecoder::Utf8);
                const QString decoded = dec(out.payload);  // the decoder is lazy: hasError() is only valid after conversion
                Q_UNUSED(decoded)
                if (dec.hasError()) { error_ = true; return false; }
            }
            return true;
        }
    }
    return false;
}

}  // namespace ww::ws
