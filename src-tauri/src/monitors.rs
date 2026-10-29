use gtk::prelude::*;
use gtk_layer_shell::LayerShell;
use serde::Deserialize;
use serde_json::{json, Value};
use std::sync::Mutex;
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter, Manager, Monitor, PhysicalPosition, PhysicalSize, WebviewWindow};

use crate::{e2s, R};

#[derive(Clone, Copy, Debug, Deserialize, PartialEq)]
pub struct Bounds {
    x: i32,
    y: i32,
    width: u32,
    height: u32,
}

impl Bounds {
    fn of(m: &Monitor) -> Self {
        Self { x: m.position().x, y: m.position().y, width: m.size().width, height: m.size().height }
    }

    fn contains(self, x: f64, y: f64) -> bool {
        x >= self.x as f64 && y >= self.y as f64
            && x < self.x as f64 + self.width as f64 && y < self.y as f64 + self.height as f64
    }
}

#[derive(Default)]
pub struct MonitorState(Mutex<Option<Bounds>>, Mutex<SwitchGate>);

#[derive(Default)]
struct SwitchGate {
    next_token: u64,
    active: Option<(u64, Instant)>,
    last_started: Option<Instant>,
}

impl SwitchGate {
    fn begin(&mut self, now: Instant) -> Option<u64> {
        if self.active.is_some_and(|(_, at)| now.duration_since(at) < Duration::from_secs(15))
            || self.last_started.is_some_and(|at| now.duration_since(at) < Duration::from_millis(280)) {
            return None;
        }
        self.next_token = self.next_token.wrapping_add(1).max(1);
        self.active = Some((self.next_token, now));
        self.last_started = Some(now);
        Some(self.next_token)
    }

    fn end(&mut self, token: u64) {
        if self.active.is_some_and(|(current, _)| current == token) { self.active = None; }
    }
}

#[tauri::command]
pub fn begin_main_monitor_switch(state: tauri::State<MonitorState>) -> Option<u64> {
    state.1.lock().unwrap().begin(Instant::now())
}

#[tauri::command]
pub fn end_main_monitor_switch(state: tauri::State<MonitorState>, token: u64) {
    state.1.lock().unwrap().end(token);
}

// One session-bus connection for every KDE call (each Connection owns an executor thread and a socket).
// A call that gets no reply must not wedge the GTK thread, hence the timeout.
static DBUS: Mutex<Option<zbus::blocking::Connection>> = Mutex::new(None);

pub fn dbus() -> R<zbus::blocking::Connection> {
    let mut c = DBUS.lock().unwrap();
    if c.is_none() {
        *c = Some(zbus::blocking::connection::Builder::session().map_err(e2s)?
            .method_timeout(Duration::from_secs(2)).build().map_err(e2s)?);
    }
    Ok(c.clone().unwrap())
}

pub fn dbus_reset() { *DBUS.lock().unwrap() = None; }

pub fn watch(app: AppHandle) -> R {
    let display = app.get_webview_window("main").ok_or("no main window")?.gtk_window().map_err(e2s)?.display();
    display.connect_monitor_removed(move |_, _| {
        *app.state::<MonitorState>().0.lock().unwrap() = None;
        let app = app.clone();
        // GDK must finish removing the output before selecting a remaining one.
        gtk::glib::idle_add_local_once(move || {
            let result = sync_main_window_to_current_monitor(app.clone()).and_then(|_| {
                if let Some(w) = app.get_webview_window("main") {
                    w.gtk_window().map_err(e2s)?.show();
                    crate::finish_main_window_monitor_switch(app.clone(), app.state())?;
                }
                sync_aux_windows_to_main_monitor(app)
            });
            if let Err(e) = result { eprintln!("[monitor] output removal recovery failed: {e}"); }
        });
    });
    Ok(())
}

pub fn current(app: &AppHandle) -> R<Monitor> {
    let w = app.get_webview_window("main").ok_or("no main window")?;
    let selected = *app.state::<MonitorState>().0.lock().unwrap();
    if let Some(bounds) = selected {
        if let Some(m) = w.available_monitors().map_err(e2s)?.into_iter().find(|m| Bounds::of(m) == bounds) {
            return Ok(m);
        }
        *app.state::<MonitorState>().0.lock().unwrap() = None;
    }
    w.current_monitor().map_err(e2s)?.or(w.primary_monitor().map_err(e2s)?)
        .or_else(|| w.available_monitors().ok()?.into_iter().next()).ok_or("no monitor".into())
}

fn gdk_monitor(w: &WebviewWindow, target: &Monitor) -> R<gtk::gdk::Monitor> {
    let display = w.gtk_window().map_err(e2s)?.display();
    let bounds = Bounds::of(target);
    (0..display.n_monitors()).filter_map(|i| display.monitor(i)).find(|m| {
        let r = m.geometry();
        let s = m.scale_factor();
        bounds == Bounds { x: r.x() * s, y: r.y() * s, width: (r.width() * s) as u32, height: (r.height() * s) as u32 }
    }).ok_or("target monitor disconnected".into())
}

