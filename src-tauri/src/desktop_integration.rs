//! Read-only KDE desktop facts and layer-shell stacking; no desktop settings are changed.
use gtk_layer_shell::{Layer, LayerShell};
use serde::Deserialize;
use std::{sync::{Arc, Mutex}, time::{Duration, Instant}};
use tauri::{AppHandle, Manager, WebviewWindow};
use crate::{e2s, monitors, R};

#[derive(Default)]
pub struct DesktopIntegration {
    fullscreen: Arc<Mutex<Option<(Instant, [f64; 4])>>>,
    policy: Mutex<LayerPolicy>,
    bridge: Mutex<Option<(zbus::blocking::Connection, String)>>,
}

#[derive(Clone, Copy)]
struct LayerPolicy { always_on_top: bool, panel_raised: bool }
impl Default for LayerPolicy {
    fn default() -> Self { Self { always_on_top: true, panel_raised: false } }
}
impl LayerPolicy {
    fn layer(self) -> Layer {
        if !self.always_on_top { Layer::Bottom }
        else if self.panel_raised { Layer::Top }
        else { Layer::Overlay }
    }
}

/// Ordinary overlay surfaces are still excluded from painting by KWin while the screen is locked.
pub fn pet_layer(always_on_top: bool) -> Layer {
    LayerPolicy { always_on_top, ..LayerPolicy::default() }.layer()
}

pub fn set_always_on_top(window: &WebviewWindow, enabled: bool) -> R {
    let state = window.app_handle().state::<DesktopIntegration>();
    let layer = { let mut policy = state.policy.lock().unwrap(); policy.always_on_top = enabled; policy.layer() };
    let gtk = window.gtk_window().map_err(e2s)?;
    if gtk.is_layer_window() { gtk.set_layer(layer); Ok(()) }
    else { window.set_always_on_top(enabled).map_err(e2s) }
}

/// The panel's keep-above peer can cover Top, while Overlay must be restored after it closes.
pub fn panel_raised(app: &AppHandle, raised: bool) -> R {
    let state = app.state::<DesktopIntegration>();
    let layer = { let mut policy = state.policy.lock().unwrap(); policy.panel_raised = raised; policy.layer() };
    let window = app.get_webview_window("main").ok_or("no main window")?;
    let gtk = window.gtk_window().map_err(e2s)?;
    if gtk.is_layer_window() { gtk.set_layer(layer); }
    Ok(())
}

#[derive(Deserialize)]
struct Panel {
    id: u64,
    hiding: String,
    location: String,
    geometry: [f64; 4],
}

fn same_output(a: [f64; 4], b: [f64; 4]) -> bool {
    a.iter().zip(b).all(|(x, y)| x.is_finite() && y.is_finite() && (x-y).abs() < 1.0)
        && a[2] > 0.0 && a[3] > 0.0 && b[2] > 0.0 && b[3] > 0.0
}

fn edge_for(panels: &[Panel], bounds: [f64; 4]) -> Option<String> {
    // The recovered frontend accepts one edge. Keep the oldest panel when several auto-hide.
    panels.iter().filter(|p| p.hiding == "autohide" && same_output(p.geometry, bounds)
        && matches!(p.location.as_str(), "left" | "top" | "right" | "bottom"))
        .min_by_key(|p| p.id).map(|p| p.location.clone())
}

const PANEL_QUERY: &str = r#"print(JSON.stringify(panels().map(function(p) {
  var g = screenGeometry(p.screen);
  return {id:p.id, hiding:p.hiding, location:p.location, geometry:[g.x,g.y,g.width,g.height]};
})))"#;

pub fn auto_hide_edge(bounds: [f64; 4]) -> R<Option<String>> {
    static CACHE: Mutex<Option<(Instant, Vec<Panel>)>> = Mutex::new(None);
    let mut cache = CACHE.lock().unwrap();
    if let Some((at, panels)) = cache.as_ref() {
        if at.elapsed() < Duration::from_millis(700) { return Ok(edge_for(panels, bounds)); }
    }
    let result = (|| {
        let conn = monitors::dbus()?;
        let proxy = zbus::blocking::Proxy::new(&conn, "org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell").map_err(e2s)?;
        let json: String = proxy.call("evaluateScript", &(PANEL_QUERY,)).map_err(e2s)?;
        let panels: Vec<Panel> = serde_json::from_str(json.trim()).map_err(e2s)?;
        let edge = edge_for(&panels, bounds);
        *cache = Some((Instant::now(), panels));
        Ok(edge)
    })();
    if result.is_err() { monitors::dbus_reset(); }
    result
}

impl DesktopIntegration {
    pub fn foreground_fullscreen(&self, bounds: [f64; 4]) -> bool {
        self.fullscreen.lock().unwrap().as_ref().is_some_and(|(at, rect)|
            at.elapsed() < Duration::from_secs(3) && same_output(*rect, bounds))
    }
}

struct Receiver(Arc<Mutex<Option<(Instant, [f64; 4])>>>);
#[zbus::interface(name = "org.wuwatappo.Desktop")]
impl Receiver {
    fn update(&mut self, payload: &str) {
        let report = (payload.len() < 256).then(|| serde_json::from_str::<(bool, [f64; 4])>(payload).ok()).flatten();
        *self.0.lock().unwrap() = report.filter(|(yes, bounds)| *yes && same_output(*bounds, *bounds))
            .map(|(_, bounds)| (Instant::now(), bounds));
    }
}

