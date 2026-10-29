//! Global (unfocused) input for the Linux port, replacing the Windows WH_KEYBOARD_LL / WH_MOUSE_LL
//! hooks of the original backend. Emits the same Tauri events the frontend listens to:
//!   key-press / key-release  payload: String key name (rdev `Key` Debug names: "KeyA", "Space", "F9", ...)
//!   mouse-button             payload: {button:'left'|'right'|'middle', action:'press'|'release', native:true}
//!   mouse-move               payload: {x, y} absolute physical screen px
//! Keystrokes: only the key *name* the frontend needs leaves this module; nothing is logged or stored.
//! Disable with WUWA_NO_GLOBAL_INPUT=1.
//!
//! Sources: /dev/input/event* (raw evdev via libc, read-only, never grabbed) for keys and buttons;
//! absolute pointer position from a tiny KWin script (Wayland/KDE, over D-Bus) or XQueryPointer (X11).
use serde_json::{json, Value};
use std::{
    fs::{File, OpenOptions},
    os::{fd::AsRawFd, unix::fs::OpenOptionsExt},
    path::PathBuf,
    sync::{atomic::{AtomicBool, AtomicU64, Ordering}, mpsc, Arc, Mutex},
    thread,
    time::{Duration, Instant},
};
use tauri::{AppHandle, Emitter, Listener, Manager};

type Sink = Arc<dyn Fn(&str, Value) + Send + Sync>;

/// Entry point: spawns background threads, returns immediately. Never panics the app.
pub fn start(app: AppHandle) {
    if std::env::var("WUWA_NO_GLOBAL_INPUT").as_deref() == Ok("1") {
        eprintln!("[input] disabled by WUWA_NO_GLOBAL_INPUT=1");
        return;
    }
    let a = app.clone();
    let trace = if cfg!(debug_assertions) { std::env::var("WUWA_INPUT_TRACE").ok() } else { None };
    let sink: Sink = Arc::new(move |ev, payload| {
        if let Some(path) = &trace { // debug builds only: counting sink for synthetic-device tests
            use std::io::Write;
            let shown = match &payload { // never record typed keys: only F13..F24 (test keys) are named
                Value::String(k) if !matches!(k.as_str(), "F13" | "F14") => "<redacted>".into(),
                v => v.to_string(),
            };
            if let Ok(mut f) = OpenOptions::new().append(true).create(true).open(path) { let _ = writeln!(f, "{} {ev} {shown}", std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis()).unwrap_or(0)); }
        }
        let _ = a.emit(ev, payload);
    });

    // Buttons seen by the DOM shim (inside the pet's hit regions) already reach the frontend as
    // `mouse-button` with a precise position/memberId; remember them so evdev does not double count.
    let dom: Arc<Mutex<Vec<(Instant, String, String)>>> = Arc::default();
    for event in ["mouse-button", "cat-panel-action"] {
        let dom = dom.clone();
        app.listen(event, move |e| {
            let Ok(p) = serde_json::from_str::<Value>(e.payload()) else { return };
            if let Some((button, action)) = delivered_button(event, &p) {
                if let Ok(mut d) = dom.lock() { d.push((Instant::now(), button.into(), action.into())); }
            }
        });
    }

    let hotkeys = Arc::new(Mutex::new(Hotkeys::default()));
    let raw_hotkeys = hotkeys.clone();
    let direct = sink.clone();
    let raw: Sink = Arc::new(move |event, payload| {
        if let Some(index) = hotkey_index(payload.as_str().unwrap_or("")) {
            if matches!(event, "key-press" | "key-release") {
                let events = raw_hotkeys.lock().map(|mut h| h.raw(index, event == "key-press")).unwrap_or_default();
                for event in events { direct(event, payload.clone()); }
                return;
            }
        }
        direct(event, payload);
    });
    let _ = thread::Builder::new().name("wuwa-evdev".into()).spawn(move || evdev_loop(raw, dom));
    let scale = Arc::new(AtomicU64::new(1.0f64.to_bits()));
    if let Some(w) = app.get_webview_window("main") {
        if let Ok(s) = w.scale_factor() { update_pointer_scale(&scale, s); }
        let scale = scale.clone();
        w.on_window_event(move |event| {
            if let tauri::WindowEvent::ScaleFactorChanged { scale_factor, .. } = event {
                update_pointer_scale(&scale, *scale_factor);
            }
        });
    }
    let updates = scale.clone();
    app.listen_any("cat-main-monitor-origin-changed", move |event| {
        if let Ok(payload) = serde_json::from_str::<Value>(event.payload()) {
            if let Some(s) = payload["monitor"]["scaleFactor"].as_f64() { update_pointer_scale(&updates, s); }
        }
    });
    let _ = thread::Builder::new().name("wuwa-pointer".into()).spawn(move || pointer_start(scale, sink, hotkeys));
}

static KWIN_BRIDGE_ACTIVE: AtomicBool = AtomicBool::new(false);

const HOTKEYS: [(&str, &str, i32); 3] = [
    ("F7", "wuwatappo-game-mode", 0x01000036),
    ("F8", "wuwatappo-switch-monitor", 0x01000037),
    ("F9", "wuwatappo-toggle-pet", 0x01000038),
];

fn hotkey_index(key: &str) -> Option<usize> { HOTKEYS.iter().position(|k| k.0 == key) }

#[derive(Default)]
struct Hotkeys {
    owned: u8,
    down: [bool; 3],
}

impl Hotkeys {
    fn raw(&mut self, index: usize, down: bool) -> Vec<&'static str> {
        if self.down[index] == down { return vec![]; }
        self.down[index] = down;
        vec![if down { "key-press" } else { "key-release" }]
    }

    fn shortcut(&mut self, index: usize, down: bool) -> Vec<&'static str> {
        if index >= HOTKEYS.len() || (down && self.owned & (1 << index) == 0) { return vec![]; }
        self.raw(index, down)
    }
}

fn update_pointer_scale(scale: &AtomicU64, value: f64) {
    if value.is_finite() && value > 0.0 { scale.store(value.to_bits(), Ordering::Relaxed); }
}

// The panel counts presses itself; only the main shim emits positioned mouse-button events.
fn delivered_button<'a>(event: &str, p: &'a Value) -> Option<(&'a str, &'a str)> {
    match event {
        "cat-panel-action" if p["type"] == "pet-clap" => Some(("left", "press")),
        "mouse-button" if p["native"] != true => Some((p["button"].as_str()?, p["action"].as_str()?)),
        _ => None,
    }
}

fn take_delivered_button(d: &mut Vec<(Instant, String, String)>, t: Instant, button: &str, action: &str, wait: Duration) -> bool {
    d.retain(|x| x.0.elapsed() < Duration::from_secs(1));
    let hit = d.iter().position(|x| x.1 == button && x.2 == action
        && x.0 + Duration::from_millis(20) >= t && x.0 <= t + wait);
    hit.map(|i| d.remove(i)).is_some()
}

