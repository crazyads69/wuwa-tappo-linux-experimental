#!/usr/bin/env python3
"""Build exact-outline Western UI subsets. Build-only: fonttools[woff]==4.68.0.
Run with a temporary venv Python; no app runtime dependency. Originals remain fallback.
"""
from pathlib import Path
import re
import json
import shutil
import subprocess
import tempfile
import sys
from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.pens.recordingPen import DecomposingRecordingPen

ASSETS = Path(__file__).resolve().parents[1] / 'dist/assets'
SKIP_CSS = False  # setup applies the CSS change through patches/frontend.json
# The last four ranges are CJK/fullwidth punctuation (U+3000-303F, U+30FB, U+FF00-FF65, U+FFE0-FFEE): the
# English UI shows names such as "A・B" and "（x）"; without them the 6 MB CJK face was fetched and decoded on
# first use of the Bag/Atlas tabs (observed 2026-10-28), which re-laid the text out mid-interaction.
RANGES = ((0, 0x24F), (0x300, 0x52F), (0x1E00, 0x1EFF), (0x2000, 0x27FF),
          (0x3000, 0x303F), (0x30FB, 0x30FB), (0xFF00, 0xFF65), (0xFFE0, 0xFFEE))


def css_ranges(ranges):
    return ','.join('U+%04X' % lo if lo == hi else 'U+%04X-%04X' % (lo, hi) for lo, hi in ranges)


def complement(ranges, top=0x10FFFF):
    result, start = [], 0
    for lo, hi in sorted(ranges):
        if lo > start:
            result.append((start, lo - 1))
        start = hi + 1
    return result + ([(start, top)] if start <= top else [])


LATIN_CSS = css_ranges(RANGES)
# The complementary full face keeps every other code point (IPA, Armenian ... CJK).
FALLBACK_CSS = css_ranges(complement(RANGES))
SOURCES = {'M': 'GenSenRounded-M-IdyG9UEk.woff2', 'B': 'GenSenRounded-B-BZwrY0nR.woff2'}
CORPUS = 'WuwaTappo Settings Account About Atlas Hello Việt Nam Tiếng Việt Đặng Nguyễn Ắằễộự € £ → ★ αβ Яя ・（）、。「」！？：～'
COPYRIGHT = "Copyright 2014-2019 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'."


def wanted(cp):
    return any(lo <= cp <= hi for lo, hi in RANGES)


def outline(glyphs, glyph):
    pen = DecomposingRecordingPen(glyphs)
    glyphs[glyph].draw(pen)
    return pen.value


def verify(original, derived):
    source_map, result_map = original.getBestCmap(), derived.getBestCmap()
    expected = {cp for cp in source_map if wanted(cp)}
    assert set(result_map) == expected
    # The complementary full-file face provides every excluded source character.
    assert expected | {cp for cp in source_map if not wanted(cp)} == set(source_map)
    assert all(cp in source_map and not wanted(cp) for cp in map(ord, '中文日本語'))
    assert all(ord(ch) in result_map for ch in CORPUS if not ch.isspace())
    original_glyphs, derived_glyphs = original.getGlyphSet(), derived.getGlyphSet()
    for cp, glyph in result_map.items():
        source_glyph = source_map[cp]
        assert outline(original_glyphs, source_glyph) == outline(derived_glyphs, glyph), hex(cp)
        assert original['hmtx'][source_glyph] == derived['hmtx'][glyph], hex(cp)
        if 'vmtx' in original:
            assert original['vmtx'][source_glyph] == derived['vmtx'][glyph], hex(cp)
    for table, fields in {
        'head': ('unitsPerEm', 'xMin', 'yMin', 'xMax', 'yMax'),
        'hhea': ('ascent', 'descent', 'lineGap'),
        'vhea': ('ascent', 'descent', 'lineGap'),
        'OS/2': ('sTypoAscender', 'sTypoDescender', 'sTypoLineGap', 'usWinAscent', 'usWinDescent', 'sxHeight', 'sCapHeight'),
    }.items():
        for field in fields:
            assert getattr(original[table], field) == getattr(derived[table], field), (table, field)
    for name_id in (0, 13, 14):
        source = {n.toUnicode() for n in original['name'].names if n.nameID == name_id}
        result = {n.toUnicode() for n in derived['name'].names if n.nameID == name_id}
        assert source <= result
    assert not any('GenSen' in n.toUnicode() for n in derived['name'].names if n.nameID in (1, 2, 3, 4, 6, 16, 17, 18, 21, 22))
    return len(expected)



