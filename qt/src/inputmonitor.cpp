#include "inputmonitor.h"
#include "desktop.h"
#include "platform.h"
#include <QCoreApplication>
#include <QCursor>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QSet>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace ww {

using namespace std::chrono_literals;
static constexpr auto kDomWait = 50ms;    // how long a physical button waits for the DOM copy
static constexpr auto kTapWait = 260ms;   // taps are synthesised by libinput, the DOM copy comes later
static constexpr auto kTapHold = 100ms;   // a back-to-back press+release counts but never shows the pose

static bool testBit(const uchar *b, int n) { return b[n / 8] & (1 << (n % 8)); }
static bool hasBit(int fd, int ev, int bit) {
    uchar buf[KEY_MAX / 8 + 1] = {};
    if (ioctl(fd, EVIOCGBIT(ev, sizeof buf), buf) < 0) return false;
    return testBit(buf, bit);
}

// ---- evdev worker ----------------------------------------------------------------------------
struct Dev {
    QString path;
    int fd = -1;
    KeyState keys;
    std::optional<Tap> tap;
    bool dropped = false;
};

class EvdevThread : public QThread {
    Q_OBJECT
public:
    explicit EvdevThread(QObject *p) : QThread(p) { if (pipe2(wake_, O_CLOEXEC) != 0) wake_[0] = wake_[1] = -1; }
    ~EvdevThread() override { for (int fd : wake_) if (fd >= 0) ::close(fd); }
    void quitSoon() { requestInterruption(); if (wake_[1] >= 0) { char c = 0; (void)!::write(wake_[1], &c, 1); } }
signals:
    void key(const QString &name, bool down);
    void button(const QString &button, const QString &action, bool tap, qint64 whenMs);

protected:
    void run() override;

private:
    std::optional<Dev> open(const QString &path);
    void scan();
    void emitKey(unsigned code, bool down);
    void handle(Dev &d, const input_event &ev);
    std::optional<TapCfg> tapConfig(int fd, const QString &path);
    QList<Dev> devs_;
    QSet<QString> rejected_;
    int wake_[2] = {-1, -1};
};

static qint64 nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void EvdevThread::emitKey(unsigned code, bool down) {
    const char *action = down ? "press" : "release";
    if (code == BTN_LEFT) emit button("left", action, false, nowMs());
    else if (code == BTN_RIGHT) emit button("right", action, false, nowMs());
    else if (code == BTN_MIDDLE) emit button("middle", action, false, nowMs());
    else if (code < 0x100) emit key(keyName(code), down);  // other BTN_* and exotic keys are not forwarded
}

// KWin exposes effective settings per event device, including defaults absent from kcminputrc.
static std::optional<std::pair<bool, bool>> kwinTapSettings(const QString &path) {
    QDBusInterface i("org.kde.KWin", "/org/kde/KWin/InputDevice/" + QFileInfo(path).fileName(), "org.kde.KWin.InputDevice",
                     QDBusConnection::sessionBus());
    i.setTimeout(2000);
    const QVariant tap = i.property("tapToClick"), lmr = i.property("lmrTapButtonMap");
    if (!i.isValid() || !tap.isValid() || !lmr.isValid()) return std::nullopt;
    return std::pair{tap.toBool(), lmr.toBool()};
}

// KDE writes the user's touchpad choice to kcminputrc; libinput defaults to tapping off on most pads.
// Unset or unreadable -> assume on. Another desktop's setting is not read (documented limit).
static bool tapEnabledInSettings() {
    QFile f(QDir::homePath() + "/.config/kcminputrc");
    const QByteArray t = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    return !(t.contains("TapToClick=false") && !t.contains("TapToClick=true"));
}

