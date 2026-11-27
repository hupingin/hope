"""Insert Doxygen comment blocks (Chinese / English) before C functions.

Two data sources:

  tools/paramdict.json   name -> "描述 / Description"
        Shared dictionary for the ~250 most frequent parameter names, so that a
        per-file note only has to spell out the ones that are unusual.

  tools/notes/<mod>__<file>.json
        name -> {
          "brief":   "中文简述 / English brief",      (required)
          "params":  { "name": "中文 / English" },     (optional overrides)
          "return":  "中文 / English",                 (optional)
          "retval":  { "0": "成功 / Success" },        (optional)
          "note":    "...",                           (optional)
          "warning": "...",                           (optional)
          "see":     "...",                           (optional)
        }

Idempotent: a function that already carries a `/**` block is skipped unless
--force is given. A plain `/* ... */` block directly above the function is
treated as a legacy note and is replaced; a section separator such as
`/* ---------- memory ---------- */` is preserved and the new block is inserted
below it.
"""

import argparse
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from doxy_extract import find_functions, find_prototypes  # noqa: E402

SECTION_RE = re.compile(r'^[\-=*_\s]{4,}$')


def load_paramdict(path):
    if not os.path.exists(path):
        return {}
    return json.load(open(path, encoding='utf-8'))


def clean(text):
    """Collapse whitespace and neutralise anything that would terminate the
    enclosing `/** ... */` block early."""
    s = re.sub(r'\s+', ' ', str(text)).strip()
    s = s.replace('/*', '/ *').replace('*/', '* /')
    return s


def infer_direction(ptype):
    """const pointer -> in; other pointer -> in,out; by-value -> in."""
    if '*' not in ptype:
        return 'in'
    if 'const' in ptype:
        return 'in'
    return 'in,out'


def direction_of(pname, ptype, explicit):
    if explicit:
        e = explicit.lower().replace(' ', '')
        if e in ('inout', 'in,out'):
            return 'in,out'
        if e in ('in', 'out'):
            return e
    return infer_direction(ptype)


# `@param[in,out] ` is 15 columns wide, so every param name starts in the same
# column no matter which direction tag precedes it (see 规范 四、要点速查「对齐」).
PARAM_TAG_WIDTH = 15


def render_block(fn, spec, paramdict, indent=''):
    """Return the list of lines making up the comment block."""
    brief = clean(spec['brief'])
    over = spec.get('params') or {}
    ret = spec.get('return')
    retvals = spec.get('retval')
    note = spec.get('note')
    warning = spec.get('warning')
    see = spec.get('see')
    details = spec.get('details')

    named = [p for p in fn['params'] if p['name']]
    missing = [p['name'] for p in named if p['name'] not in over and p['name'] not in paramdict]
    if missing:
        raise SystemExit(f"{fn['file']}::{fn['name']} undocumented params {missing}")
    extra = [k for k in over if k not in [p['name'] for p in named]]
    if extra:
        raise SystemExit(f"{fn['file']}::{fn['name']} unknown params {extra}")

    lines = [f'{indent}/**', f'{indent} * @brief {brief}']

    if details:
        lines.append(f'{indent} *')
        for i, ln in enumerate(clean(details).split('|')):
            prefix = f'{indent} * @details ' if i == 0 else f'{indent} *          '
            lines.append(prefix + ln.strip())

    if named:
        lines.append(f'{indent} *')
        for p in named:
            name = p['name']
            raw = clean(over.get(name) or paramdict.get(name) or '')
            m = re.match(r'^(in,out|inout|in|out)\s*:\s*(.*)$', raw, re.I)
            d = direction_of(name, p['type'], m.group(1) if m else None)
            desc = m.group(2) if m else raw
            tag = f'@param[{d}]'
            pad = ' ' * max(1, PARAM_TAG_WIDTH - len(tag))
            lines.append(f'{indent} * {tag}{pad}{name} {desc}')

    if ret:
        lines += [f'{indent} *', f'{indent} * @return {clean(ret)}']

    if retvals:
        if not ret:
            lines.append(f'{indent} *')
        for code, desc in retvals.items():
            pad = ' ' * max(1, 6 - len(str(code)))
            lines.append(f'{indent} * @retval{pad}{code} {clean(desc)}')

    if note:
        lines += [f'{indent} *', f'{indent} * @note {clean(note)}']
    if warning:
        lines += [f'{indent} *', f'{indent} * @warning {clean(warning)}']
    if see:
        lines += [f'{indent} *', f'{indent} * @see {clean(see)}']

    lines.append(f'{indent} */')
    return lines


