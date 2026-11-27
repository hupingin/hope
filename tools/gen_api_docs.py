"""Generate docs/api.md from the Doxygen comments in the Hope headers.

Scans every .h file under src/ (including hope.h and the internal headers),
parses function declarations / static-inline definitions and type definitions
together with their preceding comment blocks (@brief/@details/@param/@return/
@retval/@note/@see, bilingual), and renders a per-module Chinese API reference.

Idempotent: re-running overwrites docs/api.md. Read-only for src/ and docs/.
"""

import glob
import os
import re

CTRL_KEYWORDS = {
    'if', 'for', 'while', 'switch', 'else', 'do', 'return', 'sizeof',
    'case', 'default', 'goto', 'typedef', 'struct', 'union', 'enum',
}


CJK_RE = re.compile(r'[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]')


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
            out.append('\n' if c == '\n' else ' ')
            if c == '\n':
                state = 'code'
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


def line_depths(mlines):
    """Brace depth at the *start* of every masked line."""
    depths = []
    d = 0
    for l in mlines:
        depths.append(d)
        d += l.count('{') - l.count('}')
    return depths


def clean_type(t):
    t = re.sub(r'\s+', ' ', t).strip()
    t = re.sub(r'\s*\*\s*', ' *', t)
    return t.strip()


def parse_param(raw):
    """Return (type, name) for a single parameter declarator."""
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


def statement_start(mlines, depths, i):
    """Walk back from line i to the first line of its statement (depth 0)."""
    k = i
    while k - 1 >= 0 and depths[k - 1] == 0:
        prev = mlines[k - 1].strip()
        if prev == '' or prev.endswith(';') or prev.endswith('}') \
                or prev.startswith('#'):
            break
        k -= 1
    return k


def join_lines(lines, start, end):
    return ' '.join(lines[j].strip() for j in range(start, end))


# ------------------------------------------------------------------ comments

def find_comment(rlines, start):
    """Return the raw comment block lines directly above line `start`."""
    j = start
    while j - 1 >= 0 and rlines[j - 1].strip() == '':
        j -= 1
    if j - 1 < 0 or not rlines[j - 1].rstrip().endswith('*/'):
        return None
    end = j - 1
    b = end
    while b >= 0:
        if rlines[b].strip().startswith('/*'):
            break
        b -= 1
    if b < 0:
        return None
    return rlines[b:end + 1]


def parse_doxygen(block):
    """Parse a /** ... */ (or /* ... */) block into structured fields."""
    doc = {
        'brief': '', 'details': '', 'params': {}, 'return': '',
        'retvals': [], 'note': '', 'see': [], 'plain': '',
        'is_doxygen': False,
    }
    if block is None:
        return doc
    stripped = [re.sub(r'^\s*\*\s?', '', l).strip() for l in block[1:-1]]
    if block[0].strip().startswith('/**'):
        doc['is_doxygen'] = True
    if not doc['is_doxygen']:
        doc['plain'] = ' '.join(s for s in stripped if s)
        return doc
    last = None  # (field, key)
    for s in stripped:
        if not s:
            continue
        m = re.match(r'@param\s*\[([^\]]*)\]\s+([A-Za-z_]\w*)\s*(.*)$', s)
        if m:
            doc['params'][m.group(2)] = {'dir': m.group(1), 'desc': m.group(3)}
            last = ('param', m.group(2))
            continue
        m = re.match(r'@retval\s+(\S+)\s+(.*)$', s)
        if m:
            doc['retvals'].append({'value': m.group(1), 'desc': m.group(2)})
            last = ('retval', len(doc['retvals']) - 1)
            continue
        m = re.match(r'@(brief|details|return|note)\s*(.*)$', s)
        if m:
            key = m.group(1)
            doc[key] = m.group(2)
            last = ('field', key)
            continue
        m = re.match(r'@see\s*(.*)$', s)
        if m:
            doc['see'].extend(m.group(1).split())
            last = ('field', 'see')
            continue
        if s.startswith('@'):
            last = None
            continue
        # continuation line for the previous directive
        if last is None:
            continue
        kind, key = last
        if kind == 'field' and key != 'see':
            doc[key] = (doc[key] + ' ' + s).strip()
        elif kind == 'param':
            doc['params'][key]['desc'] = \
                (doc['params'][key]['desc'] + ' ' + s).strip()
        elif kind == 'retval':
            doc['retvals'][key]['desc'] = \
                (doc['retvals'][key]['desc'] + ' ' + s).strip()
    return doc


