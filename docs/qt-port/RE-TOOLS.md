# Reverse-extraction tooling for the Tauri installer

Target: `WuwaTappo-global_1.0.1_x64-setup.exe` (Tauri-wrapped NSIS installer carrying `WuwaTappo.exe`, a Tauri 2.10.3
Rust PE). Nothing was executed; each artifact went into its own empty directory outside the repo; Python ran with `-I`.
"Here" = Fedora Asahi aarch64, no sudo. Search results come from web search / DeepWiki and were not all
verified by running the tool: the last column says what was.

| Tool | Repo | Recovers | Available here | What was actually run, result |
|---|---|---|---|---|
| p7zip `7z` | https://github.com/p7zip-project/p7zip | PE sections of the outer exe, then the nested NSIS payload (`:ENDWUWAPAYLOAD` marker in `.rdata`) -> `WuwaTappo.exe`, `uninstall.exe`, plugin DLLs. NSIS script (`[NSIS].nsi`) is **not** extractable with modern 7-Zip (removed in 15.06) | installed (`/usr/bin/7z`) | Used twice (outer, inner). Worked. No `.nsi` produced |
| NRS (NSIS Reversing Suite), NSIDis | https://github.com/isra17/nrs | NSIS script/opcode disassembly (IDA plugin + python libs) | not installed, not run | Not needed: the installer script was not part of the question. Remaining way to recover install-time behaviour (registry keys, shortcuts) |
| myfreeer/7z-build-nsis | https://github.com/myfreeer/7z-build-nsis | 7-Zip build that still dumps the compiled NSIS script | not built | Not run |
| **tauri-dumper** (Mas0nShi) | https://github.com/Mas0nShi/tauri-dumper | Embedded, Brotli-compressed assets of PE/Mach-O/ELF Tauri binaries; subcommands `extract list inspect verify repack`; does **not** recover config, ACL or commands (DeepWiki) | `cargo install --locked --root <scratch>` worked (v0.2.2) | `inspect`: 133 assets, 61 400 771 compressed bytes. `extract`: 133 files, byte-identical to my own table walk. `verify`: OK 133 |
| TauriExtractor (Retiu) | https://github.com/Retiu/TauriExtractor | Python scan of `.rdata` for path strings + Brotli, Tauri v2 PE | not installed | Not run. Same name-based approach as this repo's old extractor, which missed 2 assets (below) |
| tauri-backend (iwastaken93-droid) | https://github.com/iwastaken93-droid/tauri-backend | fork of tauri-dumper with a "backend recovery" command for the IPC surface (from a web search snippet) | repo URL returned 404, not indexed by DeepWiki | Could not verify it exists. IPC surface was recovered by hand instead (below) |
| hacbit/tauri-reader | https://github.com/hacbit/tauri-reader | Python static Tauri asset extraction | not installed | Not run |
| tauri-codegen docs | https://docs.rs/tauri-codegen | Explains the embedded-asset layout (`phf` map, Brotli by default) | n/a | Background only |
| **rustbininfo** (`rbi`) | https://github.com/n0fix/rustbininfo | rustc commit/date and crate list from stripped Rust binaries (parses registry paths in strings) | pip into a scratch venv worked | 84 crates, rustc commit `59807616e1fa...` (2026-04-14 nightly-dated), toolchain `windows-msvc`. Matches my own `grep` of `registry\src\...\crate-x.y.z` paths (also 84) |
| cargo-auditable | https://github.com/rust-secure-code/cargo-auditable | Dependency list in a `.dep-v0` section | n/a (REA `inspect_rust_binary` reads it) | REA: **no** `.dep-v0` section, so the app was not built with cargo-auditable; crates come from path strings only |
| **REA** (`mcp__rea__*`) | local MCP | PE resources, Rust/auditable metadata, archive inventory, unknown tracking; deep tools (strings/xrefs/trace_feature) need Ghidra 12.1.x, Hopper, IDA or Kuna | MCP present; **no provider configured** (Ghidra not set, Kuna rejects PE) | `open_binary`, `inspect_rust_binary`, `inspect_pe_resources` (needs `max_file_bytes` > 64 MiB for this 78 MB exe) worked. `trace_feature` / `binary_overview` fail with `provider_unavailable` |
| Python `pefile` / `peres` (pev) / folbricht/pefile | https://github.com/erocarrera/pefile , https://github.com/merces/pev , https://github.com/folbricht/pefile | PE resources, RT_MANIFEST, RT_VERSION | pefile installable via pip | Resources read with REA + a 3-line Python slice instead: manifest has `requestedExecutionLevel level="requireAdministrator"`, version info CompanyName `kuro`, ProductName `Wuwa Tappo`, FileVersion `1.0.1`, 6 icon sizes |
| Ghidra 12.1.4 (+ ghidra scripts for Rust strings, e.g. Pen Test Partners / Binary Defense scripts) | https://github.com/NationalSecurityAgency/ghidra | functions, xrefs to Rust string slices (Rust strings are not NUL terminated, so auto-analysis misses them) | zip (570 MB) downloaded and unzipped in scratch; Java 25 and javac present; the release ships no `linux_arm_64` native decompiler (only `linux_x86_64`, `win_x86_64`), so only headless analysis and scripts, no decompilation | see "Ghidra run" below |
| Other Ghidra/Rust helpers: jingjing2222/rust-reverse-engineering-skill, ghidra-cli, mcghidra | https://github.com/jingjing2222/rust-reverse-engineering-skill | helpers around Ghidra pseudocode export / headless control | not installed | Not run |
| rizin / rz-ghidra / Cutter / radare2 | https://github.com/rizinorg/rizin | disassembly, xrefs | not installed. rizin release assets: static x86_64, Windows, Android; no linux aarch64 | Not run (would need building from source) |
| `strings`, `brotli`, `cmp`, `diff` | binutils / google/brotli | rodata strings; Brotli decode | installed | Main workhorse, see below |

## Method that recovered the Tauri internals

- **Config**: Tauri 2 compiles `tauri.conf.json` into a Rust struct (no JSON blob in the binary). Recovered values are
  scattered strings: `com.kuro.wuwatappo.global`, `main`, `index.html`, `http://localhost:1421` (devUrl), `../dist`,
  `icons/128x128.png` + `@2x`, the updater endpoint and pubkey, deep-link scheme `wuwa-buddy`.
- **Assets**: walk `.rdata` for 4-qword entries `(name ptr, name len, data ptr, data len)` whose name starts with `/`
  and whose data is valid Brotli. This is what `scripts/extract_frontend.py` now does.
- **ACL / capabilities**: only the *permission names* exist (`allow-*`/`deny-*` identifiers and the `core:*:default` set
  strings). The capability file itself is not in the binary in readable form; this repo's `capabilities/default.json`
  is a reconstruction.
- **IPC commands**: the app command names sit in one contiguous rodata blob (`greet`, `save_app_ui_locale`, ... ,
  `set_elevated_autostart`). Segmenting it against known names left only genuinely new names as residue.
- **Events**: hyphenated names in rodata, cross-checked against `listen("...")` calls in the frontend bundle.
- **Ghidra run**: see below.
