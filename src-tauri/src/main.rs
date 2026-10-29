// Linux shell for the WuwaTappo frontend. The original Rust backend is closed, so the commands
// the frontend invokes are re-implemented here (contract and evidence: ../README.md).
//
// The original main window is a click-through fullscreen overlay fed by a Windows global input hook
// ("mouse-move" / "mouse-button" / "key-press" events in physical screen px). Here:
//  - the main window is a wlr-layer-shell surface (always on top, no taskbar entry, no focus) when the
//    compositor supports it, else a maximized always-on-top window;
//  - its GTK input shape is limited to the hit regions the frontend reports, and `shim.js` turns DOM pointer
//    events inside them into the native events the frontend expects;
//  - `input.rs` reads /dev/input for clicks/keys everywhere else.
mod input;
mod native_ws;
mod monitors;
mod service_status;
mod desktop_integration;

use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::sync::{atomic::{AtomicU64, Ordering}, Mutex};
use std::time::{Duration, Instant};
use tauri::{
    menu::{CheckMenuItem, ContextMenu, Menu, MenuItem, PredefinedMenuItem, Submenu},
    tray::TrayIconBuilder,
    AppHandle, CursorIcon, Emitter, Listener, LogicalPosition, LogicalSize, Manager, WebviewUrl, WebviewWindow, WebviewWindowBuilder,
    WindowEvent,
};
use tauri_plugin_deep_link::DeepLinkExt;

type R<T = ()> = Result<T, String>;
fn e2s<E: ToString>(e: E) -> String { e.to_string() }

const SCALES: [u32; 6] = [50, 75, 100, 125, 150, 200];
const OPACITIES: [u32; 4] = [25, 50, 75, 100];
const COOKIES: &str = include_str!("cookies.js");
const WS_SHIM: &str = include_str!("ws.js");
const PLATFORM: &str = include_str!("platform.js");

#[derive(PartialEq)]
struct MenuCfg {
    labels: Value,
    pet_visible: bool,
    scale: u32,
    opacity: u32,
    passthrough: bool,
}

#[derive(Default)]
struct State {
    menu: Mutex<Option<MenuCfg>>,
    update_detail: Mutex<Value>,
    input_regions: Mutex<Option<Vec<Rect>>>,
    active_regions: Mutex<Vec<Rect>>,
    panel: Mutex<PanelSize>,
    pending_show: Mutex<HashMap<String, PendingShow>>,
    show_id: AtomicU64,
}

// Panel size bookkeeping. tao's `inner_size()` is only refreshed by GTK's configure event, so two toggles
// inside one frame would both start from the stale size; `pending` remembers what we just asked for.
#[derive(Default)]
struct PanelSize {
    collapsed: bool,
    pending: Option<(Instant, f64, f64)>,
}

// ---- persisted counters: one JSON file in the app data dir ----
fn store_path(app: &AppHandle) -> R<std::path::PathBuf> {
    let d = app.path().app_data_dir().map_err(e2s)?;
    std::fs::create_dir_all(&d).map_err(e2s)?;
    Ok(d.join("store.json"))
}
fn store_read(app: &AppHandle) -> Value {
    store_path(app)
        .ok()
        .and_then(|p| std::fs::read(p).ok())
        .and_then(|b| serde_json::from_slice(&b).ok())
        .unwrap_or_else(|| json!({}))
}
fn store_set(app: &AppHandle, key: &str, val: Value) -> R {
    let mut v = store_read(app);
    v[key] = val;
    std::fs::write(store_path(app)?, v.to_string()).map_err(e2s)
}

#[tauri::command]
fn load_key_press_count(app: AppHandle, environment: String) -> u64 {
    store_read(&app)[format!("key_{environment}")].as_u64().unwrap_or(0)
}
#[tauri::command]
fn save_key_press_count(app: AppHandle, count: u64, environment: String) -> R {
    store_set(&app, &format!("key_{environment}"), json!(count))
}
#[tauri::command]
fn load_cumulative_click_count(app: AppHandle, environment: String) -> Value {
    match store_read(&app)[format!("cum_{environment}")].clone() {
        Value::Null => json!({"count":0,"cuid":"","environment":environment}),
        v => v,
    }
}
#[tauri::command]
fn save_cumulative_click_count(app: AppHandle, count: u64, cuid: String, environment: String) -> R {
    store_set(&app, &format!("cum_{environment}"), json!({"count":count,"cuid":cuid,"environment":environment}))
}

// ---- layer-shell overlay (KWin supports wlr-layer-shell) ----
// Must run on the main thread BEFORE the GTK window is first shown, so windows are built `.visible(false)`.
// gtk-layer-shell may only be queried on the GTK thread, but the async inset commands run on workers:
// answer once (first call is in setup) and reuse.
fn layer_ok() -> bool {
    static OK: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *OK.get_or_init(|| std::env::var_os("WW_NO_LAYER").is_none() && gtk_layer_shell::is_supported())
}
fn layer_init(w: &WebviewWindow, ns: &str, layer: gtk_layer_shell::Layer) -> R {
    use gtk_layer_shell::{Edge, LayerShell};
    let g = w.gtk_window().map_err(e2s)?;
    g.init_layer_shell();
    g.set_layer(layer);
    g.set_keyboard_mode(gtk_layer_shell::KeyboardMode::None);
    g.set_namespace(ns);
    // cover the whole output (-1 = ignore panels) so window px == monitor px like on Windows
    for e in [Edge::Left, Edge::Right, Edge::Top, Edge::Bottom] {
        g.set_anchor(e, true);
    }
    g.set_exclusive_zone(-1);
    monitors::align_layer_to_main(w)
}