std::optional<TapCfg> EvdevThread::tapConfig(int fd, const QString &path) {
    if (qEnvironmentVariableIntValue("WUWA_NO_TAP")) return std::nullopt;
    uchar abs[8] = {}, prop[8] = {};
    ioctl(fd, EVIOCGPROP(sizeof prop), prop);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    // Touchpad = multitouch pointer device that is not a touchscreen (INPUT_PROP_DIRECT).
    if (!(testBit(abs, ABS_MT_SLOT) && testBit(abs, ABS_MT_POSITION_X) && hasBit(fd, EV_KEY, BTN_TOOL_FINGER) && !testBit(prop, INPUT_PROP_DIRECT)))
        return std::nullopt;
    const auto kw = kwinTapSettings(path);
    if (!(kw ? kw->first : tapEnabledInSettings())) return std::nullopt;
    input_absinfo x{}, y{};
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &x) < 0 || ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &y) < 0) return std::nullopt;
    const int edge = (x.maximum - x.minimum) / 20;  // libinput: palms concentrate in the outer ~5 %
    return TapCfg{tapMoveThreshold(x.minimum, x.maximum, x.resolution), tapMoveThreshold(y.minimum, y.maximum, y.resolution),
                  x.minimum + edge, x.maximum - edge, kw && kw->second};
}

std::optional<Dev> EvdevThread::open(const QString &path) {
    const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    if (!(hasBit(fd, 0, EV_KEY) && (hasBit(fd, EV_KEY, KEY_A) || hasBit(fd, EV_KEY, BTN_LEFT)))) { ::close(fd); return std::nullopt; }
    Dev d;
    d.path = path;
    d.fd = fd;
    uchar st[KEY_MAX / 8 + 1] = {};
    if (ioctl(fd, EVIOCGKEY(sizeof st), st) >= 0) {  // keys held at open time are known state: a later release is paired
        std::bitset<kKeyBits> b;
        for (int c = 0; c < kKeyBits; ++c) b[c] = testBit(st, c);
        d.keys.adopt(b);
    }
    if (auto cfg = tapConfig(fd, path)) d.tap = Tap(*cfg);
    return d;
}

void EvdevThread::scan() {
    QSet<QString> present;
    for (const auto &e : QDir("/dev/input").entryInfoList({"event*"}, QDir::System | QDir::Files)) {
        const QString p = e.absoluteFilePath();
        present.insert(p);
        if (rejected_.contains(p) || std::any_of(devs_.begin(), devs_.end(), [&](const Dev &d) { return d.path == p; })) continue;
        if (auto d = open(p)) devs_.append(*d); else rejected_.insert(p);  // do not reopen non-input devices every scan
    }
    rejected_.intersect(present);  // a replugged device reusing the name is probed again
}

void EvdevThread::handle(Dev &d, const input_event &ev) {
    if (ev.type == EV_SYN && ev.code == SYN_DROPPED) { d.dropped = true; return; }
    if (d.dropped) {  // discard until the next SYN_REPORT, then adopt the kernel's key state
        if (ev.type != EV_SYN || ev.code != 0) return;
        d.dropped = false;
        uchar st[KEY_MAX / 8 + 1] = {};
        if (ioctl(d.fd, EVIOCGKEY(sizeof st), st) >= 0) {
            std::bitset<kKeyBits> b;
            for (int c = 0; c < kKeyBits; ++c) b[c] = testBit(st, c);
            for (auto [c, down] : d.keys.resync(b)) emitKey(c, down);
        }
        if (d.tap) d.tap->reset();
        return;
    }
    const quint64 ms = quint64(ev.time.tv_sec) * 1000 + quint64(ev.time.tv_usec) / 1000;
    if (d.tap)
        if (auto i = d.tap->event(ms, ev.type, ev.code, ev.value)) {
            static const char *const names[] = {"left", "right", "middle"};
            emit button(names[*i], "press", true, nowMs());
        }
    if (ev.type == EV_KEY)
        if (auto down = d.keys.apply(ev.code, ev.value)) emitKey(ev.code, *down);
}

