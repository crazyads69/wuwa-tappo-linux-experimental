# Qt port: feature map

Native Qt 6 shell in [`qt/`](../../qt). The frontend (`dist/`, unpacked from your own installer by `scripts/setup.sh`)
runs **unmodified** in QtWebEngine; `qt/js/tauri.js` provides `window.__TAURI_INTERNALS__` over a QWebChannel to the
C++ `Backend`, so the ~60 backend commands, events and plugin calls of the original are re-implemented natively.
The earlier Rust/Tauri shell (`src-tauri/`, removed from the tree after this port; see git history) was the behavioural spec for the first passes.

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
- **Tray/dock menu**: the earlier note that the original has a `dock_check_app_update` item is **withdrawn**: the literal exists in rodata
  but is the unreferenced longer string of which `check_app_update` (the one used, from `0x140210502`) is the tail; no code and no pointer reference it
  (raw RIP-relative scan plus pointer scan). The menu ids are the contiguous blob `dock_bag ... dock_quit` and the `dock_window_opacity_*` / `dock_pet_scale_*` lists. Native log strings: `left-press over-system-menu=`, `left-press matches-menu-anchor; closing`
  (the original closes the context menu on a global left press that matches the menu anchor), `[hotkey] LL hook`,
  `[hotkey] RegisterHotKey loop`, `[overlay-cursor] hook failed`, `[input] grab failed, fallback listen` (rdev).
