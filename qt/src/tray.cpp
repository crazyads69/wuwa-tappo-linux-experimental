#include "tray.h"
#include "regions.h"
#include <QActionGroup>
#include <QGuiApplication>
#include <QIcon>
#include <QWindow>
#include <QScreen>
#include <QEvent>
#include "platform.h"
#ifdef WW_HAVE_LAYERSHELL
#ifdef WW_HAVE_LAYERSHELL_HEADERS
#include <LayerShellQt/Window>
#else
namespace LayerShellQt { class Window : public QObject { public: static Window *get(QWindow *); }; }
#endif
#endif

namespace ww {

// With the layer-shell Qt integration every window, popup menus included, becomes a layer surface and LayerShellQt's default
// anchors are all four edges: the menu was a full-output surface at the output's corner (seen in nested KWin). Anchor it top-left
// and place it with margins = its Qt global position inside the output; applied on Show/Move so submenus follow too.
namespace {
struct MenuLayer : QObject {
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override {
#ifdef WW_HAVE_LAYERSHELL
        if ((e->type() == QEvent::Show || e->type() == QEvent::Move || e->type() == QEvent::ScreenChangeInternal) && platform::layerShellSelected())
            if (auto *m = qobject_cast<QMenu *>(o); m && m->windowHandle())
                if (QObject *ls = LayerShellQt::Window::get(m->windowHandle())) {
                    const QScreen *s = m->windowHandle()->screen();
                    const QPoint p = m->pos() - (s ? s->geometry().topLeft() : QPoint());
                    ls->setProperty("scope", "wuwatappo-menu");
                    ls->setProperty("anchors", 5);  // top | left
                    ls->setProperty("exclusionZone", 0);
                    ls->setProperty("layer", 3);
                    ls->setProperty("keyboardInteractivity", 2);
                    ls->setProperty("margins", QVariant::fromValue(QMargins(p.x(), p.y(), 0, 0)));
                }
#else
        Q_UNUSED(o) Q_UNUSED(e)
#endif
        return false;
    }
};
}  // namespace

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
    static auto *layer = new MenuLayer(qApp);
    m->installEventFilter(layer);
    for (QMenu *q : m->findChildren<QMenu *>()) q->installEventFilter(layer);
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
