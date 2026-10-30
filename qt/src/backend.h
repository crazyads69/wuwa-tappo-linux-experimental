// The Tauri command surface of the original app, re-implemented natively. Pages call `invoke()` through the web
// channel (js/tauri.js); replies and events travel back as signals. Command names, argument names (camelCase, as
// Tauri converts them) and payloads follow the original app, recovered from its binary.
#pragma once
#include "desktop.h"
#include "inputmonitor.h"
#include "regions.h"
#include "store.h"
#include "tray.h"
#include "windows.h"
#include "wsclient.h"
#include <QJsonValue>
#include <QObject>
#include <functional>
#include <optional>

namespace ww {

class Backend : public QObject {
    Q_OBJECT
public:
    Backend(Windows *w, Store *store, InputMonitor *input, Desktop *desktop, Tray *tray, QObject *parent = nullptr);

    Q_INVOKABLE void invoke(const QString &label, int id, const QString &cmd, const QString &argsJson);

    void createMain();
    void handleDeepLink(const QStringList &urls);  // wuwa-buddy://auth/callback?... (also from a second instance)
    void showMain();
    void emitEvent(const QString &label, const QString &name, const QJsonValue &payload = QJsonValue::Null);  // "" = every window

signals:
    void replied(const QString &label, int id, bool ok, const QString &json);
    void pageEvent(const QString &label, const QString &name, const QString &json);
    void channelMsg(const QString &label, int callback, int index, const QString &json);

private:
    struct Ctx { QString label; QJsonObject a; };
    struct Err { QString msg; };
    using Fn = std::function<QJsonValue(const Ctx &)>;

    void registerCommands();
    QJsonValue windowPlugin(const QString &name, const Ctx &c);
    QJsonValue plugin(const QString &cmd, const Ctx &c);
    void onPageEvent(const QString &label, const QString &name, const QJsonValue &payload);

    // windows / presentation
    std::pair<QQuickWindow *, bool> aux(const QString &label, const QString &route, const QString &title, QSize size, WindowOpts o = {});
    void present(const QString &label, bool created, std::function<void(bool)> then);
    void reveal(const QString &label, std::optional<quint64> id);
    void showWindow(const QString &label);
    void emitLater(const QString &label, const QString &name, const QJsonValue &payload);
    void applyPanelShape();
    void applyMainRegions();
    void applyLayerPolicy();
    void syncAuxToMainScreen();
    QScreen *currentScreen();
    QJsonObject monitorJson(QScreen *s) const;
    QJsonObject monitorSnapshot();
    void selectMainMonitor(QScreen *s, const Bounds &b);
    QJsonValue popupMenu(double x, double y);

    Windows *win_;
    Store *store_;
    InputMonitor *input_;
    Desktop *desktop_;
    Tray *tray_;
    WsHub hub_;
    QHash<QString, Fn> cmds_;
    QStringList deepLinks_;

    std::optional<Rects> inputRegions_;  // last explicit input set (see HitRegion)
    Rects activeRegions_;
    QJsonValue updateDetail_, maintenance_ = QJsonValue::Undefined;  // Undefined: normal operation, Null: default notice
    struct Pending { quint64 id; bool gated; std::function<void(bool)> then; };
    QHash<QString, Pending> pending_;
    quint64 showId_ = 0;
    bool panelCollapsed_ = false, alwaysOnTop_ = true, panelRaised_ = false, layerMain_ = false;
    std::optional<Bounds> selected_;
    SwitchGate gate_;
    NetStatusDedupe net_;
    bool skipPanelOutsideClose_ = false;
    void closePanelOnOutsidePress();
    QHash<QString, int> wsIndex_;
    QSet<QString> ignoreInput_;
};

}  // namespace ww
