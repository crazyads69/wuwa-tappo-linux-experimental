#!/usr/bin/env bash
# Prepare the local, gitignored build inputs from an installer you already have.
#   scripts/setup.sh /path/to/WuwaTappo-global_1.0.1_x64-setup.exe
# Creates: dist/ (patched frontend), src-tauri/icons/icon.png. Nothing here is ever committed.
set -euo pipefail

installer="${1:-}"
[ -f "$installer" ] || { echo "usage: $0 /path/to/WuwaTappo-global_1.0.1_x64-setup.exe" >&2; exit 2; }
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

for tool in python3 7z brotli; do
  command -v "$tool" >/dev/null || { echo "error: '$tool' is required (see README: Requirements)" >&2; exit 1; }
done

[ ! -e dist ] || { echo "error: dist/ already exists; remove it first (rm -rf dist) to start from scratch" >&2; exit 1; }

echo "==> extracting the frontend"
python3 scripts/extract_frontend.py "$installer" dist

echo "==> applying patches"
python3 scripts/patch_frontend.py apply dist

echo "==> building the Latin font subsets (needs network once for fonttools)"
venv=".setup-venv"
[ -d "$venv" ] || python3 -m venv "$venv"
"$venv/bin/pip" install --quiet 'fonttools[woff]==4.68.0'
"$venv/bin/python" tools/subset-latin-fonts.py --assets dist/assets --skip-css
cp patches/assets/WuwaTappoLatin-OFL.txt dist/assets/

echo "==> verifying"
python3 scripts/patch_frontend.py verify dist

mkdir -p src-tauri/icons
cp dist/app-icon.png src-tauri/icons/icon.png
echo "done. Build with: cd src-tauri && cargo build --release"