pub fn align_layer_to_main(w: &WebviewWindow) -> R {
    let g = w.gtk_window().map_err(e2s)?;
    if g.is_layer_window() {
        let target = current(w.app_handle())?;
        let monitor = gdk_monitor(w, &target)?;
        // set_monitor remaps mapped surfaces, so do not set it again when unchanged.
        if g.monitor().as_ref() != Some(&monitor) { g.set_monitor(&monitor); }
    }
    Ok(())
}

#[tauri::command]
pub fn set_main_window_monitor_bounds(app: AppHandle, payload: Bounds) -> R {
    let w = app.get_webview_window("main").ok_or("no main window")?;
    let target = w.available_monitors().map_err(e2s)?.into_iter()
        .find(|m| Bounds::of(m) == payload).ok_or("target monitor disconnected or invalid bounds")?;
    let g = w.gtk_window().map_err(e2s)?;
    if g.is_layer_window() {
        let monitor = gdk_monitor(&w, &target)?;
        if g.monitor().as_ref() != Some(&monitor) { g.set_monitor(&monitor); }
    } else if g.display().type_().name().contains("Wayland") {
        place_kwin(&app, &target, true)?;
    } else {
        w.unmaximize().map_err(e2s)?;
        w.set_position(PhysicalPosition::new(payload.x, payload.y)).map_err(e2s)?;
        w.set_size(PhysicalSize::new(payload.width, payload.height)).map_err(e2s)?;
    }
    *app.state::<MonitorState>().0.lock().unwrap() = Some(payload);
    let snapshot = json!({"monitor":target,"origin":target.position(),"layerShell":g.is_layer_window()});
    app.emit_to("main", "cat-main-monitor-origin-changed", snapshot).map_err(e2s)
}

#[tauri::command]
pub fn get_main_monitor_snapshot(app: AppHandle) -> R<Value> {
    let w = app.get_webview_window("main").ok_or("no main window")?;
    let monitor = current(&app)?;
    let layer = w.gtk_window().map_err(e2s)?.is_layer_window();
    let origin = if layer { *monitor.position() } else { w.inner_position().map_err(e2s)? };
    Ok(json!({"monitor":monitor,"origin":origin,"layerShell":layer}))
}

#[tauri::command]
pub fn monitor_from_physical_point(app: AppHandle, x: f64, y: f64) -> R<Option<Monitor>> {
    let selected = current(&app)?;
    // GDK scales each output's origin separately. Prefer the selected output when mixed-scale
    // physical rectangles overlap, as the frontend uses this query to locate the main window.
    if Bounds::of(&selected).contains(x, y) { return Ok(Some(selected)); }
    Ok(app.available_monitors().map_err(e2s)?.into_iter().find(|m| Bounds::of(m).contains(x, y)))
}

#[tauri::command]
pub fn sync_main_window_to_current_monitor(app: AppHandle) -> R {
    let target = current(&app)?;
    set_main_window_monitor_bounds(app, Bounds::of(&target))
}

#[tauri::command]
pub fn sync_aux_windows_to_main_monitor(app: AppHandle) -> R {
    let target = current(&app)?;
    let windows = app.webview_windows();
    for w in windows.values().filter(|w| w.label() != "main") {
        let g = w.gtk_window().map_err(e2s)?;
        if g.is_layer_window() {
            align_layer_to_main(w)?;
        } else if !g.display().type_().name().contains("Wayland") {
            let size = w.outer_size().map_err(e2s)?;
            let pos = centered(Bounds::of(&target), size.width, size.height);
            w.set_position(PhysicalPosition::new(pos.0, pos.1)).map_err(e2s)?;
        }
    }
    if windows.values().any(|w| w.label() != "main" && w.gtk_window().is_ok_and(|g| !g.is_layer_window() && g.display().type_().name().contains("Wayland"))) {
        place_kwin(&app, &target, false)?;
    }
    Ok(())
}

fn centered(b: Bounds, width: u32, height: u32) -> (i32, i32) {
    (b.x + ((b.width as i64 - width as i64).max(0) / 2) as i32,
     b.y + ((b.height as i64 - height as i64).max(0) / 2) as i32)
}

