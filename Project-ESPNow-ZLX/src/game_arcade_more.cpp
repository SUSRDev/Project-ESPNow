#include "game_arcade_more.h"
#include "soup_puzzles.h"
#include "ui_manager.h"
#include "cn_text.h"
#include "transport_manager.h"
#include "power_manager.h"
#include <TFT_eSPI.h>
#include <math.h>
#include <string.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <map>

extern TFT_eSPI tft;
extern char localDeviceId[DEVICE_ID_MAX_LEN + 1];
extern UIState_t currentUIState;
extern bool isScreenOn;
extern std::map<String, PeerInfo_t> peerInfoMap;

static uint32_t moreSeq = 1;

void moreSendGamePacket(uint8_t kind, uint8_t opcode, int16_t v0, int16_t v1, int16_t v2, int16_t v3)
{
    GamePacket_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = GAME_PKT_MAGIC;
    pkt.gameKind = kind;
    pkt.opcode = opcode;
    strncpy(pkt.senderId, localDeviceId, DEVICE_ID_MAX_LEN);
    pkt.v0 = v0;
    pkt.v1 = v1;
    pkt.v2 = v2;
    pkt.v3 = v3;
    pkt.seq = moreSeq++;
    pkt.timestamp = millis();
    transportSend((const uint8_t *)&pkt, sizeof(pkt), nullptr);
}

void moreSendGamePacketTo(const uint8_t destMac[6], uint8_t kind, uint8_t opcode,
                          int16_t v0, int16_t v1, int16_t v2, int16_t v3)
{
    if (!destMac)
        return;
    GamePacket_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = GAME_PKT_MAGIC;
    pkt.gameKind = kind;
    pkt.opcode = opcode;
    strncpy(pkt.senderId, localDeviceId, DEVICE_ID_MAX_LEN);
    pkt.v0 = v0;
    pkt.v1 = v1;
    pkt.v2 = v2;
    pkt.v3 = v3;
    pkt.seq = moreSeq++;
    pkt.timestamp = millis();
    ensureUnicastPeer(destMac);
    transportSend((const uint8_t *)&pkt, sizeof(pkt), destMac);
}

static uint16_t idHash16(const char *id)
{
    uint32_t h = 2166136261u;
    if (!id)
        return 0;
    while (*id) {
        h ^= (uint8_t)(*id++);
        h *= 16777619u;
    }
    return (uint16_t)(h ^ (h >> 16));
}

static bool findPeerMacById(const char *id, uint8_t outMac[6])
{
    if (!id || !id[0] || !outMac)
        return false;
    for (auto const &kv : peerInfoMap) {
        if (kv.second.deviceId[0] && strcmp(kv.second.deviceId, id) == 0) {
            unsigned int b[6] = {0};
            if (sscanf(kv.first.c_str(), "%02X:%02X:%02X:%02X:%02X:%02X",
                       &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
                for (int i = 0; i < 6; i++)
                    outMac[i] = (uint8_t)b[i];
                return true;
            }
        }
    }
    return false;
}

static void drawMoreBackInv(const char *titleCn, uint16_t accent)
{
    tft.fillRect(0, 0, SCREEN_WIDTH, 24, accent);
    cnDrawUtf8(tft, 8, 6, titleCn, TFT_WHITE, accent, false);
    tft.fillRoundRect(196, 4, 40, 16, 3, tft.color565(200, 120, 40));
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_BLACK, tft.color565(200, 120, 40));
    tft.drawString("INV", 216, 12, 1);
    tft.setTextDatum(TL_DATUM);
    tft.fillRoundRect(248, 4, 60, 16, 3, tft.color565(60, 60, 80));
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, tft.color565(60, 60, 80));
    tft.drawString("BACK", 278, 12, 1);
    tft.setTextDatum(TL_DATUM);
}

static bool hitMoreBack(int x, int y)
{
    return y >= 2 && y <= 24 && x >= 248 && x <= 310;
}

int moreStatsKindToIndex(uint8_t kind)
{
    switch (kind) {
    case GAME_KIND_RPS: return 0;
    case GAME_KIND_MOLE: return 1;
    case GAME_KIND_REACT: return 2;
    case GAME_KIND_BRIDGE: return 3;
    case GAME_KIND_HOCKEY: return 4;
    case GAME_KIND_RHYTHM: return 5;
    case GAME_KIND_WEREWOLF: return 6;
    case GAME_KIND_SOUP: return 7;
    default: return -1;
    }
}

uint8_t moreStatsIndexToKind(int idx)
{
    static const uint8_t kinds[GAME_STAT_KIND_N] = {
        GAME_KIND_RPS, GAME_KIND_MOLE, GAME_KIND_REACT, GAME_KIND_BRIDGE,
        GAME_KIND_HOCKEY, GAME_KIND_RHYTHM, GAME_KIND_WEREWOLF, GAME_KIND_SOUP};
    if (idx < 0 || idx >= GAME_STAT_KIND_N)
        return GAME_KIND_NONE;
    return kinds[idx];
}

const char *moreStatsKindShort(int idx)
{
    switch (idx) {
    case 0: return "RPS";
    case 1: return "MOLE";
    case 2: return "REACT";
    case 3: return "BRIDGE";
    case 4: return "HOCKEY";
    case 5: return "RHYTHM";
    case 6: return "WOLF";
    case 7: return "SOUP";
    default: return "?";
    }
}

// =============================================================================
// 空气曲棍球（1v1，主机模拟球，客机插值显示）
// =============================================================================
static bool hkHost = false;
static bool hkJoined = false;
static char hkPeerId[DEVICE_ID_MAX_LEN + 1] = {0};
static float hkBallX = 160, hkBallY = 120;
static float hkBallVx = 2.2f, hkBallVy = 2.6f;
static float hkDispX = 160, hkDispY = 120; // 插值显示
static float hkTargetX = 160, hkTargetY = 120;
static int hkMyPadX = 160;
static int hkPeerPadX = 160;
static int hkMyScore = 0, hkPeerScore = 0;
static unsigned long hkLastPhys = 0;
static unsigned long hkLastSync = 0;
static unsigned long hkLastDraw = 0;
static bool hkDirty = true;
static const int HK_PAD_W = 56;
static const int HK_PAD_H = 10;
static const int HK_BALL_R = 7;
static const int HK_TOP = 28;
static const int HK_BOT = SCREEN_HEIGHT - 8;

void hockeyEnter()
{
    hkHost = false;
    hkJoined = true;
    hkPeerId[0] = 0;
    hkBallX = hkDispX = hkTargetX = SCREEN_WIDTH / 2;
    hkBallY = hkDispY = hkTargetY = SCREEN_HEIGHT / 2;
    hkBallVx = 2.4f;
    hkBallVy = (esp_random() & 1) ? 2.8f : -2.8f;
    hkMyPadX = hkPeerPadX = SCREEN_WIDTH / 2;
    hkMyScore = hkPeerScore = 0;
    hkLastPhys = hkLastSync = millis();
    hkDirty = true;
    moreSendGamePacket(GAME_KIND_HOCKEY, GAME_OP_JOIN, 0, 0, 0, 0);
    hockeyDraw();
}

void hockeyLeave()
{
    if (hkJoined)
        moreSendGamePacket(GAME_KIND_HOCKEY, GAME_OP_LEAVE, 0, 0, 0, 0);
    hkJoined = false;
    hkPeerId[0] = 0;
}

static void hkClampPad(int &x)
{
    if (x < HK_PAD_W / 2)
        x = HK_PAD_W / 2;
    if (x > SCREEN_WIDTH - HK_PAD_W / 2)
        x = SCREEN_WIDTH - HK_PAD_W / 2;
}

static void hkResetBall(bool towardPeer)
{
    hkBallX = SCREEN_WIDTH / 2.f;
    hkBallY = SCREEN_HEIGHT / 2.f;
    float spd = 2.6f + (hkMyScore + hkPeerScore) * 0.12f;
    if (spd > 5.5f)
        spd = 5.5f;
    hkBallVx = ((esp_random() % 100) / 100.f - 0.5f) * spd * 1.4f;
    if (fabsf(hkBallVx) < 1.2f)
        hkBallVx = (hkBallVx < 0) ? -1.4f : 1.4f;
    hkBallVy = towardPeer ? -spd : spd;
    hkTargetX = hkBallX;
    hkTargetY = hkBallY;
    hkDispX = hkBallX;
    hkDispY = hkBallY;
}

static void hkGoal(bool peerScored)
{
    if (peerScored)
        hkPeerScore++;
    else
        hkMyScore++;
    moreSendGamePacket(GAME_KIND_HOCKEY, GAME_OP_SCORE, (int16_t)hkMyScore, (int16_t)hkPeerScore, 0, 0);
    hkResetBall(!peerScored);
    hkDirty = true;
}

void hockeyDraw()
{
    uint16_t ice = tft.color565(18, 28, 48);
    uint16_t line = tft.color565(60, 100, 140);
    tft.fillScreen(ice);
    drawMoreBackInv("空气曲棍球", tft.color565(20, 50, 90));

    // rink
    tft.drawRoundRect(4, HK_TOP, SCREEN_WIDTH - 8, HK_BOT - HK_TOP, 8, line);
    tft.drawFastHLine(4, (HK_TOP + HK_BOT) / 2, SCREEN_WIDTH - 8, tft.color565(40, 70, 100));
    tft.drawCircle(SCREEN_WIDTH / 2, (HK_TOP + HK_BOT) / 2, 28, tft.color565(40, 70, 100));

    // peer paddle (top)
    tft.fillRoundRect(hkPeerPadX - HK_PAD_W / 2, HK_TOP + 4, HK_PAD_W, HK_PAD_H, 3,
                      tft.color565(220, 80, 80));
    // my paddle (bottom)
    tft.fillRoundRect(hkMyPadX - HK_PAD_W / 2, HK_BOT - HK_PAD_H - 4, HK_PAD_W, HK_PAD_H, 3,
                      tft.color565(80, 200, 140));
    // ball (interpolated)
    tft.fillCircle((int)hkDispX, (int)hkDispY, HK_BALL_R, TFT_WHITE);
    tft.drawCircle((int)hkDispX, (int)hkDispY, HK_BALL_R, tft.color565(180, 200, 255));

    char sc[24];
    snprintf(sc, sizeof(sc), "%d  :  %d", hkMyScore, hkPeerScore);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_YELLOW, ice);
    tft.drawString(sc, SCREEN_WIDTH / 2, 28, 2);
    tft.setTextDatum(TL_DATUM);

    if (!hkPeerId[0]) {
        tft.fillRect(0, SCREEN_HEIGHT - 18, SCREEN_WIDTH, 18, tft.color565(70, 50, 20));
        cnDrawUtf8(tft, 8, SCREEN_HEIGHT - 15, "等待对手加入…", TFT_YELLOW);
    } else if (hkMyScore >= GAME_HOCKEY_WIN || hkPeerScore >= GAME_HOCKEY_WIN) {
        tft.fillRoundRect(40, 90, 240, 50, 8, tft.color565(30, 20, 40));
        cnDrawUtf8(tft, 100, 108,
                   (hkMyScore >= GAME_HOCKEY_WIN) ? "你赢了！" : "对手获胜", TFT_CYAN);
    }
    hkDirty = false;
    hkLastDraw = millis();
}

