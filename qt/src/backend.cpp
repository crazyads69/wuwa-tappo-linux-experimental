#include "backend.h"
#include "platform.h"
#include <QClipboard>
#include <climits>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QNetworkInformation>
#include <QMessageBox>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>

namespace ww {

// QJsonDocument cannot hold scalars: wrap them in an array and strip the brackets.
static QString encode(const QJsonValue &v) {
    if (v.isObject()) return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    if (v.isArray()) return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    const QString a = QString::fromUtf8(QJsonDocument(QJsonArray{v.isUndefined() ? QJsonValue::Null : v}).toJson(QJsonDocument::Compact));
    return a.mid(1, a.size() - 2);
}
static QString logDir() { return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs"; }

constexpr int kSettleMs = 200, kFallbackMs = 2500;
constexpr QSize kPanelSize{1100, 760};
constexpr int kSizeMax = 16777215;  // QWIDGETSIZE_MAX

Backend::Backend(Windows *w, Store *store, InputMonitor *input, Desktop *desktop, Tray *tray, QObject *parent)
    : QObject(parent), win_(w), store_(store), input_(input), desktop_(desktop), tray_(tray), hub_(this) {
    registerCommands();
    connect(win_, &Windows::pageLoaded, this, [this](const QString &label) {
        if (label == "main") applyMainRegions();  // start click-through; regions arrive once the frontend reports them
        if (!pending_.contains(label) || pending_[label].gated) return;
        const quint64 id = pending_[label].id;
        QTimer::singleShot(kSettleMs, this, [this, label, id] { reveal(label, id); });
    });
    connect(win_, &Windows::windowClosed, this, [this](const QString &label) {
        hub_.closeWindow(label);
        pending_.remove(label);
        if (label == "panel") { panelRaised_ = false; applyLayerPolicy(); emitEvent("", "cat-panel-window-closed"); }
    });
    connect(win_, &Windows::externalUrl, this, [](const QUrl &u) {
        if (u.scheme() == "https" || u.scheme() == "http") QDesktopServices::openUrl(u);
    });
    connect(input_, &InputMonitor::input, this, [this](const QString &ev, const QJsonValue &p) {
        emitEvent("", ev, p);
        const QJsonObject o = p.toObject();
        if (ev == "mouse-button" && o["action"] == "press" && o["button"] == "left") closePanelOnOutsidePress();
    });
    // Original: WinRT NetworkInformation.NetworkStatusChanged -> {internetAccess} (FUN_140011da0, see FEATURES.md).
    if (QNetworkInformation::loadDefaultBackend() || QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability)) {
        connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this, [this](QNetworkInformation::Reachability r) {
            if (auto v = net_.update(r == QNetworkInformation::Reachability::Online)) emitEvent("", "cat-native-network-status-changed", QJsonObject{{"internetAccess", *v}});
        });
    } else {
        qInfo("[network] no QNetworkInformation backend; the frontend falls back to its own health check");
    }
    connect(tray_, &Tray::action, this, [this](const QString &id) {
        auto tab = [&](const char *t) { emitEvent("", "dock-open-panel", t); };
        bool ok = false;
        if (id == "dock_bag") tab("bag");
        else if (id == "dock_atlas") tab("atlas");
        else if (id == "dock_exchange") tab("exchange");
        else if (id == "dock_room") tab("room");
        else if (id == "dock_settings") tab("setting");
        else if (id == "dock_toggle_pet") emitEvent("", "tray-toggle-pet-visibility");
        else if (id == "dock_window_passthrough") emitEvent("", "tray-toggle-window-passthrough");
        else if (id == "dock_restart") emitEvent("", "tray-restart");
        else if (id == "dock_quit") emitEvent("", "tray-quit");
        else if (id.startsWith("dock_pet_scale_")) { const int n = id.mid(15).toInt(&ok); if (ok) emitEvent("", "tray-set-pet-scale", n); }
        else if (id.startsWith("dock_window_opacity_")) { const int n = id.mid(20).toInt(&ok); if (ok) emitEvent("", "tray-set-window-opacity", n); }
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen *) {
        selected_.reset();  // GDK-style recovery: pick a remaining output once Qt has finished removing this one
        QTimer::singleShot(0, this, [this] {
            if (QScreen *s = currentScreen()) selectMainMonitor(s, Windows::boundsOf(s));
            syncAuxToMainScreen();
        });
    });
}

// ---- IPC plumbing -----------------------------------------------------------------------------
void Backend::invoke(const QString &label, int id, const QString &cmd, const QString &argsJson) {
    const QJsonObject args = QJsonDocument::fromJson(argsJson.toUtf8()).object();
    static const bool trace = qEnvironmentVariableIsSet("WW_TRACE_IPC");  // command names only, never arguments
    if (trace) qInfo().noquote() << "[ipc]" << label << cmd;
    auto reply = [&](bool ok, const QJsonValue &v) { emit replied(label, id, ok, encode(v)); };
    try {
        reply(true, cmd.startsWith("plugin:") ? plugin(cmd, {label, args}) : [&]() -> QJsonValue {
            auto it = cmds_.find(cmd);
            if (it == cmds_.end()) throw Err{"command " + cmd + " not found"};
            return (*it)({label, args});
        }());
    } catch (const Err &e) {
        reply(false, e.msg);
    }
}

void Backend::emitEvent(const QString &label, const QString &name, const QJsonValue &payload) {
    emit pageEvent(label, name, encode(payload));
}

