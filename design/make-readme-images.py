#!/usr/bin/env python3
"""Regenerate the README's two pictures from design/layouts.html.

    python design/make-readme-images.py        # run from the repo root

Writes docs/img/readouts.svg and docs/img/boards.svg. The readouts are parsed
out of the mock rather than redrawn, so the pictures in the README cannot
quietly stop matching the thing they were designed in. Change a layout in
layouts.html, run this, commit both.

SVG rather than screenshots: GitHub strips inline <svg> from Markdown but
serves committed files fine, and a file that can be regenerated from its source
beats a PNG nobody can edit. Text uses a generic monospace stack because an SVG
loaded through <img> cannot fetch a webfont.
"""
import html
import io
import math
import re


SRC = io.open('design/layouts.html', encoding='utf-8').read()
PW, PH = 172, 320
SZ = {'s1': 8, 's2': 16, 's3': 24, 's4': 32, 's5': 40, 's6': 48, 's8': 64, 's9': 72}


def css(style):
    d = {}
    for part in style.split(';'):
        if ':' in part:
            k, v = part.split(':', 1)
            d[k.strip()] = v.strip()
    return d


def px(v, default=None):
    if v is None:
        return default
    m = re.match(r'^(-?[\d.]+)px$', v.strip())
    return float(m.group(1)) if m else default


themes = []
# Each theme block: the name, the blurb, then its pair of panels. Only the
# first panel of the pair is the calm state, which is the one worth showing.
for blk in re.split(r'<div class="th">', SRC)[1:]:
    name = re.search(r'class="nm"[^>]*>([^<]+)<', blk).group(1).strip()
    accent = re.search(r'class="nm" style="color:([^"]+)"', blk).group(1)
    panels = re.findall(r'<div class="p" style="([^"]*)">(.*?)\n  </div>', blk, re.S)
    if not panels:
        continue
    pstyle, body = panels[0]
    items, overlays = [], []
    for cls, style, text in re.findall(
            r'<div class="([^"]*)" style="([^"]*)"[^>]*>(.*?)</div>', body, re.S):
        st = css(style)
        classes = cls.split()
        if 'pulse' in classes or 'blink' in classes or 'scan' in classes:
            continue
        x, y = px(st.get('left')), px(st.get('top'))
        w, h = px(st.get('width')), px(st.get('height'))
        if x is None and st.get('right') is not None and w is not None:
            x = PW - px(st['right']) - w
        if y is None and st.get('bottom') is not None and h is not None:
            y = PH - px(st['bottom']) - h
        if 't' in classes:
            fs = next((SZ[c] for c in classes if c in SZ), 8)
            items.append({'kind': 'text', 'x': x or 0, 'y': y or 0, 'fs': fs,
                          's': re.sub(r'<[^>]+>', '', text),
                          'fill': st.get('color', '#fff')})
        else:
            bg = st.get('background')
            border = st.get('border')
            rec = {'kind': 'rect', 'x': x or 0, 'y': y or 0,
                   'w': w if w is not None else PW - (x or 0) - px(st.get('right'), 0),
                   'h': h or 0, 'rx': px(st.get('border-radius'), 0) or 0,
                   'fill': bg, 'transform': st.get('transform')}
            if border:
                bw, _, bc = border.split(None, 2)
                rec['stroke'] = bc
                rec['sw'] = px(bw, 1)
            (overlays if rec.get('stroke') or rec.get('transform') else items).append(rec)
    themes.append({'name': name, 'accent': accent,
                   'bg': css(pstyle).get('background', '#000'),
                   'w': PW, 'h': PH, 'items': items, 'overlays': overlays})


DATA = themes

MONO = "ui-monospace,SFMono-Regular,Menlo,Consolas,monospace"
ASCENT = 0.78          # top-of-box to baseline, as a fraction of font-size
PW, PH = 172, 320


def esc(t):
    t = html.unescape(t)
    return t.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def shape(it):
    if it['kind'] == 'text':
        return ('<text x="%g" y="%g" font-size="%g" fill="%s" '
                'xml:space="preserve">%s</text>'
                % (it['x'], it['y'] + it['fs'] * ASCENT, it['fs'],
                   it['fill'], esc(it['s'])))
    a = ['<rect x="%g" y="%g" width="%g" height="%g"' % (it['x'], it['y'],
                                                         it['w'], it['h'])]
    if it.get('rx'):
        a.append(' rx="%g"' % min(it['rx'], it['h'] / 2))
    a.append(' fill="%s"' % (it.get('fill') or 'none'))
    if it.get('stroke'):
        a.append(' stroke="%s" stroke-width="%g"' % (it['stroke'], it.get('sw', 1)))
    if it.get('transform'):
        # The mock rotates Neon Noir's band with a CSS transform; SVG wants
        # degrees about a point, and CSS turned it about its own centre.
        deg = it['transform'].replace('rotate(', '').replace('deg)', '').strip()
        cx, cy = it['x'] + it['w'] / 2, it['y'] + it['h'] / 2
        a.append(' transform="rotate(%s %g %g)"' % (deg, cx, cy))
    a.append('/>')
    return ''.join(a)


