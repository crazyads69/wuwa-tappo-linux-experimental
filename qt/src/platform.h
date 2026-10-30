// Desktop-environment glue: session detection, input regions (Wayland/X11), XDG autostart / Background portal.
#pragma once
#include <QList>
#include <QRect>
#include <QString>

class QWindow;

namespace ww::platform {

// Call before QGuiApplication: selects the wlr-layer-shell Qt integration when the compositor offers the protocol.
// Returns true when layer-shell was selected. WW_NO_LAYER=1 forces the plain-window fallback.
bool selectShellIntegration();
bool layerShellSelected();

bool waylandSession();  // runtime platform is "wayland"
bool x11Session();      // runtime platform is "xcb"
bool kde();             // XDG_CURRENT_DESKTOP contains KDE
bool sandboxed();       // Flatpak/Snap: autostart and URL opening go through portals

// Restrict pointer input to `rects` (window-local logical px). An empty list makes the whole window click-through.
void setInputRegion(QWindow *w, const QList<QRect> &rects);

bool autostartEnabled();
bool setAutostart(bool on, QString *error);

}  // namespace ww::platform
