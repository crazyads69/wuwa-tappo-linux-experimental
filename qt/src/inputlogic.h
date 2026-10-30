// Device-independent input logic ported from src-tauri/src/input.rs: key press pairing, touchpad tap detection
// (libinput semantics) and hotkey de-duplication. The evdev/KWin plumbing lives in inputmonitor.cpp.
#pragma once
#include <QList>
#include <QString>
#include <array>
#include <bitset>
#include <chrono>
#include <optional>

namespace ww {

inline constexpr int kKeyBits = 0x300;
inline constexpr int kEvSyn = 0, kEvKey = 1, kEvAbs = 3;
inline constexpr int kAbsMtSlot = 0x2f, kAbsMtPosX = 0x35, kAbsMtPosY = 0x36, kAbsMtToolType = 0x37, kAbsMtTrackingId = 0x39;
inline constexpr int kTapTimeoutMs = 180, kMaxSlots = 16, kMtToolPalm = 2;

QString keyName(unsigned code);  // evdev code -> rdev `Key` Debug name the frontend compares ("F8", "KeyA", ...)

// Autorepeat (value 2) and duplicate presses are ignored, a release for a key never seen pressed is ignored.
class KeyState {
public:
    std::optional<bool> apply(unsigned code, int value);
    QList<unsigned> held() const;
    QList<std::pair<unsigned, bool>> resync(const std::bitset<kKeyBits> &truth);  // missed transitions (SYN_DROPPED)
    QList<unsigned> drain();                                                        // releases for a removed device
    void adopt(const std::bitset<kKeyBits> &b) { bits_ = b; }

private:
    std::bitset<kKeyBits> bits_;
};

struct TapCfg {
    int moveX = 1, moveY = 1;  // 1.3 mm in device units
    int edgeLo = 0, edgeHi = 0;  // x outside [edgeLo, edgeHi] is the palm edge zone
    bool lmr = false;            // effective per-device tap button map
};

// Multitouch protocol B. event() returns the button index (0 left, 1 right, 2 middle) when a tap completes.
class Tap {
public:
    explicit Tap(TapCfg c = {}) : cfg_(c) {}
    std::optional<int> event(quint64 tMs, int type, int code, int value);
    void reset() { *this = Tap(cfg_); }

private:
    struct Slot { int id = -1, x = 0, y = 0, sx = 0, sy = 0; bool hx = false, hy = false, seen = false, palm = false, counted = false; };
    std::optional<int> sync(quint64 t);
    TapCfg cfg_;
    std::array<Slot, kMaxSlots> slots_{};
    int cur_ = 0;
    bool active_ = false, cancelled_ = false;
    quint64 start_ = 0;
    int maxFingers_ = 0, buttons_ = 0;
};
int tapMoveThreshold(int lo, int hi, int resolution);

// F7/F8/F9 are owned system-wide (KDE shortcuts); press/release must alternate.
struct Hotkey { const char *key, *action; int qtCode; };
inline constexpr std::array<Hotkey, 3> kHotkeys{{{"F7", "wuwatappo-game-mode", 0x01000036},
                                                  {"F8", "wuwatappo-switch-monitor", 0x01000037},
                                                  {"F9", "wuwatappo-toggle-pet", 0x01000038}}};
int hotkeyIndex(const QString &key);
class Hotkeys {
public:
    // returns true when this edge should be forwarded
    // The original debounces every hotkey press by 280 ms (FUN_14028f270); the release of a dropped press is dropped too.
    bool raw(int index, bool down, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        if (down_[index] == down) return false;
        down_[index] = down;
        if (down) {
            if (last_[index] && now - *last_[index] < std::chrono::milliseconds(280)) { dropped_[index] = true; return false; }
            last_[index] = now;
            return true;
        }
        if (dropped_[index]) { dropped_[index] = false; return false; }
        return true;
    }
private:
    std::array<bool, 3> down_{}, dropped_{};
    std::array<std::optional<std::chrono::steady_clock::time_point>, 3> last_{};
};

// A DOM-delivered button (inside the hit regions) suppresses the evdev copy of the same button.
struct Delivered { std::chrono::steady_clock::time_point at; QString button, action; };
bool takeDelivered(QList<Delivered> &d, std::chrono::steady_clock::time_point t, const QString &button, const QString &action,
                   std::chrono::milliseconds wait);

}  // namespace ww
