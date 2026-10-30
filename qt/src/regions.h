// Pure geometry/state logic ported from the earlier Rust/Tauri shell (removed; see git history). No windowing here, so it is unit-tested.
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPoint>
#include <QRegion>
#include <array>
#include <chrono>
#include <optional>

namespace ww {

struct Rect {
    double left = 0, top = 0, width = 0, height = 0;
    QJsonValue memberId;  // routes clicks on other players' pets (shim.js)
    bool operator==(const Rect &o) const;
    static Rect fromJson(const QJsonObject &o);
    QJsonObject toJson() const;
};
using Rects = QList<Rect>;
Rects parseRects(const QJsonArray &a);

// `inputRegions` only arrive with updateNativeInput=true; later calls carry just the visual `regions`.
struct HitRegion {
    Rects regions, inputRegions;
    bool updateNativeInput = false, windowPassthrough = false;
    static HitRegion fromJson(const QJsonObject &o);
    Rects inputRects(std::optional<Rects> &cached) const;
};

// floor/ceil+1 like the GTK input shape of the earlier Rust shell: the DOM hit test is inclusive on the far edge.
QList<QRect> toPixelRects(const Rects &r);

// Collapsed panel: only the narrowed card is clickable (the rest of the window is click-through).
constexpr double kPanelRailDelta = 142.0;
Rect panelRect(bool collapsed, double width, double height);

using Box = std::array<double, 4>;  // x, y, w, h
QJsonObject insetsFromRect(const Box &monitor, const Box &available);
// x/y are physical screen px; result is window-relative logical px.
QPointF relativePointer(double x, double y, QPoint origin, double scale);
int nearest(const QList<int> &options, int value);
QPoint pointerPixels(int x, int y, double scale);

struct Bounds {
    int x = 0, y = 0;
    unsigned width = 0, height = 0;
    bool operator==(const Bounds &) const = default;
    bool contains(double px, double py) const;
};
QPoint centered(const Bounds &b, int width, int height);

// KDE desktop facts (desktop.cpp feeds these): same output within 1 px, auto-hide edge, fullscreen report.
bool sameOutput(const Box &a, const Box &b);
// panels: [{id, hiding, location, geometry:[x,y,w,h]}]. Oldest auto-hiding panel on that output wins (frontend takes one).
QString autoHideEdge(const QJsonArray &panels, const Box &output);
// "[true,[x,y,w,h]]" -> output box; nullopt for "not fullscreen", garbage or oversized input.
std::optional<Box> parseFullscreenReport(const QString &payload);

// Original FUN_140011da0: internetAccess = connectivity level in {ConstrainedInternetAccess, InternetAccess}; the last
// value starts as false and an unchanged value is not re-emitted. `reachable` is true only for QNetworkInformation::Online.
class NetStatusDedupe {
public:
    std::optional<bool> update(bool reachable) { if (reachable == last_) return std::nullopt; last_ = reachable; return reachable; }
private:
    bool last_ = false;
};

// Original FUN_140021570 / FUN_140019230: a left press closes the panel when it exists, the skip flag
// (set_skip_panel_outside_click_close) is off, any grace deadline has passed and the pointer is outside the panel rect.
bool panelOutsideClose(bool panelOpen, bool skipFlag, bool pointerKnown, bool pointerInPanel, bool graceOver);

// F8 handshake: one switch at a time, abandoned leases recover after 15 s, 280 ms debounce.
class SwitchGate {
public:
    using Clock = std::chrono::steady_clock;
    std::optional<quint64> begin(Clock::time_point now);
    void end(quint64 token);

private:
    quint64 next_ = 0;
    std::optional<std::pair<quint64, Clock::time_point>> active_;
    std::optional<Clock::time_point> last_;
};

}  // namespace ww
