#include "inputlogic.h"
#include <cstdlib>

namespace ww {

QString keyName(unsigned c) {
    static const char *const fk[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10"};
    static const char *const row[] = {"Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9", "Num0", "Minus", "Equal",
                                      "Backspace", "Tab", "KeyQ", "KeyW", "KeyE", "KeyR", "KeyT", "KeyY", "KeyU", "KeyI", "KeyO",
                                      "KeyP", "LeftBracket", "RightBracket", "Return", "ControlLeft", "KeyA", "KeyS", "KeyD", "KeyF",
                                      "KeyG", "KeyH", "KeyJ", "KeyK", "KeyL", "SemiColon", "Quote", "BackQuote", "ShiftLeft",
                                      "BackSlash", "KeyZ", "KeyX", "KeyC", "KeyV", "KeyB", "KeyN", "KeyM", "Comma", "Dot", "Slash",
                                      "ShiftRight", "KpMultiply", "Alt", "Space", "CapsLock"};  // codes 2..58
    static const char *const kp[] = {"Kp7", "Kp8", "Kp9", "KpMinus", "Kp4", "Kp5", "Kp6", "KpPlus", "Kp1", "Kp2", "Kp3", "Kp0", "KpDecimal"};  // 71..83
    if (c == 1) return "Escape";
    if (c >= 2 && c <= 58) return row[c - 2];
    if (c >= 59 && c <= 68) return fk[c - 59];
    if (c >= 71 && c <= 83) return kp[c - 71];
    if (c >= 183 && c <= 194) return "F" + QString::number(c - 183 + 13);
    switch (c) {
    case 69: return "NumLock"; case 70: return "ScrollLock"; case 86: return "IntlBackslash"; case 87: return "F11"; case 88: return "F12";
    case 96: return "KpReturn"; case 97: return "ControlRight"; case 98: return "KpDivide"; case 99: return "PrintScreen";
    case 100: return "AltGr"; case 102: return "Home"; case 103: return "UpArrow"; case 104: return "PageUp"; case 105: return "LeftArrow";
    case 106: return "RightArrow"; case 107: return "End"; case 108: return "DownArrow"; case 109: return "PageDown";
    case 110: return "Insert"; case 111: return "Delete"; case 113: return "VolumeMute"; case 114: return "VolumeDown";
    case 115: return "VolumeUp"; case 117: return "KpEqual"; case 119: return "Pause"; case 125: return "MetaLeft";
    case 126: return "MetaRight"; case 127: return "Apps";
    }
    return QString("Unknown(%1)").arg(c);
}

std::optional<bool> KeyState::apply(unsigned code, int value) {
    if (code >= kKeyBits || value == 2) return std::nullopt;
    const bool down = bits_[code];
    if (value == 1 && !down) { bits_[code] = true; return true; }
    if (value == 0 && down) { bits_[code] = false; return false; }
    return std::nullopt;
}
QList<unsigned> KeyState::held() const {
    QList<unsigned> v;
    for (unsigned c = 0; c < kKeyBits; ++c) if (bits_[c]) v.append(c);
    return v;
}
QList<std::pair<unsigned, bool>> KeyState::resync(const std::bitset<kKeyBits> &truth) {
    QList<std::pair<unsigned, bool>> out;
    for (unsigned c = 0; c < kKeyBits; ++c) if (bits_[c] != truth[c]) out.append({c, truth[c]});
    bits_ = truth;
    return out;
}
QList<unsigned> KeyState::drain() {
    auto v = held();
    bits_.reset();
    return v;
}

std::optional<int> Tap::event(quint64 t, int type, int code, int value) {
    if (type == kEvAbs) {
        if (code == kAbsMtSlot) { cur_ = std::clamp(value, 0, kMaxSlots - 1); return std::nullopt; }
        Slot &s = slots_[cur_];
        switch (code) {
        // MT slot axes are sticky: a reused contact need not resend unchanged X/Y.
        case kAbsMtTrackingId: s = Slot{value, s.x, s.y, 0, 0, s.hx, s.hy}; break;
        case kAbsMtPosX: s.x = value; s.hx = true; break;
        case kAbsMtPosY: s.y = value; s.hy = true; break;
        case kAbsMtToolType: if (value == kMtToolPalm) s.palm = true; break;
        }
    } else if (type == kEvKey && code >= 0x110 && code <= 0x112) {
        if (value == 1) { ++buttons_; cancelled_ = true; }
        else if (value == 0) buttons_ = std::max(0, buttons_ - 1);
    } else if (type == kEvSyn && code == 0) {
        return sync(t);
    }
    return std::nullopt;
}

std::optional<int> Tap::sync(quint64 t) {
    int live = 0;
    for (Slot &s : slots_) {
        if (s.id < 0) continue;
        if (!s.seen && s.hx && s.hy) {
            s.seen = true; s.sx = s.x; s.sy = s.y;
            if (s.x < cfg_.edgeLo || s.x > cfg_.edgeHi) s.palm = true;
        }
        if (!s.seen) continue;
        if (s.palm) { if (s.counted) cancelled_ = true; continue; }  // a finger that turns into a palm spoils the tap
        s.counted = true;
        if (std::abs(s.x - s.sx) > cfg_.moveX || std::abs(s.y - s.sy) > cfg_.moveY) cancelled_ = true;
        ++live;
    }
    if (!active_ && live > 0) { active_ = true; start_ = t; maxFingers_ = 0; cancelled_ = buttons_ > 0; }
    if (!active_) return std::nullopt;
    maxFingers_ = std::max(maxFingers_, live);
    if (live > 3 || t - std::min(t, start_) > quint64(kTapTimeoutMs)) cancelled_ = true;
    if (live > 0) return std::nullopt;
    active_ = false;
    if (cancelled_ || maxFingers_ < 1 || maxFingers_ > 3) return std::nullopt;
    static const int lr[] = {0, 1, 2}, lmr[] = {0, 2, 1};
    return (cfg_.lmr ? lmr : lr)[maxFingers_ - 1];
}

int tapMoveThreshold(int lo, int hi, int res) {
    // shortcut: axes lacking resolution assume a 100 mm pad; use device quirks if one needs calibration.
    return std::max(1, res > 0 ? res * 13 / 10 : (hi - lo) * 13 / 1000);
}

int hotkeyIndex(const QString &key) {
    for (size_t i = 0; i < kHotkeys.size(); ++i) if (key == kHotkeys[i].key) return int(i);
    return -1;
}

bool takeDelivered(QList<Delivered> &d, std::chrono::steady_clock::time_point t, const QString &button, const QString &action,
                   std::chrono::milliseconds wait) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    d.removeIf([&](const Delivered &x) { return now - x.at >= seconds(1); });
    for (int i = 0; i < d.size(); ++i)
        if (d[i].button == button && d[i].action == action && d[i].at + milliseconds(20) >= t && d[i].at <= t + wait) {
            d.removeAt(i);
            return true;
        }
    return false;
}

}  // namespace ww