// At most one tiny callback per 1.5 s; signals update immediately. Stop after app death, even on a crash.
const KWIN_QUERY: &str = r#"
var lastAck = Date.now(), stopped = false, heartbeat = new QTimer(); heartbeat.interval = 1500;
function report() {
  if (stopped) return;
  if (Date.now() - lastAck > 6000) { stopped = true; heartbeat.stop(); return; }
  var w = workspace.activeWindow, yes = !!w && w.pid !== WW_PID && w.fullScreen;
  var g = yes && w.output ? w.output.geometry : {x:0,y:0,width:0,height:0};
  callDBus(WW_BUS, "/Desktop", "org.wuwatappo.Desktop", "Update", JSON.stringify([!!yes,[g.x,g.y,g.width,g.height]]), function() { lastAck = Date.now(); });
}
function watch(w) { w.fullScreenChanged.connect(report); w.outputChanged.connect(report); }
workspace.windowList().forEach(watch); workspace.windowAdded.connect(watch);
workspace.windowActivated.connect(report); workspace.screensChanged.connect(report);
heartbeat.timeout.connect(report); heartbeat.start(); report();
"#;

/// Call on a worker after managing DesktopIntegration::default(); no GTK work occurs here.
pub fn install(app: &AppHandle) -> R {
    let state = app.state::<DesktopIntegration>();
    let pid = std::process::id();
    let bus_name = format!("org.wuwatappo.Desktop.P{pid}");
    let conn = zbus::blocking::connection::Builder::session().map_err(e2s)?
        .method_timeout(Duration::from_secs(2))
        .name(bus_name.as_str()).map_err(e2s)?
        .serve_at("/Desktop", Receiver(state.fullscreen.clone())).map_err(e2s)?
        .build().map_err(e2s)?;
    let name = "wuwatappo-desktop".to_string();
    let dir = app.path().app_cache_dir().map_err(e2s)?;
    std::fs::create_dir_all(&dir).map_err(e2s)?;
    let path = dir.join(format!("{name}.js"));
    let js = KWIN_QUERY.replace("WW_PID", &pid.to_string()).replace("WW_BUS", &serde_json::to_string(&bus_name).map_err(e2s)?);
    std::fs::write(&path, js).map_err(e2s)?;
    let result = (|| {
        let proxy = zbus::blocking::Proxy::new(&conn, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting").map_err(e2s)?;
        let _: bool = proxy.call("unloadScript", &name).map_err(e2s)?;
        let id: i32 = proxy.call("loadScript", &(path.to_string_lossy().as_ref(), name.as_str())).map_err(e2s)?;
        if id < 0 { return Err("KWin refused desktop-state script".into()); }
        *state.bridge.lock().unwrap() = Some((conn.clone(), name.clone()));
        zbus::blocking::Proxy::new(&conn, "org.kde.KWin", format!("/Scripting/Script{id}"), "org.kde.kwin.Script").map_err(e2s)?
            .call::<_, _, ()>("run", &()).map_err(e2s)
    })();
    let _ = std::fs::remove_file(path);
    if result.is_err() { state.stop(); }
    result
}

impl DesktopIntegration {
    pub fn stop(&self) {
        *self.fullscreen.lock().unwrap() = None;
        if let Some((conn, name)) = self.bridge.lock().unwrap().take() {
            if let Ok(proxy) = zbus::blocking::Proxy::new(&conn, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting") {
                let _ = proxy.call::<_, _, bool>("unloadScript", &name);
            }
        }
    }
}
impl Drop for DesktopIntegration {
    fn drop(&mut self) { self.stop(); }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn auto_hide_matches_output_and_actual_mode() {
        let panels: Vec<Panel> = serde_json::from_str(r#"[
          {"id":1,"hiding":"none","location":"top","geometry":[-1200,0,1200,800]},
          {"id":2,"hiding":"autohide","location":"bottom","geometry":[0,0,1200,800]},
          {"id":3,"hiding":"dodgewindows","location":"left","geometry":[-1200,0,1200,800]},
          {"id":4,"hiding":"autohide","location":"right","geometry":[-1200,0,1200,800]}
        ]"#).unwrap();
        assert_eq!(edge_for(&panels, [-1200.0,0.0,1200.0,800.0]), Some("right".into()));
        assert_eq!(edge_for(&panels, [0.0,0.0,1200.0,800.0]), Some("bottom".into()));
        assert_eq!(edge_for(&panels, [0.0,0.0,800.0,1200.0]), None);
        assert_eq!(edge_for(&panels, [f64::NAN,0.0,1200.0,800.0]), None);
    }
    #[test]
    fn fullscreen_expires_and_only_affects_its_output() {
        let state = DesktopIntegration::default();
        let b = [-1200.0,0.0,1200.0,800.0];
        Receiver(state.fullscreen.clone()).update(&serde_json::to_string(&(true, b)).unwrap());
        assert!(state.foreground_fullscreen(b));
        assert!(!state.foreground_fullscreen([0.0,0.0,1200.0,800.0]));
        *state.fullscreen.lock().unwrap() = Some((Instant::now() - Duration::from_secs(4), b));
        assert!(!state.foreground_fullscreen(b));
        Receiver(state.fullscreen.clone()).update("[true,[0,0,null,800]]");
        assert!(!state.foreground_fullscreen(b));
    }
    #[test]
    fn stacking_obeys_always_on_top_and_panel_raise() {
        let mut p = LayerPolicy::default();
        assert_eq!(p.layer(), Layer::Overlay);
        p.panel_raised = true;
        assert_eq!(p.layer(), Layer::Top);
        p.always_on_top = true; // ordinary position updates must not undo the panel raise
        assert_eq!(p.layer(), Layer::Top);
        p.panel_raised = false;
        assert_eq!(p.layer(), Layer::Overlay);
        p.always_on_top = false;
        p.panel_raised = true;
        assert_eq!(p.layer(), Layer::Bottom);
        p.panel_raised = false;
        assert_eq!(p.layer(), Layer::Bottom);
    }
}
