# Third-party notices

This repository's own code is MIT licensed (see [LICENSE](LICENSE)). It builds on the following. The complete and
authoritative list of Rust crates and their versions is `src-tauri/Cargo.lock`; each crate carries its own license.

| Component | License | How it is used |
|---|---|---|
| [Tauri](https://tauri.app) 2 and its plugins (opener, dialog, process, autostart, deep-link, single-instance) | MIT or Apache-2.0 | Application framework |
| [tokio-tungstenite](https://github.com/snapview/tokio-tungstenite) 0.30 and tungstenite | MIT | Native WebSocket client for the multiplayer socket; TLS through the system OpenSSL |
| [gtk-layer-shell](https://github.com/wmww/gtk-layer-shell) (C library) and its Rust bindings | LGPL-3.0-or-later (library, mostly MIT per its README); MIT (bindings) | Dynamically linked from the system |
| GTK 3 and WebKitGTK | LGPL | Dynamically linked from the system |
| Other Rust crates (about 490, see `Cargo.lock`) | Mostly MIT, Apache-2.0 or both; some Zlib, BSD, ISC, Unicode, MPL-2.0 (a handful, file-level copyleft) | Dependencies |
| [fonttools](https://github.com/fonttools/fonttools) 4.68.0 | MIT | Used only by `scripts/setup.sh` in a temporary virtual environment, to build two font files locally |
| [Contributor Covenant](https://www.contributor-covenant.org) 2.1 | CC BY 4.0 | `CODE_OF_CONDUCT.md` |
| SIL Open Font License 1.1 text in `patches/assets/` | OFL-1.1 | Accompanies the two Latin font subsets that setup generates on your computer from the app's own open-licensed fonts |

## Not included and not licensed here

The official Wuwa Tappo application, its installer and everything extracted from them (code, artwork, fonts, icons,
sounds) belong to Kuro Games and their licensors. "Wuthering Waves", "Wuwa Tappo", "Kuro Games" and "Bongo Cat" are
names of their respective owners and are used only to describe what this software works with. This project is not
affiliated with or endorsed by any of them.
