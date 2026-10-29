#!/usr/bin/env python3
"""KDE/WebKitGTK compositor regression fixture, no app account/profile involved.
Run on an unlocked desktop: python3 tools/check-synthesis-paint.py [--reuse]
The --reuse control reproduces the old index-only Vue key behavior and should fail
on affected WebKitGTK. Uses Gtk3, WebKit2 4.1, Pillow and Spectacle already installed.
"""
import argparse
import pathlib
import subprocess
import tempfile
import gi

gi.require_version('Gtk', '3.0')
gi.require_version('WebKit2', '4.1')
from gi.repository import Gtk, WebKit2, GLib
from PIL import Image, ImageChops

args = argparse.ArgumentParser(description=__doc__)
args.add_argument('--reuse', action='store_true', help='old reused-slot control')
args = args.parse_args()
root = pathlib.Path(__file__).resolve().parents[1] / 'dist'
if not args.reuse:
    assert 'key:`${Y.value}-${j}`,class:se(["synthesis-item"' in (root / 'assets/index-B8v13rah.js').read_text()
out = pathlib.Path(tempfile.mkdtemp(prefix='ww-synthesis-paint-'))
view = WebKit2.WebView()
window = Gtk.Window(title='Wuwa isolated synthesis paint regression')
window.set_default_size(900, 760)
window.add(view)
html = '''<html><head><link rel="stylesheet" href="assets/index-BjTCKBy6.css">
<style>body{margin:0}</style></head><body><div class="module-content">
<div class="module-content__bg"></div><div class="module-block synthesis-panel">
<div class="panel-content"><div class="atlas-tab">Synthesis paint fixture</div>
<div class="synthesis-body"><div class="synthesis-main"><div class="synthesis-success"></div>
<div class="synthesis-list"></div><div class="synthesis-tip">Five slots should remain</div>
</div></div></div></div></div><script>
window.setSlots=(n,remount)=>{
 const p=document.querySelector('.synthesis-list');
 if(remount)p.replaceChildren();
 while(p.children.length>n)p.lastChild.remove();
 while(p.children.length<n)p.insertAdjacentHTML('beforeend','<div class="synthesis-item"><div class="synthesis-add"></div></div>');
};setSlots(5,false);</script></body></html>'''
view.load_html(html, root.as_uri() + '/')
window.show_all()

def set_slots(n):
    view.evaluate_javascript(f'setSlots({n},{str(not args.reuse).lower()})', -1, None, None, None, None, None)

def capture(name):
    subprocess.run(['spectacle', '-a', '-b', '-n', '-o', str(out / (name + '.png'))], check=True, timeout=10)

result = []
def finish():
    try:
        capture('after')
        before, after = [Image.open(out / (n + '.png')).convert('RGB') for n in ('before', 'after')]
        assert before.size == after.size, 'fixture window lost focus or changed size'
        diff = ImageChops.difference(before, after)
        changed = sum(any(c > 8 for c in px) for px in getattr(diff, 'get_flattened_data', diff.getdata)())
        print(f'{"reused" if args.reuse else "mode-keyed"} slots: changed pixels={changed}; captures={out}')
        assert changed < 100, f'stale paint after 5→10→5 ({changed} pixels differ)'
        result.append(True)
    except Exception as error:
        print(error)
    finally:
        Gtk.main_quit()
    return False

def start():
    capture('before')
    set_slots(10)
    GLib.timeout_add(500, lambda: (set_slots(5), GLib.timeout_add(500, finish), False)[-1])
    return False

GLib.timeout_add(2500, start)
Gtk.main()
window.destroy()
raise SystemExit(0 if result else 1)
