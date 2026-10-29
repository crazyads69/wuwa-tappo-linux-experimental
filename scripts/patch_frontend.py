#!/usr/bin/env python3
"""Apply (or, for maintainers, regenerate) our small text patches on an extracted frontend.

    scripts/patch_frontend.py apply  FRONTEND_DIR            # normal use, run by scripts/setup.sh
    scripts/patch_frontend.py verify FRONTEND_DIR            # checks every patched and generated file
    scripts/patch_frontend.py make   PRISTINE_DIR PATCHED_DIR # maintainers: rebuild patches/frontend.json

Why not ship a unified diff? The bundle is minified: a "line" is hundreds of KB. Instead each change is stored as
a tiny find/replace with a few characters of context on each side. `apply` refuses to continue unless every
context matches exactly once, and finally checks the SHA-256 of every patched file, so a wrong or already patched
input fails loudly instead of producing a broken app.
"""
import difflib
import hashlib
import json
import re
import sys
from pathlib import Path

PATCH_FILE = Path(__file__).resolve().parent.parent / "patches" / "frontend.json"
CONTEXT = 24
CHUNK = re.compile(r"[^;{},]*[;{},]|[^;{},]+$")


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def diff_ops(a: str, b: str):
    """Yield (a_start, a_end, replacement) for every differing region, found chunk-wise then refined per char."""
    ca, cb = CHUNK.findall(a), CHUNK.findall(b)
    assert "".join(ca) == a and "".join(cb) == b
    offs, pos = [], 0
    for c in ca:
        offs.append(pos)
        pos += len(c)
    offs.append(pos)
    boffs, pos = [], 0
    for c in cb:
        boffs.append(pos)
        pos += len(c)
    boffs.append(pos)
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, ca, cb, autojunk=False).get_opcodes():
        if tag == "equal":
            continue
        sa, sb = a[offs[i1]:offs[i2]], b[boffs[j1]:boffs[j2]]
        for t, x1, x2, y1, y2 in difflib.SequenceMatcher(None, sa, sb, autojunk=False).get_opcodes():
            if t != "equal":
                yield offs[i1] + x1, offs[i1] + x2, sb[y1:y2]


def make(pristine: Path, patched: Path):
    files, expected, generated = {}, {}, {}
    names = sorted(str(p.relative_to(patched)) for p in patched.rglob("*") if p.is_file())
    for name in names:
        new = (patched / name).read_bytes()
        old_path = pristine / name
        if not old_path.exists():
            generated[name] = sha(new)  # rebuilt by scripts/setup.sh, verified by `verify`
            continue
        old = old_path.read_bytes()
        if old == new:
            continue
        a, b = old.decode("utf-8"), new.decode("utf-8")
        edits = []
        for start, end, rep in diff_ops(a, b):
            n = CONTEXT
            while True:  # grow the context until before+old+after is unique in the pristine file
                before, old_text, after = a[max(0, start - n):start], a[start:end], a[end:end + n]
                if a.count(before + old_text + after) == 1 or n > 4000:
                    break
                n *= 2
            assert a.count(before + old_text + after) == 1, f"ambiguous edit in {name} at {start}"
            edits.append({"before": before, "old": old_text, "new": rep, "after": after})
        # merge edits whose contexts overlap into one (keeps apply order-independent)
        files[name] = {"edits": edits, "sha256_before": sha(old), "sha256_after": sha(new)}
        expected[name] = sha(new)
        check = apply_edits(a, edits)
        assert check == b, f"round trip failed for {name}"
        print(f"{name}: {len(edits)} edit(s)")
    PATCH_FILE.parent.mkdir(exist_ok=True)
    PATCH_FILE.write_text(json.dumps({"files": files, "generated": generated}, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {PATCH_FILE}")


def apply_edits(text: str, edits) -> str:
    spots = []
    for e in edits:
        needle = e["before"] + e["old"] + e["after"]
        n = text.count(needle)
        if n != 1:
            raise SystemExit(f"error: patch context matches {n} times (expected 1): {needle[:60]!r}")
        spots.append((text.index(needle) + len(e["before"]), e))
    spots.sort(key=lambda s: s[0], reverse=True)
    for at, e in spots:
        text = text[:at] + e["new"] + text[at + len(e["old"]):]
    return text


def verify(frontend: Path):
    spec = json.loads(PATCH_FILE.read_text(encoding="utf-8"))
    bad = 0
    for name, want in [(n, f["sha256_after"]) for n, f in spec["files"].items()] + list(spec["generated"].items()):
        path = frontend / name
        got = sha(path.read_bytes()) if path.exists() else "missing"
        if got != want:
            print(f"MISMATCH {name}")
            bad += 1
    if bad:
        raise SystemExit(f"error: {bad} file(s) do not match; re-run scripts/setup.sh on a fresh checkout")
    print(f"ok: {len(spec['files'])} patched and {len(spec['generated'])} generated files match")


def apply(frontend: Path):
    data = json.loads(PATCH_FILE.read_text(encoding="utf-8"))["files"]
    for name, spec in data.items():
        path = frontend / name
        if not path.exists():
            raise SystemExit(f"error: {name} missing; was the frontend extracted from the supported installer?")
        raw = path.read_bytes()
        if sha(raw) == spec["sha256_after"]:
            print(f"{name}: already patched")
            continue
        if sha(raw) != spec["sha256_before"]:
            raise SystemExit(f"error: {name} is not the expected pristine file (sha256 mismatch)")
        out = apply_edits(raw.decode("utf-8"), spec["edits"]).encode("utf-8")
        if sha(out) != spec["sha256_after"]:
            raise SystemExit(f"error: {name} patched result does not match the expected hash")
        path.write_bytes(out)
        print(f"{name}: patched ({len(spec['edits'])} edit(s))")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "apply":
        apply(Path(sys.argv[2]))
    elif len(sys.argv) == 3 and sys.argv[1] == "verify":
        verify(Path(sys.argv[2]))
    elif len(sys.argv) == 4 and sys.argv[1] == "make":
        make(Path(sys.argv[2]), Path(sys.argv[3]))
    else:
        sys.exit(__doc__)