def zh(text):
    """Keep the Chinese half of a `中文 / English` bilingual string."""
    if not text:
        return text
    if not CJK_RE.search(text):
        return text
    best = None
    for m in re.finditer(r'/', text):
        i = m.start()
        # candidate separator: a slash followed by whitespace (spacing optional
        # before it, so both `中文 / English` and `中文）/ English` split)
        if i + 1 >= len(text) or not text[i + 1].isspace():
            continue
        head, tail = text[:i], text[i + 1:]
        if CJK_RE.search(head) and not CJK_RE.search(tail):
            best = head
            break
    return best.strip() if best is not None else text.strip()


def zh_multiline(text):
    """Keep the Chinese lines of a line-split bilingual description."""
    if not text:
        return text
    lines = text.split('\n')
    if not any(CJK_RE.search(l) for l in lines):
        return text.strip()
    kept = [l for l in lines if CJK_RE.search(l)]
    return ' '.join(k.strip() for k in kept)


# ------------------------------------------------------------------- parsing

def find_prototypes(path, raw, masked):
    """Return function prototypes declared at brace depth 0."""
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
        k = statement_start(mlines, depths, i)
        masked_sig = join_lines(mlines, k, i + 1)
        if '(' not in masked_sig or '{' in masked_sig \
                or 'typedef' in masked_sig:
            i += 1
            continue
        if '(*' in masked_sig.replace(' ', ''):
            i += 1
            continue
        sig = join_lines(rlines, k, i + 1)
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
        for kw in ('static', 'inline'):
            if ret_raw.startswith(kw):
                ret_raw = ret_raw[len(kw):].strip()
        params = [parse_param(p) for p in split_params(param_text)]
        doc = parse_doxygen(find_comment(rlines, k))
        out.append({
            'kind': 'function',
            'name': base,
            'return_type': clean_type(ret_raw),
            'params': [{'type': t, 'name': n} for t, n in params],
            'signature': sig,
            'doc': doc,
            'line': k + 1,
        })
        i += 1
    return out


def find_inline_defs(path, raw, masked):
    """Return static/inline function *definitions* living in a header."""
    mlines = masked.split('\n')
    rlines = raw.split('\n')
    depths = line_depths(mlines)
    out = []
    i = 0
    while i < len(mlines):
        s = mlines[i].strip()
        if not s or s.startswith('#') or depths[i] != 0 \
                or not s.endswith('{'):
            i += 1
            continue
        # signature must end with ')' on the previous non-blank masked line
        j = i
        while j - 1 >= 0 and mlines[j - 1].strip() == '':
            j -= 1
        if j - 1 < 0 or not mlines[j - 1].rstrip().endswith(')'):
            i += 1
            continue
        k = statement_start(mlines, depths, j - 1)
        masked_sig = join_lines(mlines, k, i + 1)
        if '(' not in masked_sig or 'typedef' in masked_sig:
            i += 1
            continue
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
        for kw in ('static', 'inline'):
            if ret_raw.startswith(kw):
                ret_raw = ret_raw[len(kw):].strip()
        params = [parse_param(p) for p in split_params(param_text)]
        # capture the whole definition body for the code block
        d = 0
        end = i
        while end < len(mlines):
            d += mlines[end].count('{') - mlines[end].count('}')
            if d == 0:
                break
            end += 1
        sig = '\n'.join(rlines[k:end + 1])
        doc = parse_doxygen(find_comment(rlines, k))
        out.append({
            'kind': 'function',
            'name': base,
            'return_type': clean_type(ret_raw),
            'params': [{'type': t, 'name': n} for t, n in params],
            'signature': sig,
            'doc': doc,
            'line': k + 1,
            'inline': True,
        })
        i = end + 1
    return out


