"""Extract C function definitions and header prototypes from the Hope sources.

String/comment aware: literals and comments are masked before scanning so that
braces or parentheses inside them never confuse the parser.
"""

import glob
import json
import os
import re
import sys

CTRL_KEYWORDS = {
    'if', 'for', 'while', 'switch', 'else', 'do', 'return', 'sizeof',
    'case', 'default', 'goto', 'typedef', 'struct', 'union', 'enum',
}


def mask(text):
    """Replace literals and comments with spaces, preserving line structure."""
    out = []
    i = 0
    n = len(text)
    state = 'code'
    while i < n:
        c = text[i]
        if state == 'code':
            if c == '/' and i + 1 < n and text[i + 1] == '/':
                state = 'line'
                out.append(' ')
            elif c == '/' and i + 1 < n and text[i + 1] == '*':
                state = 'block'
                out.append(' ')
            elif c == '"':
                state = 'str'
                out.append(' ')
            elif c == "'":
                state = 'chr'
                out.append(' ')
            else:
                out.append(c)
        elif state == 'line':
            if c == '\n':
                state = 'code'
                out.append('\n')
            else:
                out.append(' ')
        elif state == 'block':
            if c == '*' and i + 1 < n and text[i + 1] == '/':
                out.append('  ')
                i += 2
                state = 'code'
                continue
            out.append('\n' if c == '\n' else ' ')
        else:  # str / chr
            quote = '"' if state == 'str' else "'"
            if c == '\\':
                out.append(' ')
                i += 1
                if i < n:
                    out.append('\n' if text[i] == '\n' else ' ')
            elif c == quote:
                state = 'code'
                out.append(' ')
            else:
                out.append('\n' if c == '\n' else ' ')
        i += 1
    return ''.join(out)


def join_signature(lines, start, end):
    """Join a (possibly wrapped) declarator into a single logical line."""
    parts = [lines[i].strip() for i in range(start, end)]
    return ' '.join(parts)


def split_params(param_text):
    """Split a parameter list on top-level commas."""
    params = []
    depth = 0
    cur = ''
    for ch in param_text:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            params.append(cur)
            cur = ''
            continue
        cur += ch
    if cur.strip():
        params.append(cur)
    return [p.strip() for p in params if p.strip()]


def clean_type(t):
    t = re.sub(r'\s+', ' ', t).strip()
    t = t.replace(' *', ' *').replace('* ', '* ')
    t = re.sub(r'\s*\*\s*', ' *', t)
    return t.strip()


def parse_param(raw):
    """Return (type, name) for a single parameter declarator."""
    # function pointer: void (*cb)(int *)
    m = re.match(r'^(.+?)\(\s*\*\s*(\w+)\s*\)\s*\(.*\)$', raw)
    if m:
        return clean_type(m.group(1)) + ' (*)(...)', m.group(2)
    raw = raw.strip()
    if raw == 'void':
        return 'void', ''
    m = re.match(r'^(.*?)([A-Za-z_]\w*)\s*(\[\s*\w*\s*\])?$', raw)
    if m:
        t = m.group(1).strip()
        name = m.group(2)
        arr = (m.group(3) or '').strip()
        if not t:
            t = 'int'
        if arr:
            t += ' ' + arr
        return clean_type(t), name
    return raw, ''


def find_functions(path):
    raw = open(path, encoding='utf-8', errors='replace').read()
    masked = mask(raw)
    mlines = masked.split('\n')
    rlines = raw.split('\n')
    out = []
    i = 0
    while i < len(mlines):
        line = mlines[i].rstrip()
        s = line.strip()
        if not s or s.startswith('#'):
            i += 1
            continue
        # the opening brace must be the next non-blank masked line
        j = i + 1
        while j < len(mlines) and mlines[j].strip() == '':
            j += 1
        if j >= len(mlines) or mlines[j].strip() != '{':
            i += 1
            continue
        if not line.endswith(')'):
            i += 1
            continue
        # signature may wrap: walk back until we hit a ';', '}', '#' or blank line
        k = i
        while k - 1 >= 0:
            prev = mlines[k - 1].strip()
            if prev == '' or prev.endswith(';') or prev.endswith('}') or prev.startswith('#'):
                break
            k -= 1
        sig = join_signature(rlines, k, i + 1)
        masked_sig = join_signature(mlines, k, i + 1)
        lp = masked_sig.find('(')
        rp = masked_sig.rfind(')')
        if lp < 0 or rp < lp:
            i += 1
            continue
        prefix = masked_sig[:lp].strip()
        tail = prefix.split()[-1] if prefix.split() else ''
        base = re.sub(r'^\W+', '', tail)
        if not base or base in CTRL_KEYWORDS:
            i += 1
            continue
        # a control-flow statement must never be mistaken for a definition
        first = masked_sig.strip().split()[0] if masked_sig.strip() else ''
        if re.sub(r'^\W+', '', first) in CTRL_KEYWORDS and not prefix.startswith('static'):
            i += 1
            continue
        param_text = masked_sig[lp + 1:rp]
        # return type = everything before the identifier, pointer stars included
        cut = prefix.rfind(base)
        ret_raw = prefix[:cut].strip() if cut > 0 else prefix[:cut]
        is_static = ret_raw.startswith('static')
        if is_static:
            ret_raw = ret_raw[len('static'):].strip()
        # `static inline` / `inline static` / plain `inline` headers
        if ret_raw.startswith('inline'):
            ret_raw = ret_raw[len('inline'):].strip()
            if ret_raw.startswith('static'):
                is_static = True
                ret_raw = ret_raw[len('static'):].strip()
        # body extent
        d = 0
        end = j
        while end < len(mlines):
            d += mlines[end].count('{') - mlines[end].count('}')
            if d == 0:
                break
            end += 1
        params = [parse_param(p) for p in split_params(param_text)]
        out.append({
            'file': path.replace('\\', '/'),
            'name': base,
            'return_type': clean_type(ret_raw),
            'params': [{'type': t, 'name': n} for t, n in params],
            'static': is_static,
            'sig_start': k + 1,
            'brace_line': i + 1,
            'body_lines': end - j + 1,
            'signature': sig,
        })
        i = end + 1
    return out