// ---------------------------------------------------------------------------------------------
// evdev
// ---------------------------------------------------------------------------------------------

const EV_SYN: u32 = 0;
const EV_KEY: u32 = 1;
const EV_ABS: u32 = 3;
const SYN_DROPPED: u16 = 3;
const KEY_A: usize = 30;
const BTN_LEFT: usize = 0x110;
const KEY_BITS: usize = 0x300;
const ABS_MT_SLOT: usize = 0x2f;
const ABS_MT_POSITION_X: usize = 0x35;
// shortcut: deduplication waits 50 ms for DOM delivery; use shared event routing if lag causes double counts.
const DOM_WAIT: Duration = Duration::from_millis(50);
// libinput releases a tapped button ~180 ms after the lift, so the compositor's DOM release lags our tap.
const TAP_WAIT: Duration = Duration::from_millis(260);
// The frontend shows the "pressed" pose between press and release; a physical click holds ~100 ms, a tap emitted
// back to back never showed it (count rose, no animation).
const TAP_HOLD: Duration = Duration::from_millis(100);

enum Btn {
    Physical(Instant, &'static str, &'static str),
    Tap(Instant, &'static str),
}

/// _IOC(_IOC_READ, 'E', nr, len)
fn ioc_read(nr: u32, len: u32) -> libc::c_ulong {
    ((2u32 << 30) | (len << 16) | ((b'E' as u32) << 8) | nr) as libc::c_ulong
}

fn get_bits(fd: i32, ev: u32, buf: &mut [u8]) -> bool {
    // SAFETY: buf outlives the call and is as large as the length encoded in the request.
    unsafe { libc::ioctl(fd, ioc_read(0x20 + ev, buf.len() as u32), buf.as_mut_ptr()) >= 0 }
}

fn bit(buf: &[u8], b: usize) -> bool {
    buf.get(b / 8).is_some_and(|v| v & (1 << (b % 8)) != 0)
}

fn has_bit(fd: i32, ev: u32, b: usize) -> bool {
    let mut buf = [0u8; 96];
    get_bits(fd, ev, &mut buf) && bit(&buf, b)
}

/// (min, max, resolution) of an absolute axis.
fn abs_info(fd: i32, axis: usize) -> Option<(i32, i32, i32)> {
    let mut a = [0i32; 6]; // struct input_absinfo: value, min, max, fuzz, flat, resolution
    // SAFETY: a is 24 bytes, the size encoded in the request.
    let r = unsafe { libc::ioctl(fd, ioc_read(0x40 + axis as u32, 24), a.as_mut_ptr()) };
    (r >= 0).then_some((a[1], a[2], a[5]))
}

/// Kernel's current pressed-key bitmap (EVIOCGKEY); used to resynchronise after SYN_DROPPED.
fn read_key_state(fd: i32) -> Option<[u8; KEY_BITS / 8]> {
    let mut buf = [0u8; KEY_BITS / 8];
    // SAFETY: buf is as large as the length encoded in the request.
    (unsafe { libc::ioctl(fd, ioc_read(0x18, buf.len() as u32), buf.as_mut_ptr()) } >= 0).then_some(buf)
}

/// Pressed keys of one device. Guarantees press/release pairing: autorepeat (value 2) and duplicate
/// presses are ignored, a release for a key we never saw pressed is ignored, and `drain`/`resync`
/// produce the releases a lost event or an unplugged device would otherwise leave stuck in the frontend.
struct KeyState([u8; KEY_BITS / 8]);

impl Default for KeyState {
    fn default() -> Self { KeyState([0; KEY_BITS / 8]) }
}

impl KeyState {
    /// Returns Some(true/false) for a real press/release transition, None otherwise.
    fn apply(&mut self, code: u16, value: i32) -> Option<bool> {
        let c = code as usize;
        if c >= KEY_BITS || value == 2 { return None; }
        let down = self.0[c / 8] & (1 << (c % 8)) != 0;
        match (value, down) {
            (1, false) => { self.0[c / 8] |= 1 << (c % 8); Some(true) }
            (0, true) => { self.0[c / 8] &= !(1 << (c % 8)); Some(false) }
            _ => None,
        }
    }

    fn codes(&self) -> impl Iterator<Item = u16> + '_ {
        (0..KEY_BITS).filter(|&c| bit(&self.0, c)).map(|c| c as u16)
    }

    /// Adopt the kernel's truth; returns the (code, down) transitions that were missed.
    fn resync(&mut self, truth: &[u8; KEY_BITS / 8]) -> Vec<(u16, bool)> {
        let out = (0..KEY_BITS)
            .filter(|&c| bit(&self.0, c) != bit(truth, c))
            .map(|c| (c as u16, bit(truth, c)))
            .collect();
        self.0 = *truth;
        out
    }

    /// Releases for everything still held (device removed).
    fn drain(&mut self) -> Vec<u16> {
        let v = self.codes().collect();
        self.0 = [0; KEY_BITS / 8];
        v
    }
}

// ---------------------------------------------------------------------------------------------
// touchpad tap detection (multitouch protocol B)
// ---------------------------------------------------------------------------------------------
// libinput synthesises tap-to-click in userspace, so the raw device never reports BTN_LEFT for a tap.
// Semantics follow https://wayland.freedesktop.org/libinput/doc/latest/tapping.html : 1/2/3 fingers ->
// left/right/middle, a finger that moves too far or stays too long is not a tap, a physical button
// press cancels the tap, and palms (edge zones, MT_TOOL_PALM) are ignored
// (https://wayland.freedesktop.org/libinput/doc/latest/palm-detection.html). Numeric limits are
// libinput's defaults, which its documentation leaves implementation-defined: 180 ms, 1.3 mm.
// shortcut: no pressure/size thumb detection and no single-touch (ABS_X) touchpads; add if a device needs it.

const TAP_TIMEOUT_MS: u64 = 180;
const MAX_SLOTS: usize = 16;
const ABS_MT_TRACKING_ID: u16 = 0x39;
const ABS_MT_POSITION_Y: u16 = 0x36;
const ABS_MT_TOOL_TYPE: u16 = 0x37;
const MT_TOOL_PALM: i32 = 2;

#[derive(Clone, Copy, Default)]
struct TapCfg {
    move_x: i32, // libinput 1.31: 1.3 mm in device units
    move_y: i32,
    edge_lo: i32, // x below/above these is the palm edge zone
    edge_hi: i32,
    lmr: bool, // effective per-device tap button map
}

#[derive(Clone, Copy)]
struct Slot { id: i32, x: i32, y: i32, sx: i32, sy: i32, hx: bool, hy: bool, seen: bool, palm: bool, counted: bool }
const NO_SLOT: Slot = Slot { id: -1, x: 0, y: 0, sx: 0, sy: 0, hx: false, hy: false, seen: false, palm: false, counted: false };