def find_typedefs(path, raw, masked):
    """Return typedef declarations (structs, enums, aliases, fn pointers)."""
    mlines = masked.split('\n')
    rlines = raw.split('\n')
    depths = line_depths(mlines)
    out = []
    i = 0
    while i < len(mlines):
        if depths[i] != 0 or not mlines[i].strip().startswith('typedef'):
            i += 1
            continue
        # find the end of the statement: first line bringing depth back to 0
        # that contains the terminating ';'
        end = i
        d = depths[i]
        while end < len(mlines):
            d += mlines[end].count('{') - mlines[end].count('}')
            if d == 0 and ';' in mlines[end]:
                break
            end += 1
        if end >= len(mlines):
            i += 1
            continue
        masked_stmt = '\n'.join(mlines[i:end + 1])
        raw_stmt = '\n'.join(rlines[i:end + 1])
        # type name: last identifier before the depth-0 terminating ';'
        tail_line = mlines[end][:mlines[end].rfind(';')].strip()
        m = re.search(r'([A-Za-z_]\w*)\s*$', tail_line)
        name = m.group(1) if m else ''
        if not name:
            # function-pointer typedef: `typedef ... (*Name)(...)`
            m = re.search(r'\(\s*\*\s*([A-Za-z_]\w*)\s*\)', masked_stmt)
            name = m.group(1) if m else ''
        if not name:
            i = end + 1
            continue
        doc = parse_doxygen(find_comment(rlines, i))
        # classify
        if re.search(r'\bstruct\b|\bunion\b|\benum\b', masked_stmt) and '{' in masked_stmt:
            tkind = 'enum' if re.search(r'\benum\b', masked_stmt) else 'struct'
        elif '(*' in masked_stmt.replace(' ', ''):
            tkind = 'callback'
        else:
            tkind = 'alias'
        out.append({
            'kind': 'type',
            'tkind': tkind,
            'name': name,
            'code': raw_stmt.strip('\n'),
            'doc': doc,
            'line': i + 1,
        })
        i = end + 1
    return out


# --------------------------------------------------------------- rendering

def esc(t):
    return t.replace('|', '\\|')


def slug(text):
    s = text.lower()
    s = re.sub(r'[^\w\u4e00-\u9fff\- ]', '', s)
    s = re.sub(r' ', '-', s)
    return s


def render_function(f):
    doc = f['doc']
    out = []
    out.append(f"#### `{f['name']}`\n")
    out.append('```c\n' + f['signature'].rstrip() + '\n```\n')
    if doc['brief']:
        out.append(zh(doc['brief']) + '\n')
    det = zh_multiline(doc['details'])
    if det:
        out.append(f'**详细说明**：{det}\n')
    # parameter table: 名称/方向/说明, aligned with the real signature params
    sig_names = [p['name'] for p in f['params'] if p['name']]
    rows = []
    for p in f['params']:
        if not p['name'] and p['type'] == 'void':
            continue
        info = doc['params'].get(p['name'])
        d = info['dir'] if info and info['dir'] else '—'
        desc = zh(info['desc']) if info and info['desc'] else '—'
        rows.append(f"| `{esc(p['name'])}` | {esc(d)} | {esc(desc)} |")
    if rows:
        out.append('**参数**：\n')
        out.append('| 名称 | 方向 | 说明 |')
        out.append('| --- | --- | --- |')
        out.extend(rows)
        out.append('')
    elif doc['params']:
        out.append('**参数**：无（签名与注释参数名不一致，请核对头文件）\n')
    ret = zh(doc['return'])
    if doc['retvals']:
        out.append(f'**返回**：{ret}\n' if ret else '**返回**：\n')
        for rv in doc['retvals']:
            out.append(f"- `{esc(rv['value'])}`：{esc(zh(rv['desc']))}")
        out.append('')
    elif ret:
        out.append(f'**返回**：{ret}\n')
    if doc['note']:
        out.append(f"**注意**：{zh(doc['note'])}\n")
    if doc['see']:
        out.append('**参见**：' + '、'.join(f'`{n}`' for n in doc['see']) + '\n')
    return '\n'.join(out)


def render_type(t):
    doc = t['doc']
    out = []
    out.append(f"#### `{t['name']}`\n")
    out.append('```c\n' + t['code'] + '\n```\n')
    desc = ''
    if doc['brief']:
        desc = zh(doc['brief'])
    elif doc['plain'] and CJK_RE.search(doc['plain']):
        # plain comments may also be bilingual; keep only the Chinese lines
        desc = zh_multiline(doc['plain'])
    if desc:
        out.append(desc + '\n')
    return '\n'.join(out)


