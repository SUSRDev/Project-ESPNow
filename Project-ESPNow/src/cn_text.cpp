#include "cn_text.h"
#include "cn_font_data.h"
#include "cn_pinyin_data.h"
#include <cstring>
#include <Arduino.h>

static const CnGlyph *findGlyph(uint16_t unicode)
{
    int lo = 0, hi = CN_FONT_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint16_t u = pgm_read_word(&CN_GLYPHS[mid].unicode);
        if (u == unicode)
            return &CN_GLYPHS[mid];
        if (u < unicode)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return nullptr;
}

static int utf8Next(const char *&p, uint16_t &code)
{
    const uint8_t *s = (const uint8_t *)p;
    if (!*s)
        return 0;
    if (s[0] < 0x80) {
        code = s[0];
        p += 1;
        return 1;
    }
    if ((s[0] & 0xE0) == 0xC0 && s[1]) {
        code = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        p += 2;
        return 2;
    }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
        code = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        p += 3;
        return 3;
    }
    p += 1;
    code = '?';
    return 1;
}

static void drawGlyphBitmap(TFT_eSPI &tft, int x, int y, const CnGlyph *g, uint16_t fg, uint16_t bg, bool transparentBg)
{
    uint16_t line[CN_FONT_W];
    tft.startWrite();
    for (int row = 0; row < CN_FONT_H; row++) {
        uint8_t b0 = pgm_read_byte(&g->bits[row * 2]);
        uint8_t b1 = pgm_read_byte(&g->bits[row * 2 + 1]);
        if (transparentBg) {
            for (int col = 0; col < CN_FONT_W; col++) {
                bool on = (col < 8) ? (b0 & (0x80 >> col)) : (b1 & (0x80 >> (col - 8)));
                if (on)
                    tft.drawPixel(x + col, y + row, fg);
            }
        } else {
            for (int col = 0; col < CN_FONT_W; col++) {
                bool on = (col < 8) ? (b0 & (0x80 >> col)) : (b1 & (0x80 >> (col - 8)));
                line[col] = on ? fg : bg;
            }
            tft.setAddrWindow(x, y + row, x + CN_FONT_W - 1, y + row);
            tft.pushColors(line, CN_FONT_W, true);
        }
    }
    tft.endWrite();
}

int cnTextWidth(const char *utf8)
{
    if (!utf8)
        return 0;
    const char *p = utf8;
    uint16_t code;
    int w = 0;
    while (utf8Next(p, code)) {
        if (code < 0x80)
            w += 6;
        else
            w += CN_FONT_W;
    }
    return w;
}

int cnDrawUtf8(TFT_eSPI &tft, int x, int y, const char *utf8, uint16_t fg, uint16_t bg, bool transparentBg)
{
    if (!utf8)
        return 0;
    const char *p = utf8;
    uint16_t code;
    int cx = x;
    tft.setTextFont(1);
    tft.setTextSize(1);
    while (utf8Next(p, code)) {
        if (code < 0x80) {
            char c[2] = {(char)code, 0};
            if (!transparentBg)
                tft.setTextColor(fg, bg);
            else
                tft.setTextColor(fg);
            tft.setCursor(cx, y + 2);
            tft.print(c);
            cx += 6;
        } else {
            const CnGlyph *g = findGlyph(code);
            if (g)
                drawGlyphBitmap(tft, cx, y, g, fg, bg, transparentBg);
            else
                tft.fillCircle(cx + CN_FONT_W / 2, y + CN_FONT_H / 2, 1, fg);
            cx += CN_FONT_W;
        }
    }
    return cx - x;
}