void Backend::emitLater(const QString &label, const QString &name, const QJsonValue &payload) {
    // The page registers its listeners after load, so re-send a few times instead of racing it.
    for (int ms : {100, 500, 1300}) QTimer::singleShot(ms, this, [=, this] { emitEvent(label, name, payload); });
}

void Backend::onPageEvent(const QString &label, const QString &name, const QJsonValue &p) {
    if (name == "window-ready") reveal(label, std::nullopt);
    else if (name == "window-gate") { if (pending_.contains(label)) pending_[label].gated = true; }
    else if (name == "mouse-button" && p.toObject()["native"].toBool() != true)
        input_->noteDelivered(p.toObject()["button"].toString(), p.toObject()["action"].toString());
    else if (name == "cat-panel-action" && p.toObject()["type"].toString() == "pet-clap") input_->noteDelivered("left", "press");
}

// ---- window presentation ----------------------------------------------------------------------
std::pair<QQuickWindow *, bool> Backend::aux(const QString &label, const QString &route, const QString &title, QSize size, WindowOpts o) {
    o.title = title;
    o.size = size;
    return win_->create(label, route, o);
}

void Backend::showWindow(const QString &label) {
    QQuickWindow *w = win_->get(label);
    if (!w) return;
    if (w->visibility() == QWindow::Minimized) w->showNormal(); else w->show();
    if (label == "panel") { panelRaised_ = true; applyLayerPolicy(); }
    w->requestActivate();
    syncAuxToMainScreen();
}

// A freshly built window stays hidden until its page can paint (otherwise the user sees an empty transparent
// surface that is then filled: the "flash"). Revealed by whichever comes first: the page's `window-ready` event, the
// load plus a settle delay (unless it announced `window-gate`), or a fallback timer.
void Backend::present(const QString &label, bool created, std::function<void(bool)> then) {
    if (!created) { showWindow(label); return then(false); }
    const quint64 id = ++showId_;
    pending_[label] = {id, false, std::move(then)};
    QTimer::singleShot(kFallbackMs, this, [this, label, id] { reveal(label, id); });
}

void Backend::reveal(const QString &label, std::optional<quint64> id) {
    auto it = pending_.find(label);
    if (it == pending_.end() || (id && it->id != *id)) return;
    auto then = std::move(it->then);
    pending_.erase(it);
    showWindow(label);
    if (then) then(true);
}

QScreen *Backend::currentScreen() {
    if (selected_) if (QScreen *s = win_->screenWithBounds(*selected_)) return s;
    selected_.reset();
    if (QQuickWindow *m = win_->get("main"); m && m->screen()) return m->screen();
    return QGuiApplication::primaryScreen();
}

void Backend::syncAuxToMainScreen() {
    QScreen *s = currentScreen();
    for (const QString &l : win_->labels()) if (l != "main") win_->moveToScreen(l, s);
}

void Backend::applyLayerPolicy() { win_->setLayer("main", layerFor(alwaysOnTop_, panelRaised_)); }

void Backend::applyMainRegions() {
    QJsonArray tagged;  // memberId lets pet.js route clicks on other players' pets like the native hook did
    for (const Rect &r : activeRegions_) if (!r.memberId.isUndefined() && !r.memberId.isNull()) tagged.append(r.toJson());
    emitEvent("main", "__ww_regions", tagged);
    win_->setInputRects("main", toPixelRects(activeRegions_));
}

// Original FUN_140021570: a global left press outside the panel window closes it (and emits cat-panel-window-closed).
void Backend::closePanelOnOutsidePress() {
    QQuickWindow *w = win_->get("panel");
    const QPoint p = input_->pointer();
    const bool known = p.x() != INT_MIN;
    bool inside = false;
    if (w && known) {
        const double dpr = w->devicePixelRatio();
        QRectF r(w->geometry());
        if (win_->isLayer("panel") && w->screen()) {  // anchor-less layer surfaces are centred on their output
            const QRect s = w->screen()->geometry();
            r = QRectF(s.x() + (s.width() - w->width()) / 2.0, s.y() + (s.height() - w->height()) / 2.0, w->width(), w->height());
        }
        inside = QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).contains(p);
    }
    if (panelOutsideClose(w != nullptr, skipPanelOutsideClose_, known, inside, true)) win_->destroy("panel");
}

void Backend::applyPanelShape() {
    QQuickWindow *w = win_->get("panel");
    if (!w) return;
    win_->setInputRects("panel", toPixelRects({panelRect(panelCollapsed_, w->width(), w->height())}));
}

// ---- monitors ---------------------------------------------------------------------------------
QJsonObject Backend::monitorJson(QScreen *s) const {
    const double d = s->devicePixelRatio();
    const Bounds b = Windows::boundsOf(s);
    const QRect wa = s->availableGeometry();
    return {{"name", s->name()},
            {"position", QJsonObject{{"x", b.x}, {"y", b.y}}},
            {"size", QJsonObject{{"width", int(b.width)}, {"height", int(b.height)}}},
            {"scaleFactor", d},
            {"workArea", QJsonObject{{"position", QJsonObject{{"x", int(wa.x() * d)}, {"y", int(wa.y() * d)}}},
                                     {"size", QJsonObject{{"width", int(wa.width() * d)}, {"height", int(wa.height() * d)}}}}}};
}

