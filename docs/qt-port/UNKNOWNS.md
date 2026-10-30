# Qt port: residual unknowns

Tracked with REA (`list_unknowns`); ids are the REA unknown ids. "How to resolve" is the cheapest next step.

| Id | What | Why unresolved | How to resolve |
|---|---|---|---|
| U0 (**resolved for the overlay mode on nested KWin 6.7.5, virtual, software rendering**; real GPU/GNOME still open) | QtWebEngine painted nothing on native Wayland on the dev machine (Fedora Asahi aarch64, KDE Plasma 6.7, Honeykrisp). | Second pass, `WW_DEBUG_WINDOW=1 --debug-js`, native Wayland (`WAYLAND_DEBUG` shows `zwlr_layer_surface_v1` + mesa EGL dmabuf buffers attached and committed): `[probe-raf]` = **62** for both `main` and `about`, `visibilityState` visible, about DOM 3037 chars. Same 62 with `QT_QPA_PLATFORM=wayland`, `xcb`, `QT_QUICK_BACKEND=software` and `QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu-compositing`; so no flag is needed and no default was changed. Pixels were still never captured (no safe screenshot path), and the production full-output overlay (not the 900x700 debug window) was deliberately not launched on the live desktop. | Run the normal mode once on a throwaway session; if rAF is 0 only there, suspect the huge transparent full-output surface (try `WW_NO_LAYER=1`, then `--disable-gpu-compositing`). |
| U1 `unk_1859a8f2...` (**mostly resolved**; REA record kept `investigating` because the evidence is not REA Evidence) | Trigger of `cat-native-network-status-changed`. | Decompiled `FUN_140011da0` with Ghidra + a source-built aarch64 decompiler (RE-TOOLS.md): WinRT `NetworkInformation` connectivity level >= constrained internet, deduped, `{internetAccess}`; implemented with `QNetworkInformation`. Still unseen: the subscription call site and any initial emission. | Set `GHIDRA_INSTALL_DIR=/home/trilm/ww-re/ghidra_12.1.4_PUBLIC` for the REA MCP server, then `trace_feature` the event name to get REA Evidence. |
| U2 `unk_8773a106...` (high) | Behaviour on real desktops: dock menu popup parented to an unfocused layer surface, tray icon (SNI) on KDE/GNOME, touchpad taps, F7/F8/F9, X11 XShape input region, F8 with two monitors, Background-portal autostart, second-instance forwarding of `wuwa-buddy://`, GNOME (no layer-shell: fallback maximized window, always-on-top is not honoured by mutter). | No pointer/keyboard injection, second monitor, GNOME or X11 session in the build environment. Only the layer-shell protocol trace and unit tests were possible. | Checklist below. |
| U3 (**resolved**) | 7 commands called by the frontend but absent from the binary. | Evidence: 3 are inside `try/catch`, `get_boot_diagnostics` has a `null` fallback, and the 4 monitor commands are not in the pristine bundle at all (introduced by this repo's patches). Only `greet` and `get_taskbar_insets_at_point` exist in the binary without being used by the frontend (now answered by Qt). | none |
| U4 | Chromium vs WebKit: the `patches/frontend.json` WebKit workarounds were not re-validated. | Needs U0 first. | Visual pass over panel tabs, Synthesis slots, first-paint gate. |
| U5 | Window placement of ordinary windows on Wayland (the Rust shell used a KWin script); aux windows may open on the compositor's chosen output. Dialogs under layer-shell cannot be dragged. | Not ported; per-window shell integration needs QtWayland private API. | Port `place_kwin` or switch to per-window `QWaylandShellIntegration`. |
| U6 | Build without `-devel` packages: WebEngineQuick and LayerShellQt are linked by path and called through 2 hand-declared entry points (`QtWebEngineQuick::initialize`, `LayerShellQt::Window::get` + the QObject property system). `QtWebSockets`/`QtWebChannel` C++ headers are not used at all. Property names (`layer`, `anchors`, `exclusionZone`, `keyboardInteractivity`, `scope`) were confirmed from the Wayland trace on LayerShellQt 6.7.5 only. | Environment had runtime packages only. | CI with the real `-devel` packages (`find_package` is preferred automatically). |
| U7 | `install(DIRECTORY ../dist ...)` ships the unpacked frontend in the package. | Redistribution of Kuro's assets is the user's decision; the repo forbids committing them. | Packagers must build locally from the user's own installer. |

| U8 (**mostly resolved**) | `global-clap-mouse` guard. | Read from the listing: press outside the panel rect and the pet hit regions (FEATURES.md fourth pass). Qt uses `mouse-button` for the same presses. | Compare click counts with the original on Windows if byte-identical events matter. |
| U9 | Panel outside-click close needs an absolute pointer. | Only KDE Wayland (KWin script) and X11 give one; GNOME/Sway Wayland: panel does not close on outside press (the original does). | A GNOME shell extension or the panel losing keyboard focus; unverified. |
| U10 | U0 variants (debug window only): `QSG_RHI_BACKEND=opengl` and `=vulkan` -> `[probe-raf]` 61-62 (fine); `--disable-features=DmaBufVideoFrames,Vulkan` -> 62; `QTWEBENGINE_CHROMIUM_FLAGS=--ozone-platform=wayland` -> process trap (SIGTRAP) at start. | Pixels still not captured; full-output overlay never launched on the live desktop. | See U0. |

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

## Fourth pass (nested session, see VERIFICATION.md)

- **U0 overlay mode**: `kwin_wayland --virtual` (2 outputs 1280x720, offline netns, private HOME): full production overlay painted (pet, counter strip, About dialog, panel), `[probe-raf]` 62. Pixels were captured with spectacle (KWin ScreenShot2). Hardware GPU (Honeykrisp) path not re-run.
- **New bug A (fixed)**: F8 moved the logical monitor but the layer surface stayed on its first output (`get_layer_surface` was never re-issued; clicks at the new output hit nothing). Fix in `Windows::moveToScreen`: destroy, `setScreen`, reposition, show. Verified: new `get_layer_surface(..., wl_output#26, ...)`, pet on output 2, clicks counted there.
- **New bug B (fixed)**: the dock QMenu became a full-output layer surface (anchor 15, size 0x0) with the menu drawn at the corner of the first output. `MenuLayer` in `tray.cpp` anchors it top-left with margins = its global position. Verified: menu at the pointer, item "Gallery" opened the panel on the Gallery tab. Submenu hover (`Desktop Pet Size`) was **not** observed to open with injected absolute motion; menu does not close on an outside press (Escape does).
- **New suspicion C (not fixed)**: a second launch inside the same nested session (tray "Restart" does exactly this) received no F7/F8/F9 although the first launch did. Likely stale `wuwatappo-pointer` KWin script / kglobalaccel registration; not isolated.
- U2 items now observed in the nested KWin: second-instance forwarding of `wuwa-buddy://` (`deep-link://new-url` + `get_current`), autostart file written and removed, click-through (a click outside the pet maximised a KCalc window below), F9 hides/shows the pet via the KWin shortcut, panel open/close, dock menu popup. Still needing hardware: SNI tray (no StatusNotifierWatcher in the nested bus), real touchpad, evdev counters and the outside-press panel close (global input cannot be injected without creating a real uinput device on the live desktop; `/dev/input` was masked in the sandbox), GNOME/mutter, a real GPU, the X11 session with a compositing manager, WebSocket against a TLS endpoint.
- **REA**: provider still `not_configured`. Exact change: in `~/.claude.json` (or the project `.mcp.json`) add to the `rea` server `"env": {"GHIDRA_INSTALL_DIR": "/home/trilm/ww-re/ghidra_12.1.4_PUBLIC"}` and restart the MCP client. Not applied (user config). REA records were therefore not updated.
