#include "regions.h"
#include <QJsonDocument>
#include <cmath>

namespace ww {

bool Rect::operator==(const Rect &o) const {
    return left == o.left && top == o.top && width == o.width && height == o.height && memberId == o.memberId;
}
Rect Rect::fromJson(const QJsonObject &o) {
    return {o["left"].toDouble(), o["top"].toDouble(), o["width"].toDouble(), o["height"].toDouble(), o["memberId"]};
}
QJsonObject Rect::toJson() const {
    QJsonObject o{{"left", left}, {"top", top}, {"width", width}, {"height", height}};
    if (!memberId.isUndefined() && !memberId.isNull()) o["memberId"] = memberId;
    return o;
}
Rects parseRects(const QJsonArray &a) {
    Rects r;
    for (const auto &v : a) r.append(Rect::fromJson(v.toObject()));
    return r;
}

HitRegion HitRegion::fromJson(const QJsonObject &o) {
    return {parseRects(o["regions"].toArray()), parseRects(o["inputRegions"].toArray()),
            o["updateNativeInput"].toBool(), o["windowPassthrough"].toBool()};
}
Rects HitRegion::inputRects(std::optional<Rects> &cached) const {
    if (updateNativeInput) cached = inputRegions;
    if (windowPassthrough) return {};
    return cached.value_or(regions);
}

QList<QRect> toPixelRects(const Rects &r) {
    QList<QRect> out;
    for (const auto &x : r)
        out.append({int(std::floor(x.left)), int(std::floor(x.top)), int(std::ceil(x.width)) + 1, int(std::ceil(x.height)) + 1});
    return out;
}

Rect panelRect(bool collapsed, double width, double height) {
    constexpr double whole = 32767.0;  // expanded: everything is clickable, whatever the window size
    if (!collapsed) return {0, 0, whole, whole};
    return {0, 0, std::max(0.0, width - kPanelRailDelta), height};
}

QJsonObject insetsFromRect(const Box &m, const Box &r) {
    auto px = [](double v) { return qint64(std::llround(std::max(0.0, v))); };
    return {{"left", px(r[0] - m[0])},
            {"top", px(r[1] - m[1])},
            {"right", px(m[0] + m[2] - r[0] - r[2])},
            {"bottom", px(m[1] + m[3] - r[1] - r[3])}};
}

QPointF relativePointer(double x, double y, QPoint origin, double scale) {
    return {(x - origin.x()) / scale, (y - origin.y()) / scale};
}

int nearest(const QList<int> &options, int value) {
    int best = options.first();
    for (int o : options)
        if (std::abs(o - value) < std::abs(best - value)) best = o;
    return best;
}

QPoint pointerPixels(int x, int y, double scale) { return {int(std::lround(x * scale)), int(std::lround(y * scale))}; }

bool Bounds::contains(double px, double py) const {
    return px >= x && py >= y && px < double(x) + width && py < double(y) + height;  // NaN compares false
}
QPoint centered(const Bounds &b, int w, int h) {
    return {b.x + int(std::max<qint64>(0, qint64(b.width) - w) / 2), b.y + int(std::max<qint64>(0, qint64(b.height) - h) / 2)};
}

bool sameOutput(const Box &a, const Box &b) {
    for (int i = 0; i < 4; ++i) if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::abs(a[i] - b[i]) >= 1.0) return false;
    return a[2] > 0 && a[3] > 0 && b[2] > 0 && b[3] > 0;
}

QString autoHideEdge(const QJsonArray &panels, const Box &output) {
    QString edge;
    double bestId = 1e300;
    for (const auto &v : panels) {
        const auto p = v.toObject();
        const auto g = p["geometry"].toArray();
        const QString loc = p["location"].toString();
        if (g.size() != 4 || p["hiding"].toString() != "autohide" || !(loc == "left" || loc == "top" || loc == "right" || loc == "bottom")) continue;
        if (!sameOutput({g[0].toDouble(NAN), g[1].toDouble(NAN), g[2].toDouble(NAN), g[3].toDouble(NAN)}, output)) continue;
        if (p["id"].toDouble() < bestId) { bestId = p["id"].toDouble(); edge = loc; }
    }
    return edge;
}

std::optional<Box> parseFullscreenReport(const QString &payload) {
    if (payload.size() >= 256) return std::nullopt;
    const auto a = QJsonDocument::fromJson(payload.toUtf8()).array();
    if (a.size() != 2 || !a[0].toBool() || a[1].toArray().size() != 4) return std::nullopt;
    const auto g = a[1].toArray();
    Box b{g[0].toDouble(NAN), g[1].toDouble(NAN), g[2].toDouble(NAN), g[3].toDouble(NAN)};
    if (!sameOutput(b, b)) return std::nullopt;
    return b;
}

std::optional<quint64> SwitchGate::begin(Clock::time_point now) {
    using namespace std::chrono;
    if ((active_ && now - active_->second < seconds(15)) || (last_ && now - *last_ < milliseconds(280))) return std::nullopt;
    next_ = std::max<quint64>(next_ + 1, 1);
    active_ = {next_, now};
    last_ = now;
    return next_;
}
void SwitchGate::end(quint64 token) {
    if (active_ && active_->first == token) active_.reset();
}

}  // namespace ww
