#pragma once
#include "game_arcade.h"

// 三个新游戏：由 game_arcade.cpp 在对应视图下调用

void moreSendGamePacket(uint8_t kind, uint8_t opcode, int16_t v0, int16_t v1, int16_t v2, int16_t v3);
void moreSendGamePacketTo(const uint8_t destMac[6], uint8_t kind, uint8_t opcode,
                          int16_t v0, int16_t v1, int16_t v2, int16_t v3);

// —— 空气曲棍球 ——
void hockeyEnter();
void hockeyLeave();
void hockeyDraw();
void hockeyUpdate();
bool hockeyTouch(int x, int y, bool rising);
void hockeyTouchReleased();
void hockeyApplyPacket(const GamePacket_t &pkt);

// —— 节奏点拍 ——
void rhythmEnter();
void rhythmLeave();
void rhythmDraw();
void rhythmUpdate();
bool rhythmTouch(int x, int y, bool rising);
void rhythmApplyPacket(const GamePacket_t &pkt);

// —— 狼人杀 ——
void werewolfEnter();
void werewolfLeave();
void werewolfDraw();
void werewolfUpdate();
bool werewolfTouch(int x, int y, bool rising);
void werewolfApplyPacket(const GamePacket_t &pkt);

// —— 海龟汤 ——
void soupEnter();
void soupLeave();
void soupDraw();
void soupUpdate();
bool soupTouch(int x, int y, bool rising);
void soupApplyPacket(const GamePacket_t &pkt);

// 战绩短名
const char *moreStatsKindShort(int idx);
uint8_t moreStatsIndexToKind(int idx);
int moreStatsKindToIndex(uint8_t kind);