void hockeyUpdate()
{
    if (!hkJoined)
        return;
    unsigned long now = millis();

    // 显示插值（客机/主机都做平滑）
    hkDispX += (hkTargetX - hkDispX) * 0.35f;
    hkDispY += (hkTargetY - hkDispY) * 0.35f;

    if (hkHost && hkPeerId[0] &&
        hkMyScore < GAME_HOCKEY_WIN && hkPeerScore < GAME_HOCKEY_WIN) {
        if (now - hkLastPhys >= 16UL) {
            float dt = (now - hkLastPhys) / 16.f;
            if (dt > 3.f)
                dt = 3.f;
            hkLastPhys = now;
            hkBallX += hkBallVx * dt;
            hkBallY += hkBallVy * dt;

            if (hkBallX < 8 + HK_BALL_R) {
                hkBallX = 8 + HK_BALL_R;
                hkBallVx = fabsf(hkBallVx);
            }
            if (hkBallX > SCREEN_WIDTH - 8 - HK_BALL_R) {
                hkBallX = SCREEN_WIDTH - 8 - HK_BALL_R;
                hkBallVx = -fabsf(hkBallVx);
            }

            // peer paddle (top)
            if (hkBallY - HK_BALL_R <= HK_TOP + 4 + HK_PAD_H && hkBallVy < 0) {
                if (hkBallX >= hkPeerPadX - HK_PAD_W / 2 - 4 &&
                    hkBallX <= hkPeerPadX + HK_PAD_W / 2 + 4) {
                    hkBallY = HK_TOP + 4 + HK_PAD_H + HK_BALL_R;
                    hkBallVy = fabsf(hkBallVy) * 1.03f;
                    hkBallVx += (hkBallX - hkPeerPadX) * 0.08f;
                }
            }
            // my paddle (bottom) — host side
            if (hkBallY + HK_BALL_R >= HK_BOT - 4 - HK_PAD_H && hkBallVy > 0) {
                if (hkBallX >= hkMyPadX - HK_PAD_W / 2 - 4 &&
                    hkBallX <= hkMyPadX + HK_PAD_W / 2 + 4) {
                    hkBallY = HK_BOT - 4 - HK_PAD_H - HK_BALL_R;
                    hkBallVy = -fabsf(hkBallVy) * 1.03f;
                    hkBallVx += (hkBallX - hkMyPadX) * 0.08f;
                }
            }

            if (hkBallY < HK_TOP - 6)
                hkGoal(false); // peer missed → I score
            else if (hkBallY > HK_BOT + 6)
                hkGoal(true);

            hkTargetX = hkBallX;
            hkTargetY = hkBallY;
        }
        if (now - hkLastSync >= 40UL) {
            hkLastSync = now;
            moreSendGamePacket(GAME_KIND_HOCKEY, GAME_OP_STATE,
                               (int16_t)(hkBallX * 10), (int16_t)(hkBallY * 10),
                               (int16_t)(hkBallVx * 20), (int16_t)(hkBallVy * 20));
            moreSendGamePacket(GAME_KIND_HOCKEY, GAME_OP_ACTION,
                               (int16_t)hkMyPadX, 1, (int16_t)hkMyScore, (int16_t)hkPeerScore);
        }
    } else if (!hkHost && hkPeerId[0] && now - hkLastSync >= 50UL) {
        hkLastSync = now;
        moreSendGamePacket(GAME_KIND_HOCKEY, GAME_OP_ACTION, (int16_t)hkMyPadX, 0, 0, 0);
    }

    if (hkDirty || now - hkLastDraw > 33UL)
        hockeyDraw();
}

bool hockeyTouch(int x, int y, bool rising)
{
    if (hitMoreBack(x, y) && rising)
        return false; // caller handles leave
    if (y > 26) {
        hkMyPadX = x;
        hkClampPad(hkMyPadX);
        hkDirty = true;
    }
    if (rising && (hkMyScore >= GAME_HOCKEY_WIN || hkPeerScore >= GAME_HOCKEY_WIN)) {
        hkMyScore = hkPeerScore = 0;
        hkResetBall(true);
        hkDirty = true;
    }
    return true;
}

void hockeyTouchReleased() {}

void hockeyApplyPacket(const GamePacket_t &pkt)
{
    if (pkt.opcode == GAME_OP_JOIN) {
        if (!hkPeerId[0]) {
            strncpy(hkPeerId, pkt.senderId, DEVICE_ID_MAX_LEN);
            hkPeerId[DEVICE_ID_MAX_LEN] = 0;
            // 较小哈希当主机
            hkHost = idHash16(localDeviceId) < idHash16(pkt.senderId);
            if (hkHost)
                hkResetBall(true);
            moreSendGamePacket(GAME_KIND_HOCKEY, GAME_OP_JOIN, 1, 0, 0, 0);
            hkDirty = true;
        } else if (strcmp(pkt.senderId, hkPeerId) == 0) {
            /* peer ack */
        }
        return;
    }
    if (pkt.opcode == GAME_OP_LEAVE) {
        if (strcmp(pkt.senderId, hkPeerId) == 0) {
            hkPeerId[0] = 0;
            hkHost = false;
            hkDirty = true;
        }
        return;
    }
    if (pkt.opcode == GAME_OP_ACTION) {
        // v0=padX  v1=1 means from host side scores also in v2/v3
        if (!hkPeerId[0]) {
            strncpy(hkPeerId, pkt.senderId, DEVICE_ID_MAX_LEN);
            hkPeerId[DEVICE_ID_MAX_LEN] = 0;
        }
        if (strcmp(pkt.senderId, hkPeerId) == 0) {
            hkPeerPadX = pkt.v0;
            hkClampPad(hkPeerPadX);
            if (pkt.v1 == 1) {
                // peer is host: their "my" is our peer score visually flipped
                // host sends myScore, peerScore from host view
                // for guest: host's my = peer score on our screen (top)
                hkPeerScore = pkt.v2;
                hkMyScore = pkt.v3;
            }
            hkDirty = true;
        }
        return;
    }
    if (pkt.opcode == GAME_OP_STATE && !hkHost) {
        hkTargetX = pkt.v0 / 10.f;
        hkTargetY = pkt.v1 / 10.f;
        // 轻微外推
        float vx = pkt.v2 / 20.f, vy = pkt.v3 / 20.f;
        hkTargetX += vx * 1.5f;
        hkTargetY += vy * 1.5f;
        return;
    }
    if (pkt.opcode == GAME_OP_SCORE) {
        if (hkHost)
            return;
        // guest: scores from host perspective → swap
        hkPeerScore = pkt.v0;
        hkMyScore = pkt.v1;
        hkDirty = true;
    }
}

// =============================================================================
// 节奏点拍
// =============================================================================
static const int RH_LANES = 4;
static const int RH_NOTES = 48;
struct RhNote {
    uint16_t t; // ms from start
    uint8_t lane;
    uint8_t hit; // 0 none 1 ok 2 miss
};
static RhNote rhNotes[RH_NOTES];
static int rhNoteN = 0;
static uint32_t rhSeed = 1;
static unsigned long rhStartMs = 0;
static bool rhPlaying = false;
static bool rhHost = false;
static char rhPeerId[DEVICE_ID_MAX_LEN + 1] = {0};
static int rhScore = 0, rhCombo = 0, rhPeerScore = 0;
static int rhHitIdx = 0;
static unsigned long rhLastDraw = 0;
static bool rhDirty = true;

static uint32_t rhRand(uint32_t &s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    if (!s)
        s = 0xA5u;
    return s;
}

static void rhBuildChart(uint32_t seed)
{
    rhSeed = seed ? seed : 1;
    uint32_t s = rhSeed;
    rhNoteN = 0;
    uint16_t t = 800;
    for (int i = 0; i < RH_NOTES; i++) {
        rhNotes[i].t = t;
        rhNotes[i].lane = (uint8_t)(rhRand(s) % RH_LANES);
        rhNotes[i].hit = 0;
        t = (uint16_t)(t + 280 + (rhRand(s) % 220));
        rhNoteN++;
    }
}

void rhythmEnter()
{
    rhPlaying = false;
    rhHost = false;
    rhPeerId[0] = 0;
    rhScore = rhCombo = rhPeerScore = 0;
    rhHitIdx = 0;
    rhDirty = true;
    moreSendGamePacket(GAME_KIND_RHYTHM, GAME_OP_JOIN, 0, 0, 0, 0);
    rhythmDraw();
}

void rhythmLeave()
{
    moreSendGamePacket(GAME_KIND_RHYTHM, GAME_OP_LEAVE, 0, 0, 0, 0);
    rhPlaying = false;
    rhPeerId[0] = 0;
}

static void rhStartGame(uint32_t seed, unsigned long startAt)
{
    rhBuildChart(seed);
    rhStartMs = startAt;
    rhPlaying = true;
    rhScore = rhCombo = 0;
    rhHitIdx = 0;
    rhDirty = true;
}

void rhythmDraw()
{
    uint16_t bg = tft.color565(12, 10, 24);
    tft.fillScreen(bg);
    drawMoreBackInv("节奏点拍", tft.color565(70, 30, 90));

    int laneW = SCREEN_WIDTH / RH_LANES;
    uint16_t laneCols[4] = {
        tft.color565(80, 160, 255), tft.color565(100, 220, 140),
        tft.color565(255, 180, 60), tft.color565(255, 100, 140)};
    for (int i = 0; i < RH_LANES; i++) {
        tft.drawFastVLine(i * laneW, 26, SCREEN_HEIGHT - 26, tft.color565(30, 28, 50));
        tft.fillRoundRect(i * laneW + 8, SCREEN_HEIGHT - 36, laneW - 16, 28, 6, laneCols[i]);
    }
    // hit line
    int hitY = SCREEN_HEIGHT - 48;
    tft.drawFastHLine(0, hitY, SCREEN_WIDTH, TFT_WHITE);

    if (!rhPeerId[0]) {
        cnDrawUtf8(tft, 70, 100, "等待对手…", TFT_YELLOW);
    } else if (!rhPlaying) {
        cnDrawUtf8(tft, 90, 100, "点击开始", TFT_CYAN);
    } else {
        unsigned long elapsed = millis() - rhStartMs;
        for (int i = 0; i < rhNoteN; i++) {
            if (rhNotes[i].hit)
                continue;
            long dy = (long)hitY - (long)((long)rhNotes[i].t - (long)elapsed) * 0.22f;
            // note falls: when elapsed==t, y==hitY
            float travel = 160.f;
            float p = (float)((long)elapsed - ((long)rhNotes[i].t - (long)(travel / 0.22f))) * 0.22f;
            int y = (int)(26 + p);
            if (y < 26 || y > SCREEN_HEIGHT)
                continue;
            int x = rhNotes[i].lane * laneW + laneW / 2;
            tft.fillCircle(x, y, 12, laneCols[rhNotes[i].lane]);
            tft.drawCircle(x, y, 12, TFT_WHITE);
        }
    }

    char line[40];
    snprintf(line, sizeof(line), "ME %d  COMBO %d  PEER %d", rhScore, rhCombo, rhPeerScore);
    tft.setTextColor(TFT_WHITE, bg);
    tft.drawString(line, 8, 28, 1);
    rhLastDraw = millis();
    rhDirty = false;
}

void rhythmUpdate()
{
    if (!rhPlaying)
        return;
    unsigned long elapsed = millis() - rhStartMs;
    // auto-miss
    while (rhHitIdx < rhNoteN && rhNotes[rhHitIdx].hit == 0 &&
           (long)elapsed > (long)rhNotes[rhHitIdx].t + 160) {
        rhNotes[rhHitIdx].hit = 2;
        rhCombo = 0;
        rhHitIdx++;
        rhDirty = true;
    }
    if (rhHitIdx >= rhNoteN && elapsed > rhNotes[rhNoteN - 1].t + 500) {
        rhPlaying = false;
        moreSendGamePacket(GAME_KIND_RHYTHM, GAME_OP_SCORE, (int16_t)rhScore, (int16_t)rhCombo, 1, 0);
        rhDirty = true;
    }
    if (rhDirty || millis() - rhLastDraw > 33UL)
        rhythmDraw();
}

bool rhythmTouch(int x, int y, bool rising)
{
    if (!rising)
        return true;
    if (hitMoreBack(x, y))
        return false;
    if (!rhPeerId[0])
        return true;
    if (!rhPlaying) {
        // either can start; starter is host of chart
        rhHost = true;
        uint32_t seed = (esp_random() & 0xFFFF) | 1;
        unsigned long st = millis() + 600UL;
        rhStartGame(seed, st);
        moreSendGamePacket(GAME_KIND_RHYTHM, GAME_OP_STATE, (int16_t)(seed & 0xFFFF),
                           (int16_t)(st & 0xFFFF), (int16_t)((st >> 16) & 0xFFFF), 0);
        return true;
    }
    if (y < SCREEN_HEIGHT - 50)
        return true;
    int lane = x / (SCREEN_WIDTH / RH_LANES);
    if (lane < 0)
        lane = 0;
    if (lane >= RH_LANES)
        lane = RH_LANES - 1;
    unsigned long elapsed = millis() - rhStartMs;
    // find nearest note in lane
    int best = -1;
    long bestDt = 9999;
    for (int i = 0; i < rhNoteN; i++) {
        if (rhNotes[i].hit || rhNotes[i].lane != lane)
            continue;
        long dt = labs((long)elapsed - (long)rhNotes[i].t);
        if (dt < bestDt) {
            bestDt = dt;
            best = i;
        }
    }
    if (best >= 0 && bestDt <= 150) {
        rhNotes[best].hit = 1;
        int add = (bestDt <= 50) ? 3 : (bestDt <= 100) ? 2 : 1;
        rhCombo++;
        rhScore += add + rhCombo / 5;
        if (best == rhHitIdx)
            rhHitIdx++;
        moreSendGamePacket(GAME_KIND_RHYTHM, GAME_OP_ACTION, (int16_t)rhScore, (int16_t)rhCombo,
                           (int16_t)best, (int16_t)bestDt);
        rhDirty = true;
    } else {
        rhCombo = 0;
        rhDirty = true;
    }
    return true;
}

