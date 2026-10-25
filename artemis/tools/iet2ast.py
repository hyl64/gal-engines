#!/usr/bin/env python3
"""iet2ast - convert an Artemis KAG-format scenario (.iet) to the runtime's
simplified .ast (text blocks), so the from-scratch runtime can play a real
title's dialogue.

    iet2ast.py <scenario.iet> <out.ast>

The KAG constructs handled: *labels -> blocks, dialogue lines (kana-bearing,
non-tag), [jump storage=... target=...] -> goto, [s] -> end.  Inline tags
([ruby ...], [rp], ...) are stripped from the dialogue text; the runtime
renders plain lines.  Shift-JIS input, UTF-8 output."""
import re, sys

data = open(sys.argv[1], 'rb').read().decode('shift_jis', errors='replace')
lines = data.replace('\r\n', '\n').split('\n')
blocks, order, cur = {}, [], None
kana = re.compile('[\u3040-\u309f]')

def clean(text):
    text = re.sub(r'\[ruby[^\]]*\]', '', text)
    text = re.sub(r'\[/ruby\]', '', text)
    text = re.sub(r'\[[^\]]*\]', '', text)
    return text.replace('"', '').strip()

for raw in lines:
    l = raw.strip()
    if not l or l.startswith('//') or l.startswith(';'): continue
    if l.startswith('*'):
        cur = l[1:].split()[0]
        if cur not in blocks: blocks[cur] = []; order.append(cur)
        continue
    if l.startswith('['):
        m = re.match(r'\[jump\s+storage="?([^"\]]*)"?\s*(?:target="?(\w*)"?)?\s*\]', l)
        if m and cur and m.group(2): blocks[cur].append(('goto', m.group(2)))
        continue
    text = re.sub(r'@\[?(\w+)\]?', '', l)
    text = clean(text.replace('@', ''))
    if not text or not kana.search(text): continue
    if cur: blocks[cur].append(('text', text))

out = ['astver = "2.0"', 'text = {']
for name in order:
    if not blocks[name]: continue
    out.append(f'  "{name}" = {{')
    for kind, val in blocks[name]:
        if kind == 'text': out.append(f'    "{val}"')
        elif kind == 'goto': out.append(f'    goto = "{val}"')
        elif kind == 'end': out.append('    end_now = true')
    out.append('  }')
out.append('}')
open(sys.argv[2], 'w', encoding='utf-8').write('\n'.join(out))
print(f'{sys.argv[2]}: {len(order)} blocks')
