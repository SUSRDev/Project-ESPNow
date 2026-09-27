# -*- coding: utf-8 -*-
"""Strip non-BMP codepoints from cn_font_data.h / cn_pinyin_data.h (fix uint16_t narrowing)."""
from pathlib import Path
import re

src = Path(__file__).resolve().parent.parent / "src"

# --- font ---
fp = src / "cn_font_data.h"
text = fp.read_text(encoding="utf-8")
pat = re.compile(r"\s*\{(0x[0-9A-Fa-f]+),\s*\{([^}]*)\}\},")
kept = []
dropped = 0
for m in pat.finditer(text):
    code = int(m.group(1), 16)
    if code <= 0xFFFF:
        kept.append(f"  {{{m.group(1)}, {{{m.group(2)}}}}},")
    else:
        dropped += 1
print("font kept", len(kept), "dropped", dropped)
header = f"""#pragma once
#include <stdint.h>
#include <pgmspace.h>
#define CN_FONT_W 12
#define CN_FONT_H 12
#define CN_FONT_BYTES_PER_GLYPH 24
#define CN_FONT_COUNT {len(kept)}
typedef struct {{ uint16_t unicode; uint8_t bits[24]; }} CnGlyph;
static const CnGlyph CN_GLYPHS[] PROGMEM = {{
"""
fp.write_text(header + "\n".join(kept) + "\n};\n", encoding="utf-8")
valid = {int(re.search(r"0x([0-9A-Fa-f]+)", e).group(1), 16) for e in kept}

# --- pinyin ---
pp = src / "cn_pinyin_data.h"
pt = pp.read_text(encoding="utf-8")

m = re.search(r"static const uint16_t PY_CODES\[\] PROGMEM = \{(.*?)\};", pt, re.S)
old_codes = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", m.group(1))]

syl_block = pt.split("PY_SYLS")[1].split("PY_PHRASE_WORDS")[0]
syls = re.findall(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', syl_block)

new_codes = []
new_syls = []
for py, start, count in syls:
    start, count = int(start), int(count)
    chunk = [c for c in old_codes[start : start + count] if c <= 0xFFFF and c in valid]
    if not chunk:
        chunk = [c for c in old_codes[start : start + count] if c <= 0xFFFF]
    if not chunk:
        continue
    st = len(new_codes)
    new_codes.extend(chunk)
    new_syls.append((py, st, len(chunk)))

wm = re.search(r"static const char \*const PY_PHRASE_WORDS\[[^\]]*\][^=]*= \{(.*?)\};", pt, re.S)
word_lits = re.findall(r'(?:u8)?"((?:\\.|[^"\\])*)"', wm.group(1))

key_block = pt.split("PY_PHRASE_KEYS")[1]
keys = re.findall(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', key_block)

dlines = [
    "#pragma once",
    "#include <stdint.h>",
    "#include <pgmspace.h>",
    f"#define PY_SYL_COUNT {len(new_syls)}",
    f"#define PY_CODE_COUNT {len(new_codes)}",
    f"#define PY_PHRASE_KEY_COUNT {len(keys)}",
    f"#define PY_PHRASE_COUNT {len(word_lits)}",
    "typedef struct { const char *py; uint16_t start; uint8_t count; } PySyl;",
    "typedef struct { const char *py; uint16_t start; uint8_t count; } PyPhraseKey;",
    "static const uint16_t PY_CODES[] PROGMEM = {",
]
chunk = []
for code in new_codes:
    chunk.append(f"0x{code:04X}")
    if len(chunk) == 16:
        dlines.append("  " + ", ".join(chunk) + ",")
        chunk = []
if chunk:
    dlines.append("  " + ", ".join(chunk) + ",")
dlines.append("};")
dlines.append("static const PySyl PY_SYLS[] = {")
for py, start, count in new_syls:
    dlines.append(f'  {{"{py}", {start}, {count}}},')
dlines.append("};")
dlines.append("static const char *const PY_PHRASE_WORDS[] = {")
for w in word_lits:
    dlines.append(f'  "{w}",')
dlines.append("};")
dlines.append("static const PyPhraseKey PY_PHRASE_KEYS[] = {")
for py, start, count in keys:
    dlines.append(f'  {{"{py}", {start}, {count}}},')
dlines.append("};")
pp.write_text("\n".join(dlines) + "\n", encoding="utf-8")
print("pinyin codes", len(new_codes), "syls", len(new_syls), "phrases", len(word_lits))
print("bad codes left", sum(1 for c in new_codes if c > 0xFFFF))
