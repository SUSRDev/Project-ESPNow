#ifndef GAME_ARCADE_H
#define GAME_ARCADE_H

#include <Arduino.h>
#include <stdint.h>
#include "config.h"
#include "esp_now_handler.h"

#define GAME_PKT_MAGIC 0xA7

enum GameKind_e {
    GAME_KIND_NONE = 0,
    GAME_KIND_RPS = 1,
    GAME_KIND_MOLE = 2,
    GAME_KIND_RADAR = 3,
    GAME_KIND_REACT = 4,     // 比反应
    GAME_KIND_BRIDGE = 5,    // 恐龙搭桥
    GAME_KIND_HOCKEY = 6,    // 空气曲棍球
    GAME_KIND_RHYTHM = 7,    // 节奏点拍
    GAME_KIND_WEREWOLF = 8,  // 狼人杀（多人）
    GAME_KIND_SOUP = 9       // 海龟汤（情境推理）
};
typedef enum GameKind_e GameKind_t;

enum GameOpcode_e {
    GAME_OP_JOIN = 1,
    GAME_OP_LEAVE = 2,
    GAME_OP_READY = 3,
    GAME_OP_ACTION = 4,
    GAME_OP_STATE = 5,
    GAME_OP_SCORE = 6,
    GAME_OP_PING = 7,
    GAME_OP_INVITE = 8,   // 招募进指定游戏
    GAME_OP_LINK = 9,     // 雷达：交叉链路 RSSI（相对方位）
    GAME_OP_STATS = 10    // 战绩同步：v0=kindIdx v1=wins v2=losses v3=games
};
typedef enum GameOpcode_e GameOpcode_t;

#define GAME_WIN_SCORE 10
#define GAME_RPS_ROUND_MS 30000UL
#define GAME_STAT_KIND_N 8 // +SOUP
#define GAME_WW_MIN_PLAYERS 3
#define GAME_WW_MAX_PLAYERS 12
#define GAME_HOCKEY_WIN 7
#define GAME_RHYTHM_WIN 10
#define GAME_SOUP_MIN_PLAYERS 2

#define GAME_INVITE_TIMEOUT_MS 30000UL
#define GAME_INVITE_POPUP_W 280
#define GAME_INVITE_POPUP_H 118
#define GAME_INVITE_POPUP_X ((SCREEN_WIDTH - GAME_INVITE_POPUP_W) / 2)
#define GAME_INVITE_POPUP_Y 22

// 线长与 Sync/Chat/Priv 均不同
typedef struct GamePacket_s {
    uint8_t magic;
    uint8_t gameKind;
    uint8_t opcode;
    uint8_t flags;
    char senderId[DEVICE_ID_MAX_LEN + 1];
    int16_t v0;
    int16_t v1;
    int16_t v2;
    int16_t v3;
    uint32_t seq;
    uint32_t timestamp;
    uint8_t _uniq[17];
} GamePacket_t;

// RSSI → 预计距离（米）；无效返回 -1
float estimateDistanceMeters(int8_t rssi);
void formatEstDistance(int8_t rssi, char *buf, size_t buflen);

void gameArcadeInit();
void sendGamePacket(uint8_t kind, uint8_t opcode, int16_t v0, int16_t v1, int16_t v2, int16_t v3);
// 收包路径只入队（禁止在回调里画 TFT）；主循环再处理
void enqueueGamePacket(const GamePacket_t &pkt, int8_t rssi);
void processIncomingGameQueue();

void showGameLobby();
void hideGameArcade();
void drawGameLobby();
void redrawGameArcade(); // 按当前子界面重绘
void updateGameArcade();
bool handleGameArcadeTouch(int x, int y); // true=已处理
void gameArcadeTouchReleased();

void drawGameJoinButton();
bool isGameJoinButtonPressed(int x, int y);

bool gameArcadeIsActive(); // 当前是否在娱乐界面

// 游戏招募（全页面弹窗）
void sendGameRecruit(uint8_t gameKind);
void showGameInviteDialog(const char *fromId, uint8_t gameKind, unsigned long deadlineMs);
void hideGameInviteDialog();
void drawGameInviteDialog();
void updateGameInviteDialog();
bool handleGameInviteTouch(int x, int y);
bool isGameInviteDialogVisible();
void overlayGameInviteIfAny(); // 任意页面重绘后叠层
const char *gameKindDisplayName(uint8_t kind);

#endif
