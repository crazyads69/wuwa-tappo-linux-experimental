#!/usr/bin/env python3
"""Show which input region each Wayland surface of the app told the compositor about.

An overlay that is meant to be click-through must end up with an EMPTY (or tiny) input region. A region that covers
the whole screen means every click on the desktop is swallowed. Run the app with the client protocol log on:

    WAYLAND_DEBUG=client qt/build/wuwatappo-qt 2> wayland.log
    python3 tools/trace_input_regions.py wayland.log

Prints one line per `set_input_region` request with the surface role (layer-shell namespace or window title),
the number of rectangles and their total area, plus surface creation and destruction.
"""
import re,sys
role={}; regions={}; surf_of_xdg={}; tl_of_xdg={}; ev=[]
for line in open(sys.argv[1],errors='replace'):
    m=re.search(r'\[TEST\] (.*)',line)
    if m: ev.append(('TEST',m.group(1)));continue
    t=re.match(r'\[(\d+:\d+:\d+\.\d+)\]',line); ts=t.group(1) if t else ''
    m=re.search(r'get_layer_surface\(new id zwlr_layer_surface_v1#(\d+), wl_surface#(\d+), .*"([^"]*)"',line)
    if m: role[m.group(2)]='layer:'+m.group(3); ev.append((ts,f'surface#{m.group(2)} = layer {m.group(3)}'))
    m=re.search(r'get_xdg_surface\(new id xdg_surface#(\d+), wl_surface#(\d+)\)',line)
    if m: surf_of_xdg[m.group(1)]=m.group(2)
    m=re.search(r'xdg_surface#(\d+)\.get_toplevel\(new id xdg_toplevel#(\d+)\)',line)
    if m: s=surf_of_xdg.get(m.group(1)); role[s]='xdg'; tl_of_xdg[m.group(2)]=s
    m=re.search(r'xdg_toplevel#(\d+)\.set_title\("([^"]*)"\)',line)
    if m and m.group(1) in tl_of_xdg: role[tl_of_xdg[m.group(1)]]='xdg:'+m.group(2)
    m=re.search(r'create_region\(new id wl_region#(\d+)\)',line)
    if m: regions[m.group(1)]=[]
    m=re.search(r'wl_region#(\d+)\.add\((-?\d+), (-?\d+), (\d+), (\d+)\)',line)
    if m: regions.setdefault(m.group(1),[]).append(tuple(map(int,m.groups()[1:])))
    m=re.search(r'wl_surface#(\d+)\.set_input_region\((?:wl_region#(\d+)|nil)\)',line)
    if m:
        rs=regions.get(m.group(2),[]) if m.group(2) else None
        area='NULL' if rs is None else sum(w*h for _,_,w,h in rs)
        ev.append((ts,f'surface#{m.group(1)} [{role.get(m.group(1),"?")}] input_region rects={None if rs is None else len(rs)} area={area}'))
    m=re.search(r'wl_surface#(\d+)\.destroy',line)
    if m and m.group(1) in role: ev.append((ts,f'surface#{m.group(1)} [{role[m.group(1)]}] DESTROYED'))
for e in ev: print(*e)
