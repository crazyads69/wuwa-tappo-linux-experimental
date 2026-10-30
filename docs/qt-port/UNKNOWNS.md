# Qt port: residual unknowns

Tracked with REA (`list_unknowns`); ids are the REA unknown ids. "How to resolve" is the cheapest next step.

| Id | What | Why unresolved | How to resolve |
|---|---|---|---|
| U0 `unk_26ebc2cb...` (critical) | **QtWebEngine paints nothing on native Wayland on the dev machine** (Fedora Asahi aarch64, KDE Plasma 6.7, Mesa Honeykrisp/Asahi GL). The page loads, JS and IPC work (boot log, `about` DOM with 3037 characters), `visibilityState` is `visible`, but `requestAnimationFrame` never fires and the window shows only its Qt Quick background. Under XWayland (`QT_QPA_PLATFORM=xcb`) rAF runs at ~62 fps. A bare `WebEngineView` in the stock `qml` tool also showed no content. | Tried: `QSG_RHI_BACKEND=opengl`, software Quick, `--disable-gpu`, `--disable-gpu-compositing`, ANGLE/ozone flags, render loops, integer scaling, window flags, opaque background. No cause found; no GDB/debug symbols here. `grabWindow`/`grabToImage` cannot capture WebEngine content, so **no pixel of the frontend was ever observed**. | Run `wuwatappo-qt --debug-js` and read `[probe-raf]` on another machine (x86_64 KDE, GNOME, Sway). If rAF is 0 only on Asahi it is a Qt/Mesa stack issue (try `QT_QPA_PLATFORM=xcb`). Otherwise attach a debugger to `QQuickWebEngineView` rendering. |
| U1 `unk_49a89b8d...` | Trigger and source of `cat-native-network-status-changed` in the original. Known: payload is `{internetAccess: boolean}` and the frontend falls back to its own fetch health check. | REA has no deep provider (Ghidra/IDA/Hopper/Kuna not configured), so the emitter could not be followed from the string. | Configure a provider, `search_strings` -> `xrefs` on the event name in `WuwaTappo.exe`. Likely Windows network-list-manager based. |
| U2 `unk_8773a106...` (high) | Behaviour on real desktops: dock menu popup parented to an unfocused layer surface, tray icon (SNI) on KDE/GNOME, touchpad taps, F7/F8/F9, X11 XShape input region, F8 with two monitors, Background-portal autostart, second-instance forwarding of `wuwa-buddy://`, GNOME (no layer-shell: fallback maximized window, always-on-top is not honoured by mutter). | No pointer/keyboard injection, second monitor, GNOME or X11 session in the build environment. Only the layer-shell protocol trace and unit tests were possible. | Checklist below. |
| U3 `unk_b146d206...` (low) | Argument/return schemas of 7 commands absent from the original binary's strings (see FEATURES.md). Their shapes were copied from the Rust shell, which derived them from the frontend. | Names are probably built from fragments in rodata; needs a decompiler. | Deep provider on `WuwaTappo.exe`, or capture the frontend's calls (`WW_TRACE_IPC=1` prints command names). |
| U4 | Chromium vs WebKit: the `patches/frontend.json` WebKit workarounds were not re-validated. | Needs U0 first. | Visual pass over panel tabs, Synthesis slots, first-paint gate. |
| U5 | Window placement of ordinary windows on Wayland (the Rust shell used a KWin script); aux windows may open on the compositor's chosen output. Dialogs under layer-shell cannot be dragged. | Not ported; per-window shell integration needs QtWayland private API. | Port `place_kwin` or switch to per-window `QWaylandShellIntegration`. |
| U6 | Build without `-devel` packages: WebEngineQuick and LayerShellQt are linked by path and called through 2 hand-declared entry points (`QtWebEngineQuick::initialize`, `LayerShellQt::Window::get` + the QObject property system). `QtWebSockets`/`QtWebChannel` C++ headers are not used at all. Property names (`layer`, `anchors`, `exclusionZone`, `keyboardInteractivity`, `scope`) were confirmed from the Wayland trace on LayerShellQt 6.7.5 only. | Environment had runtime packages only. | CI with the real `-devel` packages (`find_package` is preferred automatically). |
| U7 | `install(DIRECTORY ../dist ...)` ships the unpacked frontend in the package. | Redistribution of Kuro's assets is the user's decision; the repo forbids committing them. | Packagers must build locally from the user's own installer. |

## Manual checklist (needs eyes on real KDE / GNOME / X11)

1. `wuwatappo-qt --debug-js`: `[probe-raf]` > 30 for every window, then the pet is visible and clicks outside it reach
   the apps below (KDE Wayland, GNOME, X11).
2. Right-click the pet: dock menu appears at the pointer; tray icon present on KDE and on GNOME with AppIndicator.
3. Counters rise on key presses/clicks elsewhere (user in the `input` group); touchpad tap plays the pose.
4. F8 on two monitors moves the overlay; unplug the second monitor: overlay recovers.
5. Panel collapse/expand: window keeps its size, strip beyond the card is click-through.
6. Login in the browser, `wuwa-buddy://` callback reaches the running instance.
7. Multiplayer: socket connects (Origin-less); room create/join (changes server data: use a throwaway account).
8. Autostart on/off in a normal session and in Flatpak.

## Side effects of the verification runs

The frontend sends its own analytics (`mp-gb-sdklog.kurogames.net`) and one `GET /api/common/activity-config` health
check on every launch. About ten short smoke runs therefore produced such requests, exactly as the official app and the
Tauri shell do. No credentials were used, no login was attempted.
