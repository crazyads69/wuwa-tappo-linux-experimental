// Dock menu: system tray (StatusNotifierItem on KDE/GNOME-with-extension, XEmbed elsewhere) and the same menu popped
// up at the pet on right-click, which is also the fallback on desktops without any tray.
#pragma once
#include <QJsonObject>
#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>
#include <memory>

namespace ww {

struct MenuCfg {
    QJsonObject labels;
    bool petVisible = true, passthrough = false;
    int scale = 100, opacity = 100;
    bool operator==(const MenuCfg &) const = default;
};

inline const QList<int> kScales{50, 75, 100, 125, 150, 200};
inline const QList<int> kOpacities{25, 50, 75, 100};

class Tray : public QObject {
    Q_OBJECT
public:
    explicit Tray(QObject *parent = nullptr) : QObject(parent) {}
    void sync(const MenuCfg &cfg);  // rebuilds only when something changed
    bool ready() const { return menu_ != nullptr; }
    void popup(const QPoint &globalLogical, QWindow *parent);

signals:
    // Menu item ids as recovered from the original binary: dock_bag, dock_atlas, dock_exchange, dock_room,
    // dock_settings, dock_window_passthrough, dock_window_opacity_<n>, dock_pet_scale_<n>, dock_toggle_pet,
    // dock_restart, dock_quit.
    void action(const QString &id);

private:
    std::unique_ptr<QMenu> build(const MenuCfg &c);
    MenuCfg cfg_;
    std::unique_ptr<QMenu> menu_;
    QSystemTrayIcon *icon_ = nullptr;
};

}  // namespace ww