void rhythmApplyPacket(const GamePacket_t &pkt)
{
    if (pkt.opcode == GAME_OP_JOIN) {
        if (!rhPeerId[0]) {
            strncpy(rhPeerId, pkt.senderId, DEVICE_ID_MAX_LEN);
            rhPeerId[DEVICE_ID_MAX_LEN] = 0;
            moreSendGamePacket(GAME_KIND_RHYTHM, GAME_OP_JOIN, 1, 0, 0, 0);
            rhDirty = true;
        }
        return;
    }
    if (pkt.opcode == GAME_OP_LEAVE) {
        if (strcmp(pkt.senderId, rhPeerId) == 0) {
            rhPeerId[0] = 0;
            rhPlaying = false;
            rhDirty = true;
        }
        return;
    }
    if (pkt.opcode == GAME_OP_STATE) {
        uint32_t seed = (uint16_t)pkt.v0;
        unsigned long st = ((uint32_t)(uint16_t)pkt.v2 << 16) | (uint16_t)pkt.v1;
        // align roughly to local clock: if far, use now+400
        if (labs((long)st - (long)millis()) > 2000)
            st = millis() + 400;
        rhHost = false;
        strncpy(rhPeerId, pkt.senderId, DEVICE_ID_MAX_LEN);
        rhStartGame(seed, st);
        return;
    }
    if (pkt.opcode == GAME_OP_ACTION || pkt.opcode == GAME_OP_SCORE) {
        rhPeerScore = pkt.v0;
        rhDirty = true;
    }
}

// =============================================================================
// 狼人杀（3~12 人，无法官：哈希最小 ID 做时钟推进）
// 布局分区（320x240，禁止叠层）：
//   0..24  顶栏  26..44 阶段+倒计时  46..62 状态条  64..200 座位  208..236 操作栏
enum WwRole_e {
    WW_ROLE_NONE = 0,
    WW_ROLE_VILLAGER,
    WW_ROLE_WEREWOLF,
    WW_ROLE_SEER,
    WW_ROLE_WITCH,
    WW_ROLE_HUNTER,
    WW_ROLE_IDIOT
};

enum WwPhase_e {
    WW_PHASE_LOBBY = 0,
    WW_PHASE_NIGHT_WOLF,
    WW_PHASE_NIGHT_WITCH,
    WW_PHASE_NIGHT_SEER,
    WW_PHASE_DAY_ANNOUNCE,
    WW_PHASE_DAY_TALK,
    WW_PHASE_DAY_VOTE,
    WW_PHASE_HUNTER,
    WW_PHASE_END
};

struct WwPlayer {
    char id[DEVICE_ID_MAX_LEN + 1];
    uint8_t role;
    bool alive;
    bool idiotRevealed;
    bool voted;
    int8_t voteFor;
};

static WwPlayer wwP[GAME_WW_MAX_PLAYERS];
static int wwN = 0;
static bool wwStarted = false;
static uint32_t wwSeed = 1;
static uint8_t wwPhase = WW_PHASE_LOBBY;
static uint8_t wwDay = 0;
static uint8_t wwMyRole = WW_ROLE_NONE;
static int wwMySeat = -1;
static int8_t wwWolfTarget = -1;
static int8_t wwWitchSave = 0;
static int8_t wwWitchPoison = -1;
static bool wwHasSave = true, wwHasPoison = true;
static int8_t wwSeerCheck = -1;
static int8_t wwSeerResult = -1;
static int8_t wwDeathA = -1, wwDeathB = -1;
static int8_t wwHunterTarget = -1;
static bool wwHunterFromNight = false;
static bool wwDirty = true;
static unsigned long wwPhaseUntil = 0;
static unsigned long wwLastDraw = 0;
static char wwBanner[48] = {0};
static uint8_t wwWinner = 0;
static bool wwHadGod = false;
static bool wwHadVill = false;
static uint8_t wwLastAdvancePhase = 0xFF;
static int8_t wwUiSelect = -1; // 当前点选高亮

// 座位网格（与绘制/点击一致）
static const int WW_COLS = 3;
static const int WW_CELL_W = 96;
static const int WW_CELL_H = 28;
static const int WW_GAP = 4;
static const int WW_OX = 8;
static const int WW_OY = 66;
static const int WW_ACT_Y = 208;

static const char *wwRoleName(uint8_t r)
{
    switch (r) {
    case WW_ROLE_WEREWOLF: return "狼人";
    case WW_ROLE_SEER: return "预言家";
    case WW_ROLE_WITCH: return "女巫";
    case WW_ROLE_HUNTER: return "猎人";
    case WW_ROLE_IDIOT: return "白痴";
    case WW_ROLE_VILLAGER: return "平民";
    default: return "未知";
    }
}

static const char *wwPhaseShort(uint8_t ph)
{
    switch (ph) {
    case WW_PHASE_NIGHT_WOLF: return "夜晚-刀人";
    case WW_PHASE_NIGHT_WITCH: return "夜晚-女巫";
    case WW_PHASE_NIGHT_SEER: return "夜晚-预言";
    case WW_PHASE_DAY_ANNOUNCE: return "天亮公布";
    case WW_PHASE_DAY_TALK: return "白天讨论";
    case WW_PHASE_DAY_VOTE: return "白天投票";
    case WW_PHASE_HUNTER: return "猎人开枪";
    case WW_PHASE_END: return "游戏结束";
    default: return "等待开始";
    }
}

static int wwFind(const char *id)
{
    for (int i = 0; i < wwN; i++)
        if (strcmp(wwP[i].id, id) == 0)
            return i;
    return -1;
}

static int wwEnsure(const char *id)
{
    int i = wwFind(id);
    if (i >= 0)
        return i;
    if (!id || !id[0] || wwN >= GAME_WW_MAX_PLAYERS)
        return -1;
    wwN++;
    memset(&wwP[wwN - 1], 0, sizeof(wwP[0]));
    strncpy(wwP[wwN - 1].id, id, DEVICE_ID_MAX_LEN);
    wwP[wwN - 1].alive = true;
    wwP[wwN - 1].voteFor = -1;
    for (int a = 0; a < wwN; a++) {
        for (int b = a + 1; b < wwN; b++) {
            if (idHash16(wwP[b].id) < idHash16(wwP[a].id)) {
                WwPlayer t = wwP[a];
                wwP[a] = wwP[b];
                wwP[b] = t;
            }
        }
    }
    wwMySeat = wwFind(localDeviceId);
    return wwFind(id);
}

static uint32_t wwRand(uint32_t &s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    if (!s)
        s = 0xA5A5u;
    return s;
}

static bool wwRoleAlive(uint8_t role)
{
    for (int i = 0; i < wwN; i++)
        if (wwP[i].alive && wwP[i].role == role)
            return true;
    return false;
}

static void wwFormatDeaths(char *out, size_t n)
{
    if (!out || n < 8)
        return;
    if (wwDeathA < 0 && wwDeathB < 0) {
        snprintf(out, n, "平安夜");
        return;
    }
    if (wwDeathA >= 0 && wwDeathB >= 0)
        snprintf(out, n, "出局:%d号,%d号", wwDeathA + 1, wwDeathB + 1);
    else if (wwDeathA >= 0)
        snprintf(out, n, "出局:%d号", wwDeathA + 1);
    else
        snprintf(out, n, "出局:%d号", wwDeathB + 1);
}

static void wwBroadcastPhase()
{
    int16_t a = wwDeathA, b = wwDeathB;
    // 女巫回合用 v2 同步「刀口」给各端显示，尚未真正结算死亡
    if (wwPhase == WW_PHASE_NIGHT_WITCH)
        a = wwWolfTarget;
    moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_STATE, (int16_t)wwPhase, (int16_t)wwDay, a, b);
}

static void wwFillRoleBag(uint8_t *bag, int &n)
{
    int wolves = 1, seer = 0, witch = 0, hunter = 0, idiot = 0;
    if (wwN >= 12) {
        wolves = 4;
        seer = witch = hunter = idiot = 1;
    } else if (wwN >= 10) {
        wolves = 3;
        seer = witch = hunter = idiot = 1;
    } else if (wwN >= 8) {
        wolves = 2;
        seer = witch = hunter = 1;
    } else if (wwN >= 6) {
        wolves = 2;
        seer = witch = 1;
    } else if (wwN >= 5) {
        wolves = 1;
        seer = witch = 1;
    } else if (wwN >= 4) {
        wolves = 1;
        seer = 1;
    } else {
        wolves = 1;
        seer = 1;
    }
    n = 0;
    for (int i = 0; i < wolves && n < wwN; i++)
        bag[n++] = WW_ROLE_WEREWOLF;
    if (seer && n < wwN)
        bag[n++] = WW_ROLE_SEER;
    if (witch && n < wwN)
        bag[n++] = WW_ROLE_WITCH;
    if (hunter && n < wwN)
        bag[n++] = WW_ROLE_HUNTER;
    if (idiot && n < wwN)
        bag[n++] = WW_ROLE_IDIOT;
    while (n < wwN)
        bag[n++] = WW_ROLE_VILLAGER;
}

static void wwAssignRolesFromSeed(uint32_t seed)
{
    wwSeed = seed ? seed : 1;
    uint8_t bag[GAME_WW_MAX_PLAYERS];
    int n = 0;
    wwFillRoleBag(bag, n);
    uint32_t s = wwSeed;
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(wwRand(s) % (uint32_t)(i + 1));
        uint8_t t = bag[i];
        bag[i] = bag[j];
        bag[j] = t;
    }
    wwHadGod = wwHadVill = false;
    for (int i = 0; i < wwN; i++) {
        wwP[i].role = bag[i];
        wwP[i].alive = true;
        wwP[i].idiotRevealed = false;
        wwP[i].voted = false;
        wwP[i].voteFor = -1;
        if (bag[i] == WW_ROLE_VILLAGER)
            wwHadVill = true;
        if (bag[i] == WW_ROLE_SEER || bag[i] == WW_ROLE_WITCH ||
            bag[i] == WW_ROLE_HUNTER || bag[i] == WW_ROLE_IDIOT)
            wwHadGod = true;
    }
    wwMySeat = wwFind(localDeviceId);
    wwMyRole = (wwMySeat >= 0) ? wwP[wwMySeat].role : WW_ROLE_NONE;
    wwHasSave = wwRoleAlive(WW_ROLE_WITCH);
    wwHasPoison = wwHasSave;
}

static bool wwCheckWin()
{
    int wolf = 0, god = 0, vill = 0;
    for (int i = 0; i < wwN; i++) {
        if (!wwP[i].alive)
            continue;
        if (wwP[i].role == WW_ROLE_WEREWOLF)
            wolf++;
        else if (wwP[i].role == WW_ROLE_VILLAGER)
            vill++;
        else
            god++;
    }
    if (wolf == 0) {
        wwWinner = 1;
        wwPhase = WW_PHASE_END;
        snprintf(wwBanner, sizeof(wwBanner), "好人胜利");
        return true;
    }
    // 屠边：神职全灭或平民全灭；或好人全灭
    if ((wwHadGod && god == 0) || (wwHadVill && vill == 0) || (god + vill == 0)) {
        wwWinner = 2;
        wwPhase = WW_PHASE_END;
        snprintf(wwBanner, sizeof(wwBanner), "狼人胜利");
        return true;
    }
    return false;
}

static void wwKillSeat(int seat)
{
    if (seat < 0 || seat >= wwN || !wwP[seat].alive)
        return;
    wwP[seat].alive = false;
}

static bool wwSeatIsHunterDead(int seat)
{
    return seat >= 0 && seat < wwN && wwP[seat].role == WW_ROLE_HUNTER && !wwP[seat].alive;
}

static void wwResolveNight()
{
    wwDeathA = wwDeathB = -1;
    int killed = wwWolfTarget;
    if (wwWitchSave && killed >= 0 && wwHasSave) {
        killed = -1;
        wwHasSave = false;
    }
    if (killed >= 0) {
        wwKillSeat(killed);
        wwDeathA = (int8_t)killed;
    }
    if (wwWitchPoison >= 0 && wwHasPoison) {
        if (wwWitchPoison != wwDeathA) {
            wwKillSeat(wwWitchPoison);
            wwDeathB = wwWitchPoison;
        }
        wwHasPoison = false;
    }
    wwWolfTarget = -1;
    wwWitchSave = 0;
    wwWitchPoison = -1;
    wwSeerCheck = -1;
    wwUiSelect = -1;
}

static void wwEnterPhase(uint8_t ph, unsigned long durMs);
static void wwGotoNextNightOrEnd();
static void wwBeginGame(uint32_t seed);
static void wwTryStartLocal();
static void wwAfterNightResolve();

static bool wwIsClockLeader()
{
    uint16_t myH = idHash16(localDeviceId);
    for (int i = 0; i < wwN; i++) {
        if (idHash16(wwP[i].id) < myH)
            return false;
    }
    return wwN > 0;
}

static unsigned long wwPhaseDuration(uint8_t ph)
{
    switch (ph) {
    case WW_PHASE_NIGHT_WOLF: return 22000UL;
    case WW_PHASE_NIGHT_WITCH: return 20000UL;
    case WW_PHASE_NIGHT_SEER: return 18000UL;
    case WW_PHASE_DAY_ANNOUNCE: return 10000UL;
    case WW_PHASE_DAY_TALK: return 35000UL;
    case WW_PHASE_DAY_VOTE: return 28000UL;
    case WW_PHASE_HUNTER: return 18000UL;
    default: return 15000UL;
    }
}

