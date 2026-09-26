# -*- coding: utf-8 -*-
"""Simplified CN glyphs + syllable IME + 2/3/4-char phrase matching (no hand phrase enum)."""
from PIL import Image, ImageFont, ImageDraw
from pypinyin import pinyin, Style, lazy_pinyin
from pypinyin.phrases_dict import phrases_dict
from zhconv import convert
from collections import defaultdict
import os
import re

PUNCT = list("，。！？、；：""''（）【】《》…—·￥～＿＠＃＊／＼｜＋－＝＜＞")
UI = list(
    "私聊群组管理创建加入退出发送返回颜色设置成员删除保存取消公共大厅选择好友"
    "已加入未命名的聊天室在线离线系统通知拼音英文空格暂无点文字好友列表"
    "画笔橡皮同步接收发送进度确定关闭打开帮助复制粘贴清空重命名输入消息"
    "解散进入删除选择附近相同占用无法设置拼英空删返群管理列表开始私聊"
    "聊天文字颜色点右上角创建暂无在线好友邀请群主成员离开延迟信号强度"
    "本机在线画板页笔在画在擦新建清页翻页记录中人发送接收同步中"
    "已恢复来自上线掉线冲突笔迹历史字迹"
)

font = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", 12, index=0)
W = 12
MAX_GLYPHS = 7800
MAX_PER_SYL = 28
MAX_PHRASES = 3200  # 2/3/4 字词条
MAX_WORDS_PER_KEY = 8

def is_bmp(ch: str) -> bool:
    """Only BMP codepoints fit uint16_t glyph tables."""
    if not ch or len(ch) != 1:
        return False
    o = ord(ch)
    return o <= 0xFFFF and not (0xD800 <= o <= 0xDFFF)

def to_simp_ch(ch: str) -> str:
    if len(ch) != 1 or not is_bmp(ch):
        return ""
    s = convert(ch, "zh-cn")
    if len(s) == 1 and is_bmp(s):
        return s
    return ch  # zhconv 若出非 BMP，保留原 BMP 字

def to_simp_word(w: str) -> str:
    return "".join(to_simp_ch(c) or c for c in w)

def py_key_of_word(word: str) -> str:
    parts = lazy_pinyin(word, style=Style.NORMAL)
    return "".join(p for p in parts if p and p.isalpha())

def jianpin_of_word(word: str) -> str:
    """简拼：每字声母，zh/ch/sh 保留两位。如 你好→nh"""
    parts = lazy_pinyin(word, style=Style.NORMAL)
    out = []
    for p in parts:
        if not p or not p.isalpha():
            continue
        if len(p) >= 2 and p[:2] in ("zh", "ch", "sh"):
            out.append(p[:2])
        else:
            out.append(p[0])
    return "".join(out)

# --- phrases first (drive glyph demand) ---
phrase_pairs = []  # (pykey, word)
seen_phrase = set()
chars_needed = set(PUNCT + UI)

for word in phrases_dict.keys():
    if not word:
        continue
    w = to_simp_word(word)
    if not (2 <= len(w) <= 4):
        continue
    if any(ord(c) < 0x4E00 or ord(c) > 0x9FFF for c in w):
        continue
    if any(not is_bmp(c) for c in w):
        continue
    key = py_key_of_word(w)
    if not key or len(key) < 2 or not key.isalpha():
        continue
    if (key, w) in seen_phrase:
        continue
    seen_phrase.add((key, w))
    phrase_pairs.append((key, w))
    jp = jianpin_of_word(w)
    if jp and len(jp) >= 2 and jp != key and (jp, w) not in seen_phrase:
        seen_phrase.add((jp, w))
        phrase_pairs.append((jp, w))
    for c in w:
        chars_needed.add(c)
    if len(phrase_pairs) >= MAX_PHRASES * 4:
        break

# Prefer shorter then alpha key
phrase_pairs.sort(key=lambda x: (len(x[1]), x[0], x[1]))
phrase_pairs = phrase_pairs[: MAX_PHRASES * 2]  # keep room for jp+full before glyph filter

syl_map = defaultdict(list)
seen_in_syl = defaultdict(set)

for code in range(0x4E00, 0x9FA6):
    ch = to_simp_ch(chr(code))
    if not ch:
        continue
    try:
        pys = pinyin(ch, style=Style.NORMAL, heteronym=False)
        if not pys or not pys[0]:
            continue
        py = pys[0][0]
        if not py or not py.isalpha():
            continue
        if ch in seen_in_syl[py]:
            continue
        if len(syl_map[py]) >= MAX_PER_SYL:
            continue
        syl_map[py].append(ch)
        seen_in_syl[py].add(ch)
    except Exception:
        pass

ordered = []
seen = set()
for ch in PUNCT + UI:
    ch = to_simp_ch(ch) if (len(ch) == 1 and ord(ch) >= 0x4E00) else ch
    if ch and is_bmp(ch) and ch not in seen:
        ordered.append(ch)
        seen.add(ch)
for ch in sorted(chars_needed, key=ord):
    if ch and is_bmp(ch) and ch not in seen:
        ordered.append(ch)
        seen.add(ch)