# --------------------------------------------------------------------- main

MODULE_LABELS = {
    'src/base': '基础类型与工具',
    'src/css': 'CSS 解析与样式',
    'src/dom': 'DOM 树',
    'src/html': 'HTML 解析',
    'src/js': 'JavaScript 引擎',
    'src/layout': '布局',
    'src/paint': '绘制',
    'src/platform': '平台层',
    'src/render': '渲染与资源',
    'src': '引擎入口',
}

INTERNAL_FILES = {'hope_internal.h', 'hpe_js_internal.h'}


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    files = sorted(glob.glob(os.path.join(root, 'src', '**', '*.h'),
                             recursive=True))
    rel_files = [os.path.relpath(p, root).replace('\\', '/') for p in files]

    per_file = {}
    for rel, path in zip(rel_files, files):
        raw = open(path, encoding='utf-8', errors='replace').read()
        masked = mask(raw)
        funcs = find_prototypes(rel, raw, masked) + \
            find_inline_defs(rel, raw, masked)
        types = find_typedefs(rel, raw, masked)
        funcs.sort(key=lambda f: f['line'])
        types.sort(key=lambda t: t['line'])
        per_file[rel] = {'funcs': funcs, 'types': types}

    # group files by directory, root (src) last
    def dir_of(rel):
        return os.path.dirname(rel) or 'src'

    dirs = sorted({dir_of(r) for r in rel_files}, key=lambda d: (d == 'src', d))

    total_funcs = sum(len(v['funcs']) for v in per_file.values())
    total_types = sum(len(v['types']) for v in per_file.values())

    md = []
    md.append('# Hope UI 引擎 API 参考文档\n')
    md.append(
        f'本文档由 `tools/gen_api_docs.py` 从 `src/` 下的头文件自动抽取生成，'
        f'共覆盖 {len(rel_files)} 个头文件、{total_funcs} 个函数、'
        f'{total_types} 个类型定义。内容以各声明上方的 Doxygen 注释（'
        f'`@brief`/`@details`/`@param`/`@return`/`@retval`/`@note`/`@see`）'
        f'为准，仅保留中文部分。重复运行脚本会整体覆盖本文件。\n')
    md.append(
        '> 标注「内部」的章节属于引擎内部实现接口（`*_internal.h`），'
        '不作为对外 API 使用。\n')

    # table of contents
    md.append('## 目录\n')
    for d in dirs:
        dr = os.path.relpath(d, root).replace('\\', '/') if d != 'src' else 'src'
        label = MODULE_LABELS.get(dr, '')
        n_f = sum(len(per_file[r]['funcs']) for r in rel_files
                  if dir_of(r) == d)
        n_t = sum(len(per_file[r]['types']) for r in rel_files
                  if dir_of(r) == d)
        md.append(f'- [{dr}（{label}）](#{slug(dr + "（" + label + "）")})'
                  f' —— {n_f} 个函数 / {n_t} 个类型')
    md.append('')

    for d in dirs:
        dr = os.path.relpath(d, root).replace('\\', '/') if d != 'src' else 'src'
        label = MODULE_LABELS.get(dr, '')
        md.append(f'## {dr}（{label}）\n')
        dir_files = sorted([r for r in rel_files if dir_of(r) == d],
                           key=lambda r: os.path.basename(r))
        for rel in dir_files:
            base = os.path.basename(rel)
            funcs = per_file[rel]['funcs']
            types = per_file[rel]['types']
            head = f'### {base}'
            if base in INTERNAL_FILES:
                head += '（内部）'
            md.append(head + '\n')
            md.append(f'{len(funcs)} 个函数，{len(types)} 个类型。\n')
            md.append('**函数**\n')
            if funcs:
                for f in funcs:
                    md.append(render_function(f))
            else:
                md.append('无。\n')
            md.append('**类型**\n')
            if types:
                for t in types:
                    md.append(render_type(t))
            else:
                md.append('无。\n')

    out_path = os.path.join(root, 'docs', 'api.md')
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('\n'.join(md).rstrip() + '\n')

    print(f'files={len(rel_files)} functions={total_funcs} '
          f'types={total_types} -> docs/api.md')


if __name__ == '__main__':
    main()