static void wwAfterNightResolve()
{
    if (wwCheckWin()) {
        moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_SCORE, (int16_t)wwWinner, 0, 0, 0);
        wwBroadcastPhase();
        wwDirty = true;
        return;
    }
    // 夜间被刀的猎人可开枪
    if (wwSeatIsHunterDead(wwDeathA) || wwSeatIsHunterDead(wwDeathB)) {
        wwHunterFromNight = true;
        wwHunterTarget = -1;
        wwFormatDeaths(wwBanner, sizeof(wwBanner));
        wwEnterPhase(WW_PHASE_HUNTER, wwPhaseDuration(WW_PHASE_HUNTER));
        return;
    }
    wwFormatDeaths(wwBanner, sizeof(wwBanner));
    wwEnterPhase(WW_PHASE_DAY_ANNOUNCE, wwPhaseDuration(WW_PHASE_DAY_ANNOUNCE));
}

static void wwAdvancePhase()
{
    if (!wwStarted || wwPhase == WW_PHASE_LOBBY || wwPhase == WW_PHASE_END)
        return;
    if (!wwIsClockLeader())
        return;
    if (wwLastAdvancePhase == wwPhase)
        return;
    wwLastAdvancePhase = wwPhase;

    switch (wwPhase) {
    case WW_PHASE_NIGHT_WOLF:
        if (wwRoleAlive(WW_ROLE_WITCH) && (wwHasSave || wwHasPoison))
            wwEnterPhase(WW_PHASE_NIGHT_WITCH, wwPhaseDuration(WW_PHASE_NIGHT_WITCH));
        else if (wwRoleAlive(WW_ROLE_SEER))
            wwEnterPhase(WW_PHASE_NIGHT_SEER, wwPhaseDuration(WW_PHASE_NIGHT_SEER));
        else {
            wwResolveNight();
            wwAfterNightResolve();
        }
        break;
    case WW_PHASE_NIGHT_WITCH:
        if (wwRoleAlive(WW_ROLE_SEER))
            wwEnterPhase(WW_PHASE_NIGHT_SEER, wwPhaseDuration(WW_PHASE_NIGHT_SEER));
        else {
            wwResolveNight();
            wwAfterNightResolve();
        }
        break;
    case WW_PHASE_NIGHT_SEER:
        wwResolveNight();
        wwAfterNightResolve();
        break;
    case WW_PHASE_DAY_ANNOUNCE:
        snprintf(wwBanner, sizeof(wwBanner), "请讨论发言");
        wwEnterPhase(WW_PHASE_DAY_TALK, wwPhaseDuration(WW_PHASE_DAY_TALK));
        break;
    case WW_PHASE_DAY_TALK:
        snprintf(wwBanner, sizeof(wwBanner), "点座位投票");
        wwEnterPhase(WW_PHASE_DAY_VOTE, wwPhaseDuration(WW_PHASE_DAY_VOTE));
        break;
    case WW_PHASE_DAY_VOTE: {
        int votes[GAME_WW_MAX_PLAYERS];
        memset(votes, 0, sizeof(votes));
        for (int i = 0; i < wwN; i++) {
            if (!wwP[i].alive || wwP[i].idiotRevealed)
                continue;
            if (wwP[i].voteFor >= 0 && wwP[i].voteFor < wwN)
                votes[wwP[i].voteFor]++;
        }
        int best = -1, bestV = 0, tie = 0;
        for (int i = 0; i < wwN; i++) {
            if (votes[i] > bestV) {
                bestV = votes[i];
                best = i;
                tie = 0;
            } else if (votes[i] == bestV && bestV > 0)
                tie = 1;
        }
        bool hunterShot = false;
        wwDeathA = wwDeathB = -1;
        if (best >= 0 && !tie && bestV > 0) {
            if (wwP[best].role == WW_ROLE_IDIOT && !wwP[best].idiotRevealed) {
                wwP[best].idiotRevealed = true;
                snprintf(wwBanner, sizeof(wwBanner), "%d号白痴免死", best + 1);
            } else {
                uint8_t role = wwP[best].role;
                wwKillSeat(best);
                wwDeathA = (int8_t)best;
                snprintf(wwBanner, sizeof(wwBanner), "%d号被放逐", best + 1);
                if (role == WW_ROLE_HUNTER) {
                    hunterShot = true;
                    wwHunterFromNight = false;
                    wwHunterTarget = -1;
                    wwEnterPhase(WW_PHASE_HUNTER, wwPhaseDuration(WW_PHASE_HUNTER));
                }
            }
        } else {
            snprintf(wwBanner, sizeof(wwBanner), "平票无人出局");
        }
        wwUiSelect = -1;
        if (!hunterShot)
            wwGotoNextNightOrEnd();
        break;
    }
    case WW_PHASE_HUNTER:
        if (wwHunterTarget >= 0)
            wwKillSeat(wwHunterTarget);
        wwHunterTarget = -1;
        wwUiSelect = -1;
        if (wwHunterFromNight) {
            wwHunterFromNight = false;
            if (wwCheckWin()) {
                moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_SCORE, (int16_t)wwWinner, 0, 0, 0);
                wwBroadcastPhase();
            } else {
                wwFormatDeaths(wwBanner, sizeof(wwBanner));
                wwEnterPhase(WW_PHASE_DAY_ANNOUNCE, wwPhaseDuration(WW_PHASE_DAY_ANNOUNCE));
            }
        } else {
            wwGotoNextNightOrEnd();
        }
        break;
    default:
        break;
    }
}

static void wwEnterPhase(uint8_t ph, unsigned long durMs)
{
    wwPhase = ph;
    wwPhaseUntil = millis() + durMs;
    wwLastAdvancePhase = 0xFF;
    wwUiSelect = -1;
    if (ph == WW_PHASE_DAY_VOTE) {
        for (int i = 0; i < wwN; i++) {
            wwP[i].voted = false;
            wwP[i].voteFor = -1;
        }
    }
    if (ph == WW_PHASE_NIGHT_WOLF) {
        wwWolfTarget = -1;
        wwWitchSave = 0;
        wwWitchPoison = -1;
        wwSeerResult = -1;
        snprintf(wwBanner, sizeof(wwBanner), "天黑请闭眼");
    } else if (ph == WW_PHASE_NIGHT_WITCH) {
        if (wwWolfTarget >= 0)
            snprintf(wwBanner, sizeof(wwBanner), "刀口:%d号 选药", wwWolfTarget + 1);
        else
            snprintf(wwBanner, sizeof(wwBanner), "今夜空刀 选药");
    } else if (ph == WW_PHASE_NIGHT_SEER) {
        snprintf(wwBanner, sizeof(wwBanner), "预言家验人");
    } else if (ph == WW_PHASE_DAY_TALK) {
        snprintf(wwBanner, sizeof(wwBanner), "请讨论发言");
    } else if (ph == WW_PHASE_HUNTER) {
        snprintf(wwBanner, sizeof(wwBanner), "猎人请开枪");
    }
    wwBroadcastPhase();
    wwDirty = true;
}

static void wwGotoNextNightOrEnd()
{
    if (wwCheckWin()) {
        moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_SCORE, (int16_t)wwWinner, 0, 0, 0);
        wwBroadcastPhase();
        wwDirty = true;
        return;
    }
    wwDay++;
    wwEnterPhase(WW_PHASE_NIGHT_WOLF, wwPhaseDuration(WW_PHASE_NIGHT_WOLF));
}

static void wwBeginGame(uint32_t seed)
{
    if (wwN < GAME_WW_MIN_PLAYERS)
        return;
    wwAssignRolesFromSeed(seed);
    wwStarted = true;
    wwDay = 1;
    wwWinner = 0;
    wwWolfTarget = -1;
    wwWitchSave = 0;
    wwWitchPoison = -1;
    wwSeerResult = -1;
    wwDeathA = wwDeathB = -1;
    wwHunterTarget = -1;
    wwHunterFromNight = false;
    wwUiSelect = -1;
    snprintf(wwBanner, sizeof(wwBanner), "身份已发 天黑");
    wwEnterPhase(WW_PHASE_NIGHT_WOLF, wwPhaseDuration(WW_PHASE_NIGHT_WOLF));
}

static void wwTryStartLocal()
{
    if (wwPhase != WW_PHASE_LOBBY || wwN < GAME_WW_MIN_PLAYERS || wwStarted)
        return;
    uint32_t seed = (esp_random() << 1) | 1u;
    moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_READY, (int16_t)(seed & 0xFFFF),
                       (int16_t)((seed >> 16) & 0xFFFF), (int16_t)wwN, 0);
    wwBeginGame(seed);
}

static int wwHitSeat(int x, int y)
{
    for (int i = 0; i < wwN; i++) {
        int c = i % WW_COLS, r = i / WW_COLS;
        int sx = WW_OX + c * (WW_CELL_W + WW_GAP);
        int sy = WW_OY + r * (WW_CELL_H + WW_GAP);
        if (x >= sx && x <= sx + WW_CELL_W && y >= sy && y <= sy + WW_CELL_H)
            return i;
    }
    return -1;
}

static bool wwCanActAlive()
{
    return wwMySeat >= 0 && wwP[wwMySeat].alive;
}

static bool wwIsHunterShooting()
{
    // 猎人死后开枪：本机是猎人且已死亡，处于猎人阶段
    return wwPhase == WW_PHASE_HUNTER && wwMySeat >= 0 &&
           wwMyRole == WW_ROLE_HUNTER && !wwP[wwMySeat].alive;
}

static void wwDrawTimerStrip()
{
    uint16_t bar = tft.color565(28, 22, 40);
    tft.fillRect(210, 26, 110, 18, bar);
    long leftMs = (long)wwPhaseUntil - (long)millis();
    if (leftMs < 0)
        leftMs = 0;
    char day[24];
    if (wwPhase != WW_PHASE_LOBBY && wwPhase != WW_PHASE_END)
        snprintf(day, sizeof(day), "D%d %lds", (int)wwDay, leftMs / 1000L);
    else if (wwPhase == WW_PHASE_END)
        snprintf(day, sizeof(day), "D%d END", (int)wwDay);
    else
        snprintf(day, sizeof(day), "%dP", wwN);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_YELLOW, bar);
    tft.drawString(day, 316, 30, 1);
    tft.setTextDatum(TL_DATUM);
}

void werewolfEnter()
{
    memset(wwP, 0, sizeof(wwP));
    wwN = 0;
    wwStarted = false;
    wwPhase = WW_PHASE_LOBBY;
    wwDay = 0;
    wwMyRole = WW_ROLE_NONE;
    wwMySeat = -1;
    wwHasSave = wwHasPoison = true;
    wwWinner = 0;
    wwBanner[0] = 0;
    wwDeathA = wwDeathB = -1;
    wwLastAdvancePhase = 0xFF;
    wwUiSelect = -1;
    wwHunterFromNight = false;
    wwEnsure(localDeviceId);
    moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_JOIN, 0, 0, 0, 0);
    wwDirty = true;
    werewolfDraw();
}

void werewolfLeave()
{
    moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_LEAVE, 0, 0, 0, 0);
    wwPhase = WW_PHASE_LOBBY;
    wwStarted = false;
    wwN = 0;
}

