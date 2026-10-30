#!/usr/bin/env python3
"""Extract the web frontend from a Wuwa Tappo Windows installer you already own.

    scripts/extract_frontend.py INSTALLER.exe OUT_DIR

What happens (nothing is decrypted or bypassed; it is plain unpacking):
  1. The installer is a Tauri app whose .rdata section carries a nested NSIS installer after the marker
     ':ENDWUWAPAYLOAD'. `7z` unpacks the outer exe and the nested installer.
  2. WuwaTappo.exe is a Tauri 2 app. Tauri embeds its web assets, brotli-compressed, in .rdata, referenced by
     (pointer, length) pairs that follow each asset path string. We follow those pointers and run `brotli -d`.

Needs: python3 (stdlib only), `7z` (p7zip) and `brotli` on PATH. Output is for local use only; never publish it.
"""
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SUPPORTED_SHA256 = "31105cc880269afeca9e2a5fe6b671d95c80080daaa67cf9015419d732e3da12"  # WuwaTappo-global_1.0.1_x64-setup.exe
MARKER = b":ENDWUWAPAYLOAD"


def need(tool):
    if not shutil.which(tool):
        sys.exit(f"error: `{tool}` not found on PATH (install it with your package manager)")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def seven_zip(archive, out):
    subprocess.run(["7z", "x", "-y", f"-o{out}", str(archive)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class PE:
    """Just enough PE64 parsing to translate virtual addresses to file offsets."""

    def __init__(self, data):
        self.d = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        count = struct.unpack_from("<H", data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", data, pe + 20)[0]
        self.base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
        table = pe + 24 + opt_size
        self.sections = []
        for i in range(count):
            _, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", data, table + 40 * i)
            self.sections.append((va, max(vsize, rsize), raw, rsize))

    def file_offset(self, va):
        rva = va - self.base
        for sva, size, raw, _ in self.sections:
            if sva <= rva < sva + size:
                return raw + rva - sva
        return None

    def va_of(self, offset):
        for sva, _, raw, rsize in self.sections:
            if raw <= offset < raw + rsize:
                return self.base + sva + offset - raw
        return None


def carve_assets(exe_path, out):
    """Walk every (path ptr, path len, data ptr, data len) entry of Tauri's embedded asset table in .rdata.

    The table is scanned structurally instead of guessing file names from strings: a name-based scan missed
    assets whose path string is shared with another string (e.g. assets/emote_happy-*.png).
    """
    data = Path(exe_path).read_bytes()
    pe = PE(data)
    found = {}
    for sva, size, raw, rsize in pe.sections:
        for pos in range(raw, raw + rsize - 32, 8):
            pptr, plen, dptr, dlen = struct.unpack_from("<QQQQ", data, pos)
            if not (2 < plen < 300 and 0 < dlen < 60_000_000):
                continue
            po, do = pe.file_offset(pptr), pe.file_offset(dptr)
            if po is None or do is None:
                continue
            name = data[po:po + plen]
            if name[:1] != b"/" or not all(32 <= c < 127 for c in name) or name in found:
                continue
            res = subprocess.run(["brotli", "-dc"], input=data[do:do + dlen], capture_output=True)
            if res.returncode == 0:
                found[name] = res.stdout
    for name, content in found.items():
        dest = Path(out) / name.decode().lstrip("/")
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(content)
    return len(found), len(found)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    installer, out = Path(sys.argv[1]), Path(sys.argv[2])
    for tool in ("7z", "brotli"):
        need(tool)
    digest = sha256(installer)
    if digest != SUPPORTED_SHA256:
        sys.exit(f"error: unsupported installer (sha256 {digest}).\n"
                 f"This project is tested only against Wuwa Tappo global 1.0.1 ({SUPPORTED_SHA256}).")
    if out.exists() and any(out.iterdir()):
        sys.exit(f"error: {out} is not empty")
    with tempfile.TemporaryDirectory(prefix="wuwa-extract-") as tmp:
        tmp = Path(tmp)
        seven_zip(installer, tmp / "outer")
        rdata = (tmp / "outer" / ".rdata").read_bytes()
        at = rdata.find(MARKER)
        if at < 0:
            sys.exit("error: payload marker not found; is this the global 1.0.1 installer?")
        (tmp / "payload.exe").write_bytes(rdata[at + len(MARKER):])
        seven_zip(tmp / "payload.exe", tmp / "inner")
        found, _ = carve_assets(tmp / "inner" / "WuwaTappo.exe", out)
    print(f"extracted {found} files into {out}")
    if found != 133:  # tauri-dumper and a manual table walk both count 133 in this build
        sys.exit("error: unexpected asset count; extraction failed")


if __name__ == "__main__":
    main()