fn place_kwin(app: &AppHandle, target: &Monitor, main: bool) -> R {
    let titles: Vec<_> = app.webview_windows().values()
        .filter(|w| (w.label() == "main") == main)
        .filter_map(|w| w.gtk_window().ok().filter(|g| !g.is_layer_window()).and_then(|_| w.title().ok()))
        .collect();
    if titles.is_empty() { return Ok(()); }
    let scale = target.scale_factor();
    let output = json!({"x":target.position().x as f64 / scale,"y":target.position().y as f64 / scale,
        "width":target.size().width as f64 / scale,"height":target.size().height as f64 / scale});
    let script = format!(r#"
var pid = {pid}, titles = {titles}, bounds = {output}, full = {main};
var output = workspace.screens.find(function(s) {{ var g=s.geometry; return Math.abs(g.x-bounds.x)<1 && Math.abs(g.y-bounds.y)<1 && Math.abs(g.width-bounds.width)<1 && Math.abs(g.height-bounds.height)<1; }});
var done = {{}};
function place() {{
  if (!output) return;
  workspace.windowList().forEach(function(w) {{
    if (w.pid !== pid || titles.indexOf(w.caption)<0 || done[w.internalId]) return;
    workspace.sendClientToScreen(w, output);
    var area = workspace.clientArea(KWin.MaximizeArea, w), g = w.frameGeometry;
    w.frameGeometry = full ? bounds : {{x:area.x+Math.max(0,(area.width-g.width)/2),y:area.y+Math.max(0,(area.height-g.height)/2),width:g.width,height:g.height}};
    done[w.internalId] = true;
  }});
}}
place();
var attempts=0, timer=new QTimer(); timer.interval=50;
timer.timeout.connect(function() {{place(); if(++attempts>=20) timer.stop();}}); timer.start();
"#, pid=std::process::id(), titles=json!(titles), output=output, main=main);
    let dir = app.path().app_cache_dir().map_err(e2s)?;
    std::fs::create_dir_all(&dir).map_err(e2s)?;
    let name = if main { "wuwatappo-place-main" } else { "wuwatappo-place-aux" };
    let path = dir.join(format!("{name}.js"));
    // The main-window path reports failure to the F8 handshake; auxiliary placement is best effort and
    // must not block the GTK thread on KWin.
    if main { return run_kwin_script(name, &path, &script); }
    std::thread::spawn(move || if let Err(e) = run_kwin_script(name, &path, &script) { eprintln!("[monitor] auxiliary placement failed: {e}"); });
    Ok(())
}

fn run_kwin_script(name: &str, path: &std::path::Path, script: &str) -> R {
    use zbus::blocking::Proxy;
    static SERIAL: Mutex<()> = Mutex::new(()); // one script file / one loaded script name at a time
    let _g = SERIAL.lock().unwrap();
    std::fs::write(path, script).map_err(e2s)?;
    let conn = dbus()?;
    let r = (|| {
        let p = Proxy::new(&conn, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting").map_err(e2s)?;
        let _: bool = p.call("unloadScript", &name).map_err(e2s)?;
        let id: i32 = p.call("loadScript", &(path.to_string_lossy().as_ref(), name)).map_err(e2s)?;
        if id < 0 { return Err("KWin refused monitor placement script".to_string()); }
        Proxy::new(&conn, "org.kde.KWin", format!("/Scripting/Script{id}"), "org.kde.kwin.Script").map_err(e2s)?
            .call::<_, _, ()>("run", &()).map_err(e2s)
    })();
    if r.is_err() { dbus_reset(); }
    r
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn physical_bounds_include_negative_origins_and_exclude_far_edges() {
        let b = Bounds { x: -1920, y: -200, width: 1920, height: 1080 };
        assert!(b.contains(-1920.0, -200.0));
        assert!(b.contains(-1.0, 879.0));
        assert!(!b.contains(0.0, 879.0));
        assert!(!b.contains(-1.0, 880.0));
        assert!(!b.contains(f64::NAN, 0.0));
    }

    #[test]
    fn centered_window_does_not_wrap_when_larger_than_output() {
        let b = Bounds { x: -1000, y: 300, width: 1000, height: 800 };
        assert_eq!(centered(b, 500, 400), (-750, 500));
        assert_eq!(centered(b, 1200, 900), (-1000, 300));
    }

    #[test]
    fn switch_gate_deduplicates_windows_and_recovers_abandoned_leases() {
        let mut gate = SwitchGate::default();
        let now = Instant::now();
        let first = gate.begin(now).unwrap();
        assert_eq!(gate.begin(now + Duration::from_secs(1)), None);
        gate.end(first);
        assert_eq!(gate.begin(now + Duration::from_millis(100)), None);
        let second = gate.begin(now + Duration::from_millis(280)).unwrap();
        let recovered = gate.begin(now + Duration::from_secs(16)).unwrap();
        assert_ne!(second, recovered);
        gate.end(second);
        assert_eq!(gate.begin(now + Duration::from_secs(17)), None);
        gate.end(recovered);
        assert!(gate.begin(now + Duration::from_secs(17)).is_some());
    }
}
