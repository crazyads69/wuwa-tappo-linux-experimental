// Wuwa Tappo, native Qt 6 shell. The web frontend (dist/) runs unmodified in QtWebEngine; everything a web page
// cannot do (overlay window, click-through, global input, tray, multiplayer socket) is implemented natively here.
#include "backend.h"
#include "httpserver.h"
#include "platform.h"
#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QImage>
#include <QLocalServer>
#include <QPainter>
#include <QLocalSocket>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QTimer>
#include <unistd.h>

#ifdef WW_HAVE_WEBENGINE_HEADERS
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#else
namespace QtWebEngineQuick { void initialize(); }  // exported by libQt6WebEngineQuick; its headers are not installed here
#endif

using namespace ww;

static constexpr quint16 kPreferredPort = 47821;  // a stable port = a stable web origin = persistent localStorage

static QString findDist() {
    const QString app = QCoreApplication::applicationDirPath();
    for (const QString &p : {qEnvironmentVariable("WUWA_DIST"), app + "/../share/wuwatappo/dist", app + "/../../dist", app + "/../../../dist",
                             QDir::currentPath() + "/dist"})
        if (!p.isEmpty() && QFile::exists(p + "/index.html")) return QDir(p).canonicalPath();
    return {};
}

// Single instance: a second launch (e.g. the wuwa-buddy:// login callback) hands its arguments to the first and exits.
static bool forwardToRunningInstance(const QString &name, const QStringList &urls) {
    QLocalSocket s;
    s.connectToServer(name);
    if (!s.waitForConnected(300)) return false;
    s.write(urls.join('\n').toUtf8() + "\n");
    s.waitForBytesWritten(1000);
    return true;
}

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);  // required by QtWebEngine
    platform::selectShellIntegration();                          // must precede the application object
    QtWebEngineQuick::initialize();
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("com.kuro.wuwatappo.global");  // = Tauri identifier: same app-data dir as the Tauri shell
    QCoreApplication::setOrganizationName({});
    QCoreApplication::setApplicationVersion(WW_VERSION);
    QGuiApplication::setDesktopFileName("com.kuro.wuwatappo.global");
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QStringList urls;
    for (const QString &a : app.arguments().mid(1)) if (a.startsWith("wuwa-buddy:")) urls << a;

    const QString lockName = QString("wuwatappo-%1").arg(getuid());
    if (forwardToRunningInstance(lockName, urls)) { qInfo("handed over to the running instance"); return 0; }

    const QString dist = findDist();
    if (dist.isEmpty()) {
        qCritical("frontend not found: run scripts/setup.sh to unpack dist/ from your installer, or set WUWA_DIST");
        return 2;
    }
    const QString icon = dist + "/app-icon.png";
    if (QFile::exists(icon)) QGuiApplication::setWindowIcon(QIcon(icon));

    HttpServer http(dist);
    if (!http.start(kPreferredPort)) { qCritical("cannot start the local frontend server"); return 2; }

    QQmlEngine engine;
    QQmlComponent shared(&engine, QUrl("qrc:/qml/App.qml"));
    std::unique_ptr<QObject> sharedObj(shared.create());
    if (!sharedObj) { qCritical().noquote() << "QtWebEngine QML module unavailable:" << shared.errorString(); return 3; }

    Store store(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/store.json");
    Windows windows(&engine, sharedObj.get(), http.origin());
    InputMonitor input;
    Desktop desktop;
    Tray tray;
    Backend backend(&windows, &store, &input, &desktop, &tray);
    QMetaObject::invokeMethod(sharedObj->property("channel").value<QObject *>(), "registerObject", Q_ARG(QString, "backend"), Q_ARG(QObject *, &backend));

    QLocalServer::removeServer(lockName);
    QLocalServer instance;
    instance.listen(lockName);
    QObject::connect(&instance, &QLocalServer::newConnection, &app, [&] {
        while (QLocalSocket *c = instance.nextPendingConnection()) {
            QObject::connect(c, &QLocalSocket::readyRead, c, [&, c] {
                const QStringList got = QString::fromUtf8(c->readAll()).split('\n', Qt::SkipEmptyParts);
                if (!got.isEmpty()) backend.handleDeepLink(got);
                backend.showMain();
                c->disconnectFromServer();
            });
            QObject::connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
        }
    });

    backend.createMain();
    if (!urls.isEmpty()) backend.handleDeepLink(urls);
    input.start();
    desktop.start();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&] { input.stop(); desktop.stop(); });

    // Developer aid: WW_DEV_OPEN=open_panel_window (any no-argument command) runs once after the pages loaded.
    if (qEnvironmentVariableIsSet("WW_DEV_OPEN"))
        QTimer::singleShot(4000, &app, [&] { backend.invoke("main", 0, qEnvironmentVariable("WW_DEV_OPEN"), "{}"); });

    // Developer aid for headless verification: WW_GRAB_DIR=dir WW_GRAB_MS=5000 saves each window as PNG, then quits.
    if (qEnvironmentVariableIsSet("WW_GRAB_DIR")) {
        QTimer::singleShot(qEnvironmentVariableIntValue("WW_GRAB_MS") ? qEnvironmentVariableIntValue("WW_GRAB_MS") : 5000, &app, [&] {
            for (const QString &l : windows.labels())
                if (auto *w = windows.get(l)) {  // composited on gray so transparent areas are visible
                    const QImage img = w->grabWindow();
                    QImage out(img.size(), QImage::Format_ARGB32);
                    out.fill(QColor(0x55, 0x55, 0x66));
                    QPainter(&out).drawImage(0, 0, img);
                    out.save(qEnvironmentVariable("WW_GRAB_DIR") + "/" + l + ".png");
                }
            QTimer::singleShot(1500, &app, &QCoreApplication::quit);
        });
    }
    qInfo("running on %s, origin %s, layer-shell %d", qPrintable(QGuiApplication::platformName()), qPrintable(http.origin()), platform::layerShellSelected());
    return app.exec();
}