struct Tap {
    cfg: TapCfg,
    slots: [Slot; MAX_SLOTS],
    cur: usize,
    active: bool,
    cancelled: bool,
    start_ms: u64,
    max_fingers: u8,
    buttons: u8, // physical buttons currently down on this device
}

impl Tap {
    fn new(cfg: TapCfg) -> Self {
        Tap { cfg, slots: [NO_SLOT; MAX_SLOTS], cur: 0, active: false, cancelled: false, start_ms: 0, max_fingers: 0, buttons: 0 }
    }

    /// Forget the sequence in progress (SYN_DROPPED): a tap we cannot see completely is not a tap.
    fn reset(&mut self) {
        *self = Tap::new(self.cfg);
    }

    /// Feed one input event. Returns the button index (0 left, 1 right, 2 middle) when a tap completes.
    fn event(&mut self, t_ms: u64, ty: u32, code: u16, value: i32) -> Option<usize> {
        match ty {
            EV_ABS => {
                if code as usize == ABS_MT_SLOT { self.cur = (value.max(0) as usize).min(MAX_SLOTS - 1); return None; }
                let s = &mut self.slots[self.cur];
                match code {
                    // MT slot axes are sticky: a reused contact need not resend unchanged X/Y.
                    ABS_MT_TRACKING_ID => *s = Slot { id: value, x: s.x, y: s.y, hx: s.hx, hy: s.hy, ..NO_SLOT },
                    c if c as usize == ABS_MT_POSITION_X => { s.x = value; s.hx = true; }
                    ABS_MT_POSITION_Y => { s.y = value; s.hy = true; }
                    ABS_MT_TOOL_TYPE => if value == MT_TOOL_PALM { s.palm = true },
                    _ => {}
                }
                None
            }
            EV_KEY if (0x110..=0x112).contains(&code) => {
                if value == 1 { self.buttons += 1; self.cancelled = true; }
                else if value == 0 { self.buttons = self.buttons.saturating_sub(1); }
                None
            }
            EV_SYN if code == 0 => self.sync(t_ms),
            _ => None,
        }
    }

    fn sync(&mut self, t: u64) -> Option<usize> {
        let cfg = self.cfg;
        let mut live = 0u8;
        for s in self.slots.iter_mut().filter(|s| s.id >= 0) {
            if !s.seen && s.hx && s.hy {
                s.seen = true;
                (s.sx, s.sy) = (s.x, s.y);
                if s.x < cfg.edge_lo || s.x > cfg.edge_hi { s.palm = true; }
            }
            if !s.seen { continue; }
            if s.palm { if s.counted { self.cancelled = true; } continue; } // a finger that turns into a palm spoils the tap
            s.counted = true;
            if (s.x - s.sx).abs() > cfg.move_x || (s.y - s.sy).abs() > cfg.move_y { self.cancelled = true; }
            live += 1;
        }
        if !self.active && live > 0 {
            self.active = true;
            self.start_ms = t;
            self.max_fingers = 0;
            self.cancelled = self.buttons > 0;
        }
        if !self.active { return None; }
        self.max_fingers = self.max_fingers.max(live);
        if live > 3 || t.saturating_sub(self.start_ms) > TAP_TIMEOUT_MS { self.cancelled = true; }
        if live > 0 { return None; }
        self.active = false;
        (!self.cancelled && (1..=3).contains(&self.max_fingers)).then(|| {
            let map = if self.cfg.lmr { [0, 2, 1] } else { [0, 1, 2] };
            map[self.max_fingers as usize - 1]
        })
    }
}

/// Touchpad = multitouch pointer device that is not a touchscreen (INPUT_PROP_DIRECT).
fn tap_config(fd: i32, path: &std::path::Path) -> Option<TapCfg> {
    if std::env::var("WUWA_NO_TAP").as_deref() == Ok("1") { return None; }
    let mut abs = [0u8; 8];
    let mut prop = [0u8; 8];
    // SAFETY: prop is 8 bytes, the length encoded in the request.
    unsafe { libc::ioctl(fd, ioc_read(0x09, 8), prop.as_mut_ptr()) };
    let touchpad = get_bits(fd, EV_ABS, &mut abs) && bit(&abs, ABS_MT_SLOT) && bit(&abs, ABS_MT_POSITION_X)
        && has_bit(fd, EV_KEY, 0x145) /* BTN_TOOL_FINGER */ && !bit(&prop, 1);
    if !touchpad { return None; }
    let (enabled, lmr) = kwin_tap_settings(path).unwrap_or_else(|| (tap_enabled_in_settings(), false));
    if !enabled { return None; }
    let (xlo, xhi, xr) = abs_info(fd, ABS_MT_POSITION_X)?;
    let (ylo, yhi, yr) = abs_info(fd, ABS_MT_POSITION_X + 1)?;
    let mm = |lo: i32, hi: i32, res: i32| tap_move_threshold(lo, hi, res);
    let edge = (xhi - xlo) / 20; // libinput: palms concentrate in the outer ~5 %
    Some(TapCfg { move_x: mm(xlo, xhi, xr), move_y: mm(ylo, yhi, yr), edge_lo: xlo + edge, edge_hi: xhi - edge, lmr })
}

fn tap_move_threshold(lo: i32, hi: i32, resolution: i32) -> i32 {
    // shortcut: axes lacking resolution assume a 100 mm pad; use device quirks if one needs calibration.
    (if resolution > 0 { resolution * 13 / 10 } else { (hi - lo) * 13 / 1000 }).max(1)
}

/// KWin exposes effective settings per event device, including defaults absent from kcminputrc.
fn kwin_tap_settings(path: &std::path::Path) -> Option<(bool, bool)> {
    let name = path.file_name()?.to_str()?;
    let conn = zbus::blocking::connection::Builder::session().ok()?.method_timeout(Duration::from_secs(2)).build().ok()?;
    let path = format!("/org/kde/KWin/InputDevice/{name}");
    let device = zbus::blocking::Proxy::new(&conn, "org.kde.KWin", path, "org.kde.KWin.InputDevice").ok()?;
    Some((device.get_property("tapToClick").ok()?, device.get_property("lmrTapButtonMap").ok()?))
}

/// KDE writes the user's touchpad choice to kcminputrc; libinput itself defaults to tapping off on most
/// pads. Unset or unreadable -> assume on. Another desktop's setting is not read (documented limit).
fn tap_enabled_in_settings() -> bool {
    let Some(home) = std::env::var_os("HOME") else { return true };
    let text = std::fs::read_to_string(PathBuf::from(home).join(".config/kcminputrc")).unwrap_or_default();
    !(text.contains("TapToClick=false") && !text.contains("TapToClick=true"))
}

struct Dev {
    path: PathBuf,
    file: File,
    keys: KeyState,
    tap: Option<Tap>,
    dropped: bool, // inside a SYN_DROPPED gap: discard until the next SYN_REPORT
}

