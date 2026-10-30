// Global (unfocused) input, replacing the Windows WH_KEYBOARD_LL / WH_MOUSE_LL hooks of the original app.
// Emits the events the frontend listens to (key-press/key-release/mouse-button/mouse-move) via `input()`.
//  - keys, buttons, touchpad taps: raw /dev/input/event* (read-only, never grabbed; needs the `input` group on most
//    distributions). Only key *names* leave this module; nothing is logged or stored.
//  - absolute pointer: a tiny KWin script over D-Bus (KDE Wayland) or QCursor::pos() polling (X11).
// Disable with WUWA_NO_GLOBAL_INPUT=1.
#pragma once
#include "inputlogic.h"
#include <QJsonValue>
#include <QPoint>
#include <climits>
#include <cmath>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <chrono>

namespace ww {

class EvdevThread;

class InputMonitor : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.wuwatappo.Pointer")
public:
    explicit InputMonitor(QObject *parent = nullptr);
    ~InputMonitor() override;
    void start();
    void stop();
    void setPointerScale(double s) { if (s > 0 && std::isfinite(s)) scale_ = s; }
    // A button that already reached the frontend through the DOM (inside the hit regions) must not be counted twice.
    void noteDelivered(const QString &button, const QString &action);

signals:
    void input(const QString &event, const QJsonValue &payload);

public slots:
    void Pos(int x, int y);   // KWin script, logical px
    void Hotkey(int index);   // KWin script, shortcut pressed
    void onShortcutReleased(const QString &component, const QString &action, qlonglong time);
    void onKey(const QString &name, bool down);
    void onButton(const QString &button, const QString &action, bool tap, qint64 whenMs);

private:
    void startKWinPointer();
    void deliverButton(const QString &button, const QString &action, bool tap, std::chrono::steady_clock::time_point t);
    void emitHotkey(int index, bool down, bool fromShortcut);

    EvdevThread *evdev_ = nullptr;
    QTimer x11Poll_, keepAlive_;
    QList<Delivered> delivered_;
    Hotkeys hotkeys_;
    int owned_ = 0;
    QPoint lastPos_{INT_MIN, INT_MIN};
    std::atomic<double> scale_{1.0};
    bool kwinActive_ = false;
};

}  // namespace ww