void EvdevThread::run() {
    scan();
    if (devs_.isEmpty()) { qWarning("[input] no readable /dev/input/event* keyboard or mouse; global input disabled (join the 'input' group)"); return; }
    qInfo("[input] global input active on %lld device(s)", qint64(devs_.size()));
    auto last = std::chrono::steady_clock::now();
    while (!isInterruptionRequested()) {
        std::vector<pollfd> p;
        p.push_back({wake_[0], POLLIN, 0});
        for (const Dev &d : devs_) p.push_back({d.fd, POLLIN, 0});
        ::poll(p.data(), p.size(), 3000);  // wakes for input, the 3 s hotplug rescan, or quitSoon()
        QList<int> dead;
        for (size_t i = 1; i < p.size(); ++i) {
            Dev &d = devs_[int(i) - 1];
            if (p[i].revents & POLLIN) {
                input_event buf[64];
                for (;;) {
                    const ssize_t n = ::read(d.fd, buf, sizeof buf);
                    if (n < 0) { if (errno != EAGAIN) dead.append(int(i) - 1); break; }  // ENODEV: unplugged
                    if (n == 0) break;
                    for (ssize_t k = 0; k < n / ssize_t(sizeof(input_event)); ++k) handle(d, buf[k]);
                }
            }
            if ((p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) && !dead.contains(int(i) - 1)) dead.append(int(i) - 1);
        }
        std::sort(dead.rbegin(), dead.rend());
        for (int i : dead) {  // a removed device can no longer report releases
            for (unsigned c : devs_[i].keys.drain()) emitKey(c, false);
            ::close(devs_[i].fd);
            devs_.removeAt(i);
        }
        if (std::chrono::steady_clock::now() - last > 3s) { scan(); last = std::chrono::steady_clock::now(); }
    }
    for (const Dev &d : devs_) ::close(d.fd);
}

// ---- main-thread side ------------------------------------------------------------------------
static const char *kPointerScript = R"JS(
var last = "", lastSend = 0, lastAck = Date.now();
var hotkeys = __WW_HOTKEYS__;
hotkeys.forEach(function (key) {
  registerShortcut(key[1], "WuwaTappo " + key[0], key[0], function (action) {
    action.autoRepeat = false;
    callDBus("org.wuwatappo.Pointer", "/Pointer", "org.wuwatappo.Pointer", "Hotkey", key[2]);
  });
});
var t = new QTimer();
t.interval = 20;
t.timeout.connect(function () {
  var now = Date.now();
  if (now - lastAck > 4000) {
    t.stop();
    callDBus("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript", "wuwatappo-pointer");
    return;
  }
  var p = workspace.cursorPos;
  var k = Math.round(p.x) + "," + Math.round(p.y);
  if (k === last && now - lastSend < 1000) return;
  last = k; lastSend = now;
  callDBus("org.wuwatappo.Pointer", "/Pointer", "org.wuwatappo.Pointer", "Pos",
           Math.round(p.x), Math.round(p.y), function () { lastAck = Date.now(); });
});
t.start();
)JS";

InputMonitor::InputMonitor(QObject *parent) : QObject(parent) {}
InputMonitor::~InputMonitor() { stop(); }

void InputMonitor::start() {
    if (qEnvironmentVariable("WUWA_NO_GLOBAL_INPUT") == "1") { qInfo("[input] disabled by WUWA_NO_GLOBAL_INPUT=1"); return; }
    evdev_ = new EvdevThread(this);
    connect(evdev_, &EvdevThread::key, this, &InputMonitor::onKey, Qt::QueuedConnection);
    connect(evdev_, &EvdevThread::button, this, &InputMonitor::onButton, Qt::QueuedConnection);
    evdev_->start();
    if (platform::x11Session()) {
        x11Poll_.setInterval(16);
        connect(&x11Poll_, &QTimer::timeout, this, [this] {
            const QPoint p = QCursor::pos();
            if (p == lastPos_) return;
            lastPos_ = p;
            const double d = QGuiApplication::screenAt(p) ? QGuiApplication::screenAt(p)->devicePixelRatio() : 1.0;
            const QPoint px = pointerPixels(p.x(), p.y(), d);
            emit input("mouse-move", QJsonObject{{"x", px.x()}, {"y", px.y()}});
        });
        x11Poll_.start();
    } else if (platform::kde()) {
        startKWinPointer();
    }
}

void InputMonitor::stop() {
    x11Poll_.stop();
    if (evdev_) { evdev_->quitSoon(); evdev_->wait(2000); delete evdev_; evdev_ = nullptr; }
    if (kwinActive_) { kwin::unloadScript("wuwatappo-pointer"); kwinActive_ = false; }
}