/// Keyboards (have KEY_A) and mice/touchpads (have BTN_LEFT). Others are ignored.
fn open_if_wanted(path: &PathBuf) -> Option<Dev> {
    let file = OpenOptions::new().read(true).custom_flags(libc::O_NONBLOCK | libc::O_CLOEXEC).open(path).ok()?;
    let fd = file.as_raw_fd();
    if !(has_bit(fd, 0, EV_KEY as usize) && (has_bit(fd, EV_KEY, KEY_A) || has_bit(fd, EV_KEY, BTN_LEFT))) { return None; }
    let mut keys = KeyState::default();
    // Keys already held at open time are known state: a later release is then paired, not dropped.
    if let Some(t) = read_key_state(fd) { keys.0 = t; }
    Some(Dev { path: path.clone(), file, keys, tap: tap_config(fd, path).map(Tap::new), dropped: false })
}

fn scan(devs: &mut Vec<Dev>, rejected: &mut std::collections::HashSet<PathBuf>) {
    let Ok(rd) = std::fs::read_dir("/dev/input") else { return };
    let mut present = std::collections::HashSet::new();
    for e in rd.flatten() {
        let p = e.path();
        if !p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.starts_with("event")) { continue; }
        present.insert(p.clone());
        if rejected.contains(&p) || devs.iter().any(|d| d.path == p) { continue; }
        match open_if_wanted(&p) {
            Some(d) => devs.push(d),
            None => { rejected.insert(p); } // not a keyboard/pointer: do not reopen every scan
        }
    }
    rejected.retain(|p| present.contains(p)); // a replugged device reusing the name is probed again
}

fn button_name(code: u16) -> Option<&'static str> {
    match code { 0x110 => Some("left"), 0x111 => Some("right"), 0x112 => Some("middle"), _ => None }
}

fn emit_key(sink: &Sink, tx: &mpsc::Sender<Btn>, code: u16, down: bool) {
    let action = if down { "press" } else { "release" };
    if let Some(b) = button_name(code) {
        let _ = tx.send(Btn::Physical(Instant::now(), b, action));
    } else if code < 0x100 {
        sink(if down { "key-press" } else { "key-release" }, Value::String(key_name(code)));
    } // other BTN_* (gamepad, touch tools) and exotic keys: not forwarded
}

/// One event from `d`; the `dropped`/resync protocol follows
/// https://www.kernel.org/doc/html/latest/input/event-codes.html#ev-syn .
fn handle_event(d: &mut Dev, sink: &Sink, tx: &mpsc::Sender<Btn>, ev: &libc::input_event) {
    let (ty, code, value) = (ev.type_ as u32, ev.code, ev.value);
    if ty == EV_SYN && code == SYN_DROPPED {
        d.dropped = true;
        return;
    }
    if d.dropped {
        if ty != EV_SYN || code != 0 { return; }
        d.dropped = false;
        if let Some(t) = read_key_state(d.file.as_raw_fd()) {
            for (c, down) in d.keys.resync(&t) { emit_key(sink, tx, c, down); }
        }
        if let Some(t) = d.tap.as_mut() { t.reset(); }
        return;
    }
    let ms = ev.time.tv_sec as u64 * 1000 + ev.time.tv_usec as u64 / 1000;
    if let Some(t) = d.tap.as_mut() {
        if let Some(i) = t.event(ms, ty, code, value) {
            let b = ["left", "right", "middle"][i];
            let now = Instant::now();
            let _ = tx.send(Btn::Tap(now, b));
        }
    }
    if ty == EV_KEY {
        if let Some(down) = d.keys.apply(code, value) { emit_key(sink, tx, code, down); }
    }
}

/// A removed device can no longer report releases.
fn release_all(d: &mut Dev, sink: &Sink, tx: &mpsc::Sender<Btn>) {
    for c in d.keys.drain() { emit_key(sink, tx, c, false); }
}

fn deliver_button(event: Btn, sink: &Sink, dom: &Mutex<Vec<(Instant, String, String)>>) {
    let (t, button, action, wait, tap) = match event {
        Btn::Physical(t, b, a) => (t, b, a, DOM_WAIT, false),
        Btn::Tap(t, b) => (t, b, "press", TAP_WAIT, true),
    };
    if let Some(w) = (t + wait).checked_duration_since(Instant::now()) { thread::sleep(w); }
    let seen = dom.lock().ok().is_some_and(|mut d| take_delivered_button(&mut d, t, button, action, wait));
    // A delivered tap press owns its eventual DOM release, including a long tap-and-drag.
    if !seen {
        sink("mouse-button", json!({"button": button, "action": action, "native": true}));
        if tap {
            thread::sleep(TAP_HOLD);
            sink("mouse-button", json!({"button": button, "action": "release", "native": true}));
        }
    }
}

fn evdev_loop(sink: Sink, dom: Arc<Mutex<Vec<(Instant, String, String)>>>) {
    let mut devs: Vec<Dev> = Vec::new();
    let mut rejected = std::collections::HashSet::new();
    scan(&mut devs, &mut rejected);
    if devs.is_empty() {
        eprintln!("[input] no readable /dev/input/event* keyboard or mouse; global input disabled");
        return;
    }
    eprintln!("[input] global input active on {} device(s)", devs.len());

    // Buttons are emitted by a worker after their wait unless the DOM shim already reported them.
    let (tx, rx) = mpsc::channel::<Btn>();
    {
        let sink = sink.clone();
        let _ = thread::Builder::new().name("wuwa-btn".into()).spawn(move || {
            for event in rx { deliver_button(event, &sink, &dom); }
        });
    }

    let mut last_scan = Instant::now();
    // SAFETY: input_event is plain old data.
    let mut buf: [libc::input_event; 64] = unsafe { std::mem::zeroed() };
    let sz = std::mem::size_of::<libc::input_event>();
    loop {
        let mut pfds: Vec<libc::pollfd> =
            devs.iter().map(|d| libc::pollfd { fd: d.file.as_raw_fd(), events: libc::POLLIN, revents: 0 }).collect();
        // SAFETY: pfds is a valid slice of pollfd. Wakes only for input or the 3 s hotplug rescan.
        unsafe { libc::poll(pfds.as_mut_ptr(), pfds.len() as libc::nfds_t, 3000) };
        let mut dead = Vec::new();
        for (i, p) in pfds.iter().enumerate() {
            if p.revents & libc::POLLIN != 0 {
                loop {
                    // SAFETY: read() writes at most size_of_val(buf) bytes into buf.
                    let n = unsafe { libc::read(p.fd, buf.as_mut_ptr() as *mut libc::c_void, sz * buf.len()) };
                    if n < 0 {
                        if std::io::Error::last_os_error().raw_os_error() != Some(libc::EAGAIN) { dead.push(i); } // ENODEV: unplugged
                        break;
                    }
                    if n == 0 { break; }
                    for ev in &buf[..n as usize / sz] { handle_event(&mut devs[i], &sink, &tx, ev); }
                }
            }
            if p.revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 && !dead.contains(&i) { dead.push(i); }
        }
        for i in dead.into_iter().rev() {
            let mut d = devs.remove(i);
            release_all(&mut d, &sink, &tx);
        }
        if last_scan.elapsed() > Duration::from_secs(3) {
            scan(&mut devs, &mut rejected); // hotplug
            last_scan = Instant::now();
        }
    }
}


