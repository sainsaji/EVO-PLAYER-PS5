"""ui_consistency.py - compare the kit screens against each other.

Reads output/uiview/hui_metrics.tsv (written by tools/hui_preview.sh: every text
run and box of every rendered screen) and reports, per page element, what most
screens do and which screens do something else:

  title      the page title: position, size (SDF range), face, colour
  crumb      the line under the title (breadcrumb / subtitle)
  footer     the button-hint row: baseline, size, colour, right edge
  margin     where content text starts on the left
  radius     corner radius of card-sized boxes

Screens drawn over video (OSD, pickers, dialogs, toasts, keyboard) have no page
title; their panels are compared among themselves (title size, radius).

    python tools/ui_consistency.py [output/uiview]
"""
import collections
import os
import sys

out_dir = sys.argv[1] if len(sys.argv) > 1 else 'output/uiview'
rows = collections.defaultdict(lambda: {'text': [], 'box': []})
for line in open(os.path.join(out_dir, 'hui_metrics.tsv'), encoding='utf-8'):
    f = line.rstrip('\n').split('\t')
    if len(f) < 10:
        continue
    name, kind = f[0], f[1]
    x, y, w, h = map(float, f[2:6])
    a, b = float(f[6]), float(f[7])
    rows[name][kind].append({'x': x, 'y': y, 'w': w, 'h': h, 'a': a, 'b': b, 'rgb': f[8], 'alpha': float(f[9])})

OVERLAY_HINTS = ('osd', 'picker', 'info', 'dialog', 'toast', 'keyboard', 'modal', 'confirm', 'resume', 'closed')


def is_overlay(name, s):
    # A page has the rail on the left (text-free area) and a title near the top left.
    return any(name.startswith(p) or ('_' + p) in name for p in OVERLAY_HINTS)


def mode(values):
    c = collections.Counter(values)
    return c.most_common(1)[0][0] if c else None


def q(v, step):
    return round(v / step) * step


pages, overlays = {}, {}
for name, r in rows.items():
    t = r['text']
    top = [x for x in t if x['y'] < 210 and x['x'] > 140 and x['x'] < 1500]
    title = max(top, key=lambda x: x['a'], default=None)
    crumb = None
    if title:
        below = [x for x in t if 0 < x['y'] - (title['y'] + title['h']) < 70 and abs(x['x'] - title['x']) < 30]
        crumb = min(below, key=lambda x: x['y'], default=None)
    foot = [x for x in t if x['y'] > 990]
    body = [x for x in t if 140 < x['x'] < 900 and 230 < x['y'] < 980]
    cards = [bx for bx in r['box'] if bx['w'] >= 150 and bx['h'] >= 60 and bx['w'] < 1700]
    m = {
        'title': title, 'crumb': crumb, 'foot': foot,
        'margin': min((x['x'] for x in body), default=None),
        'radius': mode([q(bx['a'], 2) for bx in cards]),
    }
    (overlays if is_overlay(name, r) else pages)[name] = m


def report(label, group, key_fn, describe):
    keys = {n: key_fn(m) for n, m in group.items()}
    # `a and b` lambdas give None, False or [] when the element is missing.
    keys = {n: k for n, k in keys.items() if k is not None and k is not False and k != []}
    if not keys:
        return
    common = mode(list(keys.values()))
    odd = sorted((n, k) for n, k in keys.items() if k != common)
    print('\n== %s: most screens %s (%d of %d)' % (label, describe(common), len(keys) - len(odd), len(keys)))
    by_value = collections.defaultdict(list)
    for n, k in odd:
        by_value[k].append(n)
    for k, names in sorted(by_value.items(), key=lambda kv: -len(kv[1])):
        print('   %-40s %s' % (describe(k), ', '.join(names)))


print('PAGES (%d): %s' % (len(pages), ' '.join(sorted(pages))[:300] + ' ...'))
report('title position', pages, lambda m: m['title'] and (q(m['title']['x'], 4), q(m['title']['y'], 4)),
       lambda k: 'x=%d y=%d' % k)
report('title size/face', pages, lambda m: m['title'] and (round(m['title']['a'], 1), m['title']['b']),
       lambda k: 'range=%.1f face=%d' % k)
report('title colour', pages, lambda m: m['title'] and m['title']['rgb'], lambda k: '#' + k)
report('breadcrumb size/colour', pages,
       lambda m: m['crumb'] and (round(m['crumb']['a'], 1), m['crumb']['b'], m['crumb']['rgb']),
       lambda k: 'range=%.1f face=%d #%s' % k)
report('breadcrumb gap below title', pages,
       lambda m: m['crumb'] and m['title'] and q(m['crumb']['y'] - (m['title']['y'] + m['title']['h']), 4),
       lambda k: '%d px' % k)
report('footer hint baseline', pages, lambda m: m['foot'] and q(max(x['y'] + x['h'] for x in m['foot']), 4),
       lambda k: 'bottom=%d' % k)
report('footer hint size/colour', pages,
       lambda m: m['foot'] and mode([(round(x['a'], 1), x['b'], x['rgb']) for x in m['foot']]),
       lambda k: 'range=%.1f face=%d #%s' % k)
report('footer right edge', pages, lambda m: m['foot'] and q(max(x['x'] + x['w'] for x in m['foot']), 4),
       lambda k: 'x1=%d' % k)
report('content left margin', pages, lambda m: m['margin'] is not None and q(m['margin'], 4),
       lambda k: 'x=%d' % k)
report('card corner radius', pages, lambda m: m['radius'], lambda k: 'r=%s' % k)

print('\nOVERLAYS (%d)' % len(overlays))
report('overlay heading size/face', overlays,
       lambda m: m['title'] and (round(m['title']['a'], 1), m['title']['b']), lambda k: 'range=%.1f face=%d' % k)
report('overlay panel radius', overlays, lambda m: m['radius'], lambda k: 'r=%s' % k)
report('overlay footer size/colour', overlays,
       lambda m: m['foot'] and mode([(round(x['a'], 1), x['b'], x['rgb']) for x in m['foot']]),
       lambda k: 'range=%.1f face=%d #%s' % k)