- **Events**: the frontend also listens to `global-clap-mouse` (emitter found with Ghidra: the Windows handler that, on a global left press, resolves `WindowFromPoint`/`GetClassNameW` and closes the native popup menu; Qt's popup does not need it and clicks are counted via `mouse-button` anyway) and `dock-context-menu-debug` (debug payload, console only).

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
| 26 | `cat-native-network-status-changed` | `Backend` ctor: `QNetworkInformation::reachabilityChanged` -> `NetStatusDedupe` (`src/regions.h`, tested) -> `{internetAccess}`; no backend plugin -> not emitted | logic **done** (tested); event source **impl** (needs a QNetworkInformation plugin, not observed firing) |
| 27 | Left press outside the open panel closes it (`cat-panel-window-closed`), `set_skip_panel_outside_click_close` | `Backend::closePanelOnOutsidePress` + `panelOutsideClose` (tested), pointer from `InputMonitor::pointer()` | logic **done** (tested); needs a pointer source (KDE Wayland / X11), otherwise inert |
| 28 | F7/F8/F9 press debounce 280 ms per key | `Hotkeys::raw` (`src/inputlogic.h`, tested) | **done** |
| 29 | Foreground fullscreen = covers >= 98 % of the output within 8 px, borderless, not shell window | KWin script in `desktop.cpp` (`cover()`), `w.fullScreen` kept | **impl** (script syntax untested on a live KWin) |

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

## Binary evidence (third pass, Ghidra 12.1.4 + natively built aarch64 decompiler)

Addresses are image VAs in `WuwaTappo.exe` (sha256 `2de67b82...c6f2`). "Decompiled" = read as pseudo-code; "disasm" = read as
listing because Ghidra had no function there; "inferred" = not seen in code. Re-run with `scripts/re/` (see RE-TOOLS.md).

| Behaviour | Binary evidence | Qt | Verdict |
|---|---|---|---|
| Network status event | `FUN_140011da0` (decompiled): WinRT `NetworkInformation` connection profile, connectivity level `(l & ~1) == 2` (ConstrainedInternetAccess or InternetAccess) -> bool; static `DAT_144999000` (initially false) compared and updated, unchanged -> return; else emit `cat-native-network-status-changed` `{internetAccess}`. The strings `Windows.Networking.Connectivity.NetworkInformation` and `[network] Windows NetworkStatusChanged subscription failed:` identify the trigger. U1 resolved. Not seen: the subscribe call and whether it emits once at start | `Backend` ctor, `NetStatusDedupe` | trigger + payload + dedupe **verified**; initial emission **inferred** (none) |
| Popup (native menu) close on global press | `FUN_1400236c0` (decompiled): under a mutex, on a global press `GetCursorPos`; for button 0 `WindowFromPoint` + `GetClassNameW`, class `#32768` (Win32 menu) is left alone; otherwise `FindWindowW(L"#32768")` and, if a menu exists, `PostMessageW(WM_CANCELMODE)` + `WM_KEYDOWN VK_ESCAPE`, then logs `dock-context-menu-debug` ("left-press matches-menu-anchor; closing", "cancel-target hwnd=", "cancel-posted=", "escape-posted="). Called from the mouse-hook closure (`0x140013f62`..`0x140013fe8`) for buttons 0 and 1 | none needed: `QMenu` dismisses itself on an outside press (X11/Wayland popup grab) | Windows-only by design; Qt behaviour **not observed** on a live desktop |
| `dock-context-menu-debug` | payload built in `FUN_1400236c0` and `FUN_140019900` (string pairs `pre-popup`, `popup-return ok=`, `menu-command id=`) | frontend logs it to the console only; not emitted | debug only, **verified** |
| `global-clap-mouse` | disasm `0x14001510f`/`0x14001515b`/`0x1400151a1` (event name, length 0x11) inside the mouse closure that also emits `mouse-button` (`0x140013de0`, `"mouse-button"` len 12). Branch: `FUN_14000fba0` (pointer inside the panel rect), `FUN_140291e80` (pointer inside the native hit regions, `GetCursorPos` + `0x144999198`) and a flag; when none holds the press is sent as `global-clap-mouse` with `{button, "press"}`. Frontend handler (`index-CHhF-M4P.js`): `increment local click count` unless `Me()`/`Cn` | `InputMonitor::noteDelivered` already gives "a press the DOM saw is not counted again; one it did not see is counted once", counted through `mouse-button` | equivalent effect **inferred**; exact guard flags of the branch **not** decompiled |
| Panel closes on outside left press | `FUN_140021570` (decompiled): `panel exists` (`FUN_14000eea0`, label length 5) `&& !DAT_14499a600 && now_ms >= DAT_14499a620 && !FUN_14000fba0(pointer in panel rect)` -> `FUN_140019230`: closes the panel and emits `cat-panel-window-closed` (len 0x17). Called for a left press (button byte 0) when the press is not over the pet. The frontend never calls `set_skip_panel_outside_click_close` | `Backend::closePanelOnOutsidePress`; skip flag set by that command; `DAT_14499a620` writer not found, treated as no grace period | logic **verified** vs binary; **new in this pass** (was a no-op); the grace deadline **inferred** |
| Hotkeys | `FUN_14028f860` (decompiled): hidden message window class `DesktopCatHotkeyMsgWnd` (`HWND_MESSAGE`), `RegisterHotKey(id 0x138f/0x1390/0x1391, mods 0, vk 0x76/0x77/0x78 = F7/F8/F9)`, `GetMessageW` loop, `WM_HOTKEY` -> `FUN_14028f270`: per-key 280 ms debounce then `FUN_14028f420("F7"|"F8"|"F9")`. Fallback `WH_KEYBOARD_LL`. If no key registers: `no hotkeys registered` | `Hotkeys` (debounce added) | **verified**; what `FUN_14028f420` emits exactly (press only?) **not** decompiled |
| Insets / auto-hide / fullscreen | `FUN_140011f80` (decompiled): `SHAppBarMessage(ABM_GETSTATE)`, `Shell_TrayWnd` / `Shell_SecondaryTrayWnd` (`FindWindowExW`), tray windows within 16 px of a monitor edge -> `autoHideEdge` `left/right/top/bottom` by smallest distance; fullscreen: foreground window visible, not iconic, other process, not a shell window (`FUN_14005cd90`), same monitor as the pet, `GWL_STYLE & 0xC40000 == 0`, `DWMWA_EXTENDED_FRAME_BOUNDS` (fallback `GetWindowRect`) within 8 px of the monitor and intersection area x 100 >= monitor area x 98 | `Desktop` (KDE), KWin script updated to the same borderless 98 %/8 px rule | rule **verified**; KWin implementation **not run** |
| Aux window sizes | `FUN_140010270` callers: `about` 880x720 (`FUN_140005dd0`), `oss-notice` 880x720 (`FUN_14000c9f0`), `update` 580x600 (setup `0x140209ab0`); titles `关于Wuwa Tappo`, `开源组件声明`, `强制更新` | `Backend::aux/simple` | **verified** (matches) |
| Monitor switch gate | `FUN_14001d3d0` strings `monitor switch already in progress`, `invalid monitor bounds` | `SwitchGate`, `finish_main_window_monitor_switch` | strings **verified**, 15 s lease / 280 ms numbers are the Rust shell's, **not** re-read here |
| Elevated autostart | strings `WuwaTappo\ElevatedAutostart`, `WuwaTappoGlobal\ElevatedAutostart` (scheduled task), `FUN_14021d620` | no-op | Windows-only |
| Dock menu items | blob `dock_bag dock_atlas dock_exchange dock_room dock_toggle_pet dock_settings dock_window_passthrough dock_dock_restart dock_quit`, `dock_window_opacity_{25,50,75,100}`, `dock_pet_scale_{50,75,100,125,150,200}`; menu built in `FUN_140218ba0`, popped in `FUN_140221fc0` | `Tray` | ids **verified**; labels/order not re-read |
| Not traced this pass | window creation flags for `main`/`panel`/`login`/`toast` (inlined `WebviewWindowBuilder`), single-instance callback (`second launch received pid=` has no code reference), store/maintenance function bodies (`FUN_14000afa0`, `FUN_140222c00`), deep-link, WebSocket/Origin (done in the Tauri shell by behaviour, not from the exe), `get_taskbar_insets_at_point` body (`FUN_14021d160`) | unchanged | still **inferred** |

## Binary evidence (fourth pass: closures read through `scripts/re/DecompForce.java`, `Qw.java`)

`DecompForce.java` creates a function where Ghidra left none, then decompiles; `Qw.java` reads vtable qwords. Addresses are VAs in `WuwaTappo.exe`.

| Item | Binary evidence | Qt file | Verdict |
|---|---|---|---|
| Hotkey emit | `FUN_14028f420(key)` queues a boxed closure (vtable `0x1445dfad0`, call `0x14018c980`) that emits the event named by the 9 bytes at `0x1445dbfb1` = **`key-press`** with the key string as payload (`"F7"/"F8"/"F9"`). Only the press is emitted on the `RegisterHotKey` path; no release. Per-key 280 ms (`0x118`) debounce in `FUN_14028f270` (state `DAT_14499a630/38/40`), confirmed | `Hotkeys::raw`, `InputMonitor::emitHotkey` (`src/inputlogic.h`, `src/inputmonitor.cpp`) | press+debounce **verified**. Qt additionally emits `key-release` (the Rust-shell behaviour, harmless: the frontend only compares F5/F7/F8/F9 names on press) - deliberate difference, not changed |
| `global-clap-mouse` | Mouse-hook closure `0x140013de0..0x1400153bd` (listing `0x140014f3b..0x140015120`): for a **press** of any button: left only -> `FUN_1400236c0` (menu close) then the panel-outside-close check (`FUN_140021570`); then `inPanel = FUN_14000fba0(pt)`, `inHit = FUN_140291e80()` (+ `FUN_14005cf70` on the cursor-pos result); if `inPanel \| inHit \| r14b` the press is skipped (the DOM sees it), otherwise it emits **`global-clap-mouse`** `{button, "press"}` (`0x14001510f`, name len 0x11). Frontend: `incrementLocalClickCount` unless `Me()`/`Cn` | `InputMonitor::noteDelivered` + `mouse-button native:true` | guard **identified** (press outside panel rect and outside the pet hit regions); Qt emits `mouse-button` instead of `global-clap-mouse` for the same presses, same counting effect (frontend `mouse-button` press branch also calls `incrementLocalClickCount`), not byte-identical |
| `sync_native_menu` | `FUN_140218ba0` parses args `labels`, `petDockVisible`, `petScale`, `windowOpacity`, `windowPassthrough` (all from the frontend); `FUN_140005240` stores them; opacity/scale are snapped to the nearest preset ({25,50,75,100}, {50,75,100,125,150,200}) | `Tray::build` | labels **come from the frontend i18n** (nothing to re-read in the binary); order and ids follow the Rust shell, the id blob is contiguous in rodata; popup args `menuState`, `passthrough`, `x`, `y` (`FUN_140221fc0`, builder `FUN_140019900`) |
| `get_taskbar_insets_at_point` | `FUN_14021d160`: `MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST)`, `GetMonitorInfoW`, then per edge `max(0, work - monitor)` (left, top, right, bottom as doubles) | `Backend` command (nearest screen to the physical point, then `Desktop::insets`) | **implemented from the decompile**; no unit test (lambda over QScreen), exercised only by build |
| Window creation flags, single instance, store/maintenance bodies, WebSocket Origin, autostart/elevation, timers | `FUN_140010270` is a generic WebviewWindowBuilder wrapper (no per-label flags visible); the flag values are set in callers via inlined builder code that was **not** traced; single-instance callback string has no code xref; store and maintenance bodies not decompiled | `Windows`, `Backend` | still **inferred** (sizes 880x720/580x600 are verified, see third pass) |
| Network event at startup | The subscription call site was not found; the emitter only fires on change vs `DAT_144999000` (initially false) | `Backend` ctor | initial emission still **inferred** (none) |
