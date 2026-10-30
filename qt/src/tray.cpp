#include "tray.h"
#include "regions.h"
#include <QActionGroup>
#include <QGuiApplication>
#include <QIcon>
#include <QWindow>

namespace ww {

std::unique_ptr<QMenu> Tray::build(const MenuCfg &c) {
    auto m = std::make_unique<QMenu>();
    auto label = [&](const char *k) { return c.labels[k].isString() ? c.labels[k].toString() : QString(k); };
    auto item = [&](const QString &id, const QString &text, QMenu *to, bool checkable = false, bool checked = false) {
        QAction *a = to->addAction(text);
        a->setCheckable(checkable);
        a->setChecked(checked);
        connect(a, &QAction::triggered, this, [this, id] { emit action(id); });
        return a;
    };
    item("dock_bag", label("openBag"), m.get());
    item("dock_atlas", label("openAtlas"), m.get());
    item("dock_exchange", label("openExchange"), m.get());
    item("dock_room", label("openRoom"), m.get());
    m->addSeparator();
    item("dock_settings", label("dockSettings"), m.get());
    item("dock_window_passthrough", label("windowPassthrough"), m.get(), true, c.passthrough);
    auto *op = m->addMenu(label("windowOpacity"));
    // The original checks the option nearest to the current value (JS clamps, so e.g. 90 can occur).
    for (int n : kOpacities) item(QString("dock_window_opacity_%1").arg(n), QString("%1%").arg(n), op, true, n == nearest(kOpacities, c.opacity));
    auto *sz = m->addMenu(label("windowSize"));
    for (int n : kScales) item(QString("dock_pet_scale_%1").arg(n), QString("%1%").arg(n), sz, true, n == nearest(kScales, c.scale));
    item("dock_toggle_pet", label(c.petVisible ? "hidePet" : "showPet"), m.get());
    m->addSeparator();
    item("dock_restart", label("restart"), m.get());
    item("dock_quit", label("quit"), m.get());
    return m;
}

void Tray::sync(const MenuCfg &cfg) {
    // The frontend re-syncs on every state tick; rebuilding the menu and the tray's D-Bus menu is not free.
    if (menu_ && cfg == cfg_) return;
    cfg_ = cfg;
    auto fresh = build(cfg);
    if (!icon_ && QSystemTrayIcon::isSystemTrayAvailable()) {
        icon_ = new QSystemTrayIcon(QGuiApplication::windowIcon(), this);
        icon_->show();
    }
    if (icon_) {
        icon_->setContextMenu(fresh.get());
        icon_->setToolTip(cfg.labels["trayTooltip"].toString("Wuwa Tappo"));
    }
    menu_ = std::move(fresh);
}

void Tray::popup(const QPoint &p, QWindow *parent) {
    if (!menu_) return;
    menu_->winId();  // create the native window so it can be parented to the (unfocused) layer surface
    if (parent && menu_->windowHandle()) menu_->windowHandle()->setTransientParent(parent);
    menu_->popup(p);
}

}  // namespace ww
