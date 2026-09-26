#pragma once
#include <TFT_eSPI.h>
#include <stdint.h>

// UTF-8 中英混排绘制（精简字库，覆盖 UI + 拼音词库）
int cnDrawUtf8(TFT_eSPI &tft, int x, int y, const char *utf8, uint16_t fg, uint16_t bg = 0x0000, bool transparentBg = true);
// 超宽时截断并加省略号 …
int cnDrawUtf8Ellipsis(TFT_eSPI &tft, int x, int y, const char *utf8, uint16_t fg, int maxW,
                       uint16_t bg = 0x0000, bool transparentBg = true);
int cnTextWidth(const char *utf8);
void cnFillCandidates(const char *pinyin, const char **outZh, int maxOut, int &outCount);