def check_shaping(original, derived):
    # HarfBuzz comparison is optional tooling, not part of app startup.
    if not shutil.which('hb-shape'):
        print('hb-shape unavailable; exact outline/metric assertions still ran')
        return
    with tempfile.TemporaryDirectory(prefix='wuwa-font-shaping-') as temp:
        paths = [Path(temp) / 'source.ttf', Path(temp) / 'subset.ttf']
        for font, path in zip((original, derived), paths):
            flavor = font.flavor
            font.flavor = None
            font.save(path)
            font.flavor = flavor
        fonts = {paths[0]: original, paths[1]: derived}
        glyphsets = {path: font.getGlyphSet() for path, font in fonts.items()}
        def shape(path, text):
            result = json.loads(subprocess.check_output(
                ['hb-shape', '--no-glyph-names', '--output-format=json', str(path), text], text=True))
            for glyph in result:
                name = fonts[path].getGlyphName(glyph['g'])
                glyph['g'] = outline(glyphsets[path], name)
                glyph.pop('cl', None)
            return result
        for text in (CORPUS, 'AVATAR office ffi fi fl', 'Tiếng Việt', 'Ελληνικά Русский', '“Hello” → €12.50'):
            assert shape(paths[0], text) == shape(paths[1], text), text
        # Simulate the CSS face split for representative mixed CJK/Western runs.
        for text in ('中文Hello世界', 'Atlas 中文 123', '日本語→Settings'):
            runs = []
            for char in text:
                face = wanted(ord(char))
                if runs and runs[-1][0] == face:
                    runs[-1] = (face, runs[-1][1] + char)
                else:
                    runs.append((face, char))
            combined = []
            for face, run in runs:
                combined.extend(shape(paths[1] if face else paths[0], run))
            assert shape(paths[0], text) == combined, text


def build(weight, filename):
    original = TTFont(ASSETS / filename, recalcTimestamp=False, recalcBBoxes=False)
    font = TTFont(ASSETS / filename, recalcTimestamp=False, recalcBBoxes=False)
    options = subset.Options()
    options.layout_features = ['*']
    options.name_IDs = ['*']
    options.name_languages = ['*']
    options.name_legacy = True
    options.glyph_names = True
    options.notdef_outline = True
    options.recalc_bounds = False
    sub = subset.Subsetter(options=options)
    sub.populate(unicodes=[cp for cp in font.getBestCmap() if wanted(cp)])
    sub.subset(font)
    print(f"Subset closure complete: {filename}", flush=True)
    style = 'Medium' if weight == 'M' else 'Bold'
    names = {1: 'WuwaTappoLatin', 2: style, 3: 'WuwaTappoLatin-' + style + '-1',
             4: 'WuwaTappoLatin ' + style, 6: 'WuwaTappoLatin-' + style,
             16: 'WuwaTappoLatin', 17: style, 18: 'WuwaTappoLatin ' + style,
             21: 'WuwaTappoLatin', 22: style}
    for record in font['name'].names:
        if record.nameID in names:
            record.string = names[record.nameID].encode(record.getEncoding())
    if not any(n.nameID == 0 for n in font['name'].names):
        font['name'].setName(COPYRIGHT, 0, 3, 1, 0x409)
    output = ASSETS / ('WuwaTappoLatin-' + style + '.woff2')
    font.save(output)
    result = TTFont(output, recalcTimestamp=False, recalcBBoxes=False)
    count = verify(original, result)
    check_shaping(original, result)
    print(f'{output.name}: {output.stat().st_size:,} bytes; {count:,} code points verified')


def update_css():
    css = ASSETS / 'index-BjTCKBy6.css'
    text = css.read_text()
    for weight, original in SOURCES.items():
        style = 'Medium' if weight == 'M' else 'Bold'
        family = 'GenSenRounded-' + weight
        pattern = r'@font-face\{font-family:' + family + r';[^}]*\}'
        blocks = re.findall(pattern, text)
        assert len(blocks) in (1, 2), (family, len(blocks))
        base = next(block for block in blocks if original in block)
        base = re.sub(r';unicode-range:[^}]*', '', base)
        small = base.replace(original, 'WuwaTappoLatin-' + style + '.woff2').replace('font-display:swap', 'font-display:block')[:-1] + ';unicode-range:' + LATIN_CSS + '}'
        full = base[:-1] + ';unicode-range:' + FALLBACK_CSS + '}'
        text = re.sub(pattern, '', text)
        # One ordered pair per alias, placed before locale selectors.
        anchor = '.locale-zh{'
        assert anchor in text
        text = text.replace(anchor, small + full + anchor, 1)
    css.write_text(text)


if __name__ == '__main__':
    args = sys.argv[1:]
    if '--assets' in args:  # --assets DIR: work on another extracted frontend (used by scripts/setup.sh)
        at = args.index('--assets')
        ASSETS = Path(args[at + 1]).resolve()
        del args[at:at + 2]
    if '--skip-css' in args:
        SKIP_CSS = True
        args.remove('--skip-css')
    if args == ['--check']:
        for weight, filename in SOURCES.items():
            style = 'Medium' if weight == 'M' else 'Bold'
            original = TTFont(ASSETS / filename, recalcTimestamp=False, recalcBBoxes=False)
            derived = TTFont(ASSETS / ('WuwaTappoLatin-' + style + '.woff2'), recalcTimestamp=False, recalcBBoxes=False)
            count = verify(original, derived)
            check_shaping(original, derived)
            print(f'{style}: {count} code points and HarfBuzz assertions passed', flush=True)
    else:
        assert not args, 'Usage: subset-latin-fonts.py [--assets DIR] [--skip-css] [--check]'
        for weight, filename in SOURCES.items():
            build(weight, filename)
        if not SKIP_CSS:
            update_css()