void werewolfDraw()
{
    uint16_t bg = tft.color565(14, 12, 20);
    uint16_t bar = tft.color565(40, 20, 50);
    uint16_t phaseBg = tft.color565(28, 22, 40);
    uint16_t infoBg = tft.color565(20, 18, 30);
    tft.fillScreen(bg);
    drawMoreBackInv("狼人杀", bar);

    // 阶段条（短名，避免与倒计时重叠）
    tft.fillRect(0, 26, SCREEN_WIDTH, 18, phaseBg);
    cnDrawUtf8(tft, 6, 29, wwPhaseShort(wwPhase), TFT_CYAN, phaseBg, false);
    wwDrawTimerStrip();

    // 状态条：身份 + 提示（单行，不叠）
    tft.fillRect(0, 46, SCREEN_WIDTH, 18, infoBg);
    char line[56];
    if (wwMyRole != WW_ROLE_NONE && wwMySeat >= 0) {
        if (wwMyRole == WW_ROLE_WEREWOLF) {
            char allies[24] = {0};
            int nAlly = 0;
            for (int i = 0; i < wwN; i++) {
                if (i == wwMySeat || wwP[i].role != WW_ROLE_WEREWOLF)
                    continue;
                char bit[6];
                snprintf(bit, sizeof(bit), "%d", i + 1);
                if (nAlly)
                    strncat(allies, ",", sizeof(allies) - strlen(allies) - 1);
                strncat(allies, bit, sizeof(allies) - strlen(allies) - 1);
                nAlly++;
            }
            if (nAlly)
                snprintf(line, sizeof(line), "%d狼 伴:%s", wwMySeat + 1, allies);
            else
                snprintf(line, sizeof(line), "%d号狼人", wwMySeat + 1);
            cnDrawUtf8(tft, 6, 49, line, TFT_RED, infoBg, false);
        } else {
            snprintf(line, sizeof(line), "%d号%s", wwMySeat + 1, wwRoleName(wwMyRole));
            cnDrawUtf8(tft, 6, 49, line, TFT_GREEN, infoBg, false);
        }
    } else {
        cnDrawUtf8(tft, 6, 49, "等待发身份", TFT_LIGHTGREY, infoBg, false);
    }

    // 提示画在身份行右侧，避免与座位叠层
    {
        const char *tip = wwBanner;
        char seerTip[40];
        if (wwSeerResult >= 0 && wwMyRole == WW_ROLE_SEER && wwSeerCheck >= 0) {
            snprintf(seerTip, sizeof(seerTip), "验%d:%s", wwSeerCheck + 1,
                     wwSeerResult ? "狼" : "好");
            tip = seerTip;
        }
        if (tip && tip[0]) {
            int tipX = 118;
            tft.fillRect(tipX, 46, SCREEN_WIDTH - tipX, 18, infoBg);
            cnDrawUtf8Ellipsis(tft, tipX, 49, tip, TFT_YELLOW, SCREEN_WIDTH - tipX - 4, infoBg,
                               false);
        }
    }

    // 座位 3 列，最多 4 行，不侵入操作栏
    for (int i = 0; i < wwN; i++) {
        int c = i % WW_COLS, r = i / WW_COLS;
        int x = WW_OX + c * (WW_CELL_W + WW_GAP);
        int y = WW_OY + r * (WW_CELL_H + WW_GAP);
        if (y + WW_CELL_H > WW_ACT_Y - 2)
            break;
        uint16_t cbg = wwP[i].alive ? tft.color565(32, 38, 58) : tft.color565(48, 28, 28);
        if (wwP[i].idiotRevealed)
            cbg = tft.color565(70, 60, 30);
        tft.fillRoundRect(x, y, WW_CELL_W, WW_CELL_H, 4, cbg);
        uint16_t border = tft.color565(70, 80, 110);
        if (i == wwMySeat)
            border = TFT_CYAN;
        if (i == wwUiSelect)
            border = TFT_YELLOW;
        if (wwPhase == WW_PHASE_DAY_VOTE && wwMySeat >= 0 && wwP[wwMySeat].voteFor == i)
            border = TFT_ORANGE;
        tft.drawRoundRect(x, y, WW_CELL_W, WW_CELL_H, 4, border);
        char no[10];
        snprintf(no, sizeof(no), "%d", i + 1);
        tft.setTextColor(TFT_WHITE, cbg);
        tft.drawString(no, x + 4, y + 2, 2);
        cnDrawUtf8Ellipsis(tft, x + 22, y + 8, wwP[i].id,
                           wwP[i].alive ? TFT_LIGHTGREY : TFT_DARKGREY, WW_CELL_W - 28, cbg,
                           false);
        if (!wwP[i].alive)
            tft.drawLine(x + 2, y + 2, x + WW_CELL_W - 2, y + WW_CELL_H - 2, TFT_RED);
    }

    // 操作栏（独占底部，不与座位重叠）
    tft.fillRect(0, WW_ACT_Y - 2, SCREEN_WIDTH, SCREEN_HEIGHT - (WW_ACT_Y - 2),
                 tft.color565(18, 16, 26));
    int by = WW_ACT_Y;
    if (wwPhase == WW_PHASE_LOBBY) {
        bool ok = wwN >= GAME_WW_MIN_PLAYERS;
        tft.fillRoundRect(12, by, 110, 26, 4,
                          ok ? tft.color565(50, 120, 80) : tft.color565(50, 50, 60));
        cnDrawUtf8(tft, 30, by + 6, "开始", TFT_WHITE);
        char nbuf[28];
        snprintf(nbuf, sizeof(nbuf), "%d/%d人 无法官", wwN, GAME_WW_MIN_PLAYERS);
        tft.setTextColor(TFT_LIGHTGREY, tft.color565(18, 16, 26));
        tft.drawString(nbuf, 136, by + 8, 1);
    } else if (wwPhase == WW_PHASE_NIGHT_WITCH && wwMyRole == WW_ROLE_WITCH && wwCanActAlive()) {
        if (wwHasSave && wwWolfTarget >= 0) {
            tft.fillRoundRect(8, by, 72, 26, 4, tft.color565(40, 120, 80));
            cnDrawUtf8(tft, 24, by + 6, "解药", TFT_WHITE);
        } else {
            tft.fillRoundRect(8, by, 72, 26, 4, tft.color565(40, 40, 50));
            cnDrawUtf8(tft, 24, by + 6, "解药", TFT_DARKGREY);
        }
        if (wwHasPoison) {
            tft.fillRoundRect(90, by, 72, 26, 4, tft.color565(120, 40, 80));
            cnDrawUtf8(tft, 106, by + 6, "毒药", TFT_WHITE);
        } else {
            tft.fillRoundRect(90, by, 72, 26, 4, tft.color565(40, 40, 50));
            cnDrawUtf8(tft, 106, by + 6, "毒药", TFT_DARKGREY);
        }
        tft.fillRoundRect(172, by, 72, 26, 4, tft.color565(60, 60, 80));
        cnDrawUtf8(tft, 188, by + 6, "跳过", TFT_WHITE);
        if (wwWitchPoison >= 0) {
            char t[20];
            snprintf(t, sizeof(t), "毒%d", wwWitchPoison + 1);
            tft.setTextColor(TFT_ORANGE, tft.color565(18, 16, 26));
            tft.drawString(t, 256, by + 8, 1);
        }
    } else if (wwPhase == WW_PHASE_NIGHT_WOLF && wwMyRole == WW_ROLE_WEREWOLF && wwCanActAlive()) {
        cnDrawUtf8(tft, 12, by + 6,
                   wwWolfTarget >= 0 ? "已选刀口 等夜晚结束" : "点座位选择刀口", TFT_LIGHTGREY);
    } else if (wwPhase == WW_PHASE_NIGHT_SEER && wwMyRole == WW_ROLE_SEER && wwCanActAlive()) {
        cnDrawUtf8(tft, 12, by + 6, "点座位验人", TFT_LIGHTGREY);
    } else if (wwPhase == WW_PHASE_DAY_VOTE && wwCanActAlive() && !wwP[wwMySeat].idiotRevealed) {
        cnDrawUtf8(tft, 12, by + 6, "点座位投票放逐", TFT_LIGHTGREY);
    } else if (wwIsHunterShooting()) {
        cnDrawUtf8(tft, 12, by + 6, "点座位开枪带走", TFT_ORANGE);
    } else if (wwPhase == WW_PHASE_END) {
        cnDrawUtf8(tft, 90, by + 6, wwWinner == 1 ? "好人阵营胜利" : "狼人阵营胜利",
                   TFT_YELLOW);
    } else if (wwPhase == WW_PHASE_DAY_TALK || wwPhase == WW_PHASE_DAY_ANNOUNCE) {
        cnDrawUtf8(tft, 12, by + 6, "等待阶段结束…", TFT_DARKGREY);
    } else {
        cnDrawUtf8(tft, 12, by + 6, "请等待…", TFT_DARKGREY);
    }

    wwLastDraw = millis();
    wwDirty = false;
}

void werewolfUpdate()
{
    if (wwStarted && wwPhase != WW_PHASE_LOBBY && wwPhase != WW_PHASE_END) {
        if ((long)(millis() - wwPhaseUntil) >= 0)
            wwAdvancePhase();
    }
    if (wwDirty) {
        werewolfDraw();
        return;
    }
    // 仅刷新倒计时，避免整屏闪烁叠影
    if (wwPhase != WW_PHASE_LOBBY && wwPhase != WW_PHASE_END &&
        millis() - wwLastDraw > 500UL) {
        wwDrawTimerStrip();
        wwLastDraw = millis();
    }
}

bool werewolfTouch(int x, int y, bool rising)
{
    if (!rising)
        return true;
    if (hitMoreBack(x, y))
        return false;

    if (wwPhase == WW_PHASE_LOBBY && y >= WW_ACT_Y) {
        if (x < 130 && wwN >= GAME_WW_MIN_PLAYERS)
            wwTryStartLocal();
        return true;
    }

    int seat = wwHitSeat(x, y);

    // 猎人开枪（死后可操作）
    if (seat >= 0 && wwIsHunterShooting() && wwP[seat].alive && seat != wwMySeat) {
        wwHunterTarget = (int8_t)seat;
        wwUiSelect = (int8_t)seat;
        moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 30, seat, 0, 0);
        snprintf(wwBanner, sizeof(wwBanner), "开枪:%d号", seat + 1);
        wwDirty = true;
        return true;
    }

    if (seat >= 0 && wwCanActAlive()) {
        if (wwPhase == WW_PHASE_NIGHT_WOLF && wwMyRole == WW_ROLE_WEREWOLF && wwP[seat].alive &&
            seat != wwMySeat) {
            // 不可刀队友
            if (wwP[seat].role == WW_ROLE_WEREWOLF) {
                snprintf(wwBanner, sizeof(wwBanner), "不可刀队友");
                wwDirty = true;
                return true;
            }
            wwWolfTarget = (int8_t)seat;
            wwUiSelect = (int8_t)seat;
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 10, seat, 0, 0);
            snprintf(wwBanner, sizeof(wwBanner), "刀%d号", seat + 1);
            wwDirty = true;
        } else if (wwPhase == WW_PHASE_NIGHT_SEER && wwMyRole == WW_ROLE_SEER && wwP[seat].alive &&
                   seat != wwMySeat) {
            wwSeerCheck = (int8_t)seat;
            wwUiSelect = (int8_t)seat;
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 12, seat, 0,
                               (int16_t)idHash16(localDeviceId));
            snprintf(wwBanner, sizeof(wwBanner), "验%d号…", seat + 1);
            wwDirty = true;
        } else if (wwPhase == WW_PHASE_NIGHT_WITCH && wwMyRole == WW_ROLE_WITCH && wwHasPoison &&
                   wwP[seat].alive && seat != wwMySeat) {
            wwWitchPoison = (int8_t)seat;
            wwUiSelect = (int8_t)seat;
            snprintf(wwBanner, sizeof(wwBanner), "毒目标%d 再点毒药", seat + 1);
            wwDirty = true;
        } else if (wwPhase == WW_PHASE_DAY_VOTE && !wwP[wwMySeat].idiotRevealed &&
                   wwP[seat].alive) {
            wwP[wwMySeat].voteFor = (int8_t)seat;
            wwP[wwMySeat].voted = true;
            wwUiSelect = (int8_t)seat;
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 20, seat, 0, 0);
            snprintf(wwBanner, sizeof(wwBanner), "投%d号", seat + 1);
            wwDirty = true;
        }
    }

    if (wwPhase == WW_PHASE_NIGHT_WITCH && wwMyRole == WW_ROLE_WITCH && wwCanActAlive() &&
        y >= WW_ACT_Y) {
        if (x < 84 && wwHasSave && wwWolfTarget >= 0) {
            wwWitchSave = 1;
            wwWitchPoison = -1; // 同夜解/毒二选一：选解则清毒目标
            wwHasSave = false;
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 11, 1, wwWolfTarget, 0);
            snprintf(wwBanner, sizeof(wwBanner), "已用解药");
            wwDirty = true;
        } else if (x >= 90 && x < 166 && wwHasPoison && wwWitchPoison >= 0) {
            wwWitchSave = 0;
            wwHasPoison = false;
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 11, 2, wwWitchPoison, 0);
            snprintf(wwBanner, sizeof(wwBanner), "已下毒%d号", wwWitchPoison + 1);
            wwDirty = true;
        } else if (x >= 172 && x < 250) {
            wwWitchSave = 0;
            wwWitchPoison = -1;
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 11, 0, 0, 0);
            snprintf(wwBanner, sizeof(wwBanner), "女巫跳过");
            wwDirty = true;
        }
    }
    return true;
}

