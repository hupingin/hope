#!/usr/bin/env python3
"""Lists C functions longer than N lines (default 50) under src/.

C-aware: comments and string/char literals are stripped before brace counting,
so braces inside strings or comments never confuse the count. The reported
length is the line span from the opening brace to the matching close brace,
which is the metric the house style (docs/AI代码生成规范.md, <=50 lines) uses.
"""
import glob
import re
import sys

LIMIT = int(sys.argv[1]) if len(sys.argv) > 1 else 50


def strip_noise(src):
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            i = n if j < 0 else j
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in '"\'':
            quote = c
            i += 1
            while i < n and src[i] != quote:
                i += 2 if src[i] == '\\' else 1
            i += 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def scan(path):
    src = open(path, encoding='utf-8', errors='replace').read()
    s = strip_noise(src)
    res = []
    for m in re.finditer(r'\n([A-Za-z_][\w \t\*]*?[\w\*][ \t]*)\(', s):
        name = m.group(1).split()[-1]
        p = m.end() - 1
        depth = 0
        while p < len(s):
            if s[p] == '(':
                depth += 1
            elif s[p] == ')':
                depth -= 1
                if depth == 0:
                    break
            p += 1
        if p >= len(s):
            continue
        q = p + 1
        while q < len(s) and s[q] in ' \t\n':
            q += 1
        if q < len(s) and s[q] == '{':
            d = 0
            k = q
            while k < len(s):
                if s[k] == '{':
                    d += 1
                elif s[k] == '}':
                    d -= 1
                    if d == 0:
                        break
                k += 1
            startline = s.count('\n', 0, m.start()) + 1
            length = s.count('\n', q, k) + 1
            res.append((length, name, startline))
    return res


def main():
    rows = []
    for path in glob.glob('src/**/*.c', recursive=True):
        for length, name, ln in scan(path):
            if length > LIMIT:
                rows.append((length, path, name, ln))
    rows.sort(reverse=True)
    print(f"functions longer than {LIMIT} lines: {len(rows)}")
    for length, path, name, ln in rows:
        print(f"{length:4d}  {path}:{ln}  {name}")


if __name__ == '__main__':
    main()
