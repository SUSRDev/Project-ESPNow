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
// 狼人杀（3~12 人，无法官：全员时钟自动推进）
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
    WW_PHASE_DAY_TALK,   // 白天讨论（无发言键，仅倒计时）
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
static bool wwDirty = true;
static unsigned long wwPhaseUntil = 0;
static unsigned long wwLastDraw = 0;
static char wwBanner[40] = {0};
static uint8_t wwWinner = 0;
static bool wwHadGod = false;
static bool wwHadVill = false;
static uint8_t wwLastAdvancePhase = 0xFF;

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

static void wwBroadcastPhase()
{
    moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_STATE, (int16_t)wwPhase, (int16_t)wwDay,
                       (int16_t)wwDeathA, (int16_t)wwDeathB);
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
}

static void wwEnterPhase(uint8_t ph, unsigned long durMs);
static void wwGotoNextNightOrEnd();
static void wwBeginGame(uint32_t seed);
static void wwTryStartLocal();

static bool wwIsClockLeader()
{
    uint16_t myH = idHash16(localDeviceId);
    for (int i = 0; i < wwN; i++) {
        if (idHash16(wwP[i].id) < myH)
            return false;
    }
    return wwN > 0;
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
            wwEnterPhase(WW_PHASE_NIGHT_WITCH, 20000UL);
        else if (wwRoleAlive(WW_ROLE_SEER))
            wwEnterPhase(WW_PHASE_NIGHT_SEER, 18000UL);
        else {
            wwResolveNight();
            if (wwCheckWin()) {
                moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_SCORE, (int16_t)wwWinner, 0, 0, 0);
                wwBroadcastPhase();
            } else {
                snprintf(wwBanner, sizeof(wwBanner),
                         (wwDeathA < 0 && wwDeathB < 0) ? "昨晚是平安夜" : "昨夜有人出局");
                wwEnterPhase(WW_PHASE_DAY_ANNOUNCE, 8000UL);
            }
        }
        break;
    case WW_PHASE_NIGHT_WITCH:
        if (wwRoleAlive(WW_ROLE_SEER))
            wwEnterPhase(WW_PHASE_NIGHT_SEER, 18000UL);
        else {
            wwResolveNight();
            if (wwCheckWin()) {
                moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_SCORE, (int16_t)wwWinner, 0, 0, 0);
                wwBroadcastPhase();
            } else {
                snprintf(wwBanner, sizeof(wwBanner),
                         (wwDeathA < 0 && wwDeathB < 0) ? "昨晚是平安夜" : "昨夜有人出局");
                wwEnterPhase(WW_PHASE_DAY_ANNOUNCE, 8000UL);
            }
        }
        break;
    case WW_PHASE_NIGHT_SEER:
        wwResolveNight();
        if (wwCheckWin()) {
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_SCORE, (int16_t)wwWinner, 0, 0, 0);
            wwBroadcastPhase();
        } else {
            snprintf(wwBanner, sizeof(wwBanner),
                     (wwDeathA < 0 && wwDeathB < 0) ? "昨晚是平安夜" : "昨夜有人出局");
            wwEnterPhase(WW_PHASE_DAY_ANNOUNCE, 8000UL);
        }
        break;
    case WW_PHASE_DAY_ANNOUNCE:
        snprintf(wwBanner, sizeof(wwBanner), "自由讨论中…");
        wwEnterPhase(WW_PHASE_DAY_TALK, 30000UL);
        break;
    case WW_PHASE_DAY_TALK:
        snprintf(wwBanner, sizeof(wwBanner), "请投票放逐");
        wwEnterPhase(WW_PHASE_DAY_VOTE, 28000UL);
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
        if (best >= 0 && !tie && bestV > 0) {
            if (wwP[best].role == WW_ROLE_IDIOT && !wwP[best].idiotRevealed) {
                wwP[best].idiotRevealed = true;
                snprintf(wwBanner, sizeof(wwBanner), "%d号白痴翻牌免死", best + 1);
            } else {
                uint8_t role = wwP[best].role;
                wwKillSeat(best);
                wwDeathA = (int8_t)best;
                snprintf(wwBanner, sizeof(wwBanner), "%d号被放逐", best + 1);
                if (role == WW_ROLE_HUNTER) {
                    hunterShot = true;
                    wwEnterPhase(WW_PHASE_HUNTER, 18000UL);
                }
            }
        } else {
            snprintf(wwBanner, sizeof(wwBanner), "平票无人出局");
        }
        if (!hunterShot)
            wwGotoNextNightOrEnd();
        break;
    }
    case WW_PHASE_HUNTER:
        if (wwHunterTarget >= 0)
            wwKillSeat(wwHunterTarget);
        wwHunterTarget = -1;
        wwGotoNextNightOrEnd();
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
    if (ph == WW_PHASE_DAY_VOTE) {
        for (int i = 0; i < wwN; i++) {
            wwP[i].voted = false;
            wwP[i].voteFor = -1;
        }
    }
    if (ph == WW_PHASE_NIGHT_WOLF)
        snprintf(wwBanner, sizeof(wwBanner), "天黑请闭眼");
    if (ph == WW_PHASE_DAY_TALK)
        snprintf(wwBanner, sizeof(wwBanner), "自由讨论中…");
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
    wwEnterPhase(WW_PHASE_NIGHT_WOLF, 22000UL);
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
    snprintf(wwBanner, sizeof(wwBanner), "身份已发放");
    wwEnterPhase(WW_PHASE_NIGHT_WOLF, 22000UL);
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
    tft.fillScreen(bg);
    drawMoreBackInv("狼人杀", bar);

    // phase banner
    const char *phName = "房间";
    switch (wwPhase) {
    case WW_PHASE_NIGHT_WOLF: phName = "夜晚·狼人刀人"; break;
    case WW_PHASE_NIGHT_WITCH: phName = "夜晚·女巫用药"; break;
    case WW_PHASE_NIGHT_SEER: phName = "夜晚·预言家验人"; break;
    case WW_PHASE_DAY_ANNOUNCE: phName = "天亮了"; break;
    case WW_PHASE_DAY_TALK: phName = "白天·自由讨论"; break;
    case WW_PHASE_DAY_VOTE: phName = "白天·投票放逐"; break;
    case WW_PHASE_HUNTER: phName = "猎人开枪"; break;
    case WW_PHASE_END: phName = "游戏结束"; break;
    default: break;
    }
    tft.fillRect(0, 26, SCREEN_WIDTH, 20, tft.color565(28, 22, 40));
    cnDrawUtf8(tft, 8, 30, phName, TFT_CYAN);
    char day[20];
    long leftMs = (long)wwPhaseUntil - (long)millis();
    if (leftMs < 0)
        leftMs = 0;
    if (wwPhase != WW_PHASE_LOBBY && wwPhase != WW_PHASE_END)
        snprintf(day, sizeof(day), "D%d %lds", (int)wwDay, leftMs / 1000L);
    else
        snprintf(day, sizeof(day), "D%d", (int)wwDay);
    tft.setTextColor(TFT_YELLOW, tft.color565(28, 22, 40));
    tft.drawString(day, 230, 30, 1);

    if (wwMyRole != WW_ROLE_NONE) {
        char me[32];
        snprintf(me, sizeof(me), "%d号 ", wwMySeat + 1);
        cnDrawUtf8(tft, 8, 48, me, TFT_WHITE);
        cnDrawUtf8(tft, 40, 48, wwRoleName(wwMyRole),
                   (wwMyRole == WW_ROLE_WEREWOLF) ? TFT_RED : TFT_GREEN);
        if (wwMyRole == WW_ROLE_WEREWOLF) {
            char allies[40] = "队友:";
            bool any = false;
            for (int i = 0; i < wwN; i++) {
                if (i == wwMySeat || wwP[i].role != WW_ROLE_WEREWOLF)
                    continue;
                char bit[8];
                snprintf(bit, sizeof(bit), "%d ", i + 1);
                if (strlen(allies) + strlen(bit) < sizeof(allies) - 1)
                    strcat(allies, bit);
                any = true;
            }
            if (any)
                cnDrawUtf8(tft, 120, 48, allies, tft.color565(255, 140, 40));
        }
    }

    if (wwBanner[0])
        cnDrawUtf8(tft, 8, 64, wwBanner, TFT_YELLOW);

    // player grid 4x3
    int cols = 4;
    int cellW = 74, cellH = 36;
    int ox = 10, oy = 84;
    for (int i = 0; i < wwN; i++) {
        int c = i % cols, r = i / cols;
        int x = ox + c * (cellW + 4);
        int y = oy + r * (cellH + 4);
        uint16_t cbg = wwP[i].alive ? tft.color565(32, 38, 58) : tft.color565(40, 24, 24);
        if (!wwP[i].alive)
            cbg = tft.color565(50, 30, 30);
        tft.fillRoundRect(x, y, cellW, cellH, 5, cbg);
        tft.drawRoundRect(x, y, cellW, cellH, 5,
                          (i == wwMySeat) ? TFT_CYAN : tft.color565(70, 80, 110));
        char no[8];
        snprintf(no, sizeof(no), "%d", i + 1);
        tft.setTextColor(TFT_WHITE, cbg);
        tft.drawString(no, x + 6, y + 4, 2);
        cnDrawUtf8Ellipsis(tft, x + 4, y + 20, wwP[i].id,
                           wwP[i].alive ? TFT_LIGHTGREY : TFT_DARKGREY, cellW - 8, cbg, false);
        if (!wwP[i].alive) {
            tft.drawLine(x + 2, y + 2, x + cellW - 2, y + cellH - 2, TFT_RED);
        }
    }

    // bottom actions
    int by = SCREEN_HEIGHT - 28;
    if (wwPhase == WW_PHASE_LOBBY) {
        tft.fillRoundRect(20, by, 120, 24, 4, tft.color565(50, 120, 80));
        cnDrawUtf8(tft, 44, by + 5, "开始游戏", TFT_WHITE);
        char nbuf[28];
        snprintf(nbuf, sizeof(nbuf), "%d人(需%d+)", wwN, GAME_WW_MIN_PLAYERS);
        tft.setTextColor(TFT_LIGHTGREY, bg);
        tft.drawString(nbuf, 150, by + 4, 1);
        tft.drawString("no judge", 150, by + 14, 1);
    } else if (wwPhase == WW_PHASE_NIGHT_WITCH && wwMyRole == WW_ROLE_WITCH &&
               wwP[wwMySeat].alive) {
        if (wwHasSave && wwWolfTarget >= 0) {
            tft.fillRoundRect(8, by, 70, 24, 4, tft.color565(40, 120, 80));
            cnDrawUtf8(tft, 22, by + 5, "解药", TFT_WHITE);
        }
        if (wwHasPoison) {
            tft.fillRoundRect(90, by, 70, 24, 4, tft.color565(120, 40, 80));
            cnDrawUtf8(tft, 104, by + 5, "毒药", TFT_WHITE);
        }
        tft.fillRoundRect(170, by, 70, 24, 4, tft.color565(60, 60, 80));
        cnDrawUtf8(tft, 184, by + 5, "跳过", TFT_WHITE);
    } else if (wwPhase == WW_PHASE_END) {
        cnDrawUtf8(tft, 100, by + 4, wwWinner == 1 ? "好人阵营胜利" : "狼人阵营胜利", TFT_YELLOW);
    }

    if (wwSeerResult >= 0 && wwMyRole == WW_ROLE_SEER) {
        char tip[36];
        snprintf(tip, sizeof(tip), "验人结果:%s", wwSeerResult ? "狼人" : "好人");
        cnDrawUtf8(tft, 8, 64, tip, TFT_MAGENTA);
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
    if (wwDirty || millis() - wwLastDraw > 400UL)
        werewolfDraw();
}