int cnDrawUtf8Ellipsis(TFT_eSPI &tft, int x, int y, const char *utf8, uint16_t fg, int maxW, uint16_t bg, bool transparentBg)
{
    if (!utf8 || maxW <= 0)
        return 0;
    int full = cnTextWidth(utf8);
    if (full <= maxW)
        return cnDrawUtf8(tft, x, y, utf8, fg, bg, transparentBg);

    // 预留省略号宽度
    const char *ellipsis = "…";
    int ew = cnTextWidth(ellipsis);
    if (ew > maxW)
        ew = 0;
    int budget = maxW - ew;
    if (budget < 6)
        budget = maxW;

    const char *p = utf8;
    const char *cut = utf8;
    uint16_t code;
    int w = 0;
    while (utf8Next(p, code)) {
        int cw = (code < 0x80) ? 6 : CN_FONT_W;
        if (w + cw > budget)
            break;
        w += cw;
        cut = p;
    }

    char tmp[96];
    size_t n = (size_t)(cut - utf8);
    if (n >= sizeof(tmp))
        n = sizeof(tmp) - 1;
    memcpy(tmp, utf8, n);
    tmp[n] = 0;
    int drawn = cnDrawUtf8(tft, x, y, tmp, fg, bg, transparentBg);
    if (ew > 0)
        drawn += cnDrawUtf8(tft, x + drawn, y, ellipsis, fg, bg, transparentBg);
    return drawn;
}