/// Linux evdev key code -> name the frontend/original backend uses (rdev `Key` Debug names).
/// Only F5/F7/F8/F9 are compared by the frontend; all other names merely have to be stable per key.
pub fn key_name(code: u16) -> String {
    let s = match code {
        1 => "Escape",
        2 => "Num1", 3 => "Num2", 4 => "Num3", 5 => "Num4", 6 => "Num5",
        7 => "Num6", 8 => "Num7", 9 => "Num8", 10 => "Num9", 11 => "Num0",
        12 => "Minus", 13 => "Equal", 14 => "Backspace", 15 => "Tab",
        16 => "KeyQ", 17 => "KeyW", 18 => "KeyE", 19 => "KeyR", 20 => "KeyT",
        21 => "KeyY", 22 => "KeyU", 23 => "KeyI", 24 => "KeyO", 25 => "KeyP",
        26 => "LeftBracket", 27 => "RightBracket", 28 => "Return", 29 => "ControlLeft",
        30 => "KeyA", 31 => "KeyS", 32 => "KeyD", 33 => "KeyF", 34 => "KeyG",
        35 => "KeyH", 36 => "KeyJ", 37 => "KeyK", 38 => "KeyL",
        39 => "SemiColon", 40 => "Quote", 41 => "BackQuote", 42 => "ShiftLeft", 43 => "BackSlash",
        44 => "KeyZ", 45 => "KeyX", 46 => "KeyC", 47 => "KeyV", 48 => "KeyB", 49 => "KeyN", 50 => "KeyM",
        51 => "Comma", 52 => "Dot", 53 => "Slash", 54 => "ShiftRight", 55 => "KpMultiply", 56 => "Alt",
        57 => "Space", 58 => "CapsLock",
        59 => "F1", 60 => "F2", 61 => "F3", 62 => "F4", 63 => "F5",
        64 => "F6", 65 => "F7", 66 => "F8", 67 => "F9", 68 => "F10",
        69 => "NumLock", 70 => "ScrollLock",
        71 => "Kp7", 72 => "Kp8", 73 => "Kp9", 74 => "KpMinus",
        75 => "Kp4", 76 => "Kp5", 77 => "Kp6", 78 => "KpPlus",
        79 => "Kp1", 80 => "Kp2", 81 => "Kp3", 82 => "Kp0", 83 => "KpDecimal",
        86 => "IntlBackslash", 87 => "F11", 88 => "F12",
        96 => "KpReturn", 97 => "ControlRight", 98 => "KpDivide", 99 => "PrintScreen", 100 => "AltGr",
        102 => "Home", 103 => "UpArrow", 104 => "PageUp", 105 => "LeftArrow", 106 => "RightArrow",
        107 => "End", 108 => "DownArrow", 109 => "PageDown", 110 => "Insert", 111 => "Delete",
        113 => "VolumeMute", 114 => "VolumeDown", 115 => "VolumeUp", 117 => "KpEqual", 119 => "Pause",
        125 => "MetaLeft", 126 => "MetaRight", 127 => "Apps",
        183 => "F13", 184 => "F14", 185 => "F15", 186 => "F16", 187 => "F17", 188 => "F18",
        189 => "F19", 190 => "F20", 191 => "F21", 192 => "F22", 193 => "F23", 194 => "F24",
        _ => return format!("Unknown({code})"),
    };
    s.to_string()
}

// ---------------------------------------------------------------------------------------------
// absolute pointer position
// ---------------------------------------------------------------------------------------------

fn pointer_start(scale: Arc<AtomicU64>, sink: Sink, hotkeys: Arc<Mutex<Hotkeys>>) {
    let x11 = std::env::var("XDG_SESSION_TYPE").as_deref() == Ok("x11");
    if x11 {
        x11_loop(sink);
        return;
    }
    if let Err(e) = kwin_start(scale, sink, hotkeys) {
        // Not KDE (or scripting disabled): no global position; DOM-derived moves still work in hit regions.
        eprintln!("[input] absolute pointer position unavailable (KWin script: {e})");
    }
}

/// Runs in KWin: reports workspace.cursorPos in logical px; Rust applies the WebKit/GDK scale.
/// (plus a 1 s heartbeat) over D-Bus. Stops itself ~4 s after the app stops acknowledging calls,
/// so a crashed app leaves no busy script behind.
const KWIN_JS: &str = r#"
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
"#;

struct Ptr {
    scale: Arc<AtomicU64>,
    sink: Sink,
    last: (i32, i32),
    hotkeys: Arc<Mutex<Hotkeys>>,
}

#[zbus::interface(name = "org.wuwatappo.Pointer")]
impl Ptr {
    fn hotkey(&mut self, index: i32) {
        let index = index as usize;
        let events = self.hotkeys.lock().map(|mut h| h.shortcut(index, true)).unwrap_or_default();
        if let Some((key, _, _)) = HOTKEYS.get(index) {
            for event in events { (self.sink)(event, Value::String((*key).into())); }
        }
    }

    fn pos(&mut self, x: i32, y: i32) {
        let scale = f64::from_bits(self.scale.load(Ordering::Relaxed));
        let (x, y) = pointer_pixels(x, y, scale);
        if (x, y) != self.last {
            self.last = (x, y);
            (self.sink)("mouse-move", json!({"x": x, "y": y}));
        }
    }
}

fn pointer_pixels(x: i32, y: i32, scale: f64) -> (i32, i32) {
    ((x as f64 * scale).round() as i32, (y as f64 * scale).round() as i32)
}