QJsonObject Backend::monitorSnapshot() {
    QScreen *s = currentScreen();
    QQuickWindow *m = win_->get("main");
    const bool layer = m && win_->isLayer("main");
    const Bounds b = Windows::boundsOf(s);
    QJsonObject origin{{"x", b.x}, {"y", b.y}};
    if (!layer && m) origin = {{"x", int(m->x() * s->devicePixelRatio())}, {"y", int(m->y() * s->devicePixelRatio())}};
    return {{"monitor", monitorJson(s)}, {"origin", origin}, {"layerShell", layer}};
}

void Backend::selectMainMonitor(QScreen *s, const Bounds &b) {
    QQuickWindow *m = win_->get("main");
    if (!m || !s) return;
    win_->moveToScreen("main", s);
    if (!win_->isLayer("main")) {  // plain window: cover the output
        m->setScreen(s);
        m->setGeometry(s->geometry());
    }
    selected_ = b;
    input_->setPointerScale(s->devicePixelRatio());
    applyMainRegions();  // a remapped surface starts with the default input region
    emitEvent("main", "cat-main-monitor-origin-changed", monitorSnapshot());
}

QJsonValue Backend::popupMenu(double x, double y) {
    QQuickWindow *m = win_->get("main");
    if (!m) throw Err{"no main window"};
    if (!tray_->ready()) throw Err{"menu not synced yet"};
    QScreen *s = currentScreen();
    const Bounds b = Windows::boundsOf(s);
    const bool layer = win_->isLayer("main");
    const QPoint origin = layer ? QPoint(b.x, b.y) : QPoint(int(m->x() * s->devicePixelRatio()), int(m->y() * s->devicePixelRatio()));
    const QPointF rel = relativePointer(x, y, origin, s->devicePixelRatio());
    tray_->popup(m->mapToGlobal(rel.toPoint()), m);
    return QJsonValue::Null;
}

// ---- main window ------------------------------------------------------------------------------
void Backend::createMain() {
    // WW_DEBUG_WINDOW=1: an ordinary 900x700 window instead of a full-output overlay (safe to try on a live desktop).
    const bool debugWindow = qEnvironmentVariableIsSet("WW_DEBUG_WINDOW");
    layerMain_ = platform::layerShellSelected() && !debugWindow;
    WindowOpts o;
    o.title = "Wuwa Tappo";
    o.pet = true;
    o.layer = layerMain_;
    o.focusable = false;
    QScreen *s = QGuiApplication::primaryScreen();
    o.size = debugWindow ? QSize(900, 700) : s->size();
    auto [w, created] = win_->create("main", "", o);
    if (!w) return;
    win_->setInputRects("main", {});  // start fully click-through so a failed frontend can never lock the desktop
    if (layerMain_) {
        applyLayerPolicy();
        w->show();
    } else if (debugWindow) {
        w->show();
    } else {
        w->setFlag(Qt::WindowStaysOnTopHint);
        w->showMaximized();
    }
    input_->setPointerScale(s->devicePixelRatio());
}

void Backend::showMain() {
    if (QQuickWindow *w = win_->get("main")) w->show();
}

void Backend::handleDeepLink(const QStringList &urls) {
    deepLinks_ = urls;
    emitEvent("", "deep-link://new-url", QJsonArray::fromStringList(urls));
}