void werewolfApplyPacket(const GamePacket_t &pkt)
{
    if (pkt.opcode == GAME_OP_JOIN) {
        wwEnsure(pkt.senderId);
        if (wwPhase == WW_PHASE_LOBBY && pkt.v0 == 0)
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_JOIN, 1, (int16_t)wwN, 0, 0);
        wwDirty = true;
        return;
    }
    if (pkt.opcode == GAME_OP_LEAVE) {
        int i = wwFind(pkt.senderId);
        if (i >= 0 && wwPhase == WW_PHASE_LOBBY) {
            wwP[i] = wwP[wwN - 1];
            wwN--;
            wwMySeat = wwFind(localDeviceId);
            wwDirty = true;
        }
        return;
    }
    if (pkt.opcode == GAME_OP_READY && wwPhase == WW_PHASE_LOBBY && !wwStarted) {
        uint32_t seed = ((uint32_t)(uint16_t)pkt.v1 << 16) | (uint16_t)pkt.v0;
        if (!seed)
            seed = 1;
        wwBeginGame(seed);
        return;
    }
    if (pkt.opcode == GAME_OP_STATE) {
        uint8_t ph = (uint8_t)pkt.v0;
        wwDay = (uint8_t)pkt.v1;
        int8_t a = (int8_t)pkt.v2;
        int8_t b = (int8_t)pkt.v3;

        if (ph == WW_PHASE_NIGHT_WITCH) {
            // v2 = 刀口（未结算）
            wwWolfTarget = a;
            if (wwWolfTarget >= 0)
                snprintf(wwBanner, sizeof(wwBanner), "刀口:%d号", wwWolfTarget + 1);
        } else if (ph == WW_PHASE_DAY_ANNOUNCE || ph == WW_PHASE_DAY_TALK ||
                   ph == WW_PHASE_DAY_VOTE || ph == WW_PHASE_HUNTER || ph == WW_PHASE_END ||
                   ph == WW_PHASE_NIGHT_WOLF) {
            // 公布/白天/下一夜：应用死亡标记
            if (ph != WW_PHASE_NIGHT_WOLF) {
                wwDeathA = a;
                wwDeathB = b;
                if (wwDeathA >= 0 && wwDeathA < wwN)
                    wwP[wwDeathA].alive = false;
                if (wwDeathB >= 0 && wwDeathB < wwN)
                    wwP[wwDeathB].alive = false;
            }
        }

        if (ph != wwPhase) {
            wwPhase = ph;
            wwPhaseUntil = millis() + wwPhaseDuration(ph);
            wwLastAdvancePhase = 0xFF;
            wwUiSelect = -1;
            if (ph == WW_PHASE_DAY_VOTE) {
                for (int i = 0; i < wwN; i++) {
                    wwP[i].voted = false;
                    wwP[i].voteFor = -1;
                }
            }
        } else {
            wwPhase = ph;
        }

        if (wwPhase == WW_PHASE_DAY_ANNOUNCE)
            wwFormatDeaths(wwBanner, sizeof(wwBanner));
        else if (wwPhase == WW_PHASE_DAY_TALK)
            snprintf(wwBanner, sizeof(wwBanner), "请讨论发言");
        else if (wwPhase == WW_PHASE_NIGHT_WOLF)
            snprintf(wwBanner, sizeof(wwBanner), "天黑请闭眼");
        else if (wwPhase == WW_PHASE_DAY_VOTE)
            snprintf(wwBanner, sizeof(wwBanner), "点座位投票");
        else if (wwPhase == WW_PHASE_HUNTER)
            snprintf(wwBanner, sizeof(wwBanner), "猎人请开枪");

        wwStarted = (wwPhase != WW_PHASE_LOBBY);
        wwDirty = true;
        return;
    }
    if (pkt.opcode == GAME_OP_SCORE) {
        wwWinner = (uint8_t)pkt.v0;
        wwPhase = WW_PHASE_END;
        wwStarted = true;
        snprintf(wwBanner, sizeof(wwBanner), wwWinner == 1 ? "好人胜利" : "狼人胜利");
        wwDirty = true;
        return;
    }
    if (pkt.opcode == GAME_OP_ACTION) {
        int16_t op = pkt.v0;
        if (op == 10) {
            int from = wwFind(pkt.senderId);
            if (from >= 0 && wwP[from].alive && wwP[from].role == WW_ROLE_WEREWOLF) {
                wwWolfTarget = (int8_t)pkt.v1;
                if (wwMyRole == WW_ROLE_WEREWOLF) {
                    wwUiSelect = wwWolfTarget;
                    snprintf(wwBanner, sizeof(wwBanner), "刀%d号", wwWolfTarget + 1);
                }
            }
            wwDirty = true;
            return;
        }
        if (op == 11) {
            if (pkt.v1 == 1) {
                wwWitchSave = 1;
                wwWitchPoison = -1;
                wwHasSave = false;
            } else if (pkt.v1 == 2) {
                wwWitchSave = 0;
                wwWitchPoison = (int8_t)pkt.v2;
                wwHasPoison = false;
            } else {
                wwWitchSave = 0;
                wwWitchPoison = -1;
            }
            wwDirty = true;
            return;
        }
        if (op == 12) {
            int seat = pkt.v1;
            if (seat == wwMySeat && wwMySeat >= 0) {
                int16_t res = (wwMyRole == WW_ROLE_WEREWOLF) ? 1 : 0;
                moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 13, seat, res, pkt.v3);
            }
            return;
        }
        if (op == 13) {
            if (pkt.v3 == 0 || pkt.v3 == (int16_t)idHash16(localDeviceId)) {
                if (wwMyRole == WW_ROLE_SEER) {
                    wwSeerCheck = (int8_t)pkt.v1;
                    wwSeerResult = (int8_t)pkt.v2;
                    snprintf(wwBanner, sizeof(wwBanner), "验%d:%s", wwSeerCheck + 1,
                             wwSeerResult ? "狼" : "好");
                    wwDirty = true;
                }
            }
            return;
        }
        if (op == 20) {
            int from = wwFind(pkt.senderId);
            if (from >= 0) {
                wwP[from].voteFor = (int8_t)pkt.v1;
                wwP[from].voted = true;
                wwDirty = true;
            }
            return;
        }
        if (op == 30) {
            wwHunterTarget = (int8_t)pkt.v1;
            wwUiSelect = wwHunterTarget;
            wwDirty = true;
            return;
        }
    }
}


// =============================================================================
// 海龟汤：出题人给汤面，喝汤人口头提问；设备同步 是/不是/无关/是也不是 与揭晓
// 布局分区（320x240，禁止叠层）：
//   0..24 顶栏  26..44 阶段  46..62 状态  64..178 内容  182..236 操作栏
// =============================================================================
#define GAME_SOUP_MAX_PLAYERS 8
#define GAME_SOUP_PUZZLE_N SOUP_PUZZLE_N
#define GAME_SOUP_ANS_HIST 5

enum SoupPhase_e {
    SOUP_PHASE_LOBBY = 0,
    SOUP_PHASE_PICK,
    SOUP_PHASE_PLAY,
    SOUP_PHASE_REVEAL,
    SOUP_PHASE_END
};

enum SoupAns_e {
    SOUP_ANS_NONE = 0,
    SOUP_ANS_YES = 1,
    SOUP_ANS_NO = 2,
    SOUP_ANS_IRREL = 3,
    SOUP_ANS_BOTH = 4
};

struct SoupPlayer {
    char id[DEVICE_ID_MAX_LEN + 1];
};

static SoupPlayer soupP[GAME_SOUP_MAX_PLAYERS];
static int soupN = 0;
static uint8_t soupPhase = SOUP_PHASE_LOBBY;
static int soupPuzzle = 0;
static int soupPickIdx = 0;
static uint8_t soupDiffFilter = SOUP_DIFF_ALL;
static int soupFilt[SOUP_PUZZLE_N];
static int soupFiltN = 0;
static int soupFiltPos = 0;

static bool soupIAmHost = false;
static bool soupShowTruth = false;
static bool soupDirty = true;
static unsigned long soupLastDraw = 0;
static uint8_t soupLastAns = SOUP_ANS_NONE;
static uint8_t soupAnsHist[GAME_SOUP_ANS_HIST];
static uint8_t soupAnsHistN = 0;
static char soupBanner[48] = {0};
static int soupClaimSeat = -1;
static uint8_t soupWinner = 0; // 1=猜中 2=揭晓

static const int SOUP_ACT_Y = 182;

static const char *soupDiffName(uint8_t d)
{
    switch (d) {
    case SOUP_DIFF_EASY: return "简单";
    case SOUP_DIFF_MID: return "中等";
    case SOUP_DIFF_HARD: return "困难";
    default: return "全部";
    }
}

static const char *soupPhaseShort(uint8_t ph)
{
    switch (ph) {
    case SOUP_PHASE_PICK: return "选汤";
    case SOUP_PHASE_PLAY: return "喝汤";
    case SOUP_PHASE_REVEAL: return "揭晓";
    case SOUP_PHASE_END: return "结束";
    default: return "房间";
    }
}

static const char *soupAnsName(uint8_t a)
{
    switch (a) {
    case SOUP_ANS_YES: return "是";
    case SOUP_ANS_NO: return "不是";
    case SOUP_ANS_IRREL: return "无关";
    case SOUP_ANS_BOTH: return "是也不是";
    default: return "-";
    }
}

static void soupRebuildFilter()
{
    soupFiltN = 0;
    for (int i = 0; i < SOUP_PUZZLE_N; i++) {
        if (soupDiffFilter == SOUP_DIFF_ALL || SOUP_PUZZLES[i].diff == soupDiffFilter)
            soupFilt[soupFiltN++] = i;
    }
    if (soupFiltN <= 0) {
        soupDiffFilter = SOUP_DIFF_ALL;
        for (int i = 0; i < SOUP_PUZZLE_N; i++)
            soupFilt[soupFiltN++] = i;
    }
    if (soupFiltPos >= soupFiltN)
        soupFiltPos = 0;
    soupPickIdx = soupFilt[soupFiltPos];
}

static void soupFiltStep(int delta)
{
    if (soupFiltN <= 0)
        soupRebuildFilter();
    soupFiltPos = (soupFiltPos + delta + soupFiltN * 64) % soupFiltN;
    soupPickIdx = soupFilt[soupFiltPos];
}

static void soupFiltRandom()
{
    if (soupFiltN <= 0)
        soupRebuildFilter();
    soupFiltPos = (int)(esp_random() % (uint32_t)soupFiltN);
    soupPickIdx = soupFilt[soupFiltPos];
}

static int soupFind(const char *id)
{
    for (int i = 0; i < soupN; i++)
        if (strcmp(soupP[i].id, id) == 0)
            return i;
    return -1;
}

static int soupEnsure(const char *id)
{
    int i = soupFind(id);
    if (i >= 0)
        return i;
    if (!id || !id[0] || soupN >= GAME_SOUP_MAX_PLAYERS)
        return -1;
    strncpy(soupP[soupN].id, id, DEVICE_ID_MAX_LEN);
    soupP[soupN].id[DEVICE_ID_MAX_LEN] = 0;
    soupN++;
    for (int a = 0; a < soupN; a++) {
        for (int b = a + 1; b < soupN; b++) {
            if (idHash16(soupP[b].id) < idHash16(soupP[a].id)) {
                SoupPlayer t = soupP[a];
                soupP[a] = soupP[b];
                soupP[b] = t;
            }
        }
    }
    return soupFind(id);
}

static bool soupIsHostId(const char *id)
{
    if (soupN <= 0 || !id)
        return false;
    uint16_t best = 0xFFFF;
    int bi = 0;
    for (int i = 0; i < soupN; i++) {
        uint16_t h = idHash16(soupP[i].id);
        if (h < best) {
            best = h;
            bi = i;
        }
    }
    return strcmp(soupP[bi].id, id) == 0;
}

static void soupRefreshHost()
{
    soupIAmHost = soupIsHostId(localDeviceId);
}

static void soupPushAns(uint8_t a)
{
    soupLastAns = a;
    if (soupAnsHistN < GAME_SOUP_ANS_HIST)
        soupAnsHist[soupAnsHistN++] = a;
    else {
        for (int i = 1; i < GAME_SOUP_ANS_HIST; i++)
            soupAnsHist[i - 1] = soupAnsHist[i];
        soupAnsHist[GAME_SOUP_ANS_HIST - 1] = a;
    }
}

static void soupDrawWrapped(int x, int y, int maxW, int lineH, const char *utf8, uint16_t fg,
                            uint16_t bg, int maxLines)
{
    if (!utf8 || maxLines <= 0)
        return;
    char line[72];
    int li = 0, lineNo = 0, w = 0;
    const char *p = utf8;
    while (*p && lineNo < maxLines) {
        unsigned char c = (unsigned char)*p;
        int nbytes = 1;
        if (c >= 0xF0)
            nbytes = 4;
        else if (c >= 0xE0)
            nbytes = 3;
        else if (c >= 0xC0)
            nbytes = 2;
        char ch[5];
        for (int i = 0; i < nbytes; i++)
            ch[i] = p[i];
        ch[nbytes] = 0;
        int cw = cnTextWidth(ch);
        if (c == '\n') {
            line[li] = 0;
            cnDrawUtf8(tft, x, y + lineNo * lineH, line, fg, bg, false);
            lineNo++;
            li = 0;
            w = 0;
            p += 1;
            continue;
        }
        if (w + cw > maxW && li > 0) {
            line[li] = 0;
            cnDrawUtf8(tft, x, y + lineNo * lineH, line, fg, bg, false);
            lineNo++;
            li = 0;
            w = 0;
            if (lineNo >= maxLines)
                break;
            continue;
        }
        if (li + nbytes < (int)sizeof(line) - 1) {
            for (int i = 0; i < nbytes; i++)
                line[li++] = *p++;
            w += cw;
        } else {
            p += nbytes;
        }
    }
    if (li > 0 && lineNo < maxLines) {
        line[li] = 0;
        cnDrawUtf8(tft, x, y + lineNo * lineH, line, fg, bg, false);
    }
}