def panel(p, ox, oy, cid):
    o = ['<g transform="translate(%g,%g)" clip-path="url(#%s)">' % (ox, oy, cid),
         '<rect width="%d" height="%d" fill="%s"/>' % (PW, PH, p['bg'])]
    # Anything transformed sits behind the type it lights; anything stroked is
    # chrome drawn over the ground.
    o += [shape(i) for i in p['overlays'] if i.get('transform')]
    o += [shape(i) for i in p['items']]
    o += [shape(i) for i in p['overlays'] if not i.get('transform')]
    o.append('</g>')
    return '\n'.join(o)


def readouts(path):
    cols, gap, lab, pad, rowgap = 5, 24, 26, 10, 30
    rows = math.ceil(len(DATA) / cols)
    W = pad * 2 + cols * PW + (cols - 1) * gap
    H = pad * 2 + rows * (lab + PH) + (rows - 1) * rowgap
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
         'viewBox="0 0 %d %d" font-family="%s" role="img" '
         'aria-label="The ten readout layouts, each drawn on a 172 by 320 panel">'
         % (W, H, W, H, MONO), '<defs>',
         '<clipPath id="pan"><rect width="%d" height="%d" rx="4"/></clipPath>' % (PW, PH),
         '</defs>']
    for i, p in enumerate(DATA):
        cx = pad + (i % cols) * (PW + gap)
        cy = pad + (i // cols) * (lab + PH + rowgap)
        o.append('<text x="%g" y="%g" font-size="12" font-weight="700" '
                 'letter-spacing="1.4" fill="%s">%s</text>'
                 % (cx, cy + 13, p['accent'],
                    esc(p['name'].split(' · ')[0]).upper()))
        o.append(panel(p, cx, cy + lab, 'pan'))
    o.append('</svg>')
    io.open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(o))
    print('wrote', path)


def boards(path):
    """The three panels at their true relative physical size."""
    SPECS = [
        ("2″ LCD",      240, 320, 2.00, "about $26", "#17150f", "#d97757", "#f5f4ef"),
        ("2.16″ AMOLED", 480, 480, 2.16, "Waveshare", "#000000", "#8f4e36", "#e8e8e4"),
        ("1.47″ C6",     172, 320, 1.47, "about $20", "#17150f", "#d97757", "#faf7ef"),
    ]
    DPI, gap, pad, lab = 108, 46, 12, 50
    boxes = []
    for name, pxw, pxh, diag, price, bg, acc, ink in SPECS:
        r = pxw / pxh
        h = diag / math.sqrt(1 + r * r) * DPI      # from the diagonal and ratio
        boxes.append((name, pxw, pxh, r * h, h, price, bg, acc, ink))
    W = pad * 2 + sum(b[3] for b in boxes) + gap * (len(boxes) - 1)
    # A caption is wider than the panel it sits under -- the narrowest board
    # has the longest label -- so the canvas has to clear the text, not just
    # the glass. Monospace advance is ~0.6em.
    x0 = pad
    for b in boxes:
        cap = '%d×%d · %s' % (b[1], b[2], b[5])
        W = max(W, x0 + max(len(cap) * 11 * 0.6, len(b[0]) * 13 * 0.62) + pad)
        x0 += b[3] + gap
    tall = max(b[4] for b in boxes)
    H = pad * 2 + tall + lab
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
         'viewBox="0 0 %d %d" font-family="%s" role="img" '
         'aria-label="The three boards at their true relative sizes">'
         % (round(W), round(H), round(W), round(H), MONO)]
    x = pad
    for name, pxw, pxh, w, h, price, bg, acc, ink in boxes:
        y = pad + (tall - h)                       # stood on one line
        o.append('<rect x="%g" y="%g" width="%g" height="%g" rx="5" fill="%s"/>'
                 % (x, y, w, h, bg))
        # The same readout on each, drawn to the panel rather than pasted at
        # one size. Sameness is the point: what differs is how much glass.
        u = h / 320.0
        for k, (fig, frac) in enumerate((('63', .63), ('31', .31))):
            ty = y + (20 + k * 168) * u
            o.append('<rect x="%g" y="%g" width="%g" height="%g" fill="#6b6759"/>'
                     % (x + 10 * u, ty, w * .32, 7 * u))
            o.append('<text x="%g" y="%g" font-size="%g" fill="%s">%s</text>'
                     % (x + 10 * u, ty + 80 * u, 64 * u, ink, fig))
            by = ty + 100 * u
            o.append('<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="#4a382f"/>'
                     % (x + 10 * u, by, w - 20 * u, 11 * u, 5.5 * u))
            o.append('<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="%s"/>'
                     % (x + 10 * u, by, (w - 20 * u) * frac, 11 * u, 5.5 * u, acc))
        o.append('<text x="%g" y="%g" font-size="13" font-weight="700" '
                 'fill="#8b8d91">%s</text>' % (x, pad + tall + 21, esc(name)))
        o.append('<text x="%g" y="%g" font-size="11" fill="#8b8d91">'
                 '%d×%d · %s</text>'
                 % (x, pad + tall + 38, pxw, pxh, esc(price)))
        x += w + gap
    o.append('</svg>')
    io.open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(o))
    print('wrote', path)


readouts('docs/img/readouts.svg')
boards('docs/img/boards.svg')