syl_keys = sorted(syl_map.keys())
idx = {k: 0 for k in syl_keys}
while len(ordered) < MAX_GLYPHS:
    progressed = False
    for k in syl_keys:
        i = idx[k]
        lst = syl_map[k]
        if i < len(lst):
            ch = lst[i]
            idx[k] = i + 1
            if ch not in seen and is_bmp(ch):
                ordered.append(ch)
                seen.add(ch)
                progressed = True
                if len(ordered) >= MAX_GLYPHS:
                    break
    if not progressed:
        break

sel = set(ordered)
# drop phrases whose glyphs missing
phrase_pairs = [(k, w) for k, w in phrase_pairs if all(c in sel for c in w)]
# re-trim but keep both full + jianpin keys
phrase_pairs = phrase_pairs[: MAX_PHRASES * 2]

syl_map2 = defaultdict(list)
for k, lst in syl_map.items():
    for ch in lst:
        if ch in sel:
            syl_map2[k].append(ch)
syl_map = syl_map2
chars = sorted(ordered, key=lambda c: ord(c))
print("glyphs", len(chars), "syllables", len(syl_map), "phrases", len(phrase_pairs),
      "est_kb", round(len(chars) * 24 / 1024, 1))

out_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "src"))

lines = [
    "#pragma once",
    "#include <stdint.h>",
    "#include <pgmspace.h>",
    f"#define CN_FONT_W {W}",
    f"#define CN_FONT_H {W}",
    "#define CN_FONT_BYTES_PER_GLYPH 24",
    f"#define CN_FONT_COUNT {len(chars)}",
    "typedef struct { uint16_t unicode; uint8_t bits[24]; } CnGlyph;",
    "static const CnGlyph CN_GLYPHS[] PROGMEM = {",
]
for ch in chars:
    img = Image.new("L", (W, W), 0)
    d = ImageDraw.Draw(img)
    bbox = d.textbbox((0, 0), ch, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = max(0, (W - tw) // 2 - bbox[0])
    y = max(-2, (W - th) // 2 - bbox[1] - 1)
    d.text((x, y), ch, font=font, fill=255)
    bits = []
    for row in range(W):
        b0 = b1 = 0
        for col in range(W):
            on = img.getpixel((col, row)) > 100
            if col < 8:
                if on:
                    b0 |= 0x80 >> col
            else:
                if on:
                    b1 |= 0x80 >> (col - 8)
        bits.extend([b0, b1])
    bstr = ",".join(f"0x{b:02X}" for b in bits)
    lines.append(f"  {{0x{ord(ch):04X}, {{{bstr}}}}},")
lines.append("};")
font_path = os.path.join(out_dir, "cn_font_data.h")
open(font_path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print("font", os.path.getsize(font_path))

all_codes = []
syl_entries = []
for py in sorted(syl_map.keys()):
    cs = syl_map[py]
    if not cs:
        continue
    start = len(all_codes)
    for ch in cs:
        all_codes.append(ord(ch))
    syl_entries.append((py, start, len(cs)))

# group phrases by py key for compact lookup
from collections import OrderedDict
py_to_words = OrderedDict()
for key, w in sorted(phrase_pairs, key=lambda x: x[0]):
    py_to_words.setdefault(key, [])
    if w not in py_to_words[key] and len(py_to_words[key]) < MAX_WORDS_PER_KEY:
        py_to_words[key].append(w)

phrase_flat = []  # utf8 strings stored as C string literals
phrase_index = []  # (py, start, count)
for key, words in py_to_words.items():
    start = len(phrase_flat)
    for w in words:
        phrase_flat.append(w)
    phrase_index.append((key, start, len(words)))

dlines = [
    "#pragma once",
    "#include <stdint.h>",
    "#include <pgmspace.h>",
    f"#define PY_SYL_COUNT {len(syl_entries)}",
    f"#define PY_CODE_COUNT {len(all_codes)}",
    f"#define PY_PHRASE_KEY_COUNT {len(phrase_index)}",
    f"#define PY_PHRASE_COUNT {len(phrase_flat)}",
    "typedef struct { const char *py; uint16_t start; uint8_t count; } PySyl;",
    "typedef struct { const char *py; uint16_t start; uint8_t count; } PyPhraseKey;",
    "static const uint16_t PY_CODES[] PROGMEM = {",
]
chunk = []
for code in all_codes:
    chunk.append(f"0x{code:04X}")
    if len(chunk) == 16:
        dlines.append("  " + ", ".join(chunk) + ",")
        chunk = []
if chunk:
    dlines.append("  " + ", ".join(chunk) + ",")
dlines.append("};")
dlines.append("static const PySyl PY_SYLS[] = {")
for py, start, count in syl_entries:
    dlines.append(f'  {{"{py}", {start}, {count}}},')
dlines.append("};")
dlines.append("static const char *const PY_PHRASE_WORDS[] = {")
for w in phrase_flat:
    esc = w.replace("\\", "\\\\").replace('"', '\\"')
    dlines.append(f'  "{esc}",')
dlines.append("};")
dlines.append("static const PyPhraseKey PY_PHRASE_KEYS[] = {")
for py, start, count in phrase_index:
    dlines.append(f'  {{"{py}", {start}, {count}}},')
dlines.append("};")
dict_path = os.path.join(out_dir, "cn_pinyin_data.h")
open(dict_path, "w", encoding="utf-8").write("\n".join(dlines) + "\n")
print("pinyin", os.path.getsize(dict_path), "codes", len(all_codes), "syl", len(syl_entries),
      "phraseKeys", len(phrase_index), "phrases", len(phrase_flat))
