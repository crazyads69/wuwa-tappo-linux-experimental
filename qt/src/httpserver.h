// Serves the (unpacked, patched) frontend from a directory on 127.0.0.1 so the pages get a stable http origin
// (cookies and localStorage persist). The headers of QtWebEngine's custom-scheme API are not always installed, and
// file:// cannot load the frontend's ES modules. Only GET/HEAD, only below `root`, only for Host 127.0.0.1:<port>.
#pragma once
#include <QDir>
#include <QTcpServer>

namespace ww {

class HttpServer : public QTcpServer {
    Q_OBJECT
public:
    HttpServer(QString root, QObject *parent = nullptr) : QTcpServer(parent), root_(QDir(root).canonicalPath()) {}
    // Prefers `preferredPort` (stable origin across runs), falls back to any free port.
    bool start(quint16 preferredPort);
    QString origin() const { return QString("http://127.0.0.1:%1").arg(serverPort()); }

    struct Reply { int status; QByteArray type; QString file; };
    // Pure request routing, exposed for tests: maps a request target to a file below `root` (or a status).
    static Reply route(const QString &root, const QByteArray &method, const QByteArray &target, const QByteArray &host, quint16 port);

private:
    void incomingConnection(qintptr fd) override;
    void handle(QTcpSocket *s);
    QString root_;
};

}  // namespace ww
