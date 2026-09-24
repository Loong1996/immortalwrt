#!/usr/bin/env python3
"""Check the language of every string in net/httpd.c outside the page.

Usage: i18ncheck.py page.html src/net/httpd.c

Two rules, both about strings the embedded page does not contain:

  - What goes to the console is English.  printf() and friends end up on
    the serial line and in /log, which the page shows verbatim; a Chinese
    line there is mojibake on most terminals.
  - What goes to the page in Chinese has an English translation.  The page
    translates everything the device sends through its I18N / I18P
    dictionaries; a string they do not cover shows up as Chinese in the
    English interface.

The second rule works on each string the way the page will see it: adjacent
literals joined (a macro in between becomes a placeholder, as does every
conversion like %s), the write-progress protocol letters taken off and the
line put together the way wrstep() shows it, {"err":"..."} unwrapped.  A %s
that is filled with Chinese at run time is beyond it -- those need a test.

Exits 1 and lists every offender with its line number.
"""
import io
import json
import re
import sys

CJK = re.compile('[一-鿿　-〿＀-￯]')
CONV = re.compile(r'%(?:pM|pI4|0?\d*(?:ll|l|z|h)?[sduxXcp])')
CONSOLE = {'printf', 'puts', 'putc', 'debug', 'panic', 'log_err',
           'log_warning', 'log_info', 'log_debug', 'pr_err', 'pr_warn',
           'pr_info', 'pr_debug'}
HOLE = 'X9'


def dictionaries(page):
    def one(name):
        a = page.index('var ' + name + '={\n') + len('var ' + name + '=')
        z = page.index('\n};', a)
        return json.loads(re.sub(r',\s*}$', '}', page[a:z + 2]))

    n, p = one('I18N'), one('I18P')
    # The parallel NAND half keeps its own, merged into these at run time.
    if 'var I18N_PN={\n' in page:
        n.update(one('I18N_PN'))
        p.update(one('I18P_PN'))
    rx = re.compile('|'.join(re.escape(k)
                             for k in sorted(p, key=len, reverse=True)))
    return n, p, rx


def outside_page(src):
    """The C around the page, with the page's lines blanked (numbers kept)."""
    lines = src.split('\n')
    b = next(i for i, l in enumerate(lines) if '@@PAGE_BEGIN@@' in l)
    e = next(i for i, l in enumerate(lines) if '@@PAGE_END@@' in l)
    return '\n'.join('' if b <= i <= e else l for i, l in enumerate(lines))


def literals(code):
    """Yield (line, call, text) for each run of adjacent string literals.

    call is the function whose argument list the run sits in.  Comments are
    skipped; so are character constants.
    """
    i, n, line = 0, len(code), 1
    stack = []              # enclosing call names, '' for plain parens
    run = None              # [line, call, parts, depth]
    last_ident = ''

    def flush():
        nonlocal run
        if run:
            yield_list.append((run[0], run[1], ''.join(run[2])))
        run = None

    yield_list = []
    while i < n:
        c = code[i]
        if c == '\n':
            line += 1
            i += 1
            continue
        if code.startswith('/*', i):
            j = code.index('*/', i + 2)
            line += code.count('\n', i, j)
            i = j + 2
            continue
        if code.startswith('//', i):
            i = code.index('\n', i)
            continue
        if c == "'":
            j = i + 1
            while code[j] != "'":
                j += 2 if code[j] == '\\' else 1
            i = j + 1
            last_ident = ''
            continue
        if c == '"':
            j = i + 1
            while code[j] != '"':
                j += 2 if code[j] == '\\' else 1
            body = code[i + 1:j]
            if run and run[3] == len(stack):
                if last_ident:
                    run[2].append(HOLE)
                run[2].append(body)
            else:
                flush()
                run = [line, stack[-1] if stack else '', [body], len(stack)]
            last_ident = ''
            i = j + 1
            continue
        if c.isalpha() or c == '_':
            j = i
            while j < n and (code[j].isalnum() or code[j] == '_'):
                j += 1
            last_ident = code[i:j]
            i = j
            continue
        if c.isspace():
            i += 1
            continue
        if c == '(':
            flush()
            stack.append(last_ident)
        elif c == ')':
            flush()
            if stack:
                stack.pop()
        else:
            flush()
        last_ident = ''
        i += 1
    flush()
    return yield_list


def unescape(s):
    return (s.replace('\\n', '\n').replace('\\r', '').replace('\\t', ' ')
             .replace('\\"', '"').replace('\\\\', '\\'))


def as_shown(s):
    """The lines the page will run through its dictionaries."""
    s = CONV.sub(HOLE, unescape(s))
    m = re.fullmatch(r'\{"err":"(.*)"\}\n?', s, re.S)
    if m:
        s = m.group(1)
    out = []
    for l in s.split('\n'):
        l = l.strip()
        if not l:
            continue
        # wrstep(): s <what> [size] | f <why> | r <name> ... | c bad <why>
        if l.startswith('s '):
            w = l[2:].split(' ')
            if w[-1] == HOLE:
                w.pop()
            out.append(' '.join(w) + '…')
        elif l.startswith('f '):
            out.append(l[2:])
        elif l.startswith('r '):
            out.append(l[2:].split(' ')[0])
        elif l.startswith('c bad '):
            out.append('回读校验没通过：' + l[6:])
        else:
            out.append(l)
    return out


def main():
    page = io.open(sys.argv[1], encoding='utf-8').read()
    src = io.open(sys.argv[2], encoding='utf-8').read()
    n, p, rx = dictionaries(page)

    def tr(s):
        return n[s] if s in n else rx.sub(lambda m: p[m.group(0)], s)

    bad = []
    total = 0
    for line, call, text in literals(outside_page(src)):
        if not CJK.search(text):
            continue
        total += 1
        if call in CONSOLE:
            bad.append((line, 'console output in Chinese', text))
            continue
        for shown in as_shown(text):
            left = tr(shown)
            if CJK.search(left) and CJK.search(shown):
                bad.append((line, 'no English for it', shown + '  =>  ' + left))

    for line, why, what in bad:
        print('httpd.c:%d: %s: %s' % (line, why, what))
    if bad:
        print('%d of %d Chinese strings failed' % (len(bad), total))
        sys.exit(1)
    print('OK: %d Chinese strings, all translated, none on the console'
          % total)


if __name__ == '__main__':
    main()
