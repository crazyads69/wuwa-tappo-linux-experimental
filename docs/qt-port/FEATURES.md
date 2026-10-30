# Qt port: feature map

Native Qt 6 shell in [`qt/`](../../qt). The frontend (`dist/`, unpacked from your own installer by `scripts/setup.sh`)
runs **unmodified** in QtWebEngine; `qt/js/tauri.js` provides `window.__TAURI_INTERNALS__` over a QWebChannel to the
C++ `Backend`, so the ~60 backend commands, events and plugin calls of the original are re-implemented natively.
The Rust shell in `src-tauri/` stays the behavioural spec and still builds.

Status legend: **done** = implemented and exercised (unit test, protocol trace or run log, named in the last column);
**impl** = implemented but not exercised on real hardware/desktop (see [UNKNOWNS.md](UNKNOWNS.md)); **partial**;
**none** = intentionally absent.

## How the original was enumerated

`WuwaTappo-global_1.0.1_x64-setup.exe` (sha256 `31105cc8...3da12`) is a Tauri *installer* that carries a nested NSIS
package after the marker `:ENDWUWAPAYLOAD`. Both layers were unpacked with `7z` into fresh directories (nothing was
executed), the app binary `WuwaTappo.exe` (sha256 `2de67b82...c6f2`, Tauri 2.10.3) opened with REA
(`open_binary`, `inspect_artifact`, `inspect_desktop_application`, `inspect_rust_binary`; no deep provider such as
Ghidra/IDA is configured here, so REA gave identity and inventory only) and its read-only data mined with `strings`.
The frontend bundle was then searched for what it actually invokes.