// ---- commands ---------------------------------------------------------------------------------
void Backend::registerCommands() {
    auto num = [](const Ctx &c, const char *k) { return c.a[k].toDouble(); };

    // persisted counters (store.json is shared with the Tauri shell: same app-data dir)
    cmds_["load_key_press_count"] = [this](const Ctx &c) { return store_->read()["key_" + c.a["environment"].toString()].toDouble(0); };
    cmds_["save_key_press_count"] = [this](const Ctx &c) {
        if (!store_->set("key_" + c.a["environment"].toString(), c.a["count"])) throw Err{"cannot write store"};
        return QJsonValue();
    };
    cmds_["load_cumulative_click_count"] = [this](const Ctx &c) {
        const auto v = store_->read()["cum_" + c.a["environment"].toString()];
        return v.isNull() || v.isUndefined() ? QJsonValue(QJsonObject{{"count", 0}, {"cuid", ""}, {"environment", c.a["environment"]}}) : v;
    };
    cmds_["save_cumulative_click_count"] = [this](const Ctx &c) {
        if (!store_->set("cum_" + c.a["environment"].toString(), QJsonObject{{"count", c.a["count"]}, {"cuid", c.a["cuid"]}, {"environment", c.a["environment"]}}))
            throw Err{"cannot write store"};
        return QJsonValue();
    };

    // native WebSocket bridge
    cmds_["native_ws_connect"] = [this](const Ctx &c) {
        const QString cb = c.a["events"].toString();
        if (!cb.startsWith("__CHANNEL__:")) throw Err{"invalid event channel"};
        const int cbId = cb.mid(12).toInt();
        const QString id = c.a["id"].toString(), label = c.label;
        QStringList protos;
        for (const auto &p : c.a["protocols"].toArray()) protos << p.toString();
        auto index = std::make_shared<int>(0);
        const QString err = hub_.connectSocket(label, id, c.a["url"].toString(), protos, [=, this](const QJsonObject &e) {
            emit channelMsg(label, cbId, (*index)++, encode(e));
        });
        if (!err.isEmpty()) throw Err{err};
        return QJsonValue();
    };
    cmds_["native_ws_send"] = [this](const Ctx &c) {
        const QString err = hub_.send(c.a["id"].toString(), c.a["kind"].toString(), c.a["data"].toString());
        if (!err.isEmpty()) throw Err{err};
        return QJsonValue();
    };
    cmds_["native_ws_close"] = [this](const Ctx &c) {
        const QString err = hub_.close(c.a["id"].toString(), c.a["code"].toInt(1000), c.a["reason"].toString());
        if (!err.isEmpty()) throw Err{err};
        return QJsonValue();
    };

    // main window input
    cmds_["set_main_hit_region"] = [this](const Ctx &c) {
        const auto hr = HitRegion::fromJson(c.a["payload"].toObject());
        const Rects all = hr.inputRects(inputRegions_);
        if (all == activeRegions_) return QJsonValue();
        activeRegions_ = all;
        applyMainRegions();
        return QJsonValue();
    };
    cmds_["finish_main_window_monitor_switch"] = [this](const Ctx &) { applyMainRegions(); return QJsonValue(); };
    auto cursor = [this](const Ctx &c) {
        const QString k = c.a["kind"].toString();
        if (QQuickWindow *w = win_->get("main"))
            w->setCursor(k == "pointer" ? Qt::PointingHandCursor : k == "grab" ? Qt::OpenHandCursor : k == "grabbing" ? Qt::ClosedHandCursor : Qt::ArrowCursor);
        return QJsonValue();
    };
    cmds_["set_native_input_cursor"] = cursor;
    cmds_["set_overlay_cursor"] = cursor;
    // A layer surface already covers the output; the frontend falls back to the window API if this errors.
    cmds_["set_main_window_fullscreen_shell"] = [this](const Ctx &) {
        if (layerMain_) return QJsonValue();
        if (QQuickWindow *w = win_->get("main")) { w->setFlag(Qt::WindowStaysOnTopHint); w->showMaximized(); }
        return QJsonValue();
    };
    cmds_["apply_main_window_position"] = [this](const Ctx &c) {
        alwaysOnTop_ = c.a["payload"].toObject()["alwaysOnTop"].toBool(true);
        if (layerMain_) applyLayerPolicy();
        else if (QQuickWindow *w = win_->get("main")) w->setFlag(Qt::WindowStaysOnTopHint, alwaysOnTop_);
        return QJsonValue();
    };
    cmds_["set_main_skip_taskbar"] = [](const Ctx &) { return QJsonValue(); };  // layer surfaces and Qt::Tool have no taskbar entry

    // auxiliary windows (flags/sizes recovered from the original builders)
    cmds_["open_login_window"] = [this](const Ctx &c) {
        // The original starts at the monitor rect and shrinks to content; a Wayland window cannot be placed, so start at card size.
        auto [w, created] = aux("login", "/login", "登录", {520, 420});
        if (!w) throw Err{"cannot create window"};
        w->setFlag(Qt::WindowStaysOnTopHint);
        const QJsonObject payload{{"intent", c.a["intent"].isString() ? c.a["intent"] : QJsonValue("login")}, {"compact", c.a["compact"]}};
        present("login", created, [=, this](bool) { emitLater("login", "login-modal-open", payload); });
        return QJsonValue();
    };
    cmds_["close_login_window"] = [this](const Ctx &) { win_->destroy("login"); return QJsonValue(); };
    cmds_["resize_login_window_to_content"] = [this](const Ctx &c) {
        if (QQuickWindow *w = win_->get("login")) {
            const QJsonObject p = c.a["payload"].toObject();
            const QSize sz(qBound(498, int(std::lround(p["width"].toDouble())), 900), qBound(396, int(std::lround(p["height"].toDouble())), 760));
            w->setMinimumSize(sz); w->setMaximumSize(sz); w->resize(sz);
        }
        return QJsonValue();
    };
    auto simple = [this](const char *label, const char *route, const char *title, const char *event) {
        return [=, this](const Ctx &) {
            auto [w, created] = aux(label, route, title, {880, 720});
            if (!w) throw Err{"cannot create window"};
            present(label, created, [=, this](bool fresh) { fresh ? emitLater(label, event, QJsonValue::Null) : emitEvent(label, event); });
            return QJsonValue();
        };
    };
    cmds_["open_about_window"] = simple("about", "/about", "关于Wuwa Tappo", "about-open");
    cmds_["close_about_window"] = [this](const Ctx &) { win_->destroy("about"); return QJsonValue(); };
    cmds_["open_oss_notice_window"] = simple("oss-notice", "/oss-notice", "开源组件声明", "oss-notice-open");
    cmds_["close_oss_notice_window"] = [this](const Ctx &) { win_->destroy("oss-notice"); return QJsonValue(); };
    cmds_["open_update_window"] = [this](const Ctx &c) {
        updateDetail_ = c.a["detail"];
        auto [w, created] = aux("update", "/update", "强制更新", {580, 600});
        if (!w) throw Err{"cannot create window"};
        const QJsonValue detail = c.a["detail"];
        present("update", created, [=, this](bool) {
            emitLater("update", "update-modal-open", QJsonValue::Null);
            emitLater("update", "cat-app-update-available", detail);
        });
        return QJsonValue();
    };
    cmds_["close_update_window"] = [this](const Ctx &) { win_->destroy("update"); return QJsonValue(); };
    cmds_["get_app_update_detail"] = [this](const Ctx &) { return updateDetail_; };

    cmds_["open_panel_window"] = [this](const Ctx &c) {
        const QString tab = c.a["payload"].toObject()["tab"].toString();
        WindowOpts o;
        o.resizable = true;
        o.minSize = kPanelSize;
        auto [w, created] = aux("panel", tab.isEmpty() ? "/panel" : "/panel?tab=" + tab, "功能面板", kPanelSize, o);
        if (!w) throw Err{"cannot create window"};
        if (created) {
            panelCollapsed_ = false;
            connect(w, &QWindow::widthChanged, this, [this] { if (panelCollapsed_) applyPanelShape(); });
            connect(w, &QWindow::heightChanged, this, [this] { if (panelCollapsed_) applyPanelShape(); });
        }
        present("panel", created, [=, this](bool fresh) { if (!fresh && !tab.isEmpty()) emitEvent("panel", "panel-open-tab", tab); });
        return QJsonValue();
    };
    cmds_["close_panel_window"] = [this](const Ctx &) { win_->destroy("panel"); return QJsonValue(); };
    cmds_["is_panel_window_open"] = [this](const Ctx &) { return win_->get("panel") != nullptr; };
    // The original resizes the window on collapse/expand; here the window keeps its size, the page narrows its card
    // and the strip beyond it is click-through (see regions.cpp::panelRect and docs/qt-port/FEATURES.md).
    cmds_["set_panel_window_collapsed"] = [this](const Ctx &c) {
        panelCollapsed_ = c.a["payload"].toObject()["collapsed"].toBool();
        applyPanelShape();
        return QJsonValue();
    };
    cmds_["raise_panel_window_above_pets"] = [this](const Ctx &) {
        if (QQuickWindow *w = win_->get("panel")) {
            if (w->isVisible()) { panelRaised_ = true; applyLayerPolicy(); }
            w->setFlag(Qt::WindowStaysOnTopHint);
            w->requestActivate();
        }
        return QJsonValue();
    };

    // The toast page caches "ready" in JS, so the window is created once and then only shown. Never takes input.
    cmds_["show_toast_overlay"] = [this](const Ctx &) {
        WindowOpts o;
        o.layer = layerMain_;
        o.focusable = false;
        auto [w, created] = aux("toast", "/toast", "Toast", {800, 600}, o);
        if (!w) throw Err{"cannot create window"};
        win_->setInputRects("toast", {});
        if (!layerMain_) {
            w->setFlag(Qt::WindowStaysOnTopHint);
            w->setGeometry(currentScreen()->geometry());
        }
        w->show();
        return QJsonValue();
    };
    // A mapped, transparent, click-through layer surface is invisible; re-mapping it would reset its input region.
    cmds_["hide_toast_overlay"] = [this](const Ctx &) {
        if (!layerMain_) if (QQuickWindow *w = win_->get("toast")) w->hide();
        return QJsonValue();
    };
    cmds_["window_ready"] = [this](const Ctx &c) { reveal(c.label, std::nullopt); return QJsonValue(); };
    cmds_["window_gate"] = [this](const Ctx &c) { if (pending_.contains(c.label)) pending_[c.label].gated = true; return QJsonValue(); };

    // dock menu / tray
    cmds_["sync_native_menu"] = [this](const Ctx &c) {
        MenuCfg m;
        m.labels = c.a["labels"].toObject();
        m.petVisible = c.a["petDockVisible"].toBool();
        m.scale = int(std::lround(c.a["petScale"].toDouble(100)));
        m.opacity = int(std::lround(c.a["windowOpacity"].toDouble(100)));
        m.passthrough = c.a["windowPassthrough"].toBool();
        tray_->sync(m);
        return QJsonValue();
    };
    cmds_["popup_main_dock_context_menu"] = [this, num](const Ctx &c) { return popupMenu(num(c, "x"), num(c, "y")); };

    // insets
    cmds_["get_main_desktop_safe_area"] = [this](const Ctx &) { return desktop_->safeArea(currentScreen(), layerMain_); };
    cmds_["get_main_taskbar_insets"] = [this](const Ctx &) { return desktop_->insets(currentScreen(), layerMain_); };
    // Present in the original binary (frontend never calls them): template demo and a point-based variant.
    cmds_["get_taskbar_insets_at_point"] = [this](const Ctx &) { return desktop_->insets(currentScreen(), layerMain_); };
    cmds_["greet"] = [](const Ctx &c) { return QJsonValue(QString("Hello, %1! You've been greeted from Rust!").arg(c.a["name"].toString())); };

    // misc
    cmds_["exit_app"] = [](const Ctx &) { QTimer::singleShot(0, qApp, &QCoreApplication::quit); return QJsonValue(); };
    cmds_["exit_maintenance_app"] = cmds_["exit_app"];
    cmds_["write_app_log"] = [](const Ctx &c) {
        const QString level = c.a["level"].toString(), msg = c.a["message"].toString();
        qInfo().noquote() << "[app:" + level + "]" << msg;
        QDir().mkpath(logDir());
        QFile f(logDir() + "/WuwaTappo.log");
        if (f.open(QIODevice::Append)) f.write(("[" + level + "] " + msg + "\n").toUtf8());
        return QJsonValue();
    };
    cmds_["get_track_os_info"] = [](const Ctx &) {
        return QJsonObject{{"os", "Linux"}, {"osVersion", QSysInfo::prettyProductName()}};
    };
    cmds_["get_installer_launch_locale"] = [](const Ctx &) { return QJsonValue::Null; };
    cmds_["get_boot_diagnostics"] = [](const Ctx &) { return QJsonObject{{"system", QJsonObject{{"os", "linux"}}}}; };
    cmds_["is_internal_testing_build"] = [](const Ctx &) { return false; };
    cmds_["is_devtools_hotkeys_build"] = [](const Ctx &) { return false; };
    cmds_["is_elevated_autostart_enabled"] = [](const Ctx &) { return false; };  // a Windows scheduled task; the autostart plugin covers Linux
    cmds_["set_skip_panel_outside_click_close"] = [this](const Ctx &c) {
        for (const auto &v : c.a) if (v.isBool()) { skipPanelOutsideClose_ = v.toBool(); break; }
        return QJsonValue();
    };
    for (const char *n : {"save_app_ui_locale", "set_elevated_autostart", "set_main_force_cursor_ignore", "set_main_swallow_mouse",
                          "set_main_window_content_bounds"})
        cmds_[n] = [](const Ctx &) { return QJsonValue(); };  // Windows-only placement/hook glue

    // maintenance notice (HTTP 601 handshake)
    cmds_["open_maintenance_notice"] = [this](const Ctx &c) {
        QString err;
        const QJsonValue payload = sanitizeNotice(c.a["payload"], &err);
        if (!err.isEmpty()) throw Err{err};
        auto [w, created] = aux("update", "/update", "Service maintenance", {580, 600});
        if (!w) throw Err{"cannot create window"};
        maintenance_ = payload;
        emitEvent("", "cat-force-update-lock", true);
        present("update", created, [=, this](bool) { emitLater("update", "maintenance-notice-open", payload); });
        return QJsonValue();
    };
    cmds_["has_maintenance_notice"] = [this](const Ctx &) { return !maintenance_.isUndefined(); };
    cmds_["is_force_update_locked"] = cmds_["has_maintenance_notice"];
    cmds_["get_maintenance_notice_payload"] = [this](const Ctx &) { return maintenance_.isUndefined() ? QJsonValue::Null : maintenance_; };
    cmds_["hide_main_after_maintenance_notice_ready"] = [this](const Ctx &c) {
        if (c.label != "update" || maintenance_.isUndefined()) throw Err{"maintenance notice is not active in this window"};
        if (QQuickWindow *m = win_->get("main")) m->hide();
        return QJsonValue();
    };
    // The official feed ships Windows installers, not signed releases of this independent Linux port.
    cmds_["check_app_update"] = [](const Ctx &) -> QJsonValue {
        throw Err{"No signed Linux update feed is configured. Windows installers cannot update this Linux port."};
    };

    // monitors
    cmds_["set_main_window_monitor_bounds"] = [this](const Ctx &c) {
        const QJsonObject p = c.a["payload"].toObject();
        const Bounds b{p["x"].toInt(), p["y"].toInt(), unsigned(p["width"].toInt()), unsigned(p["height"].toInt())};
        QScreen *s = win_->screenWithBounds(b);
        if (!s) throw Err{"target monitor disconnected or invalid bounds"};
        selectMainMonitor(s, b);
        return QJsonValue();
    };
    cmds_["sync_main_window_to_current_monitor"] = [this](const Ctx &) {
        QScreen *s = currentScreen();
        selectMainMonitor(s, Windows::boundsOf(s));
        return QJsonValue();
    };
    cmds_["sync_aux_windows_to_main_monitor"] = [this](const Ctx &) { syncAuxToMainScreen(); return QJsonValue(); };
    cmds_["get_main_monitor_snapshot"] = [this](const Ctx &) { return monitorSnapshot(); };
    cmds_["monitor_from_physical_point"] = [this, num](const Ctx &c) -> QJsonValue {
        const double x = num(c, "x"), y = num(c, "y");
        QScreen *sel = currentScreen();
        // Prefer the selected output when mixed-scale physical rectangles overlap: the frontend uses this to locate the main window.
        if (Windows::boundsOf(sel).contains(x, y)) return monitorJson(sel);
        for (QScreen *s : QGuiApplication::screens()) if (Windows::boundsOf(s).contains(x, y)) return monitorJson(s);
        return QJsonValue::Null;
    };
    cmds_["begin_main_monitor_switch"] = [this](const Ctx &) {
        const auto t = gate_.begin(SwitchGate::Clock::now());
        return t ? QJsonValue(double(*t)) : QJsonValue::Null;
    };
    cmds_["end_main_monitor_switch"] = [this](const Ctx &c) { gate_.end(quint64(c.a["token"].toDouble())); return QJsonValue(); };
}

