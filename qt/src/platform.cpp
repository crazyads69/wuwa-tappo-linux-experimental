#include "platform.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QRegion>
#include <QSaveFile>
#include <QStandardPaths>
#include <QWindow>
#include <QFileInfo>
#include <QtGui/qguiapplication_platform.h>
#include <cstring>
#ifdef WW_HAVE_WAYLAND_CLIENT
#include <wayland-client.h>
#endif
#ifdef WW_HAVE_XCB
#include <xcb/shape.h>
#include <xcb/xcb.h>
#endif

namespace ww::platform {

static bool g_layer = false;
bool layerShellSelected() { return g_layer; }

#ifdef WW_HAVE_WAYLAND_CLIENT
static bool compositorHasLayerShell() {
    wl_display *d = wl_display_connect(nullptr);
    if (!d) return false;
    bool found = false;
    static const wl_registry_listener l{
        [](void *f, wl_registry *, uint32_t, const char *iface, uint32_t) { if (!strcmp(iface, "zwlr_layer_shell_v1")) *static_cast<bool *>(f) = true; },
        [](void *, wl_registry *, uint32_t) {}};
    wl_registry *r = wl_display_get_registry(d);
    wl_registry_add_listener(r, &l, &found);
    wl_display_roundtrip(d);
    wl_registry_destroy(r);
    wl_display_disconnect(d);
    return found;
}
#else
static bool compositorHasLayerShell() { return false; }
#endif

bool selectShellIntegration() {
#ifdef WW_HAVE_LAYERSHELL
    const bool wayland = qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland" || qEnvironmentVariableIsSet("WAYLAND_DISPLAY");
    const QByteArray forced = qgetenv("QT_QPA_PLATFORM");
    if (wayland && qEnvironmentVariableIntValue("WW_NO_LAYER") == 0 && (forced.isEmpty() || forced == "wayland") && compositorHasLayerShell()) {
        qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
        g_layer = true;
    }
#endif
    return g_layer;
}

bool waylandSession() { return QGuiApplication::platformName() == "wayland"; }
bool x11Session() { return QGuiApplication::platformName() == "xcb"; }
bool kde() { return qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("KDE", Qt::CaseInsensitive); }
bool sandboxed() { return QFile::exists("/.flatpak-info") || qEnvironmentVariableIsSet("SNAP"); }

void setInputRegion(QWindow *w, const QList<QRect> &rects) {
#ifdef WW_HAVE_XCB
    if (x11Session()) {
        auto *x = qApp->nativeInterface<QNativeInterface::QX11Application>();
        if (x && x->connection() && w->winId()) {
            std::vector<xcb_rectangle_t> xr;
            for (const QRect &r : rects) xr.push_back({qint16(r.x()), qint16(r.y()), quint16(r.width()), quint16(r.height())});
            // Input shape only: the visual (bounding) shape stays the whole window.
            xcb_shape_rectangles(x->connection(), XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                                 xcb_window_t(w->winId()), 0, 0, uint32_t(xr.size()), xr.data());
            xcb_flush(x->connection());
            return;
        }
    }
#endif
    // Wayland: QWindow::mask is the input region, and an empty mask means "no mask" (= everything clickable), so
    // "nothing clickable" is a 1x1 region outside the surface, which the compositor clips to empty.
    QRegion region;
    for (const QRect &r : rects) region += r;
    w->setMask(rects.isEmpty() ? QRegion(-16, -16, 1, 1) : region);
}

static QString autostartFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/autostart/com.kuro.wuwatappo.global.desktop";
}

bool autostartEnabled() { return QFile::exists(autostartFile()); }

bool setAutostart(bool on, QString *error) {
    if (sandboxed()) {  // org.freedesktop.portal.Background.RequestBackground(autostart)
        QDBusMessage m = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                        "org.freedesktop.portal.Background", "RequestBackground");
        QVariantMap opts{{"autostart", on}, {"background", on}, {"reason", "Start Wuwa Tappo when you log in"},
                         {"commandline", QStringList{"wuwatappo-qt"}}};
        m << QString() << opts;
        QDBusConnection::sessionBus().asyncCall(m);  // result only matters for the user's consent prompt
        return true;
    }
    if (!on) return !QFile::exists(autostartFile()) || QFile::remove(autostartFile());
    QDir().mkpath(QFileInfo(autostartFile()).absolutePath());
    QSaveFile f(autostartFile());
    const bool ok = f.open(QIODevice::WriteOnly)
                    && f.write("[Desktop Entry]\nType=Application\nName=Wuwa Tappo\nExec=" + QCoreApplication::applicationFilePath().toUtf8()
                               + "\nIcon=com.kuro.wuwatappo.global\nX-GNOME-Autostart-enabled=true\nTerminal=false\n") > 0
                    && f.commit();
    if (!ok && error) *error = f.errorString();
    return ok;
}

}  // namespace ww::platform