def line_depths(mlines):
    """Brace depth at the *start* of every masked line."""
    depths = []
    d = 0
    for l in mlines:
        depths.append(d)
        d += l.count('{') - l.count('}')
    return depths


def find_prototypes(path):
    raw = open(path, encoding='utf-8', errors='replace').read()
    masked = mask(raw)
    mlines = masked.split('\n')
    rlines = raw.split('\n')
    depths = line_depths(mlines)
    out = []
    i = 0
    while i < len(mlines):
        if not mlines[i].rstrip().endswith(');') or depths[i] != 0:
            i += 1
            continue
        if mlines[i].strip().startswith(('*', '/')):
            i += 1
            continue
        # walk back to the statement start, staying at depth 0
        k = i
        while k - 1 >= 0 and depths[k - 1] == 0:
            prev = mlines[k - 1].strip()
            if prev == '' or prev.endswith(';') or prev.endswith('}') or prev.startswith('#'):
                break
            k -= 1
        masked_sig = join_signature(mlines, k, i + 1)
        if '(' not in masked_sig or '{' in masked_sig or 'return' in masked_sig:
            i += 1
            continue
        # function-pointer typedefs are documented with their typedef, not here
        if '(*' in masked_sig.replace(' ', '') or 'typedef' in masked_sig:
            i += 1
            continue
        sig = join_signature(rlines, k, i + 1)
        head = masked_sig.split('(')[0].strip()
        tail = head.split()[-1] if head.split() else ''
        base = re.sub(r'^\W+', '', tail)
        if not base or base in CTRL_KEYWORDS:
            i += 1
            continue
        lp = masked_sig.find('(')
        rp = masked_sig.rfind(')')
        if rp < lp:
            i += 1
            continue
        param_text = masked_sig[lp + 1:rp]
        head_raw = masked_sig[:lp].strip()
        cut = head_raw.rfind(base)
        ret_raw = head_raw[:cut].strip() if cut > 0 else ''
        is_static = ret_raw.startswith('static')
        if is_static:
            ret_raw = ret_raw[len('static'):].strip()
        if ret_raw.startswith('inline'):
            ret_raw = ret_raw[len('inline'):].strip()
        params = [parse_param(p) for p in split_params(param_text)]
        out.append({
            'file': path.replace('\\', '/'),
            'name': base,
            'return_type': clean_type(ret_raw),
            'params': [{'type': t, 'name': n} for t, n in params],
            'static': is_static,
            'line': k + 1,
            'end_line': i + 1,
            'signature': sig,
        })
        i += 1
    return out


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    defs = []
    for p in sorted(glob.glob('src/**/*.c', recursive=True)):
        defs.extend(find_functions(p))
    # headers may carry `static inline` definitions alongside prototypes
    protos = []
    for p in sorted(glob.glob('src/**/*.h', recursive=True)):
        defs.extend(find_functions(p))
        protos.extend(find_prototypes(p))
    data = {'defs': defs, 'protos': protos}
    with open('tools/functions.json', 'w', encoding='utf-8') as fh:
        json.dump(data, fh, ensure_ascii=False, indent=1)
    print(f'defs={len(defs)} protos={len(protos)}')
    byfile = {}
    for d in defs:
        byfile[d['file']] = byfile.get(d['file'], 0) + 1
    for k in sorted(byfile, key=lambda x: -byfile[x]):
        print(f'{byfile[k]:5d}  {k}')


if __name__ == '__main__':
    main()