static void soupBroadcastState()
{
    // PLAY/END: puzzle=正式题；PICK: puzzle=预览题号（仅汤面）
    int pz = (soupPhase == SOUP_PHASE_PICK) ? soupPickIdx : soupPuzzle;
    moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_STATE, (int16_t)soupPhase, (int16_t)pz,
                       (int16_t)soupLastAns, (int16_t)soupClaimSeat);
}

static void soupBroadcastPickPreview()
{
    if (!soupIAmHost || soupPhase != SOUP_PHASE_PICK)
        return;
    moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_STATE, (int16_t)SOUP_PHASE_PICK,
                       (int16_t)soupPickIdx, 0, -1);
}

static void soupResetToLobbyKeepPlayers()
{
    soupPhase = SOUP_PHASE_LOBBY;
    soupShowTruth = false;
    soupLastAns = SOUP_ANS_NONE;
    soupAnsHistN = 0;
    soupClaimSeat = -1;
    soupWinner = 0;
    soupBanner[0] = 0;
    soupRefreshHost();
    soupBroadcastState();
    soupDirty = true;
}

void soupEnter()
{
    memset(soupP, 0, sizeof(soupP));
    soupN = 0;
    soupPhase = SOUP_PHASE_LOBBY;
    soupPuzzle = 0;
    soupPickIdx = 0;
    soupDiffFilter = SOUP_DIFF_ALL;
    soupFiltPos = 0;
    soupRebuildFilter();
    soupShowTruth = false;
    soupLastAns = SOUP_ANS_NONE;
    soupAnsHistN = 0;
    soupBanner[0] = 0;
    soupClaimSeat = -1;
    soupWinner = 0;
    soupEnsure(localDeviceId);
    soupRefreshHost();
    moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_JOIN, 0, 0, 0, 0);
    soupDirty = true;
    soupDraw();
}

void soupLeave()
{
    moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_LEAVE, 0, 0, 0, 0);
    soupPhase = SOUP_PHASE_LOBBY;
    soupN = 0;
}

void soupDraw()
{
    uint16_t bg = tft.color565(12, 28, 24);
    uint16_t bar = tft.color565(20, 70, 55);
    uint16_t phaseBg = tft.color565(18, 40, 34);
    uint16_t infoBg = tft.color565(16, 34, 30);
    uint16_t actBg = tft.color565(14, 26, 24);
    tft.fillScreen(bg);
    drawMoreBackInv("海龟汤", bar);

    // 阶段条
    tft.fillRect(0, 26, SCREEN_WIDTH, 18, phaseBg);
    cnDrawUtf8(tft, 6, 29, soupPhaseShort(soupPhase), TFT_CYAN, phaseBg, false);
    cnDrawUtf8(tft, 56, 29, soupIAmHost ? "出题" : "喝汤", soupIAmHost ? TFT_YELLOW : TFT_LIGHTGREY,
               phaseBg, false);
    char nb[16];
    snprintf(nb, sizeof(nb), "%d人", soupN);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_WHITE, phaseBg);
    tft.drawString(nb, 316, 30, 1);
    tft.setTextDatum(TL_DATUM);

    // 状态条（单行）
    tft.fillRect(0, 46, SCREEN_WIDTH, 16, infoBg);
    if (soupPhase == SOUP_PHASE_PLAY && soupIAmHost && soupShowTruth) {
        cnDrawUtf8Ellipsis(tft, 6, 48, SOUP_PUZZLES[soupPuzzle].truth, TFT_ORANGE, SCREEN_WIDTH - 12,
                           infoBg, false);
    } else if (soupBanner[0]) {
        cnDrawUtf8Ellipsis(tft, 6, 48, soupBanner, TFT_YELLOW, SCREEN_WIDTH - 12, infoBg, false);
    } else if (soupPhase == SOUP_PHASE_PLAY && soupLastAns != SOUP_ANS_NONE) {
        char t[28];
        snprintf(t, sizeof(t), "回答:%s", soupAnsName(soupLastAns));
        cnDrawUtf8(tft, 6, 48, t, TFT_CYAN, infoBg, false);
    } else if (soupPhase == SOUP_PHASE_LOBBY) {
        cnDrawUtf8(tft, 6, 48, "口头提问·设备同步答案", TFT_LIGHTGREY, infoBg, false);
    }

    // 内容区上限，不侵入操作栏
    const int contentBot = SOUP_ACT_Y - 4;

    if (soupPhase == SOUP_PHASE_LOBBY) {
        cnDrawUtf8(tft, 8, 68, "出题给汤面,别人只问是/否", TFT_LIGHTGREY, bg, false);
        char bankTip[32];
        snprintf(bankTip, sizeof(bankTip), "题库%d道 可筛难度", GAME_SOUP_PUZZLE_N);
        cnDrawUtf8(tft, 8, 86, bankTip, TFT_CYAN, bg, false);
        int oy = 108;
        for (int i = 0; i < soupN && i < 4; i++) {
            if (oy + 16 > contentBot)
                break;
            char line[8];
            snprintf(line, sizeof(line), "%d.", i + 1);
            tft.setTextColor(TFT_CYAN, bg);
            tft.drawString(line, 10, oy, 1);
            bool isH = soupIsHostId(soupP[i].id);
            cnDrawUtf8Ellipsis(tft, 28, oy, soupP[i].id, isH ? TFT_YELLOW : TFT_WHITE, 200, bg,
                               false);
            if (isH)
                cnDrawUtf8(tft, 240, oy, "庄", TFT_YELLOW, bg, false);
            oy += 18;
        }
    } else if (soupPhase == SOUP_PHASE_PICK) {
        // 难度四键（在内容区内顶部）
        static const uint8_t diffs[4] = {SOUP_DIFF_ALL, SOUP_DIFF_EASY, SOUP_DIFF_MID,
                                        SOUP_DIFF_HARD};
        for (int i = 0; i < 4; i++) {
            int bx = 6 + i * 78;
            bool on = (soupDiffFilter == diffs[i]);
            tft.fillRoundRect(bx, 66, 74, 18, 3,
                              on ? tft.color565(40, 120, 90) : tft.color565(36, 48, 52));
            cnDrawUtf8(tft, bx + 10, 68, soupDiffName(diffs[i]), on ? TFT_YELLOW : TFT_LIGHTGREY);
        }
        char idx[28];
        snprintf(idx, sizeof(idx), "%d/%d [%s]", soupFiltPos + 1, soupFiltN > 0 ? soupFiltN : 1,
                 soupDiffName(SOUP_PUZZLES[soupPickIdx].diff));
        tft.setTextColor(TFT_YELLOW, bg);
        tft.drawString(idx, 8, 90, 1);
        cnDrawUtf8(tft, 8, 106, "汤面", TFT_CYAN, bg, false);
        soupDrawWrapped(8, 122, SCREEN_WIDTH - 16, 14, SOUP_PUZZLES[soupPickIdx].face, TFT_WHITE, bg,
                        soupIAmHost ? 2 : 3);
        if (soupIAmHost) {
            cnDrawUtf8(tft, 8, 154, "汤底(仅你)", tft.color565(255, 160, 60), bg, false);
            soupDrawWrapped(8, 168, SCREEN_WIDTH - 16, 12, SOUP_PUZZLES[soupPickIdx].truth,
                            TFT_LIGHTGREY, bg, 1);
        } else {
            cnDrawUtf8(tft, 8, 168, "等待出题人选汤…", TFT_YELLOW, bg, false);
        }
    } else if (soupPhase == SOUP_PHASE_PLAY) {
        cnDrawUtf8(tft, 8, 66, "汤面", TFT_CYAN, bg, false);
        soupDrawWrapped(8, 82, SCREEN_WIDTH - 16, 14, SOUP_PUZZLES[soupPuzzle].face, TFT_WHITE, bg,
                        4);
        // 最近回答条（固定区域）
        tft.fillRoundRect(6, 142, SCREEN_WIDTH - 12, 34, 4, tft.color565(18, 36, 32));
        cnDrawUtf8(tft, 12, 146, "近答", TFT_DARKGREY);
        int ax = 48;
        for (int i = 0; i < soupAnsHistN; i++) {
            const char *an = soupAnsName(soupAnsHist[i]);
            uint16_t col = TFT_YELLOW;
            if (soupAnsHist[i] == SOUP_ANS_YES)
                col = TFT_GREEN;
            else if (soupAnsHist[i] == SOUP_ANS_NO)
                col = TFT_RED;
            else if (soupAnsHist[i] == SOUP_ANS_IRREL)
                col = TFT_DARKGREY;
            cnDrawUtf8(tft, ax, 146, an, col);
            ax += cnTextWidth(an) + 8;
            if (ax > SCREEN_WIDTH - 20)
                break;
        }
        if (soupClaimSeat >= 0 && soupClaimSeat < soupN) {
            char cl[40];
            snprintf(cl, sizeof(cl), "声称:%s", soupP[soupClaimSeat].id);
            cnDrawUtf8Ellipsis(tft, 12, 162, cl, TFT_MAGENTA, SCREEN_WIDTH - 24,
                               tft.color565(18, 36, 32), false);
        } else {
            cnDrawUtf8(tft, 12, 162, soupIAmHost ? "点下方回答提问" : "口头提问·可点我猜到了",
                       TFT_LIGHTGREY);
        }
    } else {
        // REVEAL / END
        cnDrawUtf8(tft, 8, 66, "汤面", TFT_CYAN, bg, false);
        soupDrawWrapped(8, 82, SCREEN_WIDTH - 16, 13, SOUP_PUZZLES[soupPuzzle].face, TFT_LIGHTGREY,
                        bg, 2);
        cnDrawUtf8(tft, 8, 114, "汤底", tft.color565(255, 160, 60), bg, false);
        soupDrawWrapped(8, 130, SCREEN_WIDTH - 16, 13, SOUP_PUZZLES[soupPuzzle].truth, TFT_WHITE, bg,
                        3);
        if (soupPhase == SOUP_PHASE_END) {
            cnDrawUtf8(tft, 8, 172,
                       soupWinner == 1 ? "喝汤人猜中！" : "已揭晓汤底", TFT_YELLOW, bg, false);
        }
    }

    // 操作栏（独占）
    tft.fillRect(0, SOUP_ACT_Y - 2, SCREEN_WIDTH, SCREEN_HEIGHT - (SOUP_ACT_Y - 2), actBg);
    int by = SOUP_ACT_Y;
    int by2 = SOUP_ACT_Y + 28;

    if (soupPhase == SOUP_PHASE_LOBBY) {
        bool ok = soupN >= GAME_SOUP_MIN_PLAYERS && soupIAmHost;
        tft.fillRoundRect(12, by + 6, 120, 28, 4,
                          ok ? tft.color565(40, 130, 90) : tft.color565(45, 50, 55));
        cnDrawUtf8(tft, 36, by + 12, "开始出题", ok ? TFT_WHITE : TFT_DARKGREY);
        char tip[24];
        snprintf(tip, sizeof(tip), "需%d人+", GAME_SOUP_MIN_PLAYERS);
        tft.setTextColor(TFT_DARKGREY, actBg);
        tft.drawString(tip, 150, by + 14, 1);
        if (!soupIAmHost)
            cnDrawUtf8(tft, 210, by + 12, "等庄家", TFT_YELLOW);
    } else if (soupPhase == SOUP_PHASE_PICK && soupIAmHost) {
        tft.fillRoundRect(6, by + 4, 50, 24, 3, tft.color565(50, 70, 90));
        cnDrawUtf8(tft, 14, by + 8, "上题", TFT_WHITE);
        tft.fillRoundRect(60, by + 4, 50, 24, 3, tft.color565(50, 70, 90));
        cnDrawUtf8(tft, 68, by + 8, "下题", TFT_WHITE);
        tft.fillRoundRect(114, by + 4, 50, 24, 3, tft.color565(70, 60, 40));
        cnDrawUtf8(tft, 122, by + 8, "随机", TFT_WHITE);
        tft.fillRoundRect(172, by + 4, 100, 24, 3, tft.color565(40, 120, 80));
        cnDrawUtf8(tft, 188, by + 8, "确认开局", TFT_WHITE);
    } else if (soupPhase == SOUP_PHASE_PICK) {
        cnDrawUtf8(tft, 12, by + 12, "等待出题人选汤确认…", TFT_YELLOW);
    } else if (soupPhase == SOUP_PHASE_PLAY && soupIAmHost) {
        // 上排：揭晓 / 猜对 / 驳回 / 看底
        tft.fillRoundRect(4, by, 72, 24, 3, tft.color565(50, 90, 120));
        cnDrawUtf8(tft, 18, by + 4, "揭晓", TFT_WHITE);
        tft.fillRoundRect(80, by, 72, 24, 3,
                          soupClaimSeat >= 0 ? tft.color565(40, 120, 80)
                                            : tft.color565(45, 55, 50));
        cnDrawUtf8(tft, 90, by + 4, "猜对了",
                   soupClaimSeat >= 0 ? TFT_WHITE : TFT_DARKGREY);
        tft.fillRoundRect(156, by, 72, 24, 3, tft.color565(90, 50, 50));
        cnDrawUtf8(tft, 170, by + 4, "继续", TFT_WHITE);
        tft.fillRoundRect(232, by, 72, 24, 3, tft.color565(70, 55, 30));
        cnDrawUtf8(tft, 246, by + 4, soupShowTruth ? "藏底" : "看底", TFT_WHITE);
        // 下排：四答案
        tft.fillRoundRect(4, by2, 72, 24, 3, tft.color565(30, 110, 60));
        cnDrawUtf8(tft, 28, by2 + 4, "是", TFT_WHITE);
        tft.fillRoundRect(80, by2, 72, 24, 3, tft.color565(110, 40, 40));
        cnDrawUtf8(tft, 96, by2 + 4, "不是", TFT_WHITE);
        tft.fillRoundRect(156, by2, 72, 24, 3, tft.color565(70, 70, 80));
        cnDrawUtf8(tft, 172, by2 + 4, "无关", TFT_WHITE);
        tft.fillRoundRect(232, by2, 72, 24, 3, tft.color565(90, 70, 30));
        cnDrawUtf8(tft, 240, by2 + 4, "也是", TFT_WHITE);
    } else if (soupPhase == SOUP_PHASE_PLAY) {
        bool claimed = (soupClaimSeat == soupFind(localDeviceId));
        tft.fillRoundRect(20, by + 8, 140, 28, 4,
                          claimed ? tft.color565(100, 60, 40) : tft.color565(80, 50, 110));
        cnDrawUtf8(tft, 40, by + 14, claimed ? "取消声称" : "我猜到了", TFT_WHITE);
        cnDrawUtf8(tft, 180, by + 14, "等庄家答", TFT_DARKGREY);
    } else if (soupPhase == SOUP_PHASE_END || soupPhase == SOUP_PHASE_REVEAL) {
        if (soupIAmHost) {
            tft.fillRoundRect(20, by + 8, 130, 28, 4, tft.color565(40, 120, 80));
            cnDrawUtf8(tft, 44, by + 14, "再来一局", TFT_WHITE);
            tft.fillRoundRect(170, by + 8, 110, 28, 4, tft.color565(50, 70, 90));
            cnDrawUtf8(tft, 190, by + 14, "换题重选", TFT_WHITE);
        } else {
            cnDrawUtf8(tft, 40, by + 14, "等待出题人开下一局", TFT_LIGHTGREY);
        }
    }

    soupLastDraw = millis();
    soupDirty = false;
}