def find_leading_block(lines, sig_index):
    j = sig_index - 1
    gap = 0
    while j >= 0 and lines[j].strip() == '':
        gap += 1
        j -= 1
        if gap > 2:
            return None
    if j < 0 or not lines[j].strip().endswith('*/'):
        return None
    end = j
    while j >= 0 and '/*' not in lines[j]:
        j -= 1
    if j < 0:
        return None
    # a trailing comment on the previous statement line is not this function's
    # documentation block (`char *f(void);   /* note */`)
    if not lines[j].lstrip().startswith('/*'):
        return None
    return (j, end)


def block_kind(lines, span):
    first = lines[span[0]].strip()
    if first.startswith('/**'):
        return 'doxygen'
    body = []
    for l in lines[span[0]:span[1] + 1]:
        s = l.strip().strip('/*').strip('*').strip()
        if s:
            body.append(s)
    if len(body) == 1 and SECTION_RE.match(body[0]):
        return 'section'
    return 'legacy'


def load_notes(notes_path):
    """Load the description table, merging any `<name>.part*.json` shards that
    sibling workers produced."""
    notes = json.load(open(notes_path, encoding='utf-8')) if os.path.exists(notes_path) else {}
    base = notes_path[:-len('.json')]
    d = os.path.dirname(notes_path) or '.'
    prefix = os.path.basename(base) + '.part'
    for fn in sorted(os.listdir(d)):
        if fn.startswith(prefix) and fn.endswith('.json'):
            part = json.load(open(os.path.join(d, fn), encoding='utf-8'))
            for k, v in part.items():
                notes.setdefault(k, v)
    return notes


def apply_file(src_path, notes_path, paramdict, force=False, dry=False):
    raw = open(src_path, encoding='utf-8').read()
    notes = load_notes(notes_path)
    is_header = src_path.endswith('.h')
    # a header may hold both `static inline` definitions and plain prototypes
    fns = find_functions(src_path)
    if is_header:
        fns = fns + find_prototypes(src_path)
    lines = raw.split('\n')

    ops = []
    skipped = 0
    for fn in fns:
        spec = notes.get(fn['name'])
        if spec is None:
            skipped += 1
            continue
        idx = (fn.get('line') or fn.get('sig_start')) - 1
        indent = re.match(r'^(\s*)', lines[idx]).group(1)
        span = find_leading_block(lines, idx)
        kind = block_kind(lines, span) if span else None
        if kind == 'doxygen' and not force:
            continue
        block = render_block(fn, spec, paramdict, indent)
        if kind == 'section':
            at = span[1] + 1
            if at < len(lines) and lines[at].strip() == '':
                at += 1
            ops.append((at, at, block))
        elif span:
            ops.append((span[0], span[1] + 1, block))
        else:
            ops.append((idx, idx, block))

    ops.sort(key=lambda o: -o[0])
    for start, end, block in ops:
        lines[start:end] = block

    # never let a block abut the previous statement: keep one blank line between
    out = []
    for idx, ln in enumerate(lines):
        if ln.strip().startswith('/**') and out and out[-1].strip() != '' \
                and not out[-1].strip().startswith(('/*', '*/')):
            out.append('')
        out.append(ln)
    lines = out

    text = '\n'.join(lines)
    if not dry and text != raw:
        open(src_path, 'w', encoding='utf-8', newline='\n').write(text)
    return len(ops), skipped


def notes_key(src):
    """src/base/hpe_util.c -> base__hpe_util.c.json (extension kept so that the
    .c definition and the .h prototype get separate description tables)."""
    rel = src.replace('\\', '/')
    for pre in ('src/', './src/'):
        if rel.startswith(pre):
            rel = rel[len(pre):]
    return rel.replace('/', '__') + '.json'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('paths', nargs='*')
    ap.add_argument('--notes-dir', default='tools/notes')
    ap.add_argument('--paramdict', default='tools/paramdict.json')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--dry', action='store_true')
    ap.add_argument('--allow-missing', action='store_true')
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    paramdict = load_paramdict(args.paramdict)

    targets = args.paths or sorted(glob.glob('src/**/*.c', recursive=True) +
                                   glob.glob('src/**/*.h', recursive=True))
    total = undoc = 0
    for src in targets:
        src = src.replace('\\', '/')
        npath = os.path.join(args.notes_dir, notes_key(src))
        if not os.path.exists(npath) and not glob.glob(
                os.path.join(args.notes_dir, notes_key(src)[:-len('.json')] + '.part*.json')):
            if not args.allow_missing:
                print(f'   ?  {src}  (no notes)')
            continue
        n, sk = apply_file(src, npath, paramdict, args.force, args.dry)
        total += n
        undoc += sk
        flag = 'dry' if args.dry else 'ok'
        print(f'{n:4d} blocks  {flag:3s}  skip={sk:4d}  {src}')
    print(f'---- {total} blocks, {undoc} functions without notes')


if __name__ == '__main__':
    main()