// ---- main window click-through ----
#[derive(Deserialize, Clone, PartialEq)]
#[serde(rename_all = "camelCase")]
struct Rect {
    left: f64,
    top: f64,
    width: f64,
    height: f64,
    #[serde(default)]
    member_id: Option<Value>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct HitRegion {
    regions: Vec<Rect>,
    #[serde(default)]
    input_regions: Vec<Rect>,
    #[serde(default)]
    update_native_input: bool,
    #[serde(default)]
    window_passthrough: bool,
}

impl HitRegion {
    fn input_rects(self, cached: &mut Option<Vec<Rect>>) -> Vec<Rect> {
        if self.update_native_input {
            *cached = Some(self.input_regions);
        }
        if self.window_passthrough {
            vec![]
        } else {
            cached.clone().unwrap_or(self.regions)
        }
    }
}

fn set_input_shape(w: &WebviewWindow, rects: &[Rect]) -> R {
    use gtk::prelude::WidgetExt;
    let gdk = w.gtk_window().map_err(e2s)?.window().ok_or("window not realized")?;
    let ri: Vec<_> = rects
        .iter()
        .map(|r| gtk::cairo::RectangleInt::new(r.left.floor() as i32, r.top.floor() as i32, r.width.ceil() as i32 + 1, r.height.ceil() as i32 + 1))
        .collect();
    gdk.input_shape_combine_region(&gtk::cairo::Region::create_rectangles(&ri), 0, 0);
    Ok(())
}

// `inputRegions` only arrive with updateNativeInput=true; later calls carry just the visual `regions`.
// Keep the last input set and only fall back to the visual regions before one has been seen.
#[tauri::command]
fn set_main_hit_region(app: AppHandle, state: tauri::State<State>, payload: HitRegion) -> R {
    let w = app.get_webview_window("main").ok_or("no main window")?;
    let all = payload.input_rects(&mut state.input_regions.lock().unwrap());
    {
        let mut active = state.active_regions.lock().unwrap();
        if *active == all { return Ok(()); }
        *active = all.clone();
    }
    apply_input_regions(&w, &all)
}

fn apply_input_regions(w: &WebviewWindow, all: &[Rect]) -> R {
    // memberId lets shim.js route clicks on other players' pets like the native hook did
    let tagged: Vec<_> = all.iter().filter(|r| r.member_id.is_some()).map(|r| json!({"left":r.left,"top":r.top,"width":r.width,"height":r.height,"memberId":r.member_id})).collect();
    w.eval(&format!("window.__wwRegions={}", Value::Array(tagged))).map_err(e2s)?;
    set_input_shape(w, all)
}

#[tauri::command]
fn set_native_input_cursor(app: AppHandle, kind: String) { set_cursor(&app, &kind) }
#[tauri::command]
fn set_overlay_cursor(app: AppHandle, kind: String) { set_cursor(&app, &kind) }
fn set_cursor(app: &AppHandle, kind: &str) {
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.set_cursor_icon(match kind {
            "pointer" => CursorIcon::Hand,
            "grab" => CursorIcon::Grab,
            "grabbing" => CursorIcon::Grabbing,
            _ => CursorIcon::Default,
        });
    }
}