bool werewolfTouch(int x, int y, bool rising)
{
    if (!rising)
        return true;
    if (hitMoreBack(x, y))
        return false;

    // start button
    if (wwPhase == WW_PHASE_LOBBY && y >= SCREEN_HEIGHT - 28) {
        if (x < 140 && wwN >= GAME_WW_MIN_PLAYERS) {
            wwTryStartLocal();
        }
        return true;
    }

    // seat tap
    int cols = 4, cellW = 74, cellH = 36, ox = 10, oy = 84;
    int seat = -1;
    for (int i = 0; i < wwN; i++) {
        int c = i % cols, r = i / cols;
        int sx = ox + c * (cellW + 4);
        int sy = oy + r * (cellH + 4);
        if (x >= sx && x <= sx + cellW && y >= sy && y <= sy + cellH) {
            seat = i;
            break;
        }
    }

    if (wwMySeat < 0 || !wwP[wwMySeat].alive) {
        // still allow idiot? no vote if dead
    }

    if (seat >= 0 && wwMySeat >= 0 && wwP[wwMySeat].alive) {
        if (wwPhase == WW_PHASE_NIGHT_WOLF && wwMyRole == WW_ROLE_WEREWOLF && wwP[seat].alive) {
            wwWolfTarget = (int8_t)seat;
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 10, seat, 0, 0);
            snprintf(wwBanner, sizeof(wwBanner), "刀 %d 号", seat + 1);
            wwDirty = true;
        } else if (wwPhase == WW_PHASE_NIGHT_SEER && wwMyRole == WW_ROLE_SEER && wwP[seat].alive) {
            wwSeerCheck = (int8_t)seat;
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 12, seat, 0,
                               (int16_t)idHash16(localDeviceId));
            snprintf(wwBanner, sizeof(wwBanner), "验 %d 号…", seat + 1);
            wwDirty = true;
        } else if (wwPhase == WW_PHASE_NIGHT_WITCH && wwMyRole == WW_ROLE_WITCH &&
                   wwHasPoison && wwP[seat].alive) {
            // select poison target then confirm via poison btn — store tentative
            wwWitchPoison = (int8_t)seat;
            snprintf(wwBanner, sizeof(wwBanner), "毒目标 %d 号", seat + 1);
            wwDirty = true;
        } else if (wwPhase == WW_PHASE_DAY_VOTE && !wwP[wwMySeat].idiotRevealed &&
                   wwP[seat].alive) {
            wwP[wwMySeat].voteFor = (int8_t)seat;
            wwP[wwMySeat].voted = true;
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 20, seat, 0, 0);
            snprintf(wwBanner, sizeof(wwBanner), "投票给 %d 号", seat + 1);
            wwDirty = true;
        } else if (wwPhase == WW_PHASE_HUNTER && wwMyRole == WW_ROLE_HUNTER && wwP[seat].alive) {
            wwHunterTarget = (int8_t)seat;
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 30, seat, 0, 0);
            wwDirty = true;
        }
    }

    // witch buttons
    if (wwPhase == WW_PHASE_NIGHT_WITCH && wwMyRole == WW_ROLE_WITCH && y >= SCREEN_HEIGHT - 28) {
        if (x < 80 && wwHasSave && wwWolfTarget >= 0) {
            wwWitchSave = 1;
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 11, 1, wwWolfTarget, 0);
            snprintf(wwBanner, sizeof(wwBanner), "已用药解");
            wwDirty = true;
        } else if (x >= 90 && x < 160 && wwHasPoison && wwWitchPoison >= 0) {
            moreSendGamePacket(GAME_KIND_WEREWOLF, GAME_OP_ACTION, 11, 2, wwWitchPoison, 0);
            snprintf(wwBanner, sizeof(wwBanner), "已下毒");
            wwDirty = true;
        } else if (x >= 170) {
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
        // 回声花名册（仅大厅，避免刷包）
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
        // 对齐阶段（他端广播）；本地已结算过则仍接受死亡标记
        uint8_t ph = (uint8_t)pkt.v0;
        wwDay = (uint8_t)pkt.v1;
        wwDeathA = (int8_t)pkt.v2;
        wwDeathB = (int8_t)pkt.v3;
        if (wwDeathA >= 0 && wwDeathA < wwN)
            wwP[wwDeathA].alive = false;
        if (wwDeathB >= 0 && wwDeathB < wwN)
            wwP[wwDeathB].alive = false;
        if (ph != wwPhase) {
            wwPhase = ph;
            // 粗同步剩余时间，避免落后端卡死
            if (ph == WW_PHASE_NIGHT_WOLF)
                wwPhaseUntil = millis() + 22000UL;
            else if (ph == WW_PHASE_NIGHT_WITCH)
                wwPhaseUntil = millis() + 20000UL;
            else if (ph == WW_PHASE_NIGHT_SEER)
                wwPhaseUntil = millis() + 18000UL;
            else if (ph == WW_PHASE_DAY_ANNOUNCE)
                wwPhaseUntil = millis() + 8000UL;
            else if (ph == WW_PHASE_DAY_TALK)
                wwPhaseUntil = millis() + 30000UL;
            else if (ph == WW_PHASE_DAY_VOTE)
                wwPhaseUntil = millis() + 28000UL;
            else if (ph == WW_PHASE_HUNTER)
                wwPhaseUntil = millis() + 18000UL;
            else
                wwPhaseUntil = millis() + 15000UL;
            wwLastAdvancePhase = 0xFF;
        }
        if (wwPhase == WW_PHASE_DAY_ANNOUNCE) {
            snprintf(wwBanner, sizeof(wwBanner),
                     (wwDeathA < 0 && wwDeathB < 0) ? "昨晚是平安夜" : "昨夜有人出局");
        } else if (wwPhase == WW_PHASE_DAY_TALK) {
            snprintf(wwBanner, sizeof(wwBanner), "自由讨论中…");
        } else if (wwPhase == WW_PHASE_NIGHT_WOLF) {
            snprintf(wwBanner, sizeof(wwBanner), "天黑请闭眼");
        }
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
        // 全员同步行动（无法官）
        if (op == 10) {
            int from = wwFind(pkt.senderId);
            if (from >= 0 && wwP[from].alive &&
                (wwP[from].role == WW_ROLE_WEREWOLF || wwP[from].role == WW_ROLE_NONE))
                wwWolfTarget = (int8_t)pkt.v1;
            // 若本机已有身份且发送者是狼
            if (from >= 0 && wwP[from].role == WW_ROLE_WEREWOLF && wwP[from].alive)
                wwWolfTarget = (int8_t)pkt.v1;
            wwDirty = true;
            return;
        }
        if (op == 11) {
            int from = wwFind(pkt.senderId);
            if (from >= 0 && (wwP[from].role == WW_ROLE_WITCH || wwMyRole == WW_ROLE_WITCH)) {
                if (pkt.v1 == 1)
                    wwWitchSave = 1;
                else if (pkt.v1 == 2)
                    wwWitchPoison = (int8_t)pkt.v2;
                else if (pkt.v1 == 0) {
                    /* skip */
                }
            }
            // 宽松：任意端广播的女巫操作都采纳（身份已由 seed 对齐）
            if (pkt.v1 == 1)
                wwWitchSave = 1;
            else if (pkt.v1 == 2)
                wwWitchPoison = (int8_t)pkt.v2;
            wwDirty = true;
            return;
        }
        if (op == 12) {
            // 被验者本机自动回阵营（无法官）
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
                    snprintf(wwBanner, sizeof(wwBanner), "验人:%s",
                             wwSeerResult ? "狼人" : "好人");
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
            wwDirty = true;
            return;
        }
    }
}