fn kwin_start(scale: Arc<AtomicU64>, sink: Sink, hotkeys: Arc<Mutex<Hotkeys>>) -> Result<(), Box<dyn std::error::Error>> {
    use zbus::blocking::{connection::Builder, Proxy};
    let dir = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".into());
    let path = format!("{dir}/wuwatappo-pointer.js");
    let conn = Builder::session()?
        .method_timeout(Duration::from_secs(2))
        .name("org.wuwatappo.Pointer")?
        .serve_at("/Pointer", Ptr { scale, sink: sink.clone(), last: (i32::MIN, i32::MIN), hotkeys: hotkeys.clone() })?
        .build()?;
    let accel = Proxy::new(&conn, "org.kde.kglobalaccel", "/kglobalaccel", "org.kde.KGlobalAccel").ok();
    let releases_ready = shortcut_releases(&conn, hotkeys.clone(), sink);
    let allowed: Vec<_> = HOTKEYS.iter().enumerate().filter_map(|(index, (key, action, code))| {
        if !releases_ready { return None; }
        let owner: Vec<String> = accel.as_ref()?.call("action", code).ok()?;
        (owner.is_empty() || is_our_shortcut(&owner, action)).then(|| json!([key, action, index]))
    }).collect();
    std::fs::write(&path, KWIN_JS.replace("__WW_HOTKEYS__", &serde_json::to_string(&allowed)?))?;
    let scripting = Proxy::new(&conn, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting")?;
    let _: bool = scripting.call("unloadScript", &"wuwatappo-pointer")?; // drop a stale copy from a crashed run
    let id: i32 = scripting.call("loadScript", &(path.as_str(), "wuwatappo-pointer"))?;
    if id < 0 {
        return Err("KWin refused loadScript".into());
    }
    let script = Proxy::new(&conn, "org.kde.KWin", format!("/Scripting/Script{id}"), "org.kde.kwin.Script")?;
    script.call::<_, _, ()>("run", &())?;
    KWIN_BRIDGE_ACTIVE.store(true, Ordering::Relaxed);
    eprintln!("[input] pointer position via KWin script");
    loop {
        let mut mask = 0;
        if let Some(accel) = &accel {
            for (index, (_, action, _)) in HOTKEYS.iter().enumerate() {
                let keys: Vec<i32> = accel.call("shortcut", &vec!["kwin", action, "", ""]).unwrap_or_default();
                let assigned = keys.iter().any(|code| {
                    let owner: Vec<String> = accel.call("action", code).unwrap_or_default();
                    is_our_shortcut(&owner, action)
                });
                if assigned { mask |= 1 << index; }
            }
        }
        if let Ok(mut h) = hotkeys.lock() { h.owned = mask; }
        thread::sleep(Duration::from_secs(5)); // detects user reassignment; no device grab
    }
}

fn shortcut_releases(conn: &zbus::blocking::Connection, hotkeys: Arc<Mutex<Hotkeys>>, sink: Sink) -> bool {
    let conn = conn.clone();
    let (tx, rx) = mpsc::channel();
    let _ = thread::Builder::new().name("wuwa-shortcuts".into()).spawn(move || {
        let Ok(proxy) = zbus::blocking::Proxy::new(&conn, "org.kde.kglobalaccel", "/component/kwin", "org.kde.kglobalaccel.Component") else { let _ = tx.send(false); return; };
        let Ok(releases) = proxy.receive_signal("globalShortcutReleased") else { let _ = tx.send(false); return; };
        let _ = tx.send(true);
        for message in releases {
            let Ok((component, action, _)) = message.body().deserialize::<(String, String, i64)>() else { continue };
            if component != "kwin" { continue; }
            let Some(index) = HOTKEYS.iter().position(|k| k.1 == action) else { continue };
            let events = hotkeys.lock().map(|mut h| h.shortcut(index, false)).unwrap_or_default();
            for event in events { sink(event, Value::String(HOTKEYS[index].0.into())); }
        }
    });
    rx.recv_timeout(Duration::from_secs(2)).unwrap_or(false)
}

fn is_our_shortcut(owner: &[String], action: &str) -> bool {
    owner.first().is_some_and(|v| v == "kwin") && owner.get(1).is_some_and(|v| v == action)
}

/// Called at orderly exit; crash cleanup is the KWin script's heartbeat lease.
pub fn stop() {
    if !KWIN_BRIDGE_ACTIVE.swap(false, Ordering::Relaxed) { return; }
    let conn = zbus::blocking::connection::Builder::session()
        .and_then(|b| b.method_timeout(Duration::from_secs(2)).build());
    if let Ok(conn) = conn {
        if let Ok(proxy) = zbus::blocking::Proxy::new(&conn, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting") {
            let _ = proxy.call::<_, _, bool>("unloadScript", &"wuwatappo-pointer");
        }
    }
}

/// X11: XQueryPointer through a dlopen'd libX11 (no extra crate). Only used for real X11 sessions;
/// under Wayland/XWayland it returns stale data.
fn x11_loop(sink: Sink) {
    use std::ffi::{c_char, c_int, c_uint, c_ulong, c_void};
    type Open = unsafe extern "C" fn(*const c_char) -> *mut c_void;
    type Root = unsafe extern "C" fn(*mut c_void) -> c_ulong;
    type Query = unsafe extern "C" fn(
        *mut c_void, c_ulong, *mut c_ulong, *mut c_ulong, *mut c_int, *mut c_int, *mut c_int, *mut c_int, *mut c_uint,
    ) -> c_int;
    // SAFETY: plain dlopen/dlsym of well-known libX11 symbols with their documented C signatures.
    unsafe {
        let lib = libc::dlopen(c"libX11.so.6".as_ptr(), libc::RTLD_NOW);
        let sym = |n: &std::ffi::CStr| if lib.is_null() { std::ptr::null_mut() } else { libc::dlsym(lib, n.as_ptr()) };
        let (o, r, q) = (sym(c"XOpenDisplay"), sym(c"XDefaultRootWindow"), sym(c"XQueryPointer"));
        if o.is_null() || r.is_null() || q.is_null() {
            eprintln!("[input] libX11 unavailable; no absolute pointer position");
            return;
        }
        let (open, root, query): (Open, Root, Query) =
            (std::mem::transmute(o), std::mem::transmute(r), std::mem::transmute(q));
        let dpy = open(std::ptr::null());
        if dpy.is_null() {
            eprintln!("[input] cannot open X display; no absolute pointer position");
            return;
        }
        let w = root(dpy);
        let mut last = (i32::MIN, i32::MIN);
        loop {
            let (mut rr, mut cc, mut rx, mut ry, mut wx, mut wy, mut m) = (0, 0, 0, 0, 0, 0, 0);
            if query(dpy, w, &mut rr, &mut cc, &mut rx, &mut ry, &mut wx, &mut wy, &mut m) != 0 && (rx, ry) != last {
                last = (rx, ry);
                sink("mouse-move", json!({"x": rx, "y": ry}));
            }
            thread::sleep(Duration::from_millis(16));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::key_name;

    #[test]
    fn hotkeys_deliver_once_in_either_native_source_order() {
        use super::*;
        let mut h = Hotkeys { owned: 0b111, ..Hotkeys::default() };
        assert_eq!(h.shortcut(0, true), ["key-press"]);
        assert!(h.raw(0, true).is_empty());
        assert_eq!(h.raw(0, false), ["key-release"]);
        assert!(h.shortcut(0, false).is_empty());
        assert_eq!(h.raw(1, true), ["key-press"]);
        assert!(h.shortcut(1, true).is_empty());
        assert_eq!(h.shortcut(1, false), ["key-release"]);
        assert!(h.raw(1, false).is_empty());
        assert!(h.shortcut(3, true).is_empty());
        h.owned = 0;
        assert!(h.shortcut(2, true).is_empty());
        assert_eq!(h.raw(2, true), ["key-press"]);
        assert_eq!(h.raw(2, false), ["key-release"]);
    }

    #[test]
    fn rapid_real_hotkey_presses_are_counted_and_release_survives_reassignment() {
        use super::*;
        let mut h = Hotkeys { owned: 0b111, ..Hotkeys::default() };
        for _ in 0..10 {
            assert_eq!(h.shortcut(0, true), ["key-press"]);
            assert!(h.raw(0, true).is_empty());
            assert_eq!(h.shortcut(0, false), ["key-release"]);
            assert!(h.raw(0, false).is_empty());
        }
        assert_eq!(h.shortcut(1, true), ["key-press"]);
        h.owned = 0;
        assert_eq!(h.shortcut(1, false), ["key-release"]);
    }

    #[test]
    fn shortcut_owner_requires_exact_component_and_action() {
        use super::*;
        assert!(is_our_shortcut(&["kwin".into(), HOTKEYS[0].1.into()], HOTKEYS[0].1));
        assert!(!is_our_shortcut(&["other".into(), HOTKEYS[0].1.into()], HOTKEYS[0].1));
        assert!(!is_our_shortcut(&["kwin".into(), "other".into()], HOTKEYS[0].1));
        assert!(!is_our_shortcut(&[], HOTKEYS[0].1));
    }

    #[test]
    #[ignore = "requires a live KWin touchpad; set WUWA_TEST_INPUT_DEVICE=eventN"]
    fn reads_effective_live_kwin_tap_settings() {
        let device = std::env::var("WUWA_TEST_INPUT_DEVICE").expect("set the touchpad event device");
        let settings = super::kwin_tap_settings(&std::path::Path::new("/dev/input").join(device));
        assert!(settings.is_some(), "KWin must expose typed per-device tap settings");
    }

    #[test]
    fn cached_pointer_scale_changes_immediately_and_rejects_invalid_updates() {
        use super::*;
        let scale = AtomicU64::new(1.0f64.to_bits());
        update_pointer_scale(&scale, 2.0);
        update_pointer_scale(&scale, 0.0);
        update_pointer_scale(&scale, f64::NAN);
        assert_eq!(pointer_pixels(800, 500, f64::from_bits(scale.load(Ordering::Relaxed))), (1600, 1000));
    }

    #[test]
    fn panel_click_consumes_one_native_press_without_hiding_release() {
        use super::*;
        let p = json!({"type":"pet-clap"});
        let (button, action) = delivered_button("cat-panel-action", &p).unwrap();
        let now = Instant::now();
        let mut delivered = vec![(now, button.into(), action.into())];
        assert!(!take_delivered_button(&mut delivered, now, "left", "release", DOM_WAIT));
        assert!(take_delivered_button(&mut delivered, now, "left", "press", DOM_WAIT));
        assert!(!take_delivered_button(&mut delivered, now, "left", "press", DOM_WAIT));
        assert!(delivered_button("mouse-button", &json!({"button":"left","action":"press","native":true})).is_none());
        assert!(delivered_button("cat-panel-action", &json!({"type":"open-bag"})).is_none());
    }

    #[test]
    fn delivered_tap_press_owns_delayed_drag_release() {
        use super::*;
        let out = Arc::new(Mutex::new(Vec::new()));
        let events = out.clone();
        let sink: Sink = Arc::new(move |_, p| events.lock().unwrap().push(p));
        let t = Instant::now() - TAP_WAIT;
        let dom = Mutex::new(vec![(t, "left".into(), "press".into())]);
        deliver_button(Btn::Tap(t, "left"), &sink, &dom);
        assert!(out.lock().unwrap().is_empty()); // no premature synthetic release
        let started = Instant::now();
        deliver_button(Btn::Tap(t, "left"), &sink, &dom);
        assert!(started.elapsed() >= TAP_HOLD); // press and release must not be back to back
        let out = out.lock().unwrap();
        assert_eq!(out.len(), 2); // a tap outside the DOM still gets one complete pair
        assert_eq!(out[0]["action"], "press");
        assert_eq!(out[1]["action"], "release");
    }

    #[test]
    fn pointer_uses_webkit_scale_even_with_fractional_output_scale() {
        assert_eq!(super::pointer_pixels(800, 500, 2.0), (1600, 1000));
        assert_eq!(super::pointer_pixels(-800, 500, 1.0), (-800, 500));
    }

    #[test]
    fn maps_frontend_hotkeys_and_common_keys() {
        // frontend compares exactly "F5"/"F7"/"F8"/"F9" (Set also has lowercase variants)
        assert_eq!(key_name(63), "F5");
        assert_eq!(key_name(65), "F7");
        assert_eq!(key_name(66), "F8");
        assert_eq!(key_name(67), "F9");
        assert_eq!(key_name(57), "Space");
        assert_eq!(key_name(30), "KeyA");
        assert_eq!(key_name(16), "KeyQ");
        assert_eq!(key_name(25), "KeyP");
        assert_eq!(key_name(44), "KeyZ");
        assert_eq!(key_name(2), "Num1");
        assert_eq!(key_name(11), "Num0");
        assert_eq!(key_name(42), "ShiftLeft");
        assert_eq!(key_name(194), "F24");
        assert_eq!(key_name(0x2fe), "Unknown(766)");
    }

    #[test]
    fn letters_are_unique() {
        let mut seen = std::collections::HashSet::new();
        for c in 1..=194u16 {
            let n = key_name(c);
            if !n.starts_with("Unknown") {
                assert!(seen.insert(n.clone()), "duplicate name {n} for code {c}");
            }
        }
    }

    // ---- key pairing -------------------------------------------------------------------------
    #[test]
    fn autorepeat_and_duplicates_never_emit_and_release_pairs() {
        use super::KeyState;
        let mut k = KeyState::default();
        assert_eq!(k.apply(111, 0), None); // release of an unseen key
        assert_eq!(k.apply(111, 1), Some(true));
        for _ in 0..50 { assert_eq!(k.apply(111, 2), None); } // held Delete
        assert_eq!(k.apply(111, 1), None);
        assert_eq!(k.apply(111, 0), Some(false));
        assert_eq!(k.apply(111, 0), None);
    }

    #[test]
    fn drain_and_resync_release_stuck_keys() {
        use super::KeyState;
        let mut k = KeyState::default();
        k.apply(111, 1); k.apply(42, 1);
        let mut truth = [0u8; 96];
        truth[30 / 8] |= 1 << (30 % 8); // KEY_A now held, Delete/Shift released while events were dropped
        let mut missed = k.resync(&truth);
        missed.sort();
        assert_eq!(missed, vec![(30, true), (42, false), (111, false)]);
        assert_eq!(k.drain(), vec![30]);
        assert!(k.drain().is_empty());
    }

    // ---- tap state machine -------------------------------------------------------------------
    use super::{Tap, TapCfg, EV_ABS, EV_KEY, EV_SYN};
    const X: u16 = 0x35; const Y: u16 = 0x36; const ID: u16 = 0x39; const SLOT: u16 = 0x2f;
    fn pad() -> Tap { Tap::new(TapCfg { move_x: 30, move_y: 30, edge_lo: 100, edge_hi: 1900, lmr: false }) }
    fn feed(t: &mut Tap, ms: u64, evs: &[(u32, u16, i32)]) -> Option<usize> {
        let mut r = None;
        for &(ty, c, v) in evs { r = r.or(t.event(ms, ty, c, v)); }
        r.or(t.event(ms, EV_SYN, 0, 0))
    }
    fn down(t: &mut Tap, ms: u64, slot: i32, id: i32, x: i32, y: i32) -> Option<usize> {
        feed(t, ms, &[(EV_ABS, SLOT, slot), (EV_ABS, ID, id), (EV_ABS, X, x), (EV_ABS, Y, y)])
    }
    fn up(t: &mut Tap, ms: u64, slot: i32) -> Option<usize> { feed(t, ms, &[(EV_ABS, SLOT, slot), (EV_ABS, ID, -1)]) }
    fn mv(t: &mut Tap, ms: u64, slot: i32, x: i32) -> Option<usize> { feed(t, ms, &[(EV_ABS, SLOT, slot), (EV_ABS, X, x)]) }

    #[test]
    fn one_two_three_finger_taps_map_to_left_right_middle() {
        let mut t = pad();
        assert_eq!(down(&mut t, 0, 0, 1, 1000, 500), None);
        assert_eq!(up(&mut t, 80, 0), Some(0));
        down(&mut t, 1000, 0, 2, 1000, 500); down(&mut t, 1010, 1, 3, 1200, 500);
        assert_eq!(up(&mut t, 1070, 0), None);
        assert_eq!(up(&mut t, 1080, 1), Some(1));
        down(&mut t, 2000, 0, 4, 900, 500); down(&mut t, 2005, 1, 5, 1100, 500); down(&mut t, 2010, 2, 6, 1300, 500);
        up(&mut t, 2050, 0); up(&mut t, 2055, 1);
        assert_eq!(up(&mut t, 2060, 2), Some(2));
    }

    #[test]
    fn tap_uses_calibrated_threshold_and_effective_lmr_mapping() {
        assert_eq!(super::tap_move_threshold(-5318, 5787, 93), 120);
        assert_eq!(super::tap_move_threshold(-7102, 157, 90), 117);
        assert_eq!(super::tap_move_threshold(0, 10000, 0), 130);
        let mut t = pad();
        t.cfg.lmr = true;
        down(&mut t, 0, 0, 1, 1000, 500); down(&mut t, 0, 1, 2, 1200, 500);
        up(&mut t, 50, 0);
        assert_eq!(up(&mut t, 60, 1), Some(2)); // two fingers -> middle
        down(&mut t, 1000, 0, 3, 900, 500); down(&mut t, 1000, 1, 4, 1100, 500); down(&mut t, 1000, 2, 5, 1300, 500);
        up(&mut t, 1050, 0); up(&mut t, 1050, 1);
        assert_eq!(up(&mut t, 1060, 2), Some(1)); // three fingers -> right
    }

    #[test]
    fn reused_slot_retains_unchanged_coordinates_for_later_taps() {
        let mut t = pad();
        down(&mut t, 0, 0, 1, 1000, 500);
        assert_eq!(up(&mut t, 80, 0), Some(0));
        // The kernel only reports changed slot axes, even across tracking IDs.
        feed(&mut t, 1000, &[(EV_ABS, ID, 2)]);
        assert_eq!(up(&mut t, 1080, 0), Some(0));
        feed(&mut t, 2000, &[(EV_ABS, ID, 3), (EV_ABS, X, 1010)]);
        assert_eq!(up(&mut t, 2080, 0), Some(0));
    }

    #[test]
    fn hold_drag_and_scroll_are_not_taps() {
        let mut t = pad();
        down(&mut t, 0, 0, 1, 1000, 500);
        assert_eq!(up(&mut t, 400, 0), None); // tap-and-hold released late
        down(&mut t, 1000, 0, 2, 1000, 500);
        mv(&mut t, 1050, 0, 1100); // drag
        assert_eq!(up(&mut t, 1100, 0), None);
        down(&mut t, 2000, 0, 3, 900, 500); down(&mut t, 2000, 1, 4, 1100, 500);
        mv(&mut t, 2040, 0, 900 + 60); mv(&mut t, 2050, 1, 1100 + 60); // two-finger scroll
        up(&mut t, 2090, 0);
        assert_eq!(up(&mut t, 2100, 1), None);
        down(&mut t, 3000, 0, 5, 1000, 500); // small jitter still taps
        mv(&mut t, 3020, 0, 1010);
        assert_eq!(up(&mut t, 3060, 0), Some(0));
    }

    #[test]
    fn palm_edge_and_tool_type_are_ignored() {
        let mut t = pad();
        down(&mut t, 0, 0, 1, 50, 500); // starts in left edge zone
        assert_eq!(up(&mut t, 50, 0), None);
        down(&mut t, 1000, 0, 2, 1000, 500);
        feed(&mut t, 1010, &[(EV_ABS, SLOT, 0), (EV_ABS, 0x37, 2)]); // firmware says palm
        assert_eq!(up(&mut t, 1050, 0), None);
        down(&mut t, 2000, 0, 3, 1000, 500); down(&mut t, 2000, 1, 4, 1950, 500); // finger + palm: one finger -> left
        up(&mut t, 2030, 1);
        assert_eq!(up(&mut t, 2050, 0), Some(0));
    }

    #[test]
    fn physical_click_cancels_tap_and_is_not_counted_twice() {
        let mut t = pad();
        down(&mut t, 0, 0, 1, 1000, 500);
        feed(&mut t, 20, &[(EV_KEY, 0x110, 1)]);
        feed(&mut t, 60, &[(EV_KEY, 0x110, 0)]);
        assert_eq!(up(&mut t, 90, 0), None); // BTN_LEFT itself is counted by the key path
        down(&mut t, 1000, 0, 2, 1000, 500); // button already down when the finger lands
        feed(&mut t, 1010, &[(EV_KEY, 0x110, 1)]);
        assert_eq!(up(&mut t, 1050, 0), None);
        feed(&mut t, 1060, &[(EV_KEY, 0x110, 0)]);
        down(&mut t, 2000, 0, 3, 1000, 500);
        assert_eq!(up(&mut t, 2050, 0), Some(0)); // later taps work again
    }

    #[test]
    fn reset_forgets_partial_sequence() {
        let mut t = pad();
        down(&mut t, 0, 0, 1, 1000, 500);
        t.reset();
        assert_eq!(up(&mut t, 50, 0), None);
    }
}
