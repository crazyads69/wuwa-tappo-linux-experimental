# Qt port: verification matrix

Honest status. "Original" = evidence in `WuwaTappo.exe` (see FEATURES.md for addresses). "Nested" = a throwaway session:
`unshare -rn` (loopback only, **no network**) + private `HOME`/`XDG_RUNTIME_DIR` + `dbus-run-session` + `kwin_wayland --virtual
--output-count 2 --width 1280 --height 720` (KWin 6.7.5, software rendering, aarch64), `/dev/input` masked, input injected with
`scripts/nest/fake_input.c` (KWin `org_kde_kwin_fake_input`), page state read with `scripts/nest/cdp.py` (DevTools on loopback),
pixels from `spectacle -b -f` (KWin ScreenShot2). Screenshots were read by eye; they contain Kuro assets and are not committed.
Unit tests: `ctest` = 6 suites, 73 test functions, all pass (`tst_httpserver` 4, `tst_input` 13, `tst_regions` 16, `tst_store` 16, `tst_wsclient` 6, `tst_wsframe` 18).

| Feature | Original binary | Qt implementation | How verified | Result |
|---|---|---|---|---|
| Overlay renders (U0) | n/a | layer-shell main window + QtWebEngine | nested screenshot x many | pass: pet with two chest bubbles, counter strip, menu button painted; `[probe-raf]` 62. Software rendering only |
| About dialog first run, closes on Confirm | window 880x720 (verified) | `Backend::aux` | nested: injected click on Confirm | pass: dialog centred on output 1, closed by the click |
| Pet click counting | frontend | DOM path (`pet.js`) | nested: clicks via fake_input, counter read from pixels | pass: 3 clicks 1 s apart = +3; 3 rapid clicks = +2 (frontend throttling, original not compared) |
| Input region / click-through | `set_main_hit_region` | `setInputRegion` | nested: click outside the pet maximised a KCalc window beneath | pass |
| Panel open / close | outside-press close verified in binary | `panelRect`, X button | nested: pet menu button opened the panel (Backpack tab with items); X closed it | pass for open/X; **outside-press close not verified** (needs global pointer events) |
| Dock menu | `sync_native_menu`/`popup_main_dock_context_menu` args, labels from frontend | `Tray` + new `MenuLayer` | nested: right-click, screenshot, click "Gallery" | pass after fix (was at output corner, full-output surface); submenu hover unobserved; no outside-press close |
| F9 toggle pet | `RegisterHotKey`, emits `key-press` only | KWin shortcut | nested: injected F9 hid and re-showed the pet | pass on first launch; **fails on a second launch in the same session** (suspected) |
| F8 monitor switch | gate strings only; 280 ms | `Windows::moveToScreen`, `SwitchGate` | nested, 2 outputs | **bug found and fixed** (surface did not move); after fix pet on output 2 and clicks counted there; 15 s lease **not** re-read from binary |
| F7 game mode | same as F9 | same | not pressed | unverified |
| Hotkey debounce 280 ms | `FUN_14028f270` | `Hotkeys::raw` | unit test | pass |
| Global keys/clicks counting (evdev) | WH_*_LL hooks | `EvdevThread` | not possible (no injectable device); an earlier run read the host devices by accident, so counts there were discarded | unverified |
| Touchpad tap | n/a | `Tap` | unit tests only | logic pass, hardware unverified |
| Tray icon | tray-icon crate | `QSystemTrayIcon` | no StatusNotifierWatcher in nested bus | unverified |
| Autostart | Windows task | `~/.config/autostart/*.desktop` | nested: `plugin:autostart` enable/is_enabled/disable | pass (file content checked, removed again) |
| Single instance + `wuwa-buddy://` | plugin strings only | `QLocalServer`, `handleDeepLink` | nested: second process with a URL | pass: second exited 0, page got `deep-link://new-url`, `get_current` returned the URL |
| Multi-monitor | n/a | `QScreen`, `selectMainMonitor` | nested, 2 virtual outputs | pass after fix; hot-unplug untested |
| X11 path | n/a | xcb, XShape | Xvfb without compositor + xdotool | pass for rendering (About, pet, panel; transparent areas are black without a compositor); F9 does nothing there (no KWin, no evdev); XShape click-through unverified |
| Multiplayer WebSocket | origin handling is by behaviour | `WsClient` | local RFC 6455 server in unit tests; frontend only allows the Kuro host (TLS), so no end-to-end run | unit pass, end-to-end unverified |
| Network status event | `FUN_140011da0` | `QNetworkInformation` + dedupe | unit test of dedupe; in the offline netns the frontend logged `[activityInfo] skipped: network gate` | logic pass, event source unobserved |
| Maintenance notice | not decompiled | `sanitizeNotice` | unit test | unit pass, real 601 never seen |
| `get_taskbar_insets_at_point` | `FUN_14021d160` decompiled | `Backend` | compiled only | unverified at runtime |
| Window flags per label | not traced | `Windows::create` | n/a | inferred |
| Store | not decompiled | `Store` | unit tests | unit pass; format shared with the Tauri shell |
| Updates | updater endpoint strings | explicit error | n/a | by design |
| REA provider | n/a | n/a | `open_binary(provider_id=ghidra)` -> `not_configured` | blocked, see UNKNOWNS.md for the exact env change |

Network: nothing left the machine (offline namespace); the frontend's analytics and activity check were attempted and failed locally.