// ---- Tauri plugins the frontend uses ------------------------------------------------------------
QJsonValue Backend::plugin(const QString &cmd, const Ctx &c) {
    const QString name = cmd.mid(7, cmd.indexOf('|') - 7), op = cmd.mid(cmd.indexOf('|') + 1);
    if (name == "window") return windowPlugin(op, c);
    if (name == "event") {
        if (op == "emit" || op == "emit_to") {
            const QString target = op == "emit_to" ? (c.a["target"].isString() ? c.a["target"].toString() : c.a["target"].toObject()["label"].toString()) : QString();
            const QString ev = c.a["event"].toString();
            onPageEvent(c.label, ev, c.a["payload"]);
            emitEvent(target, ev, c.a["payload"]);
            return QJsonValue();
        }
        return QJsonValue();
    }
    if (name == "app") {
        if (op == "version") return WW_VERSION;
        if (op == "name") return "WuwaTappo";
        if (op == "identifier") return "com.kuro.wuwatappo.global";
        if (op == "tauri_version") return "2.10.3";
    }
    if (name == "opener") {
        const QString p = c.a["path"].toString();
        if (op == "open_url") {
            const QUrl u(c.a["url"].toString());
            if (!(u.scheme() == "https" || u.scheme() == "http" || u.scheme() == "mailto")) throw Err{"url scheme not allowed"};
            return QDesktopServices::openUrl(u);
        }
        // Only the app log directory ($APPLOG), as in the original capability file.
        const QString allowed = QDir::cleanPath(logDir());
        const QString clean = QDir::cleanPath(p.isEmpty() ? c.a["paths"].toArray().first().toString() : p);
        if (op == "open_path" || op == "reveal_item_in_dir") {
            if (clean != allowed && !clean.startsWith(allowed + "/")) throw Err{"path not allowed"};
            QDir().mkpath(allowed);
            return QDesktopServices::openUrl(QUrl::fromLocalFile(op == "reveal_item_in_dir" ? QFileInfo(clean).absolutePath() : clean));
        }
    }
    if (name == "autostart") {
        if (op == "is_enabled") return platform::autostartEnabled();
        QString err;
        if (!platform::setAutostart(op == "enable", &err)) throw Err{err};
        return QJsonValue();
    }
    if (name == "process") {
        if (op == "exit") { QTimer::singleShot(0, qApp, &QCoreApplication::quit); return QJsonValue(); }
        if (op == "restart") {
            QProcess::startDetached(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1));
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
            return QJsonValue();
        }
    }
    if (name == "dialog" && op == "message") {
        QMessageBox box(QMessageBox::Information, c.a["title"].toString("Wuwa Tappo"), c.a["message"].toString());
        box.exec();
        return "Ok";
    }
    if (name == "deep-link" && op == "get_current") return deepLinks_.isEmpty() ? QJsonValue::Null : QJsonValue(QJsonArray::fromStringList(deepLinks_));
    if (name == "path" && op == "resolve_directory") {
        using SP = QStandardPaths;
        const int dir = c.a["directory"].toInt();
        QString base;
        switch (dir) {
        case 13: base = SP::writableLocation(SP::AppConfigLocation); break;
        case 14: case 15: base = SP::writableLocation(SP::AppDataLocation); break;
        case 16: base = SP::writableLocation(SP::CacheLocation); break;
        case 17: base = logDir(); break;
        case 21: base = QDir::homePath(); break;
        case 12: base = SP::writableLocation(SP::TempLocation); break;
        case 7: base = SP::writableLocation(SP::DownloadLocation); break;
        default: throw Err{"directory not supported"};
        }
        const QString rel = c.a["path"].toString();
        return rel.isEmpty() ? base : QDir::cleanPath(base + "/" + rel);
    }
    if (name == "updater") throw Err{"No signed Linux update feed is configured."};
    if (name == "webview" || name == "resources" || name == "image") return QJsonValue();  // nothing to do (no native webview objects)
    qWarning().noquote() << "[ipc] unimplemented" << cmd;
    return QJsonValue();
}