// The frontend falls back to the plain window API if this errors; a layer surface is already full-output.
#[tauri::command]
fn set_main_window_fullscreen_shell(app: AppHandle) -> R {
    if layer_ok() { return Ok(()); }
    let w = app.get_webview_window("main").ok_or("no main window")?;
    w.set_always_on_top(true).map_err(e2s)?;
    w.maximize().map_err(e2s)
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct ApplyPos { always_on_top: bool }
#[tauri::command]
fn apply_main_window_position(app: AppHandle, payload: ApplyPos) -> R {
    let window = app.get_webview_window("main").ok_or("no main window")?;
    desktop_integration::set_always_on_top(&window, payload.always_on_top)
}
#[tauri::command]
fn set_main_skip_taskbar(app: AppHandle, skip: bool) -> R {
    app.get_webview_window("main").ok_or("no main window")?.set_skip_taskbar(skip).map_err(e2s)
}

// ---- auxiliary windows (flags/sizes recovered from the original builders) ----
#[derive(Deserialize)]
struct Tab { tab: Option<String> }
#[derive(Deserialize)]
struct Collapsed { collapsed: bool }
#[derive(Deserialize)]
struct Size { width: f64, height: f64 }

type Builder<'a> = WebviewWindowBuilder<'a, tauri::Wry, AppHandle>;

thread_local! {
    // GTK objects stay on the GUI thread; all app routes share one security origin and renderer.
    static MAIN_WEBVIEW: std::cell::RefCell<Option<webkit2gtk::WebView>> = const { std::cell::RefCell::new(None) };
}

fn configure_webview(w: &WebviewWindow) -> R {
    let main = w.label() == "main";
    w.with_webview(move |view| {
        use webkit2gtk::{WebContextExt, WebViewExt};
        if main { MAIN_WEBVIEW.with(|v| *v.borrow_mut() = Some(view.inner().clone())); }
        #[cfg(debug_assertions)]
        if !main {
            MAIN_WEBVIEW.with(|v| {
                if let Some(main) = v.borrow().as_ref() {
                    debug_assert_ne!(main.user_content_manager(), view.inner().user_content_manager(), "window scripts must remain independent");
                }
            });
        }
        if let Some(context) = view.inner().context() {
            // Local app pages need resource reuse, but not a browser-sized document cache.
            context.set_cache_model(webkit2gtk::CacheModel::DocumentBrowser);
            debug_assert_eq!(context.cache_model(), webkit2gtk::CacheModel::DocumentBrowser);
        }
    }).map_err(e2s)
}

fn aux(app: &AppHandle, label: &str, route: &str, title: &str, w: f64, h: f64, build: impl FnOnce(Builder<'_>) -> Builder<'_>) -> R<(WebviewWindow, bool)> {
    if let Some(win) = app.get_webview_window(label) {
        return Ok((win, false));
    }
    let url = format!("tauri://localhost/index.html#{route}").parse().map_err(e2s)?;
    let b = WebviewWindowBuilder::new(app, label, WebviewUrl::External(url))
        .title(title)
        .inner_size(w, h)
        .decorations(false)
        .transparent(true)
        .shadow(false)
        .skip_taskbar(true)
        .minimizable(false)
        .maximizable(false)
        .visible(false)
        .initialization_script(COOKIES)
        .initialization_script(WS_SHIM)
        .initialization_script(PLATFORM)
        .on_page_load(|w, p| {
            if let tauri::webview::PageLoadEvent::Finished = p.event() { page_loaded(w.app_handle(), w.label()) }
        })
        .center();
    let related = MAIN_WEBVIEW.with(|v| v.borrow().clone()).ok_or("main WebView is not ready on the GTK thread")?;
    let window = build(b.with_related_view(related)).build().map_err(e2s)?;
    configure_webview(&window)?;
    Ok((window, true))
}
fn close(app: &AppHandle, label: &str) {
    if let Some(w) = app.get_webview_window(label) {
        let _ = w.destroy();
    }
}
fn show(w: &WebviewWindow) {
    let _ = w.unminimize();
    let _ = w.show();
    if w.label() == "panel" { let _ = desktop_integration::panel_raised(w.app_handle(), true); }
    let _ = w.set_focus();
    if let Err(e) = monitors::sync_aux_windows_to_main_monitor(w.app_handle().clone()) {
        eprintln!("[monitor] auxiliary placement unavailable: {e}");
    }
}

// ---- show-after-ready ----
// A freshly built auxiliary window is hidden until its page can paint, otherwise the user sees an empty
// transparent surface that WebKit then fills (the "flash"). A window is revealed by whichever comes first:
//  - the page calling `window_ready` (or emitting the `window-ready` event) once it has mounted;
//  - PageLoadEvent::Finished plus SETTLE_MS, unless the page announced `window-gate` (command `window_gate`
//    or event) to say "I will signal ready myself";
//  - FALLBACK_MS after creation, so a page that never signals cannot stay invisible.
const SETTLE_MS: u64 = 200;
const FALLBACK_MS: u64 = 2500;

struct PendingShow {
    id: u64,
    gated: bool,
    then: Option<Box<dyn FnOnce(&AppHandle) + Send>>,
}

fn after(ms: u64, f: impl FnOnce() + Send + 'static) {
    gtk::glib::timeout_add_once(Duration::from_millis(ms), f);
}

// `id` ties a timer to the window instance that armed it; None = the page itself asked.
fn reveal(app: &AppHandle, label: &str, id: Option<u64>) {
    let st = app.state::<State>();
    let p = {
        let mut m = st.pending_show.lock().unwrap();
        if id.is_some_and(|i| m.get(label).map(|p| p.id) != Some(i)) { return; }
        m.remove(label)
    };
    let Some(p) = p else { return };
    let (a, label) = (app.clone(), label.to_string());
    let _ = app.run_on_main_thread(move || {
        if let Some(w) = a.get_webview_window(&label) {
            show(&w);
            if let Some(f) = p.then { f(&a) }
        }
    });
}

fn page_loaded(app: &AppHandle, label: &str) {
    let id = app.state::<State>().pending_show.lock().unwrap().get(label).filter(|p| !p.gated).map(|p| p.id);
    if let Some(id) = id {
        let (app, label) = (app.clone(), label.to_string());
        after(SETTLE_MS, move || reveal(&app, &label, Some(id)));
    }
}

fn mark_gated(app: &AppHandle, label: &str) {
    if let Some(p) = app.state::<State>().pending_show.lock().unwrap().get_mut(label) { p.gated = true; }
}

#[tauri::command]
fn window_ready(window: WebviewWindow) { reveal(window.app_handle(), window.label(), None) }
#[tauri::command]
fn window_gate(window: WebviewWindow) { mark_gated(window.app_handle(), window.label()) }

// Existing window: show now. New window: arm the gate; `then(app, created)` runs once it is visible.
fn present(app: &AppHandle, w: &WebviewWindow, created: bool, then: impl FnOnce(&AppHandle, bool) + Send + 'static) {
    if !created {
        show(w);
        return then(app, false);
    }
    let label = w.label().to_string();
    let id = app.state::<State>().show_id.fetch_add(1, Ordering::Relaxed) + 1;
    app.state::<State>().pending_show.lock().unwrap()
        .insert(label.clone(), PendingShow { id, gated: false, then: Some(Box::new(move |a| then(a, true))) });
    let (a, l) = (app.clone(), label.clone());
    w.once("window-ready", move |_| reveal(&a, &l, None));
    let (a, l) = (app.clone(), label.clone());
    w.once("window-gate", move |_| mark_gated(&a, &l));
    let a = app.clone();
    after(FALLBACK_MS, move || reveal(&a, &label, Some(id)));
}

// The page registers its listeners after load, so re-send a few times instead of racing it.
// Main-loop timers, not threads: nothing sleeps and nothing is spawned per call.
fn emit_later(app: &AppHandle, label: &'static str, event: &'static str, payload: Value) {
    for ms in [100u64, 500, 1300] {
        let (app, payload) = (app.clone(), payload.clone());
        after(ms, move || { let _ = app.emit_to(label, event, payload); });
    }
}

#[tauri::command]
fn open_login_window(app: AppHandle, intent: Option<String>, compact: Option<bool>) -> R {
    // The original starts at the monitor rect and shrinks to content via resize_login_window_to_content
    // (498x396 ..= 900x760); a normal Wayland window cannot be placed, so start at card size.
    let (w, created) = aux(&app, "login", "/login", "登录", 520.0, 420.0, |b| b)?;
    let _ = w.set_always_on_top(true);
    let payload = json!({"intent": intent.unwrap_or_else(|| "login".into()), "compact": compact});
    present(&app, &w, created, move |a, _| emit_later(a, "login", "login-modal-open", payload));
    Ok(())
}
#[tauri::command]
fn close_login_window(app: AppHandle) { close(&app, "login") }
#[tauri::command]
fn resize_login_window_to_content(app: AppHandle, payload: Size) -> R {
    let Some(w) = app.get_webview_window("login") else { return Ok(()) };
    let (cw, ch) = (payload.width.round().clamp(498.0, 900.0), payload.height.round().clamp(396.0, 760.0));
    w.set_size(LogicalSize::new(cw, ch)).map_err(e2s)?;
    let _ = w.center();
    Ok(())
}
#[tauri::command]
fn open_about_window(app: AppHandle) -> R {
    let (w, created) = aux(&app, "about", "/about", "关于Wuwa Tappo", 880.0, 720.0, |b| b)?;
    present(&app, &w, created, |a, created| {
        if created { emit_later(a, "about", "about-open", Value::Null) } else { let _ = a.emit_to("about", "about-open", ()); }
    });
    Ok(())
}
#[tauri::command]
fn close_about_window(app: AppHandle) { close(&app, "about") }
#[tauri::command]
fn open_oss_notice_window(app: AppHandle) -> R {
    let (w, created) = aux(&app, "oss-notice", "/oss-notice", "开源组件声明", 880.0, 720.0, |b| b)?;
    present(&app, &w, created, |a, created| {
        if created { emit_later(a, "oss-notice", "oss-notice-open", Value::Null) } else { let _ = a.emit_to("oss-notice", "oss-notice-open", ()); }
    });
    Ok(())
}
#[tauri::command]
fn close_oss_notice_window(app: AppHandle) { close(&app, "oss-notice") }
#[tauri::command]
fn open_update_window(app: AppHandle, state: tauri::State<State>, detail: Value) -> R {
    *state.update_detail.lock().unwrap() = detail.clone();
    let (w, created) = aux(&app, "update", "/update", "强制更新", 580.0, 600.0, |b| b)?;
    present(&app, &w, created, move |a, _| {
        emit_later(a, "update", "update-modal-open", Value::Null);
        emit_later(a, "update", "cat-app-update-available", detail);
    });
    Ok(())
}
#[tauri::command]
fn close_update_window(app: AppHandle) { close(&app, "update") }
#[tauri::command]
fn get_app_update_detail(state: tauri::State<State>) -> Value { state.update_detail.lock().unwrap().clone() }

#[tauri::command]
fn open_panel_window(app: AppHandle, payload: Option<Tab>) -> R {
    let tab = payload.and_then(|p| p.tab);
    let route = match &tab { Some(t) => format!("/panel?tab={t}"), None => "/panel".into() };
    let (w, created) = aux(&app, "panel", &route, "功能面板", 1100.0, 760.0, |b| b.resizable(true).min_inner_size(1100.0, 760.0))?;
    if created { *app.state::<State>().panel.lock().unwrap() = PanelSize::default(); }
    present(&app, &w, created, move |a, created| {
        if let (false, Some(t)) = (created, tab) { let _ = a.emit_to("panel", "panel-open-tab", t); }
    });
    Ok(())
}
#[tauri::command]
fn close_panel_window(app: AppHandle) { close(&app, "panel") }
#[tauri::command]
fn is_panel_window_open(app: AppHandle) -> bool { app.get_webview_window("panel").is_some() }
// New logical size, or None when nothing would change (no redundant resize).
fn panel_target(changed: bool, (cw, ch): (f64, f64), min_w: f64, dw: f64) -> Option<(f64, f64)> {
    let new = (if changed { cw + dw } else { cw }.max(min_w), ch.max(760.0));
    (changed || new != (cw, ch)).then_some(new)
}

// Original math (0x140012c40): width +-142 on a state change, floored at the new min width (1100 / 958).
// Order matters on GTK: gtk_window_resize is clamped by the current geometry-hint minimum, so when shrinking
// the minimum must be lowered first, and when growing it is raised last (never clamp an in-flight resize).
// tao queues both requests FIFO and runs them in one main-loop iteration, so no frame is painted between them.
#[tauri::command]
fn set_panel_window_collapsed(app: AppHandle, state: tauri::State<State>, payload: Collapsed) -> R {
    let Some(w) = app.get_webview_window("panel") else { return Ok(()) };
    let mut p = state.panel.lock().unwrap();
    let (min_w, dw) = if payload.collapsed { (958.0, -142.0) } else { (1100.0, 142.0) };
    let changed = p.collapsed != payload.collapsed;
    let (cw, ch) = match p.pending {
        Some((at, pw, ph)) if at.elapsed() < Duration::from_millis(500) => (pw, ph),
        _ => {
            let sf = w.scale_factor().map_err(e2s)?;
            let s = w.inner_size().map_err(e2s)?.to_logical::<f64>(sf);
            (s.width, s.height)
        }
    };
    let Some(new) = panel_target(changed, (cw, ch), min_w, dw) else { return Ok(()) };
    p.collapsed = payload.collapsed;
    let min = Some(LogicalSize::new(min_w, 760.0));
    if payload.collapsed {
        w.set_min_size(min).map_err(e2s)?;
        w.set_size(LogicalSize::new(new.0, new.1)).map_err(e2s)?;
    } else {
        w.set_size(LogicalSize::new(new.0, new.1)).map_err(e2s)?;
        w.set_min_size(min).map_err(e2s)?;
    }
    p.pending = Some((Instant::now(), new.0, new.1));
    Ok(())
}
#[tauri::command]
fn raise_panel_window_above_pets(app: AppHandle) {
    if let Some(w) = app.get_webview_window("panel") {
        if w.is_visible().unwrap_or(false) { let _ = desktop_integration::panel_raised(&app, true); }
        let _ = w.set_always_on_top(true);
        let _ = w.set_focus();
    }
}

// The toast page caches "ready" in JS, so the window is created once and then only hidden/shown.
// Original: full monitor rect, always on top, click-through.
#[tauri::command]
fn show_toast_overlay(app: AppHandle) -> R {
    let layer = layer_ok();
    let (w, created) = aux(&app, "toast", "/toast", "Toast", 800.0, 600.0, |b| b.focused(false).always_on_top(!layer))?;
    if created {
        if layer { layer_init(&w, "wuwatappo-toast", gtk_layer_shell::Layer::Overlay)?; }
        // Every (re)map of the surface starts with GDK's default full-window input region.
        use gtk::prelude::WidgetExt;
        w.gtk_window().map_err(e2s)?.connect_map(|g| {
            if let Some(gdk) = g.window() { gdk.input_shape_combine_region(&gtk::cairo::Region::create(), 0, 0); }
        });
    }
    w.show().map_err(e2s)?;
    toast_click_through(&app);
    Ok(())
}
// Verified with WAYLAND_DEBUG: without this the full-output toast surface kept a full-screen input region
// (set_ignore_cursor_events never reached the compositor), swallowing every click on the desktop while shown.
// The map handler is the primary fix; the delayed repeats cover GDK committing its default after the map.
fn toast_click_through(app: &AppHandle) {
    let app = app.clone();
    std::thread::spawn(move || {
        for ms in [0u64, 150, 400, 1000] {
            std::thread::sleep(std::time::Duration::from_millis(ms));
            let a = app.clone();
            let _ = app.run_on_main_thread(move || {
                if let Some(w) = a.get_webview_window("toast") {
                    use gtk::prelude::WidgetExt;
                    if let Ok(g) = w.gtk_window() { g.realize(); }
                    let _ = set_input_shape(&w, &[]);
                }
            });
        }
    });
}
// In layer mode the surface is never unmapped: a re-mapped layer surface starts with GDK's full-output input
// region and the compositor only takes our empty one on the next redraw, which an idle toast page never
// triggers (verified with WAYLAND_DEBUG: ~8 s of a desktop that ignored every click). The page removes its
// own DOM after the delay, so a mapped, transparent, click-through surface is invisible.
#[tauri::command]
fn hide_toast_overlay(app: AppHandle) {
    if layer_ok() { return; }
    if let Some(w) = app.get_webview_window("toast") { let _ = w.hide(); }
}

// ---- native menu (dock right-click) and tray ----
fn label(c: &MenuCfg, k: &str) -> String { c.labels[k].as_str().unwrap_or(k).to_string() }
// The original checks the option nearest to the current value (JS clamps, so e.g. 90 can occur).
fn nearest(opts: &[u32], v: u32) -> u32 { *opts.iter().min_by_key(|o| (**o as i64 - v as i64).abs()).unwrap() }

// Item order, ids and option lists as recovered from the original builder (0x14000ce50).
fn build_menu(app: &AppHandle, c: &MenuCfg) -> tauri::Result<Menu<tauri::Wry>> {
    let m = Menu::new(app)?;
    let item = |id: &str, text: String| MenuItem::with_id(app, id, text, true, None::<&str>);
    m.append(&item("dock_bag", label(c, "openBag"))?)?;
    m.append(&item("dock_atlas", label(c, "openAtlas"))?)?;
    m.append(&item("dock_exchange", label(c, "openExchange"))?)?;
    m.append(&item("dock_room", label(c, "openRoom"))?)?;
    m.append(&PredefinedMenuItem::separator(app)?)?;
    m.append(&item("dock_settings", label(c, "dockSettings"))?)?;
    m.append(&CheckMenuItem::with_id(app, "dock_window_passthrough", label(c, "windowPassthrough"), true, c.passthrough, None::<&str>)?)?;
    let op = Submenu::with_id(app, "dock_window_opacity_submenu", label(c, "windowOpacity"), true)?;
    for n in OPACITIES {
        op.append(&CheckMenuItem::with_id(app, format!("dock_window_opacity_{n}"), format!("{n}%"), true, n == nearest(&OPACITIES, c.opacity), None::<&str>)?)?;
    }
    m.append(&op)?;
    let size = Submenu::with_id(app, "dock_pet_scale_submenu", label(c, "windowSize"), true)?;
    for n in SCALES {
        size.append(&CheckMenuItem::with_id(app, format!("dock_pet_scale_{n}"), format!("{n}%"), true, n == nearest(&SCALES, c.scale), None::<&str>)?)?;
    }
    m.append(&size)?;
    m.append(&item("dock_toggle_pet", label(c, if c.pet_visible { "hidePet" } else { "showPet" }))?)?;
    m.append(&PredefinedMenuItem::separator(app)?)?;
    m.append(&item("dock_restart", label(c, "restart"))?)?;
    m.append(&item("dock_quit", label(c, "quit"))?)?;
    Ok(m)
}

fn on_menu(app: &AppHandle, id: &str) {
    let tab = |t: &str| { let _ = app.emit("dock-open-panel", t); };
    let num = |p: &str| id.strip_prefix(p).and_then(|n| n.parse::<u32>().ok());
    match id {
        "dock_bag" => tab("bag"),
        "dock_atlas" => tab("atlas"),
        "dock_exchange" => tab("exchange"),
        "dock_room" => tab("room"),
        "dock_settings" => tab("setting"),
        "dock_toggle_pet" => { let _ = app.emit("tray-toggle-pet-visibility", ()); }
        "dock_window_passthrough" => { let _ = app.emit("tray-toggle-window-passthrough", ()); }
        "dock_restart" => { let _ = app.emit("tray-restart", ()); }
        "dock_quit" => { let _ = app.emit("tray-quit", ()); }
        _ => {
            if let Some(n) = num("dock_pet_scale_") { let _ = app.emit("tray-set-pet-scale", n); }
            else if let Some(n) = num("dock_window_opacity_") { let _ = app.emit("tray-set-window-opacity", n); }
        }
    }
}

#[tauri::command]
fn sync_native_menu(app: AppHandle, state: tauri::State<State>, labels: Value, pet_dock_visible: bool, pet_scale: f64, window_opacity: f64, window_passthrough: bool) -> R {
    let cfg = MenuCfg { labels, pet_visible: pet_dock_visible, scale: pet_scale.round() as u32, opacity: window_opacity.round() as u32, passthrough: window_passthrough };
    // The frontend re-syncs on every state tick; rebuilding the GTK menu and the tray's D-Bus menu is not free.
    if app.tray_by_id("main-tray").is_some() && state.menu.lock().unwrap().as_ref() == Some(&cfg) { return Ok(()); }
    let menu = build_menu(&app, &cfg).map_err(e2s)?;
    match app.tray_by_id("main-tray") {
        Some(t) => {
            t.set_menu(Some(menu)).map_err(e2s)?;
            t.set_tooltip(Some(label(&cfg, "trayTooltip"))).map_err(e2s)?;
        }
        None => {
            let mut b = TrayIconBuilder::with_id("main-tray").menu(&menu).tooltip(label(&cfg, "trayTooltip"));
            if let Some(i) = app.default_window_icon() { b = b.icon(i.clone()); }
            b.build(&app).map_err(e2s)?;
        }
    }
    *state.menu.lock().unwrap() = Some(cfg);
    Ok(())
}

// x/y are physical screen px like the native mouse events; convert to window-relative logical px.
#[tauri::command]
fn popup_main_dock_context_menu(app: AppHandle, state: tauri::State<State>, x: f64, y: f64) -> R {
    use gtk_layer_shell::LayerShell;
    let w = app.get_webview_window("main").ok_or("no main window")?;
    let (inner, sf) = if w.gtk_window().map_err(e2s)?.is_layer_window() {
        let monitor = monitors::current(&app)?;
        (*monitor.position(), monitor.scale_factor())
    } else {
        (w.inner_position().map_err(e2s)?, w.scale_factor().map_err(e2s)?)
    };
    let menu = {
        let g = state.menu.lock().unwrap();
        build_menu(&app, g.as_ref().ok_or("menu not synced yet")?).map_err(e2s)?
    };
    let win = w.as_ref().window();
    menu.popup_at(win, relative_pointer(x, y, inner, sf)).map_err(e2s)
}

fn relative_pointer(x: f64, y: f64, origin: tauri::PhysicalPosition<i32>, scale: f64) -> LogicalPosition<f64> {
    LogicalPosition::new((x - origin.x as f64) / scale, (y - origin.y as f64) / scale)
}

fn insets_from_rect([mx, my, mw, mh]: [f64; 4], [x, y, rw, rh]: [f64; 4]) -> Value {
    let px = |v: f64| v.max(0.0).round() as i64;
    json!({"left":px(x - mx),"top":px(y - my),"right":px(mx + mw - x - rw),"bottom":px(my + mh - y - rh)})
}

// main and toast poll this every 1.5 s through two commands back to back; one D-Bus round trip serves both.
fn plasma_available_rect(name: &str) -> R<[f64; 4]> {
    static CACHE: Mutex<Option<(String, Instant, [f64; 4])>> = Mutex::new(None);
    if let Some((n, at, r)) = &*CACHE.lock().unwrap() {
        if n == name && at.elapsed() < Duration::from_millis(700) { return Ok(*r); }
    }
    let result = (|| {
        let plasma = zbus::blocking::Proxy::new(&monitors::dbus()?, "org.kde.plasmashell", "/StrutManager", "org.kde.PlasmaShell.StrutManager").map_err(e2s)?;
        let (x, y, rw, rh): (i32, i32, i32, i32) = plasma.call("availableScreenRect", &(name,)).map_err(e2s)?;
        Ok([x as f64, y as f64, rw as f64, rh as f64])
    })();
    match &result {
        Ok(r) => *CACHE.lock().unwrap() = Some((name.to_string(), Instant::now(), *r)),
        Err(_) => monitors::dbus_reset(),
    }
    result
}

// ---- insets ----
// The frontend subtracts these (logical/CSS px) from the window height. The layer surface covers the whole
// output (exclusive zone -1), so the panel space must be reported. Plasma publishes the free rect over
// D-Bus; without it (other desktops / maximized fallback, which already excludes panels) report 0.
fn panel_insets(app: &AppHandle) -> Value {
    let zero = json!({"left":0,"top":0,"right":0,"bottom":0});
    if !layer_ok() { return zero; }
    (|| {
        let monitor = monitors::current(app).ok()?;
        let sf = monitor.scale_factor();
        let name = monitor.name()?.clone();
        let (mx, my) = (monitor.position().x as f64 / sf, monitor.position().y as f64 / sf);
        let (mw, mh) = (monitor.size().width as f64 / sf, monitor.size().height as f64 / sf);
        Some(insets_from_rect([mx, my, mw, mh], plasma_available_rect(&name).ok()?))
    })().unwrap_or(zero)
}
// async: the D-Bus call must never block the GTK thread (a hung plasmashell froze the whole UI).
#[tauri::command]
async fn get_main_desktop_safe_area(app: AppHandle) -> Value {
    let bounds = monitors::current(&app).ok().map(|m| {
        let sf = m.scale_factor();
        [m.position().x as f64 / sf, m.position().y as f64 / sf, m.size().width as f64 / sf, m.size().height as f64 / sf]
    });
    let edge = bounds.and_then(|b| desktop_integration::auto_hide_edge(b).ok().flatten());
    let fullscreen = bounds.is_some_and(|b| app.state::<desktop_integration::DesktopIntegration>().foreground_fullscreen(b));
    json!({"insets":panel_insets(&app),"autoHideEdge":edge,"foregroundFullscreen":fullscreen})
}
#[tauri::command]
async fn get_main_taskbar_insets(app: AppHandle) -> Value { panel_insets(&app) }

// ---- misc commands ----
#[tauri::command]
fn exit_app(app: AppHandle) { app.exit(0) }
// Maintenance dialog "Exit" button.
#[tauri::command]
fn exit_maintenance_app(app: AppHandle) { app.exit(0) }
#[tauri::command]
fn write_app_log(app: AppHandle, level: String, message: String) {
    eprintln!("[app:{level}] {message}");
    if let Ok(dir) = app.path().app_log_dir() {
        use std::io::Write;
        let _ = std::fs::create_dir_all(&dir);
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(dir.join("WuwaTappo.log")) {
            let _ = writeln!(f, "[{level}] {message}");
        }
    }
}
#[tauri::command]
fn get_track_os_info() -> Value {
    let name = std::fs::read_to_string("/etc/os-release").ok()
        .and_then(|s| s.lines().find_map(|l| l.strip_prefix("PRETTY_NAME=").map(|v| v.trim_matches('"').to_string())))
        .unwrap_or_default();
    json!({"os":"Linux","osVersion":name})
}
#[tauri::command]
fn get_installer_launch_locale() -> Value { Value::Null }
#[tauri::command]
fn get_boot_diagnostics() -> Value { json!({"system":{"os":"linux"}}) }
#[tauri::command]
fn is_internal_testing_build() -> bool { false }
#[tauri::command]
fn is_devtools_hotkeys_build() -> bool { false }
// "elevated autostart" is a Windows scheduled task; the regular autostart plugin handles Linux.
#[tauri::command]
fn is_elevated_autostart_enabled() -> bool { false }

#[tauri::command]
fn finish_main_window_monitor_switch(app: AppHandle, state: tauri::State<State>) -> R {
    let w = app.get_webview_window("main").ok_or("no main window")?;
    let regions = state.active_regions.lock().unwrap().clone();
    apply_input_regions(&w, &regions)
}

// Windows-only placement/hook glue with no Linux counterpart: accept and ignore.
macro_rules! noop {
    ($($n:ident),*) => { $( #[tauri::command] fn $n(_args: Option<Value>) {} )* };
}
noop!(
    save_app_ui_locale,
    set_elevated_autostart, set_main_force_cursor_ignore, set_main_swallow_mouse, set_main_window_content_bounds,
    set_skip_panel_outside_click_close
);

fn main() {
    // Let WebKit select its compositor; disabling DMABUF also disables compositing on current WebKitGTK.
    tauri::Builder::default()
        // single-instance must come first; its deep-link feature forwards argv URLs to the running app
        .plugin(tauri_plugin_single_instance::init(|app, _argv, _cwd| {
            if let Some(w) = app.get_webview_window("main") { let _ = w.show(); }
        }))
        .plugin(tauri_plugin_deep_link::init())
        .manage(State::default())
        .manage(native_ws::Sockets::default())
        .manage(monitors::MonitorState::default())
        .manage(service_status::ServiceStatus::default())
        .manage(desktop_integration::DesktopIntegration::default())
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_process::init())
        .plugin(tauri_plugin_autostart::init(tauri_plugin_autostart::MacosLauncher::LaunchAgent, None))
        .on_menu_event(|app, ev| on_menu(app, ev.id().as_ref()))
        .on_window_event(|w, ev| {
            if let WindowEvent::Destroyed = ev { w.app_handle().state::<native_ws::Sockets>().close_window(w.label()); }
            if let (WindowEvent::Destroyed, "panel") = (ev, w.label()) {
                let _ = desktop_integration::panel_raised(w.app_handle(), false);
                let _ = w.app_handle().emit("cat-panel-window-closed", ());
            }
        })
        .setup(|app| {
            // register_all shells out (xdg-mime, update-desktop-database); keep it off the startup path.
            let h = app.handle().clone();
            std::thread::spawn(move || if let Err(e) = h.deep_link().register_all() { eprintln!("[deep-link] register_all failed: {e}"); });
            if let Ok(d) = app.path().app_log_dir() { let _ = std::fs::create_dir_all(d); }
            let layer = layer_ok();
            let b = WebviewWindowBuilder::new(app, "main", WebviewUrl::App("index.html".into()))
                .title("Wuwa Tappo")
                .transparent(true)
                .decorations(false)
                .shadow(false)
                .focusable(false)
                .initialization_script(COOKIES)
        .initialization_script(WS_SHIM)
                .initialization_script(PLATFORM)
                .initialization_script(include_str!("shim.js"))
                // Start fully click-through so a failed frontend can never lock the desktop.
                .on_page_load(|w, p| {
                    if let tauri::webview::PageLoadEvent::Finished = p.event() {
                        let regions = w.app_handle().state::<State>().active_regions.lock().unwrap().clone();
                        let _ = apply_input_regions(&w, &regions);
                    }
                });
            // layer surface: built hidden, configured, then shown; elsewhere keep the maximized always-on-top window
            let w = if layer { b.visible(false) } else { b.always_on_top(true).maximized(true) }.build()?;
            configure_webview(&w)?;
            if layer {
                layer_init(&w, "wuwatappo", desktop_integration::pet_layer(true))?;
                w.show()?;
            }
            {
                use gtk::prelude::WidgetExt;
                w.gtk_window()?.realize();
                set_input_shape(&w, &[])?;
            }
            monitors::watch(app.handle().clone())?;
            input::start(app.handle().clone());
            let desktop = app.handle().clone();
            std::thread::spawn(move || {
                if let Err(e) = desktop_integration::install(&desktop) { eprintln!("[desktop] KDE state bridge unavailable: {e}"); }
            });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            load_key_press_count, save_key_press_count, load_cumulative_click_count, save_cumulative_click_count,
            native_ws::native_ws_connect, native_ws::native_ws_send, native_ws::native_ws_close,
            set_main_hit_region, set_native_input_cursor, set_overlay_cursor, set_main_window_fullscreen_shell,
            apply_main_window_position, set_main_skip_taskbar,
            open_login_window, close_login_window, resize_login_window_to_content, open_about_window, close_about_window,
            open_oss_notice_window, close_oss_notice_window, open_update_window, close_update_window, get_app_update_detail,
            open_panel_window, close_panel_window, is_panel_window_open, set_panel_window_collapsed, window_ready, window_gate, raise_panel_window_above_pets,
            show_toast_overlay, hide_toast_overlay, sync_native_menu, popup_main_dock_context_menu,
            exit_app, exit_maintenance_app, write_app_log, get_track_os_info, service_status::check_app_update, get_installer_launch_locale,
            get_main_desktop_safe_area, get_main_taskbar_insets, get_boot_diagnostics,
            is_internal_testing_build, is_devtools_hotkeys_build, service_status::is_force_update_locked, service_status::has_maintenance_notice,
            service_status::get_maintenance_notice_payload, is_elevated_autostart_enabled,
            finish_main_window_monitor_switch, service_status::hide_main_after_maintenance_notice_ready, save_app_ui_locale,
            set_elevated_autostart, set_main_force_cursor_ignore, set_main_swallow_mouse, set_main_window_content_bounds,
            monitors::set_main_window_monitor_bounds, set_skip_panel_outside_click_close, monitors::sync_aux_windows_to_main_monitor,
            monitors::sync_main_window_to_current_monitor, monitors::get_main_monitor_snapshot, monitors::monitor_from_physical_point,
            monitors::begin_main_monitor_switch, monitors::end_main_monitor_switch, service_status::open_maintenance_notice
        ])
        .build(tauri::generate_context!())
        .expect("error while building WuwaTappo")
        .run(|app, event| {
            if let tauri::RunEvent::Exit = event {
                input::stop();
                app.state::<desktop_integration::DesktopIntegration>().stop();
            }
        });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[ignore = "requires live Plasma and WUWA_TEST_OUTPUT set to an output name"]
    fn reads_live_plasma_rectangle() {
        let name = std::env::var("WUWA_TEST_OUTPUT").expect("set WUWA_TEST_OUTPUT");
        let [_, _, width, height] = plasma_available_rect(&name).unwrap();
        assert!(width > 0.0 && height > 0.0);
    }

    #[test]
    fn menu_anchor_subtracts_selected_output_origin() {
        let origin = tauri::PhysicalPosition::new(-1920, 400);
        assert_eq!(relative_pointer(-1720.0, 500.0, origin, 2.0), LogicalPosition::new(100.0, 50.0));
        let origin = tauri::PhysicalPosition::new(3600, 0);
        assert_eq!(relative_pointer(3800.0, 100.0, origin, 2.0), LogicalPosition::new(100.0, 50.0));
    }

    #[test]
    fn plasma_global_rect_preserves_negative_origins_and_only_counts_panel_space() {
        for x in [-1707.0, 0.0, 1707.0] {
            let rect = [x, -100.0, 1707.0, 1005.0];
            assert_eq!(insets_from_rect([x, -100.0, 1707.0, 1067.0], rect), json!({"left":0,"top":0,"right":0,"bottom":62}));
        }
    }

    #[test]
    fn panel_resize_follows_the_pending_size_and_skips_noops() {
        // collapse then expand inside one frame, both starting from the stale 1100 or the pending size
        assert_eq!(panel_target(true, (1100.0, 760.0), 958.0, -142.0), Some((958.0, 760.0)));
        assert_eq!(panel_target(true, (958.0, 760.0), 1100.0, 142.0), Some((1100.0, 760.0)));
        // user-widened window keeps its extra width, never drops below the new minimum
        assert_eq!(panel_target(true, (1300.0, 900.0), 958.0, -142.0), Some((1158.0, 900.0)));
        assert_eq!(panel_target(true, (1000.0, 700.0), 958.0, -142.0), Some((958.0, 760.0)));
        // same state again: nothing to do unless the window is below its minimum
        assert_eq!(panel_target(false, (1200.0, 800.0), 1100.0, 142.0), None);
        assert_eq!(panel_target(false, (1000.0, 800.0), 1100.0, 142.0), Some((1100.0, 800.0)));
    }

    fn hit_region(update: bool, input: Vec<Rect>, passthrough: bool) -> HitRegion {
        HitRegion {
            regions: vec![Rect { left: 10.0, top: 20.0, width: 30.0, height: 40.0, member_id: None }],
            input_regions: input,
            update_native_input: update,
            window_passthrough: passthrough,
        }
    }

    #[test]
    fn explicit_empty_input_region_stays_click_through() {
        let mut cached = None;
        assert_eq!(hit_region(false, vec![], false).input_rects(&mut cached).len(), 1);
        assert!(hit_region(true, vec![], false).input_rects(&mut cached).is_empty());
        assert!(hit_region(false, vec![], false).input_rects(&mut cached).is_empty());
    }

    #[test]
    fn input_region_cache_survives_passthrough() {
        let mut cached = None;
        let input = hit_region(false, vec![], false).regions;
        assert!(hit_region(true, input, true).input_rects(&mut cached).is_empty());
        assert_eq!(hit_region(false, vec![], false).input_rects(&mut cached).len(), 1);
    }
}
