# -*- coding: utf-8 -*-
from pypinyin import pinyin, Style
from zhconv import convert
from collections import defaultdict

def is_bmp(ch):
    if not ch or len(ch) != 1:
        return False
    o = ord(ch)
    return o <= 0xFFFF and not (0xD800 <= o <= 0xDFFF)

def to_simp_ch(ch):
    if len(ch) != 1:
        return ""
    s = convert(ch, "zh-cn")
    out = s if len(s) == 1 else ch
    return out if is_bmp(out) else ""

syl_map = defaultdict(list)
empty = 0
ok = 0
for code in range(0x4E00, 0x9FA6):
    ch = to_simp_ch(chr(code))
    if not ch:
        empty += 1
        continue
    try:
        pys = pinyin(ch, style=Style.NORMAL, heteronym=False)
        if not pys or not pys[0]:
            continue
        py = pys[0][0]
        if not py or not py.isalpha():
            continue
        if len(syl_map[py]) >= 22:
            continue
        if ch not in syl_map[py]:
            syl_map[py].append(ch)
            ok += 1
    except Exception:
        pass

print("empty_simp", empty, "ok", ok, "syllables", len(syl_map))