static int findSylExact(const char *pinyin)
{
    int lo = 0, hi = PY_SYL_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int cmp = strcmp(PY_SYLS[mid].py, pinyin);
        if (cmp == 0)
            return mid;
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static int findSylPrefixFirst(const char *pinyin, size_t n)
{
    int lo = 0, hi = PY_SYL_COUNT - 1, ans = -1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int cmp = strncmp(PY_SYLS[mid].py, pinyin, n);
        if (cmp >= 0) {
            if (cmp == 0)
                ans = mid;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    if (ans < 0) {
        for (int i = lo; i < PY_SYL_COUNT && i < lo + 3; i++) {
            if (strncmp(PY_SYLS[i].py, pinyin, n) == 0)
                return i;
        }
        return -1;
    }
    while (ans > 0 && strncmp(PY_SYLS[ans - 1].py, pinyin, n) == 0)
        ans--;
    return ans;
}

static int findPhraseKeyExact(const char *pinyin)
{
    int lo = 0, hi = PY_PHRASE_KEY_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int cmp = strcmp(PY_PHRASE_KEYS[mid].py, pinyin);
        if (cmp == 0)
            return mid;
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static int findPhraseKeyPrefixFirst(const char *pinyin, size_t n)
{
    int lo = 0, hi = PY_PHRASE_KEY_COUNT - 1, ans = -1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int cmp = strncmp(PY_PHRASE_KEYS[mid].py, pinyin, n);
        if (cmp >= 0) {
            if (cmp == 0)
                ans = mid;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    if (ans < 0) {
        for (int i = lo; i < PY_PHRASE_KEY_COUNT && i < lo + 4; i++) {
            if (strncmp(PY_PHRASE_KEYS[i].py, pinyin, n) == 0)
                return i;
        }
        return -1;
    }
    while (ans > 0 && strncmp(PY_PHRASE_KEYS[ans - 1].py, pinyin, n) == 0)
        ans--;
    return ans;
}

// 声母：zh/ch/sh 优先，其余单字母。返回消耗长度，失败 0
static int consumeInitial(const char *s)
{
    if (!s || !s[0])
        return 0;
    if ((s[0] == 'z' || s[0] == 'c' || s[0] == 's') && s[1] == 'h')
        return 2;
    char c = s[0];
    if ((c >= 'a' && c <= 'z') &&
        (c == 'b' || c == 'p' || c == 'm' || c == 'f' || c == 'd' || c == 't' ||
         c == 'n' || c == 'l' || c == 'g' || c == 'k' || c == 'h' || c == 'j' ||
         c == 'q' || c == 'x' || c == 'r' || c == 'z' || c == 'c' || c == 's' ||
         c == 'y' || c == 'w'))
        return 1;
    return 0;
}

// 纯简拼：整串可拆成 ≥2 个声母（如 nh / zj / shgr）
static bool looksLikeJianPin(const char *s)
{
    if (!s || !s[0])
        return false;
    int count = 0;
    const char *p = s;
    while (*p) {
        int n = consumeInitial(p);
        if (n <= 0)
            return false;
        p += n;
        count++;
    }
    return count >= 2;
}

void cnFillCandidates(const char *pinyin, const char **outZh, int maxOut, int &outCount)
{
    outCount = 0;
    if (!pinyin || !pinyin[0] || maxOut <= 0)
        return;

    static char bufs[48][16];
    int filled = 0;

    auto pushUtf8 = [&](const char *s) {
        if (!s || !s[0] || filled >= maxOut || filled >= 48)
            return;
        // 去重
        for (int i = 0; i < filled; i++) {
            if (strcmp(bufs[i], s) == 0)
                return;
        }
        strncpy(bufs[filled], s, sizeof(bufs[0]) - 1);
        bufs[filled][sizeof(bufs[0]) - 1] = 0;
        outZh[filled] = bufs[filled];
        filled++;
    };

    auto pushCode = [&](uint16_t uni) {
        if (filled >= maxOut || filled >= 48)
            return;
        char *b = bufs[filled];
        if (uni < 0x80) {
            b[0] = (char)uni;
            b[1] = 0;
        } else if (uni < 0x800) {
            b[0] = 0xC0 | (uni >> 6);
            b[1] = 0x80 | (uni & 0x3F);
            b[2] = 0;
        } else {
            b[0] = 0xE0 | (uni >> 12);
            b[1] = 0x80 | ((uni >> 6) & 0x3F);
            b[2] = 0x80 | (uni & 0x3F);
            b[3] = 0;
        }
        for (int i = 0; i < filled; i++) {
            if (strcmp(bufs[i], b) == 0)
                return;
        }
        outZh[filled] = b;
        filled++;
    };

    auto pushSylIndex = [&](int idx) {
        if (idx < 0)
            return;
        uint16_t start = PY_SYLS[idx].start;
        uint8_t count = PY_SYLS[idx].count;
        for (uint8_t k = 0; k < count && filled < maxOut; k++)
            pushCode(pgm_read_word(&PY_CODES[start + k]));
    };

    auto pushPhraseIndex = [&](int idx) {
        if (idx < 0)
            return;
        uint16_t start = PY_PHRASE_KEYS[idx].start;
        uint8_t count = PY_PHRASE_KEYS[idx].count;
        for (uint8_t k = 0; k < count && filled < maxOut; k++)
            pushUtf8(PY_PHRASE_WORDS[start + k]);
    };

    auto pushPhrasePrefix = [&](const char *key, size_t n) {
        int first = findPhraseKeyPrefixFirst(key, n);
        if (first < 0)
            return;
        for (int i = first; i < PY_PHRASE_KEY_COUNT && filled < maxOut; i++) {
            if (strncmp(PY_PHRASE_KEYS[i].py, key, n) != 0)
                break;
            pushPhraseIndex(i);
        }
    };

    size_t n = strlen(pinyin);

    // 1) 完整单音节（如 ge）→ 只出单字，绝不混词组
    int exactSyl = findSylExact(pinyin);
    if (exactSyl >= 0) {
        pushSylIndex(exactSyl);
        outCount = filled;
        return;
    }

    // 2) 简拼双/多音节（如 nh）→ 只出词组
    if (looksLikeJianPin(pinyin)) {
        int pe = findPhraseKeyExact(pinyin);
        if (pe >= 0)
            pushPhraseIndex(pe);
        pushPhrasePrefix(pinyin, n);
        outCount = filled;
        return;
    }

    // 3) 完整词组全拼（如 nihao）
    int pe = findPhraseKeyExact(pinyin);
    if (pe >= 0) {
        pushPhraseIndex(pe);
        outCount = filled;
        return;
    }

    // 4) 单音节前缀（如 g / zh）→ 只出对应音节单字
    int sylFirst = findSylPrefixFirst(pinyin, n);
    if (sylFirst >= 0) {
        for (int i = sylFirst; i < PY_SYL_COUNT && filled < maxOut; i++) {
            if (strncmp(PY_SYLS[i].py, pinyin, n) != 0)
                break;
            pushSylIndex(i);
        }
        outCount = filled;
        return;
    }

    // 5) 其它：词组前缀兜底
    pushPhrasePrefix(pinyin, n);
    outCount = filled;
}