| Surface | Original (evidence) |
|---|---|
| Framework | tauri 2.10.3, wry 0.54.4, tao 0.34.6, tray-icon 0.21.3, windows 0.61.3, rdev (strings) |
| Plugins | autostart 2.5.1, deep-link 2.4.9, log 2.8.0, opener 2.5.3, single-instance 2.4.2 (strings; `dialog`/`process` are only in this repo's capability file) |
| Config | identifier `com.kuro.wuwatappo.global`, `windows: []` (all windows built at runtime), `csp: null`, deep-link scheme `wuwa-buddy` (`tauri.conf.json`) |
| Windows | labels `main` (overlay), `panel`, `login`, `about`, `oss-notice`, `update`, `toast` (strings + frontend routes) |
| Tray | id `main-tray`; items `dock_bag/atlas/exchange/room/settings/window_passthrough/window_opacity_{25..100}/pet_scale_{50..200}/toggle_pet/restart/quit` (strings) |
| Global input | `SetWindowsHookExW` `WH_KEYBOARD_LL` / `WH_MOUSE_LL`, `RegisterHotKey`, `SetWindowDisplayAffinity` (imports) |
| Commands | The app command names form one rodata blob (`greet` ... `set_elevated_autostart`, see [RE-TOOLS.md](RE-TOOLS.md)). Frontend-invoked names absent from the binary: `get_installer_launch_locale`, `is_internal_testing_build`, `is_devtools_hotkeys_build` (all three called inside `try/catch`, so the original degrades by ignoring the error) and `get_boot_diagnostics` (wrapped with a `null` fallback). `begin_main_monitor_switch`, `end_main_monitor_switch`, `get_main_monitor_snapshot`, `monitor_from_physical_point` are **not** original at all: they come from this repo's `patches/frontend.json` (the pristine bundle only calls `finish_main_window_monitor_switch`). Binary-only names: `greet` (template demo) and `get_taskbar_insets_at_point`, both now answered by the Qt Backend |
| Events (backend -> page) | `cat-app-update-available`, `cat-force-update-lock`, `cat-installer-download-progress`, `cat-native-network-status-changed`, `cat-panel-window-closed`, `dock-open-panel`, `tray-set-pet-scale`, `tray-set-window-opacity`, `tray-toggle-pet-visibility`, `tray-toggle-window-passthrough`, `tray-restart`, `tray-quit`, `update-modal-open`, `maintenance-notice-open`, `mouse-move`, `mouse-button`, `key-press`, `key-release` |
| Tauri plugin IPC used by the frontend | `window` (set_*/is_*/position/size/monitors/start_dragging...), `webview`, `event`, `app`, `path`, `opener`, `dialog`, `autostart`, `process`, `deep-link`, `updater`, `image`, `resources` (grep of `dist/assets`) |

The Windows-only pieces (hooks, display affinity, elevated-autostart scheduled task, installer download progress,
WebView2 flags) have no Linux counterpart and are listed as such.

## What the full dump added (second pass)

- **Assets**: 133 embedded files (`tauri-dumper` and a structural table walk agree). Against `dist/`: five JS/CSS files plus
  `index.html` differ (exactly the patches in `patches/frontend.json`, `patch_frontend.py verify` passes), three
  files are repo-generated (Latin font subsets + OFL text), and **two assets were missing from `dist/`** because the
  old name-based extractor skipped them: `assets/emote_happy-DbeXE30w.png` (referenced by `index-ULLd3gpz.js`; the happy
  emote 404ed) and `kurodata-browser/kurodata.esm.min.d.ts`. `scripts/extract_frontend.py` now walks the table; both files restored in the local `dist/`.
- **Manifest**: the original exe asks for `requireAdministrator` (explains the "elevated autostart" scheduled task);
  version resource: CompanyName `kuro`, ProductName `Wuwa Tappo`, 1.0.1.
- **Updater**: endpoint `https://kurowuwabuddy2602.test.modiarts.com/app/json-host/latest-global_host.json` (a *test*
  host) and minisign public key id `2DB584868D18BB49` are compiled in; verification via `minisign-verify` 0.2.5.
  Qt never contacts it (`check_app_update` errors by design).
- **Crates** (84, via `rbi` and path strings): tauri 2.10.3, wry 0.54.4, tao 0.34.6, muda 0.17.1, tray-icon 0.21.3,
  tokio 1.50.0, reqwest 0.12.28 and 0.13.2, hyper 1.8.1, rustls 0.23.40, webview2-com 0.38.2, windows 0.61.3; rustc commit
  `59807616e1fa...`; not cargo-auditable.
- **Tray/dock menu**: besides the items already mapped, the original has `dock_check_app_update` (not in Qt: its
  action has no Linux meaning without a feed). Native log strings: `left-press over-system-menu=`, `left-press matches-menu-anchor; closing`
  (the original closes the context menu on a global left press that matches the menu anchor), `[hotkey] LL hook`,
  `[hotkey] RegisterHotKey loop`, `[overlay-cursor] hook failed`, `[input] grab failed, fallback listen` (rdev).
- **Events**: the frontend also listens to `global-clap-mouse` (string present in the binary, emitter not located; neither
  shell emits it, the frontend counts clicks through `mouse-button` anyway) and `dock-context-menu-debug` (debug payload, console only).

## Feature -> Qt implementation -> status

| # | Original feature | Qt implementation | Status |
|---|---|---|---|
| 1 | Transparent frameless window, always on top, no taskbar, no focus | `Windows::create` (`src/windows.cpp`), `qml/Page.qml` (transparent `Window` + `WebEngineView`); layer-shell overlay via LayerShellQt, fallback maximized `WindowStaysOnTopHint` window | **done** on KDE Wayland: `WAYLAND_DEBUG` shows `zwlr_layer_surface_v1` layer 3, anchor 15, exclusive zone -1, keyboard none. Fallback path **impl** |
| 2 | Click-through outside pet/buttons (`set_main_hit_region`) | `HitRegion`/`toPixelRects` (`src/regions.cpp`, tested), `platform::setInputRegion` (Wayland `set_input_region`, X11 XShape input) | **done** (Wayland trace: off-surface 1x1 region at start, then the pet rectangles after the frontend reports them; tests for the cache semantics). X11 **impl** |
| 3 | Pet clicks/moves inside regions -> `mouse-*` events | `js/pet.js` (copy of `src-tauri/src/shim.js`) + `__ww_regions` event | impl (page side runs; no pointer injection available) |
| 4 | Global keys, buttons, wheel counting | `InputMonitor` + `EvdevThread` (`src/inputmonitor.cpp`), `KeyState` (tested) | impl (needs `input` group; no synthetic device was created) |
| 5 | Touchpad tap-to-click, held 100 ms so the pet animates | `Tap` state machine (`src/inputlogic.cpp`, 9 tests), tap hold in `InputMonitor::deliverButton` | logic **done**, hardware **impl** |
| 6 | Absolute pointer position | KWin script over D-Bus (KDE Wayland), `QCursor::pos()` poll (X11) | impl; GNOME/Sway Wayland: **none** (DOM-derived moves only, as in the Rust shell) |
| 7 | F7/F8/F9 global hotkeys | KWin `registerShortcut` + `kglobalaccel` ownership check, evdev dedupe (`Hotkeys`, tested) | impl |
| 8 | Tray icon + dock menu (`sync_native_menu`) | `Tray` (`src/tray.cpp`): QSystemTrayIcon (StatusNotifierItem), same QMenu popped at the pet (`popup_main_dock_context_menu`) = fallback when there is no tray | impl; menu rebuild/`nearest` logic tested |
| 9 | Auxiliary windows login/about/oss-notice/update/panel, show-after-ready gate | `Backend::aux/present/reveal` (200 ms settle, 2.5 s fallback, `window_ready`/`window_gate`) | **done**: about page loaded and rendered DOM (3037 chars) through the real IPC. Under layer-shell they are centered anchor-less overlay surfaces (cannot be dragged) |
| 10 | Panel collapse/expand without native resize | `panelRect` + `applyPanelShape` (same design as commit deb2883) | logic **done** (tested), visual **impl** |
| 11 | Toast overlay never takes input | `show_toast_overlay`: layer surface, empty input region, never unmapped in layer mode | impl |
| 12 | Multi-monitor: selection, F8 switch gate, aux placement, output removal | `Backend::selectMainMonitor/currentScreen`, `SwitchGate` (tested), `QScreen` signals; aux windows follow the pet via `QWindow::setScreen` | partial: KWin placement script for ordinary windows is not ported; real second monitor untested |
| 13 | Panel insets / auto-hide edge / foreground fullscreen | `Desktop` (`src/desktop.cpp`): async `StrutManager`, plasmashell scripting, KWin script; pure parts in `regions.cpp` (tested) | impl (KDE only; zeros elsewhere, same as Rust shell) |
| 14 | Multiplayer WebSocket (Origin must be absent) | `WsClient`/`WsHub` (`src/wsclient.cpp`) on `QSslSocket` + `ws::Parser` (`src/wsframe.cpp`); page side `js/ws.js` reused verbatim | **done** against a local RFC 6455 server (no Origin/cookie, ping/pong, close, 403) and allow-list tests; live Kuro server **not** contacted |
| 15 | Counters (`load/save_*_count`) | `Store` JSON file in `~/.local/share/com.kuro.wuwatappo.global/store.json` (shared with the Tauri shell) | **done** (tested incl. atomic write) |
| 16 | Maintenance notice (HTTP 601) | `sanitizeNotice` + `open_maintenance_notice` & co. | **done** (tested); real 601 response never captured |
| 17 | Updates | `check_app_update` -> explicit error (no signed Linux feed) | **none** by design |
| 18 | Single instance + deep link `wuwa-buddy://` | `QLocalServer` in `main.cpp`, `Backend::handleDeepLink` (`deep-link://new-url`, `plugin:deep-link\|get_current`); desktop file `MimeType=x-scheme-handler/wuwa-buddy` | impl (second-instance forward not run) |
| 19 | Autostart | `platform::setAutostart`: XDG `~/.config/autostart/*.desktop`, or `org.freedesktop.portal.Background` inside Flatpak/Snap | impl |
| 20 | Opener (URL / log dir), dialogs, process restart/exit, app info, path | `Backend::plugin` (only http/https/mailto; only `$APPLOG`) | impl |
| 21 | Window plugin surface (monitors, scale, position, size, drag, focus...) | `Backend::windowPlugin`; physical px = logical x dpr like GDK | partial: placement is a no-op on Wayland; `set_effects`, `set_shadow`, `set_content_protected`... accepted without effect |
| 22 | Cookies | Not needed: stable `http://127.0.0.1:47821` origin on QtWebEngine's persistent default profile | impl |
| 23 | Static frontend serving | `HttpServer` (`src/httpserver.cpp`): GET/HEAD only, Host pinned (DNS rebinding), no `..`/symlink escape, JS module MIME | **done** (tested incl. over TCP) |
| 24 | Packaging | `qt/data/*.desktop`, `*.metainfo.xml`, CMake `install()`, `qt/packaging/com.kuro.wuwatappo.global.yaml` (Flatpak sketch) | desktop/metainfo/CMake install **done**; Flatpak/AppImage **not built** |
| 25 | Windows-only: hooks, display affinity, elevated autostart, installer progress, WebView2 | no-op commands (`set_elevated_autostart`, ...) | **none** |
| 26 | `cat-native-network-status-changed` | not emitted; the frontend health-checks `/api/common/activity-config` itself | **none** (UNKNOWNS U1) |

## Deliberate differences from the Tauri shell

- **Web engine**: Chromium (QtWebEngine) instead of WebKitGTK. The WebKit-specific frontend patches in `patches/` are
  harmless but unverified under Chromium.
- **No custom `tauri://` scheme**: QtWebEngine's scheme API needs headers that are not always packaged, so `dist/` is
  served from a loopback HTTP server on a fixed port (47821, any free port as fallback).
- **WebSocket**: QtWebSockets headers were not installed either; ~300 lines of RFC 6455 are tested instead.
- **Layer-shell**: LayerShellQt's Qt integration turns *every* window into a layer surface, so dialogs are centered
  overlay surfaces. A per-window shell integration (QtWayland private API, as Quickshell does) would restore
  draggable xdg toplevels.
- **Telemetry**: the frontend itself posts analytics to `mp-gb-sdklog.kurogames.net` (visible in its console); neither
  shell blocks it.