// =============================================================================
// 海龟汤（情境推理）：出题人给汤面，喝汤人只问是/否类问题，出题人答
// 是 / 不是 / 无关 / 是也不是；近距离可口头提问，设备同步答案与揭晓。
// =============================================================================
#define GAME_SOUP_MAX_PLAYERS 8
#define GAME_SOUP_PUZZLE_N SOUP_PUZZLE_N
#define GAME_SOUP_ANS_HIST 4

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

static const char *soupDiffName(uint8_t d)
{
    switch (d) {
    case SOUP_DIFF_EASY: return "简单";
    case SOUP_DIFF_MID: return "中等";
    case SOUP_DIFF_HARD: return "困难";
    default: return "全部";
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

static bool soupIAmHost = false;
static bool soupShowTruth = false;
static bool soupDirty = true;
static unsigned long soupLastDraw = 0;
static uint8_t soupLastAns = SOUP_ANS_NONE;
static uint8_t soupAnsHist[GAME_SOUP_ANS_HIST];
static uint8_t soupAnsHistN = 0;
static char soupBanner[40] = {0};
static int soupClaimSeat = -1;
static uint8_t soupWinner = 0; // 1=喝汤人猜中 2=揭晓结束

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

static void soupDrawWrapped(int x, int y, int maxW, int lineH, const char *utf8, uint16_t fg, int maxLines)
{
    if (!utf8 || maxLines <= 0)
        return;
    char line[64];
    int li = 0, lineNo = 0, w = 0;
    const char *p = utf8;
    while (*p && lineNo < maxLines) {
        unsigned char c = (unsigned char)*p;
        int nbytes = 1;
        if (c >= 0xF0) nbytes = 4;
        else if (c >= 0xE0) nbytes = 3;
        else if (c >= 0xC0) nbytes = 2;
        char ch[5];
        for (int i = 0; i < nbytes; i++) ch[i] = p[i];
        ch[nbytes] = 0;
        int cw = cnTextWidth(ch);
        if (c == '\n') {
            line[li] = 0;
            cnDrawUtf8(tft, x, y + lineNo * lineH, line, fg);
            lineNo++;
            li = 0; w = 0;
            p += 1;
            continue;
        }
        if (w + cw > maxW && li > 0) {
            line[li] = 0;
            cnDrawUtf8(tft, x, y + lineNo * lineH, line, fg);
            lineNo++;
            li = 0; w = 0;
            if (lineNo >= maxLines) break;
            continue; // retry same char
        }
        if (li + nbytes < (int)sizeof(line) - 1) {
            for (int i = 0; i < nbytes; i++) line[li++] = *p++;
            w += cw;
        } else {
            p += nbytes;
        }
    }
    if (li > 0 && lineNo < maxLines) {
        line[li] = 0;
        cnDrawUtf8(tft, x, y + lineNo * lineH, line, fg);
    }
}

static void soupBroadcastState()
{
    moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_STATE, (int16_t)soupPhase, (int16_t)soupPuzzle,
                       (int16_t)soupLastAns, (int16_t)soupClaimSeat);
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
    tft.fillScreen(bg);
    drawMoreBackInv("海龟汤", bar);

    const char *ph = "房间";
    if (soupPhase == SOUP_PHASE_PICK)
        ph = "选汤面";
    else if (soupPhase == SOUP_PHASE_PLAY)
        ph = "喝汤中";
    else if (soupPhase == SOUP_PHASE_REVEAL)
        ph = "揭晓汤底";
    else if (soupPhase == SOUP_PHASE_END)
        ph = "结束";
    tft.fillRect(0, 26, SCREEN_WIDTH, 18, tft.color565(18, 40, 34));
    cnDrawUtf8(tft, 8, 28, ph, TFT_CYAN);
    if (soupIAmHost)
        cnDrawUtf8(tft, 90, 28, "出题人", TFT_YELLOW);
    else
        cnDrawUtf8(tft, 90, 28, "喝汤人", TFT_LIGHTGREY);

    char nb[20];
    snprintf(nb, sizeof(nb), "%d人", soupN);
    tft.setTextColor(TFT_WHITE, tft.color565(18, 40, 34));
    tft.drawString(nb, 280, 30, 1);

    if (soupBanner[0])
        cnDrawUtf8(tft, 8, 46, soupBanner, TFT_YELLOW);

    int by = SCREEN_HEIGHT - 28;

    if (soupPhase == SOUP_PHASE_LOBBY) {
        cnDrawUtf8(tft, 8, 66, "规则:出题给汤面,别人提问,", TFT_LIGHTGREY);
        cnDrawUtf8(tft, 8, 82, "只答 是/不是/无关/是也不是", TFT_LIGHTGREY);
        char bankTip[28];
        snprintf(bankTip, sizeof(bankTip), "题库%d道·可设难度", GAME_SOUP_PUZZLE_N);
        cnDrawUtf8(tft, 8, 98, bankTip, TFT_CYAN);
        int oy = 116;
        for (int i = 0; i < soupN && i < 5; i++) {
            char line[28];
            snprintf(line, sizeof(line), "%d.", i + 1);
            tft.setTextColor(TFT_CYAN, bg);
            tft.drawString(line, 10, oy + i * 16, 1);
            cnDrawUtf8Ellipsis(tft, 28, oy + i * 16, soupP[i].id, TFT_WHITE, 200, bg, false);
        }
        if (soupIAmHost) {
            tft.fillRoundRect(20, by, 130, 24, 4, tft.color565(40, 130, 90));
            cnDrawUtf8(tft, 44, by + 5, "开始出题", TFT_WHITE);
        }
        char tip[32];
        snprintf(tip, sizeof(tip), "需%d人+", GAME_SOUP_MIN_PLAYERS);
        tft.setTextColor(TFT_DARKGREY, bg);
        tft.drawString(tip, 170, by + 8, 1);
    } else if (soupPhase == SOUP_PHASE_PICK) {
        // 难度筛选条
        static const uint8_t diffs[4] = {SOUP_DIFF_ALL, SOUP_DIFF_EASY, SOUP_DIFF_MID, SOUP_DIFF_HARD};
        for (int i = 0; i < 4; i++) {
            int bx = 8 + i * 76;
            bool on = (soupDiffFilter == diffs[i]);
            tft.fillRoundRect(bx, 46, 72, 18, 3,
                              on ? tft.color565(40, 120, 90) : tft.color565(40, 50, 55));
            cnDrawUtf8(tft, bx + 8, 48, soupDiffName(diffs[i]), on ? TFT_YELLOW : TFT_LIGHTGREY);
        }
        char idx[24];
        snprintf(idx, sizeof(idx), "%d/%d", soupFiltPos + 1, soupFiltN > 0 ? soupFiltN : 1);
        tft.setTextColor(TFT_YELLOW, bg);
        tft.drawString(idx, 250, 68, 1);
        cnDrawUtf8(tft, 8, 68, "汤面:", TFT_CYAN);
        char dbadge[16];
        snprintf(dbadge, sizeof(dbadge), "[%s]", soupDiffName(SOUP_PUZZLES[soupPickIdx].diff));
        cnDrawUtf8(tft, 50, 68, dbadge, tft.color565(255, 180, 80));
        soupDrawWrapped(8, 86, SCREEN_WIDTH - 16, 14, SOUP_PUZZLES[soupPickIdx].face, TFT_WHITE, 3);
        if (soupIAmHost) {
            cnDrawUtf8(tft, 8, 138, "汤底(仅你可见):", tft.color565(255, 140, 40));
            soupDrawWrapped(8, 154, SCREEN_WIDTH - 16, 13, SOUP_PUZZLES[soupPickIdx].truth,
                            TFT_LIGHTGREY, 3);
            tft.fillRoundRect(8, by, 44, 24, 4, tft.color565(50, 70, 90));
            cnDrawUtf8(tft, 14, by + 5, "上题", TFT_WHITE);
            tft.fillRoundRect(56, by, 44, 24, 4, tft.color565(50, 70, 90));
            cnDrawUtf8(tft, 62, by + 5, "下题", TFT_WHITE);
            tft.fillRoundRect(104, by, 44, 24, 4, tft.color565(70, 60, 40));
            cnDrawUtf8(tft, 110, by + 5, "随机", TFT_WHITE);
            tft.fillRoundRect(156, by, 90, 24, 4, tft.color565(40, 120, 80));
            cnDrawUtf8(tft, 168, by + 5, "确认开局", TFT_WHITE);
        } else {
            cnDrawUtf8(tft, 8, 160, "等待出题人选汤…", TFT_YELLOW);
        }
    } else if (soupPhase == SOUP_PHASE_PLAY || soupPhase == SOUP_PHASE_REVEAL ||
               soupPhase == SOUP_PHASE_END) {
        cnDrawUtf8(tft, 8, 64, "汤面:", TFT_CYAN);
        soupDrawWrapped(8, 80, SCREEN_WIDTH - 16, 14, SOUP_PUZZLES[soupPuzzle].face, TFT_WHITE, 3);

        if (soupPhase == SOUP_PHASE_PLAY) {
            cnDrawUtf8(tft, 8, 128, "最近回答:", TFT_LIGHTGREY);
            int ax = 8;
            for (int i = 0; i < soupAnsHistN; i++) {
                const char *an = soupAnsName(soupAnsHist[i]);
                uint16_t col = TFT_YELLOW;
                if (soupAnsHist[i] == SOUP_ANS_YES)
                    col = TFT_GREEN;
                else if (soupAnsHist[i] == SOUP_ANS_NO)
                    col = TFT_RED;
                else if (soupAnsHist[i] == SOUP_ANS_IRREL)
                    col = TFT_DARKGREY;
                cnDrawUtf8(tft, ax, 144, an, col);
                ax += cnTextWidth(an) + 10;
            }
            if (soupLastAns != SOUP_ANS_NONE) {
                char big[24];
                snprintf(big, sizeof(big), "→ %s", soupAnsName(soupLastAns));
                cnDrawUtf8(tft, 8, 164, big, TFT_CYAN);
            }
            if (soupClaimSeat >= 0 && soupClaimSeat < soupN) {
                char cl[36];
                snprintf(cl, sizeof(cl), "%s 声称猜到了", soupP[soupClaimSeat].id);
                cnDrawUtf8Ellipsis(tft, 8, 182, cl, TFT_MAGENTA, SCREEN_WIDTH - 16, bg, false);
            }

            if (soupIAmHost && soupShowTruth) {
                soupDrawWrapped(8, 186, SCREEN_WIDTH - 16, 12, SOUP_PUZZLES[soupPuzzle].truth,
                                TFT_DARKGREY, 2);
            }
            if (soupIAmHost) {
                // 4 answer buttons + 揭晓 / 猜对
                tft.fillRoundRect(4, by, 52, 24, 3, tft.color565(30, 110, 60));
                cnDrawUtf8(tft, 18, by + 5, "是", TFT_WHITE);
                tft.fillRoundRect(60, by, 58, 24, 3, tft.color565(110, 40, 40));
                cnDrawUtf8(tft, 70, by + 5, "不是", TFT_WHITE);
                tft.fillRoundRect(122, by, 52, 24, 3, tft.color565(70, 70, 80));
                cnDrawUtf8(tft, 132, by + 5, "无关", TFT_WHITE);
                tft.fillRoundRect(178, by, 70, 24, 3, tft.color565(90, 70, 30));
                cnDrawUtf8(tft, 182, by + 5, "是也不是", TFT_WHITE);
                // second row of host actions slightly above
                int hy = by - 26;
                tft.fillRoundRect(4, hy, 70, 22, 3, tft.color565(50, 90, 120));
                cnDrawUtf8(tft, 14, hy + 4, "揭晓", TFT_WHITE);
                tft.fillRoundRect(80, hy, 70, 22, 3, tft.color565(40, 120, 80));
                cnDrawUtf8(tft, 90, hy + 4, "猜对了", TFT_WHITE);
                tft.fillRoundRect(156, hy, 70, 22, 3, tft.color565(90, 50, 50));
                cnDrawUtf8(tft, 166, hy + 4, "未猜中", TFT_WHITE);
                tft.fillRoundRect(232, hy, 56, 22, 3, tft.color565(70, 55, 30));
                cnDrawUtf8(tft, 242, hy + 4, soupShowTruth ? "藏底" : "看底", TFT_WHITE);
            } else {
                cnDrawUtf8(tft, 8, 200, "口头提问,等出题人点答", TFT_LIGHTGREY);
                tft.fillRoundRect(20, by, 120, 24, 4, tft.color565(80, 50, 110));
                cnDrawUtf8(tft, 40, by + 5, "我猜到了", TFT_WHITE);
            }
        } else {
            // reveal / end
            cnDrawUtf8(tft, 8, 128, "汤底:", tft.color565(255, 140, 40));
            soupDrawWrapped(8, 144, SCREEN_WIDTH - 16, 13, SOUP_PUZZLES[soupPuzzle].truth, TFT_WHITE, 4);
            if (soupPhase == SOUP_PHASE_END) {
                cnDrawUtf8(tft, 8, by + 4,
                           soupWinner == 1 ? "喝汤人猜中！" : "已揭晓汤底", TFT_YELLOW);
            }
        }
    }

    soupLastDraw = millis();
    soupDirty = false;
}

void soupUpdate()
{
    if (soupDirty || millis() - soupLastDraw > 500UL)
        soupDraw();
}

bool soupTouch(int x, int y, bool rising)
{
    if (!rising)
        return true;
    if (hitMoreBack(x, y))
        return false;

    int by = SCREEN_HEIGHT - 28;

    if (soupPhase == SOUP_PHASE_LOBBY) {
        if (soupIAmHost && y >= by && x < 160 && soupN >= GAME_SOUP_MIN_PLAYERS) {
            soupPhase = SOUP_PHASE_PICK;
            soupFiltPos = 0;
            soupDiffFilter = SOUP_DIFF_ALL;
            soupRebuildFilter();
            soupShowTruth = true;
            snprintf(soupBanner, sizeof(soupBanner), "请选出题");
            soupBroadcastState();
            soupDirty = true;
        }
        return true;
    }

    if (soupPhase == SOUP_PHASE_PICK && soupIAmHost) {
        // 难度条
        if (y >= 44 && y <= 66) {
            static const uint8_t diffs[4] = {SOUP_DIFF_ALL, SOUP_DIFF_EASY, SOUP_DIFF_MID, SOUP_DIFF_HARD};
            int i = (x - 8) / 76;
            if (i >= 0 && i < 4) {
                soupDiffFilter = diffs[i];
                soupFiltPos = 0;
                soupRebuildFilter();
                soupDirty = true;
            }
            return true;
        }
        if (y >= by) {
            if (x < 52) {
                soupFiltStep(-1);
                soupDirty = true;
            } else if (x < 100) {
                soupFiltStep(1);
                soupDirty = true;
            } else if (x < 150) {
                soupFiltRandom();
                soupDirty = true;
            } else if (x >= 156) {
                soupPuzzle = soupPickIdx;
                soupPhase = SOUP_PHASE_PLAY;
                soupLastAns = SOUP_ANS_NONE;
                soupAnsHistN = 0;
                soupClaimSeat = -1;
                soupShowTruth = false;
                snprintf(soupBanner, sizeof(soupBanner), "开始喝汤");
                moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_READY, (int16_t)soupPuzzle, 0, 0, 0);
                soupBroadcastState();
                soupDirty = true;
            }
        }
        return true;
    }

    if (soupPhase == SOUP_PHASE_PLAY) {
        if (soupIAmHost) {
            int hy = by - 26;
            if (y >= hy && y < by) {
                if (x < 74) {
                    // 揭晓
                    soupPhase = SOUP_PHASE_REVEAL;
                    soupWinner = 2;
                    snprintf(soupBanner, sizeof(soupBanner), "揭晓汤底");
                    soupBroadcastState();
                    moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_SCORE, 2, soupPuzzle, 0, 0);
                    soupPhase = SOUP_PHASE_END;
                    soupDirty = true;
                } else if (x < 150) {
                    // 猜对了
                    if (soupClaimSeat >= 0) {
                        soupWinner = 1;
                        soupPhase = SOUP_PHASE_END;
                        snprintf(soupBanner, sizeof(soupBanner), "猜中了！");
                        moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_SCORE, 1, soupClaimSeat,
                                           soupPuzzle, 0);
                        soupBroadcastState();
                        soupDirty = true;
                    } else {
                        snprintf(soupBanner, sizeof(soupBanner), "还无人声称");
                        soupDirty = true;
                    }
                } else if (x < 230) {
                    soupClaimSeat = -1;
                    snprintf(soupBanner, sizeof(soupBanner), "继续提问");
                    soupBroadcastState();
                    soupDirty = true;
                } else {
                    soupShowTruth = !soupShowTruth;
                    soupDirty = true;
                }
                return true;
            }
            if (y >= by) {
                uint8_t ans = SOUP_ANS_NONE;
                if (x < 56)
                    ans = SOUP_ANS_YES;
                else if (x < 118)
                    ans = SOUP_ANS_NO;
                else if (x < 174)
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
            if (y >= by && x < 160) {
                int me = soupFind(localDeviceId);
                soupClaimSeat = me;
                snprintf(soupBanner, sizeof(soupBanner), "你已声称猜到");
                moreSendGamePacket(GAME_KIND_SOUP, GAME_OP_ACTION, 10, (int16_t)me, 0, 0);
                soupDirty = true;
            }
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
        // host echo state if already past lobby
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
        snprintf(soupBanner, sizeof(soupBanner), "开始喝汤");
        soupDirty = true;
        return;
    }
    if (pkt.opcode == GAME_OP_STATE) {
        soupPhase = (uint8_t)pkt.v0;
        soupPuzzle = pkt.v1;
        if (soupPuzzle < 0 || soupPuzzle >= GAME_SOUP_PUZZLE_N)
            soupPuzzle = 0;
        if (pkt.v2 > 0)
            soupLastAns = (uint8_t)pkt.v2;
        soupClaimSeat = (int)pkt.v3;
        soupRefreshHost();
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
        snprintf(soupBanner, sizeof(soupBanner),
                 soupWinner == 1 ? "喝汤人猜中！" : "已揭晓汤底");
        soupDirty = true;
        return;
    }
}