QJsonValue Backend::windowPlugin(const QString &op, const Ctx &c) {
    const QString label = c.a["label"].toString(c.label);
    QQuickWindow *w = win_->get(label);
    // Tauri sizes/positions are Physical or Logical; Qt's are logical.
    auto logicalOf = [&](const QJsonObject &v, const char *a, const char *b) {
        const double f = v["type"].toString() == "Physical" && w ? w->devicePixelRatio() : 1.0;
        return std::pair{v["data"].toObject()[a].toDouble() / f, v["data"].toObject()[b].toDouble() / f};
    };
    if (op == "get_all_windows") return QJsonArray::fromStringList(win_->labels());
    if (op == "available_monitors") {
        QJsonArray a;
        for (QScreen *s : QGuiApplication::screens()) a.append(monitorJson(s));
        return a;
    }
    if (op == "primary_monitor") return monitorJson(QGuiApplication::primaryScreen());
    if (op == "monitor_from_point") {
        const double x = c.a["x"].toDouble(), y = c.a["y"].toDouble();
        for (QScreen *s : QGuiApplication::screens()) if (Windows::boundsOf(s).contains(x, y)) return monitorJson(s);
        return QJsonValue::Null;
    }
    if (op == "cursor_position") { const QPoint p = QCursor::pos(); const double d = currentScreen()->devicePixelRatio(); return QJsonObject{{"x", p.x() * d}, {"y", p.y() * d}}; }
    if (!w) {
        if (op.startsWith("is_")) return false;
        throw Err{"window not found: " + label};
    }
    QScreen *scr = w->screen() ? w->screen() : QGuiApplication::primaryScreen();
    const double d = scr->devicePixelRatio();
    const bool mainLayer = label == "main" && win_->isLayer("main");
    if (op == "scale_factor") return d;
    if (op == "current_monitor") return label == "main" ? monitorSnapshot()["monitor"] : QJsonValue(monitorJson(scr));
    if (op == "inner_position" || op == "outer_position") {
        if (mainLayer) return monitorSnapshot()["origin"];  // a layer surface has no position of its own: use its output's origin
        return QJsonObject{{"x", int(w->x() * d)}, {"y", int(w->y() * d)}};
    }
    if (op == "inner_size" || op == "outer_size") return QJsonObject{{"width", int(w->width() * d)}, {"height", int(w->height() * d)}};
    if (op == "title") return w->title();
    if (op == "is_visible") return w->isVisible();
    if (op == "is_focused") return w->isActive();
    if (op == "is_minimized") return w->visibility() == QWindow::Minimized;
    if (op == "is_maximized") return w->visibility() == QWindow::Maximized;
    if (op == "is_fullscreen") return w->visibility() == QWindow::FullScreen;
    if (op == "is_always_on_top") return bool(w->flags() & Qt::WindowStaysOnTopHint) || mainLayer;
    if (op == "is_resizable") return w->minimumSize() != w->maximumSize();
    if (op == "is_decorated") return false;
    if (op == "is_enabled" || op == "is_closable" || op == "is_minimizable" || op == "is_maximizable") return true;
    if (op == "show") { if (label == "main") win_->get("main")->show(); else showWindow(label); return QJsonValue(); }
    if (op == "hide") { w->hide(); return QJsonValue(); }
    if (op == "close" || op == "destroy") { win_->destroy(label); return QJsonValue(); }
    if (op == "minimize") { w->showMinimized(); return QJsonValue(); }
    if (op == "unminimize") { w->showNormal(); return QJsonValue(); }
    if (op == "maximize") { w->showMaximized(); return QJsonValue(); }
    if (op == "unmaximize") { w->showNormal(); return QJsonValue(); }
    if (op == "toggle_maximize") { w->visibility() == QWindow::Maximized ? w->showNormal() : w->showMaximized(); return QJsonValue(); }
    if (op == "set_fullscreen") { c.a["fullscreen"].toBool() ? w->showFullScreen() : w->showNormal(); return QJsonValue(); }
    if (op == "set_focus") { w->requestActivate(); return QJsonValue(); }
    if (op == "set_title") { w->setTitle(c.a["value"].toString()); return QJsonValue(); }
    if (op == "set_always_on_top") {
        if (label == "main") { alwaysOnTop_ = c.a["alwaysOnTop"].toBool(c.a["value"].toBool(true)); if (mainLayer) applyLayerPolicy(); else w->setFlag(Qt::WindowStaysOnTopHint, alwaysOnTop_); }
        else w->setFlag(Qt::WindowStaysOnTopHint, c.a["alwaysOnTop"].toBool(c.a["value"].toBool(true)));
        return QJsonValue();
    }
    if (op == "set_ignore_cursor_events") { win_->setIgnoreInput(label, c.a["ignore"].toBool()); return QJsonValue(); }
    if (op == "set_focusable") { w->setFlag(Qt::WindowDoesNotAcceptFocus, !c.a["focusable"].toBool(true)); return QJsonValue(); }
    if (op == "set_size") { const auto [a, b] = logicalOf(c.a["value"].toObject(), "width", "height"); if (!mainLayer) w->resize(int(a), int(b)); return QJsonValue(); }
    if (op == "set_min_size" || op == "set_max_size") {
        const auto v = c.a["value"];
        if (v.isObject()) {
            const auto [a, b] = logicalOf(v.toObject(), "width", "height");
            op == "set_min_size" ? w->setMinimumSize(QSize(int(a), int(b))) : w->setMaximumSize(QSize(int(a), int(b)));
        } else {
            op == "set_min_size" ? w->setMinimumSize(QSize(0, 0)) : w->setMaximumSize(QSize(kSizeMax, kSizeMax));
        }
        return QJsonValue();
    }
    if (op == "set_resizable") {
        if (c.a["resizable"].toBool(true)) { w->setMinimumSize({0, 0}); w->setMaximumSize({kSizeMax, kSizeMax}); }
        else { w->setMinimumSize(w->size()); w->setMaximumSize(w->size()); }
        return QJsonValue();
    }
    if (op == "set_position") {  // Wayland clients cannot place xdg toplevels; X11 can
        const auto [a, b] = logicalOf(c.a["value"].toObject(), "x", "y");
        if (!mainLayer && platform::x11Session()) w->setPosition(int(a), int(b));
        return QJsonValue();
    }
    if (op == "center") {
        if (!mainLayer && platform::x11Session()) w->setPosition(scr->geometry().center() - QPoint(w->width() / 2, w->height() / 2));
        return QJsonValue();
    }
    if (op == "start_dragging") { w->startSystemMove(); return QJsonValue(); }
    if (op == "start_resize_dragging") {
        static const QHash<QString, Qt::Edges> e{{"North", Qt::TopEdge}, {"South", Qt::BottomEdge}, {"East", Qt::RightEdge}, {"West", Qt::LeftEdge},
            {"NorthEast", Qt::TopEdge | Qt::RightEdge}, {"NorthWest", Qt::TopEdge | Qt::LeftEdge},
            {"SouthEast", Qt::BottomEdge | Qt::RightEdge}, {"SouthWest", Qt::BottomEdge | Qt::LeftEdge}};
        w->startSystemResize(e.value(c.a["value"].toString()));
        return QJsonValue();
    }
    if (op == "set_cursor_icon") { return QJsonValue(); }
    // set_skip_taskbar, set_decorations, set_shadow, set_effects, set_background_color, set_content_protected,
    // set_cursor_*, set_theme, set_visible_on_all_workspaces ...: accepted, no native effect (see FEATURES.md).
    return QJsonValue();
}

}  // namespace ww
