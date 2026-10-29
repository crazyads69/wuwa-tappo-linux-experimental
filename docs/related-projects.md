# Related projects

Survey date: 2026-10-30. Stars are approximate; "last push" is GitHub's `pushed_at`. Facts come from the GitHub
API/README of each repo (fetched during the survey) unless marked **(unverified)**. This is a neutral list, not an
endorsement, and none of the code listed was audited.

## 0. Existing ports of the same app (read first)

Other community ports of Wuwa Tappo exist and are very recent (all created within days of each other, Oct 2026).

| Project | License | Lang | Last push | Stars | Notes |
|---|---|---|---|---|---|
| [Alphaproject1998/wuwa-tappo-linux](https://github.com/Alphaproject1998/wuwa-tappo-linux) | GPL-2.0 | Python/shell | 2026-10-29 | ~7 | Linux, KDE Plasma/Wayland, runs the unmodified Windows build under a private patched Wine + evdev "bridge" that injects F13/F14 stand-in keys. README says none of the app is in the repo; installer downloads Kuro's installer from Kuro's download page. No CONTRIBUTING/CoC/SECURITY files (a Contributing section in the README). |
| [Last-emo-boy/wuwa-tappo-mac](https://github.com/Last-emo-boy/wuwa-tappo-mac) | MIT | JS/Rust (Tauri) | 2026-10-27 | ~8 | macOS native Rust+Tauri backend, same general approach as ours. `setup` downloads the official installer, checks its hash and statically extracts assets; README says asset dirs, `build/`, `dist/` are not committed. Its release bundles, however, include the official frontend/art ("complete resources"), so its Releases do redistribute Kuro assets. |
| [Averionx/WuwaTappo-FullAtlas](https://github.com/Averionx/WuwaTappo-FullAtlas) | NOASSERTION (MIT for new code, per README) | JS | 2026-10-28 | ~29 | Windows mod (auto-clicker, unlocks). README states repo contains no official EXE or full extracted frontend; license does not grant rights to Kuro art. |
| [JoenardoQ/Wuwa-Tappo-Offline-Edition](https://github.com/JoenardoQ/Wuwa-Tappo-Offline-Edition) | MIT | JS | 2026-10-26 | ~6 | Offline re-implementation that bundles 3098 official images (README). Copyright notice at top says art belongs to rights holders and MIT covers only its own code. Counter-example: this is the asset-shipping pattern we avoid. |
| Others (Windows helpers/auto-clickers): [rnpfdl/WuwaTappoHelper](https://github.com/rnpfdl/WuwaTappoHelper), [i02Zane/WuwaTappoPlus](https://github.com/i02Zane/WuwaTappoPlus), [kano0222/WuwaTappo-helper](https://github.com/kano0222/WuwaTappo-helper), [Chuniex-0631/WuwaTappo-Companion-Tools](https://github.com/Chuniex-0631/WuwaTappo-Companion-Tools) | mixed | Py/C# | Oct 2026 | 2-6 | Found via `gh search repos "wuwa tappo"`. Several are auto-click/auto-claim tools, which are a different risk class from a shell. Not reviewed. |

How this project differs: Alphaproject1998 runs the original Windows binary under Wine; this project replaces the closed backend
natively with a re-implementation and a lightly patched frontend. Both are Linux-only in practice and should be described as
alternatives. Searches of AUR, Flathub and Reddit/Discord found nothing relevant (the generic web search returned
no hits; AUR/Flathub/Reddit were **not** queried directly, so absence there is **unverified**). `gh search` for
"kuro tappo" returned nothing.

## 1. Desktop pets (Bongo Cat style)

| Project | License | Lang | Last push | Stars | Relevance |
|---|---|---|---|---|---|
| [ayangweb/BongoCat](https://github.com/ayangweb/BongoCat) | Apache-2.0 | Rust/Vue (Tauri + Live2D) | 2026-10-28 | ~24k | The reference Tauri Bongo Cat. Cross-platform; Linux/Wayland is an acknowledged weak spot (issues [#88](https://github.com/ayangweb/BongoCat/issues/88), [#781](https://github.com/ayangweb/BongoCat/issues/781), [#840](https://github.com/ayangweb/BongoCat/issues/840): events missing in some apps on Wayland, black background/smearing reported on KDE). Has CONTRIBUTING (en/zh), issue templates, AGENTS.md; no CODE_OF_CONDUCT/SECURITY at root. "Bring your own models" wording. |
| [ayangweb/Awesome-BongoCat](https://github.com/ayangweb/Awesome-BongoCat) | not checked | - | 2026-09-18 | ~2.2k | Community model index for the above. |
| [vladelaina/BongoCat](https://github.com/vladelaina/BongoCat) | AGPL-3.0 | - | 2026-10-10 | ~3.9k | Another BongoCat line (relationship to the above not established; **unverified**). Note the AGPL difference. |
| [Externalizable/bongo.cat](https://github.com/Externalizable/bongo.cat) | not checked | JS | 2024-05-01 | ~3.8k | The original web Bongo Cat; stale. The "Bongo Cat" brand is licensed by Irox Games, so avoid reusing its art. |
| [gaetan1903/WayPenguin](https://github.com/gaetan1903/WayPenguin) | MIT | Rust | 2026-07-18 | ~2 | Native Wayland desktop pet; useful as a small layer-shell pet example. |
| [lubabs770/gnat](https://github.com/lubabs770/gnat) | NOASSERTION | Rust | 2026-10-05 | ~11 | Hyprland/Wayland desktop pet (fly sim). |
| [Chesszyh/neuro-hypr-pet-release](https://github.com/Chesszyh/neuro-hypr-pet-release) | MIT | Python | 2026-10-27 | ~0 | Hyprland pet that imports user-supplied assets locally; same "bring your own assets" idea. |
| [YuZhu412/zhizhiji-shimeji](https://github.com/YuZhu412/zhizhiji-shimeji) | none | Java | 2026-04-24 | ~3 | Mentions `wl_shimeji` (Linux Wayland shimeji). I did not locate/verify the upstream `wl_shimeji` repo itself. |

Other Tauri pets (e.g. [marine841023/duty-on](https://github.com/marine841023/duty-on), MIT, Tauri 2 + Live2D) exist but are
small and not Linux-focused; I did not review them.

## 2. Wayland overlay and input techniques

### Layer shell
- [wmww/gtk-layer-shell](https://github.com/wmww/gtk-layer-shell): C library for GTK3/4. LGPL-3.0-or-later (most files MIT per its README; GitHub's API mislabels it GPL-3.0). ~390 stars, last push 2026-08-16. Core of our overlay.
- Rust bindings: crate [`gtk-layer-shell`](https://crates.io/crates/gtk-layer-shell) 0.8.2 (2024-12-09), repo [pentamassiv/gtk-layer-shell-gir](https://github.com/pentamassiv/gtk-layer-shell-gir), MIT, ~9 stars, last push 2024-12-09. The older [subgraph/gtk-layer-shell-rs](https://github.com/subgraph/gtk-layer-shell-rs) is stale (2021). GTK4 variant: [pentamassiv/gtk4-layer-shell-gir](https://github.com/pentamassiv/gtk4-layer-shell-gir) (active, 2026-10-26); Tauri 2 is GTK3, so the GTK3 crate applies.
- [JakeStanger/ironbar](https://github.com/JakeStanger/ironbar): ~1.5k stars, active; a real-world Rust gtk-layer-shell consumer (license not checked).
- Compositor support for `wlr-layer-shell` per [wayland.app protocol table](https://wayland.app/protocols/wlr-layer-shell-unstable-v1) (fetched 2026-10-30): supported by KWin 6.7, Hyprland 0.52.1, Sway 1.11, niri, COSMIC, Wayfire, labwc, river, Mir, Treeland, Muffin (Cinnamon) 6.7.4, phoc, GameScope, Louvre, Jay. Listed as **not** supported: **Mutter 51 (GNOME)**, Weston 15, Cage. (The page's "x" marker is not defined in the text I received, so the Mutter reading is by table layout; GNOME's lack of layer-shell is also widely known but I have no second fetched source.) Implication: on GNOME the app can only use the normal-window fallback.

### Tauri / wry / tao
- [tauri-apps/tauri#15913](https://github.com/tauri-apps/tauri/issues/15913) (closed 2026-10-04): request for layer-shell init via tao builder. Maintainer note: create the window with `.visible(false)`, configure layer shell, then `show()` - the technique our shell relies on.
- [tauri#11814](https://github.com/tauri-apps/tauri/issues/11814) (open): showing a window without taking focus on Linux.
- [tauri#10564](https://github.com/tauri-apps/tauri/issues/10564) and [#6164](https://github.com/tauri-apps/tauri/issues/6164) (open): `set_ignore_cursor_events` on Webview / forward option. I did not find an issue that documents click-through input regions on Wayland, so our GTK input-shape approach is custom.
- [tauri#14924](https://github.com/tauri-apps/tauri/issues/14924) (open): Linux/NVIDIA crashes and artifacts with transparent windows; related to our rendering notes.
- [tauri-apps/tao#1389](https://github.com/tauri-apps/tao/issues/1389) (open): Hyprland resize reverts after hide/show.
- Repos: [tauri](https://github.com/tauri-apps/tauri) (Apache-2.0, ~112k, active), [wry](https://github.com/tauri-apps/wry) (~5k), [tao](https://github.com/tauri-apps/tao) (~2.2k).

### Global input and shortcuts
- [emberian/evdev](https://github.com/emberian/evdev): Rust evdev, Apache-2.0, ~232 stars, 2026-05-28.
- [Smithay/input.rs](https://github.com/Smithay/input.rs): libinput bindings, MIT, ~101 stars, 2026-04-05.
- [Narsil/rdev](https://github.com/Narsil/rdev): global listen/send; MIT, ~746, 2026-05-12. BongoCat uses a fork (issue [#620](https://github.com/ayangweb/BongoCat/issues/620) mentions `ayangweb/rdev`); on Wayland rdev has limits (BongoCat #781).
- [sezanzeb/input-remapper](https://github.com/sezanzeb/input-remapper): GPL-3.0, ~6k, active. Reads evdev and re-injects via uinput; shows the `input` group / udev permission model.
- [XDG GlobalShortcuts portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html): the sandbox-friendly route for hotkeys; v2 adds ConfigureShortcuts. Compositor support not verified here.
- [tauri-plugin-global-shortcut](https://github.com/tauri-apps/tauri-plugin-global-shortcut): read-only mirror, pushed 2026-10-14; no Wayland support verified.
- KDE KGlobalAccel: used by us; I did not fetch a KDE doc page in this survey (**unverified** external source).

## 3. Porting and inspecting Windows Tauri/Electron apps

- [Mas0nShi/tauri-dumper](https://github.com/Mas0nShi/tauri-dumper): MIT, Rust, ~54 stars, 2026-06-17. Lists/extracts/patch-replaces Brotli frontend assets from PE/Mach-O/ELF Tauri binaries. Its README states intent: interoperability research, authorized patching, use only on software you own or may analyze, no bypass of licensing/DRM/server authorization. Closest to our extraction step and a good model for disclaimer wording.
- [Retiely/TauriExtractor](https://github.com/Retiely/TauriExtractor): small Python extractor, ~2 stars, 2026-04-16 (license not checked).
- [tauri-codegen](https://docs.rs/crate/tauri-codegen/) (embedding format source) and a [write-up on reverse engineering a Tauri app](https://infosecwriteups.com/reverse-engineering-a-native-desktop-application-tauri-app-5a2d92772da5) (surfaced by search; not read in full).
- Wine route: Alphaproject1998 above is the only Tauri-under-Wine port found. Needs WebView2 under Wine and a patched Wine.
- Static extraction + native rebuild: wuwa-tappo-mac above (same approach as this project).
- No general "repackage a Windows Tauri app for Linux" tool was found.

## 4. Community-health exemplars

- Disclaimer wording seen: Alphaproject1998 ("Unofficial, and nothing to do with Kuro Games. None of the app is in this repo"), FullAtlas ("MIT applies to our new code; it grants no extra licence to third-party assets"), Offline Edition (copyright notice banner), tauri-dumper (intended use + "grants no rights"). None of those repos warn about account bans or ToS violation prominently; this project does, and also says explicitly that it is "not affiliated" wording.
- Files: of the Tappo repos, none had CODE_OF_CONDUCT or SECURITY.md at root (mac repo root: LICENSE, README, THIRD_PARTY_NOTICES). BongoCat has CONTRIBUTING + issue templates. The `THIRD_PARTY_NOTICES.md` pattern (mac, FullAtlas, Offline) is worth copying.
- Licensing: this project uses the MIT license for its own code, separate from Kuro content; GPL-2.0 (Alphaproject) and MIT/Apache-2.0 are common. Note `gtk-layer-shell` is LGPL, no conflict.
- DMCA history, from the public [github/dmca](https://github.com/github/dmca) repo (tree listing of 22,567 entries, file names matched case-insensitively for kuro, wuthering, wuwa, tappo, irox, bongo): **none found** for Kuro Games, Wuthering Waves, Wuwa Tappo, Irox or Bongo Cat. Limits: file names only, not full-text; notices may be filed under other names; takedowns not published there (e.g. direct to repo owners) would not appear. Absence is not assurance.
- For context, the same tree shows many notices by Genshin Impact / HoYoverse (2021-2024), e.g. [2024-09-27](https://github.com/github/dmca/blob/master/2024/09/2024-09-27-hoyoverse.md) claiming copyright in game package code. Chinese publishers do use DMCA; the repo is not evidence about Kuro.

## 5. What this project takes from them

1. Do not ship Kuro files, installers or the extracted frontend, neither in the repository nor in releases. Some other projects do ship official art; that is the highest-risk pattern and is avoided here.
2. Fetch-and-extract-at-setup with hash pinning (mac repo, `upstream-*.json`) is the common "bring your own files" mechanism; this project does the same.
3. Disclaimers: say unofficial, not affiliated, no assets distributed, own-installer only, can violate ToS/risk account action, use at own risk. See `THIRD_PARTY_NOTICES.md`.
4. Layer-shell works on KWin/Hyprland/Sway/etc. but not Mutter; document GNOME as fallback-only. Create the window hidden, then configure layer shell, then show (Tauri#15913).
5. Global input on Wayland needs evdev (`input` group) or compositor shortcuts; say so plainly (input-remapper and Alphaproject1998 both do). Counters-not-text is a good trust statement.
6. Link existing projects instead of competing; the Wine approach and the native approach serve different users.

## Not verified

AUR/Flathub/Reddit/Discord direct searches; licenses of Awesome-BongoCat, ironbar, TauriExtractor, Bongo.cat; Wayland status of GlobalShortcuts portal and Tauri global-shortcut plugin; any private or non-GitHub takedowns.