void InputMonitor::startKWinPointer() {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService("org.wuwatappo.Pointer") || !bus.registerObject("/Pointer", this, QDBusConnection::ExportAllSlots)) {
        qWarning("[input] cannot own org.wuwatappo.Pointer (another instance?)");
        return;
    }
    // A key already owned by another shortcut is left alone.
    QJsonArray allowed;
    owned_ = 0;
    for (size_t i = 0; i < kHotkeys.size(); ++i) {
        QDBusInterface accel("org.kde.kglobalaccel", "/kglobalaccel", "org.kde.KGlobalAccel", bus);
        accel.setTimeout(500);
        const QDBusReply<QStringList> owner = accel.call("action", kHotkeys[i].qtCode);
        const bool ours = owner.isValid() && owner.value().size() > 1 && owner.value()[0] == "kwin" && owner.value()[1] == kHotkeys[i].action;
        if (owner.isValid() && (owner.value().isEmpty() || ours)) {
            allowed.append(QJsonArray{kHotkeys[i].key, kHotkeys[i].action, int(i)});
            owned_ |= 1 << i;
        }
    }
    bus.connect("org.kde.kglobalaccel", "/component/kwin", "org.kde.kglobalaccel.Component", "globalShortcutReleased", this,
                SLOT(onShortcutReleased(QString, QString, qlonglong)));
    QString js = kPointerScript, err;
    js.replace("__WW_HOTKEYS__", QJsonDocument(allowed).toJson(QJsonDocument::Compact));
    if (kwin::runScript("wuwatappo-pointer", js, &err)) {
        kwinActive_ = true;
        qInfo("[input] pointer position via KWin script");
    } else {
        qWarning().noquote() << "[input] absolute pointer position unavailable (KWin script:" << err << ")";
    }
}

void InputMonitor::Pos(int x, int y) {
    const QPoint p = pointerPixels(x, y, scale_);
    if (p == lastPos_) return;
    lastPos_ = p;
    emit input("mouse-move", QJsonObject{{"x", p.x()}, {"y", p.y()}});
}

void InputMonitor::emitHotkey(int i, bool down, bool fromShortcut) {
    if (fromShortcut && down && !(owned_ & (1 << i))) return;
    if (hotkeys_.raw(i, down)) emit input(down ? "key-press" : "key-release", QString(kHotkeys[size_t(i)].key));
}
void InputMonitor::Hotkey(int index) { if (index >= 0 && index < int(kHotkeys.size())) emitHotkey(index, true, true); }
void InputMonitor::onShortcutReleased(const QString &component, const QString &action, qlonglong) {
    if (component != "kwin") return;
    for (size_t i = 0; i < kHotkeys.size(); ++i) if (action == kHotkeys[i].action) emitHotkey(int(i), false, true);
}

void InputMonitor::onKey(const QString &name, bool down) {
    const int h = hotkeyIndex(name);
    if (h >= 0) return emitHotkey(h, down, false);
    emit input(down ? "key-press" : "key-release", name);
}

void InputMonitor::noteDelivered(const QString &button, const QString &action) {
    delivered_.append({std::chrono::steady_clock::now(), button, action});
}

void InputMonitor::onButton(const QString &button, const QString &action, bool tap, qint64 whenMs) {
    using namespace std::chrono;
    const auto t = steady_clock::time_point(milliseconds(whenMs));
    const auto wait = tap ? kTapWait : kDomWait;
    QTimer::singleShot(duration_cast<milliseconds>(wait), this, [=, this] { deliverButton(button, action, tap, t); });
}

void InputMonitor::deliverButton(const QString &button, const QString &action, bool tap, std::chrono::steady_clock::time_point t) {
    const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(tap ? kTapWait : kDomWait);
    // A delivered tap press owns its eventual DOM release, including a long tap-and-drag.
    if (takeDelivered(delivered_, t, button, action, wait)) return;
    emit input("mouse-button", QJsonObject{{"button", button}, {"action", action}, {"native", true}});
    if (tap)
        QTimer::singleShot(std::chrono::duration_cast<std::chrono::milliseconds>(kTapHold), this,
                           [=, this] { emit input("mouse-button", QJsonObject{{"button", button}, {"action", "release"}, {"native", true}}); });
}

}  // namespace ww

#include "inputmonitor.moc"