void soupUpdate()
{
    if (soupDirty)
        soupDraw();
}

bool soupTouch(int x, int y, bool rising)
{
    if (!rising)
        return true;
    if (hitMoreBack(x, y))
        return false;

    int by = SOUP_ACT_Y;
    int by2 = SOUP_ACT_Y + 28;

    if (soupPhase == SOUP_PHASE_LOBBY) {
        if (soupIAmHost && y >= by && x < 140 && soupN >= GAME_SOUP_MIN_PLAYERS) {
            soupPhase = SOUP_PHASE_PICK;
            soupFiltPos = 0;
            soupDiffFilter = SOUP_DIFF_ALL;
            soupRebuildFilter();
            soupShowTruth = true;
            snprintf(soupBanner, sizeof(soupBanner), "请选汤面");
            soupBroadcastState();
            soupDirty = true;
        }
        return true;
    }

    if (soupPhase == SOUP_PHASE_PICK && soupIAmHost) {
        // 难度
        if (y >= 64 && y <= 88) {
            static const uint8_t diffs[4] = {SOUP_DIFF_ALL, SOUP_DIFF_EASY, SOUP_DIFF_MID,
                                            SOUP_DIFF_HARD};
            int i = (x - 6) / 78;
            if (i >= 0 && i < 4) {
                soupDiffFilter = diffs[i];
                soupFiltPos = 0;
                soupRebuildFilter();
                soupBroadcastPickPreview();
                soupDirty = true;
            }
            return true;
        }
        if (y >= by) {
            if (x < 56) {
                soupFiltStep(-1);
                soupBroadcastPickPreview();
                soupDirty = true;
            } else if (x < 110) {
                soupFiltStep(1);
                soupBroadcastPickPreview();
                soupDirty = true;
            } else if (x < 166) {
                soupFiltRandom();
                soupBroadcastPickPreview();
                soupDirty = true;
            } else if (x >= 172) {
                soupPuzzle = soupPickIdx;
                soupPhase = SOUP_PHASE_PLAY;
                soupLastAns = SOUP_ANS_NONE;
                soupAnsHistN = 0;
                soupClaimSeat = -1;
                soupShowTruth = false;
                snprintf(soupBanner, sizeof(soupBanner), "开始喝汤·请提问");
                moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_READY, (int16_t)soupPuzzle, 0, 0, 0);
                soupBroadcastState();
                soupDirty = true;
            }
        }
        return true;
    }

    if (soupPhase == SOUP_PHASE_PLAY) {
        if (soupIAmHost) {
            if (y >= by && y < by2) {
                if (x < 76) {
                    // 揭晓
                    soupWinner = 2;
                    soupPhase = SOUP_PHASE_END;
                    soupShowTruth = true;
                    snprintf(soupBanner, sizeof(soupBanner), "已揭晓汤底");
                    moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_SCORE, 2, soupPuzzle, 0, 0);
                    soupBroadcastState();
                    soupDirty = true;
                } else if (x < 152) {
                    if (soupClaimSeat >= 0) {
                        soupWinner = 1;
                        soupPhase = SOUP_PHASE_END;
                        soupShowTruth = true;
                        snprintf(soupBanner, sizeof(soupBanner), "猜中了！");
                        moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_SCORE, 1, soupClaimSeat,
                                           soupPuzzle, 0);
                        soupBroadcastState();
                        soupDirty = true;
                    } else {
                        snprintf(soupBanner, sizeof(soupBanner), "还无人声称");
                        soupDirty = true;
                    }
                } else if (x < 228) {
                    // 继续：驳回声称
                    soupClaimSeat = -1;
                    snprintf(soupBanner, sizeof(soupBanner), "继续提问");
                    soupBroadcastState();
                    soupDirty = true;
                } else {
                    soupShowTruth = !soupShowTruth;
                    snprintf(soupBanner, sizeof(soupBanner), soupShowTruth ? "已显示汤底" : "已隐藏汤底");
                    soupDirty = true;
                }
                return true;
            }
            if (y >= by2) {
                uint8_t ans = SOUP_ANS_NONE;
                if (x < 76)
                    ans = SOUP_ANS_YES;
                else if (x < 152)
                    ans = SOUP_ANS_NO;
                else if (x < 228)
                    ans = SOUP_ANS_IRREL;
                else
                    ans = SOUP_ANS_BOTH;
                soupPushAns(ans);
                snprintf(soupBanner, sizeof(soupBanner), "回答:%s", soupAnsName(ans));
                moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_ACTION, (int16_t)ans, 0, 0, 0);
                soupBroadcastState();
                soupDirty = true;
                return true;
            }
        } else {
            if (y >= by && x < 170) {
                int me = soupFind(localDeviceId);
                if (soupClaimSeat == me) {
                    soupClaimSeat = -1;
                    snprintf(soupBanner, sizeof(soupBanner), "已取消声称");
                    moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_ACTION, 10, -1, 0, 0);
                } else {
                    soupClaimSeat = me;
                    snprintf(soupBanner, sizeof(soupBanner), "你已声称猜到");
                    moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_ACTION, 10, (int16_t)me, 0, 0);
                }
                soupDirty = true;
            }
        }
        return true;
    }

    if ((soupPhase == SOUP_PHASE_END || soupPhase == SOUP_PHASE_REVEAL) && soupIAmHost &&
        y >= by) {
        if (x < 160) {
            // 再来一局：回房间
            soupResetToLobbyKeepPlayers();
            snprintf(soupBanner, sizeof(soupBanner), "新一局·等人齐开");
            soupDirty = true;
        } else if (x >= 170) {
            // 换题重选：直接进选汤
            soupPhase = SOUP_PHASE_PICK;
            soupShowTruth = true;
            soupLastAns = SOUP_ANS_NONE;
            soupAnsHistN = 0;
            soupClaimSeat = -1;
            soupWinner = 0;
            soupRebuildFilter();
            snprintf(soupBanner, sizeof(soupBanner), "请重新选汤");
            soupBroadcastState();
            soupDirty = true;
        }
        return true;
    }
    return true;
}

void soupApplyPacket(const GamePacket_t &pkt)
{
    if (pkt.opcode == GAME_OP_JOIN) {
        soupEnsure(pkt.senderId);
        soupRefreshHost();
        if (soupPhase == SOUP_PHASE_LOBBY && pkt.v0 == 0)
            moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_JOIN, 1, (int16_t)soupN, 0, 0);
        if (soupIAmHost && soupPhase != SOUP_PHASE_LOBBY)
            soupBroadcastState();
        soupDirty = true;
        return;
    }
    if (pkt.opcode == GAME_OP_LEAVE) {
        int i = soupFind(pkt.senderId);
        if (i >= 0 && soupPhase == SOUP_PHASE_LOBBY) {
            soupP[i] = soupP[soupN - 1];
            soupN--;
            soupRefreshHost();
            soupDirty = true;
        }
        return;
    }
    if (pkt.opcode == GAME_OP_READY) {
        soupPuzzle = pkt.v0;
        if (soupPuzzle < 0 || soupPuzzle >= GAME_SOUP_PUZZLE_N)
            soupPuzzle = 0;
        soupPhase = SOUP_PHASE_PLAY;
        soupLastAns = SOUP_ANS_NONE;
        soupAnsHistN = 0;
        soupClaimSeat = -1;
        soupShowTruth = false;
        soupWinner = 0;
        snprintf(soupBanner, sizeof(soupBanner), "开始喝汤·请提问");
        soupDirty = true;
        return;
    }
    if (pkt.opcode == GAME_OP_STATE) {
        uint8_t ph = (uint8_t)pkt.v0;
        int pz = pkt.v1;
        if (pz < 0 || pz >= GAME_SOUP_PUZZLE_N)
            pz = 0;
        soupPhase = ph;
        if (ph == SOUP_PHASE_PICK) {
            soupPickIdx = pz;
            soupPuzzle = pz; // 预览用
        } else {
            soupPuzzle = pz;
        }
        if (pkt.v2 > 0 && pkt.v2 <= SOUP_ANS_BOTH)
            soupLastAns = (uint8_t)pkt.v2;
        soupClaimSeat = (int)pkt.v3;
        soupRefreshHost();
        if (ph == SOUP_PHASE_PICK)
            snprintf(soupBanner, sizeof(soupBanner), "出题人选汤中");
        else if (ph == SOUP_PHASE_PLAY && soupLastAns != SOUP_ANS_NONE)
            snprintf(soupBanner, sizeof(soupBanner), "回答:%s", soupAnsName(soupLastAns));
        else if (ph == SOUP_PHASE_LOBBY)
            soupBanner[0] = 0;
        soupDirty = true;
        return;
    }
    if (pkt.opcode == GAME_OP_ACTION) {
        if (pkt.v0 >= SOUP_ANS_YES && pkt.v0 <= SOUP_ANS_BOTH) {
            soupPushAns((uint8_t)pkt.v0);
            snprintf(soupBanner, sizeof(soupBanner), "回答:%s", soupAnsName((uint8_t)pkt.v0));
            soupDirty = true;
        } else if (pkt.v0 == 10) {
            soupClaimSeat = pkt.v1;
            if (soupClaimSeat < 0)
                snprintf(soupBanner, sizeof(soupBanner), "声称已取消");
            else
                snprintf(soupBanner, sizeof(soupBanner), "有人声称猜到了");
            soupDirty = true;
        }
        return;
    }
    if (pkt.opcode == GAME_OP_SCORE) {
        soupWinner = (uint8_t)pkt.v0;
        if (soupWinner == 1) {
            soupClaimSeat = (int)pkt.v1;
            soupPuzzle = (int)pkt.v2;
        } else {
            soupPuzzle = (int)pkt.v1;
        }
        if (soupPuzzle < 0 || soupPuzzle >= GAME_SOUP_PUZZLE_N)
            soupPuzzle = 0;
        soupPhase = SOUP_PHASE_END;
        soupShowTruth = true;
        snprintf(soupBanner, sizeof(soupBanner),
                 soupWinner == 1 ? "喝汤人猜中！" : "已揭晓汤底");
        soupDirty = true;
        return;
    }
}
