#include "game_arcade.h"
#include "ui_manager.h"
#include "cn_text.h"
#include "transport_manager.h"
#include "power_manager.h"
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <esp_system.h>
#include <math.h>
#include <string.h>

extern TFT_eSPI tft;
extern char localDeviceId[DEVICE_ID_MAX_LEN + 1];
extern UIState_t currentUIState;
extern bool isScreenOn;
extern bool inCustomColorMode;

enum ArcadeView_e {
    ARCADE_LOBBY = 0,
    ARCADE_RPS,
    ARCADE_MOLE,
    ARCADE_RADAR,
    ARCADE_REACT,
    ARCADE_BRIDGE,
    ARCADE_MY_STATS,
    ARCADE_BOARD,
    ARCADE_PEER_STATS
};
typedef enum ArcadeView_e ArcadeView_t;

static ArcadeView_t arcadeView = ARCADE_LOBBY;
static bool arcadeFingerDown = false;
static uint32_t gameSeq = 1;
static int lobbyScrollY = 0;
static const int LOBBY_ITEM_H = 40;
static const int LOBBY_N = 5;
static volatile bool gameUiDirty = false;
static int statsScrollY = 0;
static char statsPeerId[DEVICE_ID_MAX_LEN + 1] = {0};
static bool matchEnded = false;
static char matchWinnerId[DEVICE_ID_MAX_LEN + 1] = {0};
static uint8_t matchKind = GAME_KIND_NONE;

// 游戏包环形队列（收包回调只入队）
static const int GAME_Q_MAX = 16;
struct GameIncoming_t {
    GamePacket_t pkt;
    int8_t rssi;
};
static GameIncoming_t gameQ[GAME_Q_MAX];
static volatile uint8_t gameQHead = 0;
static volatile uint8_t gameQTail = 0;

void enqueueGamePacket(const GamePacket_t &pkt, int8_t rssi)
{
    uint8_t next = (uint8_t)((gameQHead + 1) % GAME_Q_MAX);
    if (next == gameQTail)
        return; // full, drop
    gameQ[gameQHead].pkt = pkt;
    gameQ[gameQHead].rssi = rssi;
    gameQHead = next;
}

static void applyIncomingGamePacket(const GamePacket_t &pkt, int8_t rssi);

float estimateDistanceMeters(int8_t rssi)
{
    if (rssi == 0)
        return -1.0f;
    const float A = -48.0f;
    const float n = 2.6f;
    float d = powf(10.0f, (A - (float)rssi) / (10.0f * n));
    if (d < 0.1f)
        d = 0.1f;
    if (d > 80.0f)
        d = 80.0f;
    return d;
}

void formatEstDistance(int8_t rssi, char *buf, size_t buflen)
{
    if (!buf || buflen < 4)
        return;
    float d = estimateDistanceMeters(rssi);
    if (d < 0) {
        snprintf(buf, buflen, "--");
        return;
    }
    if (d < 10.0f)
        snprintf(buf, buflen, "%.1fm", (double)d);
    else
        snprintf(buf, buflen, "%.0fm", (double)d);
}

void sendGamePacket(uint8_t kind, uint8_t opcode, int16_t v0, int16_t v1, int16_t v2, int16_t v3)
{
    GamePacket_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = GAME_PKT_MAGIC;
    pkt.gameKind = kind;
    pkt.opcode = opcode;
    strncpy(pkt.senderId, localDeviceId, DEVICE_ID_MAX_LEN);
    pkt.senderId[DEVICE_ID_MAX_LEN] = '\0';
    pkt.v0 = v0;
    pkt.v1 = v1;
    pkt.v2 = v2;
    pkt.v3 = v3;
    pkt.seq = gameSeq++;
    pkt.timestamp = millis();
    static_assert(sizeof(GamePacket_t) != sizeof(SyncMessage_t), "Game/Sync size collide");
    static_assert(sizeof(GamePacket_t) != sizeof(ChatPacket_t), "Game/Chat size collide");
    static_assert(sizeof(GamePacket_t) != sizeof(PrivCanvasPacket_t), "Game/Priv size collide");
    transportSend((const uint8_t *)&pkt, sizeof(pkt), nullptr);
}

// ===== Persistent / peer game stats (KD) =====
struct KindStat_t {
    uint16_t wins;
    uint16_t losses;
    uint16_t games;
};
struct PlayerStats_t {
    char id[DEVICE_ID_MAX_LEN + 1];
    KindStat_t k[GAME_STAT_KIND_N];
    unsigned long lastMs;
};
static PlayerStats_t localStats;
static const int PEER_STATS_MAX = 12;
static PlayerStats_t peerStats[PEER_STATS_MAX];
static int peerStatsN = 0;
static Preferences gameStatPrefs;

static int statsKindIndex(uint8_t gameKind)
{
    switch (gameKind) {
    case GAME_KIND_RPS: return 0;
    case GAME_KIND_MOLE: return 1;
    case GAME_KIND_REACT: return 2;
    case GAME_KIND_BRIDGE: return 3;
    default: return -1;
    }
}

static uint8_t statsIndexToKind(int idx)
{
    static const uint8_t kinds[GAME_STAT_KIND_N] = {
        GAME_KIND_RPS, GAME_KIND_MOLE, GAME_KIND_REACT, GAME_KIND_BRIDGE};
    if (idx < 0 || idx >= GAME_STAT_KIND_N)
        return GAME_KIND_NONE;
    return kinds[idx];
}

static const char *statsKindShort(int idx)
{
    switch (idx) {
    case 0: return "RPS";
    case 1: return "MOLE";
    case 2: return "REACT";
    case 3: return "BRIDGE";
    default: return "?";
    }
}

static void statsClearPlayer(PlayerStats_t &p, const char *id)
{
    memset(&p, 0, sizeof(p));
    if (id) {
        strncpy(p.id, id, DEVICE_ID_MAX_LEN);
        p.id[DEVICE_ID_MAX_LEN] = '\0';
    }
}

static int peerStatsFind(const char *id)
{
    for (int i = 0; i < peerStatsN; i++) {
        if (strcmp(peerStats[i].id, id) == 0)
            return i;
    }
    return -1;
}

static int peerStatsEnsure(const char *id)
{
    int i = peerStatsFind(id);
    if (i >= 0)
        return i;
    if (!id || !id[0] || peerStatsN >= PEER_STATS_MAX)
        return -1;
    i = peerStatsN++;
    statsClearPlayer(peerStats[i], id);
    return i;
}

static PlayerStats_t *statsLookup(const char *id)
{
    if (!id || !id[0])
        return nullptr;
    if (strcmp(id, localDeviceId) == 0)
        return &localStats;
    int i = peerStatsFind(id);
    return (i >= 0) ? &peerStats[i] : nullptr;
}

static uint16_t statsTotalWins(const PlayerStats_t &p)
{
    uint16_t t = 0;
    for (int i = 0; i < GAME_STAT_KIND_N; i++)
        t = (uint16_t)(t + p.k[i].wins);
    return t;
}

static uint16_t statsTotalLosses(const PlayerStats_t &p)
{
    uint16_t t = 0;
    for (int i = 0; i < GAME_STAT_KIND_N; i++)
        t = (uint16_t)(t + p.k[i].losses);
    return t;
}

static void statsSaveLocal()
{
    if (!gameStatPrefs.begin("gamestat", false))
        return;
    for (int i = 0; i < GAME_STAT_KIND_N; i++) {
        char k[12];
        snprintf(k, sizeof(k), "w%d", i);
        gameStatPrefs.putUShort(k, localStats.k[i].wins);
        snprintf(k, sizeof(k), "l%d", i);
        gameStatPrefs.putUShort(k, localStats.k[i].losses);
        snprintf(k, sizeof(k), "g%d", i);
        gameStatPrefs.putUShort(k, localStats.k[i].games);
    }
    gameStatPrefs.end();
}

static void statsLoadLocal()
{
    statsClearPlayer(localStats, localDeviceId);
    if (!gameStatPrefs.begin("gamestat", true))
        return;
    for (int i = 0; i < GAME_STAT_KIND_N; i++) {
        char k[12];
        snprintf(k, sizeof(k), "w%d", i);
        localStats.k[i].wins = gameStatPrefs.getUShort(k, 0);
        snprintf(k, sizeof(k), "l%d", i);
        localStats.k[i].losses = gameStatPrefs.getUShort(k, 0);
        snprintf(k, sizeof(k), "g%d", i);
        localStats.k[i].games = gameStatPrefs.getUShort(k, 0);
    }
    gameStatPrefs.end();
}

static void statsBroadcast()
{
    strncpy(localStats.id, localDeviceId, DEVICE_ID_MAX_LEN);
    for (int i = 0; i < GAME_STAT_KIND_N; i++) {
        sendGamePacket(statsIndexToKind(i), GAME_OP_STATS, (int16_t)i,
                       (int16_t)localStats.k[i].wins,
                       (int16_t)localStats.k[i].losses,
                       (int16_t)localStats.k[i].games);
    }
}

static void statsApplyRemote(const char *id, int16_t kindIdx, int16_t wins, int16_t losses, int16_t games)
{
    if (!id || !id[0] || strcmp(id, localDeviceId) == 0)
        return;
    if (kindIdx < 0 || kindIdx >= GAME_STAT_KIND_N)
        return;
    int i = peerStatsEnsure(id);
    if (i < 0)
        return;
    peerStats[i].k[kindIdx].wins = (wins < 0) ? 0 : (uint16_t)wins;
    peerStats[i].k[kindIdx].losses = (losses < 0) ? 0 : (uint16_t)losses;
    peerStats[i].k[kindIdx].games = (games < 0) ? 0 : (uint16_t)games;
    peerStats[i].lastMs = millis();
}

static void statsRecordMatch(uint8_t gameKind, const char *winnerId, const char **ids, int idN)
{
    int ki = statsKindIndex(gameKind);
    if (ki < 0 || !winnerId || !winnerId[0] || idN <= 0)
        return;
    bool localPlayed = false;
    bool localWon = (strcmp(winnerId, localDeviceId) == 0);
    for (int i = 0; i < idN; i++) {
        if (ids[i] && strcmp(ids[i], localDeviceId) == 0)
            localPlayed = true;
    }
    if (localPlayed) {
        localStats.k[ki].games++;
        if (localWon)
            localStats.k[ki].wins++;
        else
            localStats.k[ki].losses++;
        statsSaveLocal();
        statsBroadcast();
    }
}

static void formatKd(char *buf, size_t n, uint16_t w, uint16_t l)
{
    if (!buf || n < 4)
        return;
    if (l == 0)
        snprintf(buf, n, w ? "%u.0" : "0.0", (unsigned)w);
    else
        snprintf(buf, n, "%.2f", (double)w / (double)l);
}

static void drawMatchEndBannerIfAny()
{
    if (!matchEnded)
        return;
    bool win = (strcmp(matchWinnerId, localDeviceId) == 0);
    uint16_t bg = win ? tft.color565(20, 80, 40) : tft.color565(80, 30, 30);
    tft.fillRoundRect(30, 70, 260, 70, 8, bg);
    tft.drawRoundRect(30, 70, 260, 70, 8, TFT_YELLOW);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_YELLOW, bg);
    tft.drawString(win ? "YOU WIN! 10pts" : "YOU LOSE", SCREEN_WIDTH / 2, 92, 2);
    tft.setTextColor(TFT_WHITE, bg);
    tft.drawString("TAP = rematch", SCREEN_WIDTH / 2, 118, 1);
    tft.setTextDatum(TL_DATUM);
}

static bool gameTryFinishMatch(uint8_t kind, const char **ids, const int16_t *scores, int n)
{
    if (matchEnded || n <= 0)
        return false;
    int best = -1;
    int16_t bestSc = -1;
    for (int i = 0; i < n; i++) {
        if (scores[i] >= GAME_WIN_SCORE && scores[i] > bestSc) {
            bestSc = scores[i];
            best = i;
        }
    }
    if (best < 0)
        return false;
    matchEnded = true;
    matchKind = kind;
    strncpy(matchWinnerId, ids[best], DEVICE_ID_MAX_LEN);
    matchWinnerId[DEVICE_ID_MAX_LEN] = '\0';
    statsRecordMatch(kind, matchWinnerId, ids, n);
    return true;
}

static void matchResetForNewGame()
{
    matchEnded = false;
    matchWinnerId[0] = '\0';
    matchKind = GAME_KIND_NONE;
}

void gameArcadeInit()
{
    arcadeView = ARCADE_LOBBY;
    arcadeFingerDown = false;
    lobbyScrollY = 0;
    gameQHead = 0;
    gameQTail = 0;
    gameUiDirty = false;
    peerStatsN = 0;
    matchResetForNewGame();
    statsLoadLocal();
}

bool gameArcadeIsActive()
{
    return currentUIState == UI_STATE_ARCADE;
}

static const int LIVE_MAX = 8;
struct LiveScore_t {
    char id[DEVICE_ID_MAX_LEN + 1];
    int16_t score;
    int16_t extra; // react ms / bridge alive
};
static LiveScore_t liveScores[LIVE_MAX];
static int liveScoreN = 0;

static int liveFind(const char *id)
{
    for (int i = 0; i < liveScoreN; i++) {
        if (strcmp(liveScores[i].id, id) == 0)
            return i;
    }
    return -1;
}

static void liveSet(const char *id, int16_t score, int16_t extra)
{
    int i = liveFind(id);
    if (i < 0) {
        if (liveScoreN >= LIVE_MAX)
            return;
        i = liveScoreN++;
        strncpy(liveScores[i].id, id, DEVICE_ID_MAX_LEN);
        liveScores[i].id[DEVICE_ID_MAX_LEN] = '\0';
    }
    liveScores[i].score = score;
    liveScores[i].extra = extra;
}

static uint16_t idHash16(const char *id)
{
    uint16_t h = 5381;
    if (!id)
        return 0;
    while (*id)
        h = (uint16_t)(((h << 5) + h) + (uint8_t)*id++);
    return h;
}

static void liveRemove(const char *id)
{
    int i = liveFind(id);
    if (i < 0)
        return;
    liveScores[i] = liveScores[liveScoreN - 1];
    liveScoreN--;
}

static void liveClear()
{
    liveScoreN = 0;
}

static void drawLiveScoresStrip(int y, uint16_t bg)
{
    tft.fillRect(0, y, SCREEN_WIDTH, 28, bg);
    tft.setTextColor(TFT_LIGHTGREY, bg);
    tft.drawString("LIVE", 4, y + 8, 1);
    int sx = 36;
    for (int i = 0; i < liveScoreN && sx < SCREEN_WIDTH - 4; i++) {
        cnDrawUtf8Ellipsis(tft, sx, y + 4, liveScores[i].id, TFT_CYAN, 52);
        char b[16];
        snprintf(b, sizeof(b), "%d", (int)liveScores[i].score);
        tft.setTextColor(TFT_GREENYELLOW, bg);
        tft.drawString(b, sx + 54, y + 4, 1);
        if (liveScores[i].extra > 0 && liveScores[i].extra < 9000) {
            snprintf(b, sizeof(b), "%dms", (int)liveScores[i].extra);
            tft.setTextColor(TFT_ORANGE, bg);
            tft.drawString(b, sx + 54, y + 14, 1);
        }
        sx += 90;
    }
}

// ===== RPS =====
enum RpsChoice_e { RPS_NONE = 0, RPS_ROCK = 1, RPS_PAPER = 2, RPS_SCISSORS = 3 };
struct RpsPlayer_t {
    char id[DEVICE_ID_MAX_LEN + 1];
    uint8_t choice;
    uint8_t score;
    unsigned long lastMs;
};
static const int RPS_MAX = 8;
static RpsPlayer_t rpsPlayers[RPS_MAX];
static int rpsCount = 0;
static uint8_t rpsMyChoice = RPS_NONE;
static bool rpsRevealed = false;
static unsigned long rpsRoundDeadline = 0;
static bool rpsSoftDirty = false;
static uint8_t rpsLastDrawnChoice = 0xFF;
static int rpsLastDrawnCount = -1;

static int rpsFind(const char *id)
{
    for (int i = 0; i < rpsCount; i++) {
        if (strcmp(rpsPlayers[i].id, id) == 0)
            return i;
    }
    return -1;
}

static int rpsEnsure(const char *id)
{
    int i = rpsFind(id);
    if (i >= 0)
        return i;
    if (rpsCount >= RPS_MAX)
        return -1;
    i = rpsCount++;
    memset(&rpsPlayers[i], 0, sizeof(rpsPlayers[i]));
    strncpy(rpsPlayers[i].id, id, DEVICE_ID_MAX_LEN);
    rpsPlayers[i].id[DEVICE_ID_MAX_LEN] = '\0';
    return i;
}

static int rpsBeats(uint8_t a, uint8_t b)
{
    if (a == RPS_NONE || b == RPS_NONE || a == b)
        return 0;
    if ((a == RPS_ROCK && b == RPS_SCISSORS) ||
        (a == RPS_PAPER && b == RPS_ROCK) ||
        (a == RPS_SCISSORS && b == RPS_PAPER))
        return 1;
    return -1;
}

static const char *rpsName(uint8_t c)
{
    switch (c) {
    case RPS_ROCK: return "ROCK";
    case RPS_PAPER: return "PAPER";
    case RPS_SCISSORS: return "SCISSORS";
    default: return "-";
    }
}

static void rpsBroadcastRoster()
{
    int me = rpsEnsure(localDeviceId);
    int16_t sc = (me >= 0) ? (int16_t)rpsPlayers[me].score : 0;
    // v0=choice v1=score v2=revealed v3=1 roster beacon
    sendGamePacket(GAME_KIND_RPS, GAME_OP_SCORE, (int16_t)rpsMyChoice, sc,
                   (int16_t)(rpsRevealed ? 1 : 0), 1);
}

static void rpsApplyPeerRoster(const char *id, int16_t choice, int16_t score, int16_t revealed)
{
    int i = rpsEnsure(id);
    if (i < 0)
        return;
    rpsPlayers[i].lastMs = millis();
    if (score >= 0)
        rpsPlayers[i].score = (uint8_t)((score > 255) ? 255 : score);
    if (choice >= RPS_NONE && choice <= RPS_SCISSORS)
        rpsPlayers[i].choice = (uint8_t)choice;
    (void)revealed;
    rpsSoftDirty = true;
}

static void rpsResetRound()
{
    if (matchEnded)
        return;
    rpsMyChoice = RPS_NONE;
    rpsRevealed = false;
    rpsRoundDeadline = millis() + GAME_RPS_ROUND_MS;
    for (int i = 0; i < rpsCount; i++)
        rpsPlayers[i].choice = RPS_NONE;
    int me = rpsEnsure(localDeviceId);
    if (me >= 0)
        rpsPlayers[me].lastMs = millis();
    rpsSoftDirty = true;
    rpsLastDrawnChoice = 0xFF;
    rpsBroadcastRoster();
}

static void rpsCheckWin()
{
    if (matchEnded || rpsCount < 2)
        return;
    const char *ids[RPS_MAX];
    int16_t sc[RPS_MAX];
    for (int i = 0; i < rpsCount; i++) {
        ids[i] = rpsPlayers[i].id;
        sc[i] = (int16_t)rpsPlayers[i].score;
    }
    if (gameTryFinishMatch(GAME_KIND_RPS, ids, sc, rpsCount))
        rpsSoftDirty = true;
}

static void rpsTryReveal()
{
    if (rpsRevealed || matchEnded)
        return;
    if (rpsCount < 2)
        return;
    int readyN = 0;
    for (int i = 0; i < rpsCount; i++) {
        if (rpsPlayers[i].choice != RPS_NONE)
            readyN++;
    }
    bool timeUp = (millis() > rpsRoundDeadline);
    bool allReady = (readyN >= rpsCount);
    // 全员出拳，或 30s 超时（未出拳算输）
    if (!allReady && !timeUp)
        return;
    if (!timeUp && readyN < 2)
        return;

    rpsRevealed = true;
    for (int i = 0; i < rpsCount; i++) {
        for (int j = i + 1; j < rpsCount; j++) {
            uint8_t a = rpsPlayers[i].choice;
            uint8_t b = rpsPlayers[j].choice;
            if (a != RPS_NONE && b != RPS_NONE) {
                int r = rpsBeats(a, b);
                if (r > 0)
                    rpsPlayers[i].score++;
                else if (r < 0)
                    rpsPlayers[j].score++;
            } else if (timeUp) {
                // 超时未出拳：对方+分
                if (a != RPS_NONE && b == RPS_NONE)
                    rpsPlayers[i].score++;
                else if (b != RPS_NONE && a == RPS_NONE)
                    rpsPlayers[j].score++;
            }
        }
    }
    rpsSoftDirty = true;
    rpsBroadcastRoster();
    rpsCheckWin();
}

// ===== Mole =====
static const int MOLE_CELLS = 9;
static uint8_t moleActive = 0;
static uint32_t moleSeed = 1;
static unsigned long moleNextFlip = 0;
static bool moleIsHost = false;
static int moleMyScore = 0;
struct MoleScore_t {
    char id[DEVICE_ID_MAX_LEN + 1];
    int16_t score;
};
static MoleScore_t moleScores[RPS_MAX];
static int moleScoreN = 0;
static bool moleFrameReady = false;
static uint8_t moleDrawnActive = 0xFE;
static int moleDrawnMyScore = -999;
static int moleDrawnScoreN = -1;
static int moleDrawnWait = -1;
static uint16_t moleScoreSig = 0;

static int moleFindScore(const char *id)
{
    for (int i = 0; i < moleScoreN; i++) {
        if (strcmp(moleScores[i].id, id) == 0)
            return i;
    }
    return -1;
}

static void moleSetScore(const char *id, int16_t sc)
{
    int i = moleFindScore(id);
    if (i < 0) {
        if (moleScoreN >= RPS_MAX)
            return;
        i = moleScoreN++;
        strncpy(moleScores[i].id, id, DEVICE_ID_MAX_LEN);
        moleScores[i].id[DEVICE_ID_MAX_LEN] = '\0';
    }
    moleScores[i].score = sc;
}

static uint32_t moleRand()
{
    moleSeed ^= moleSeed << 13;
    moleSeed ^= moleSeed >> 17;
    moleSeed ^= moleSeed << 5;
    if (moleSeed == 0)
        moleSeed = 0xA5A5u;
    return moleSeed;
}

static void moleCheckWin()
{
    if (matchEnded || moleScoreN < 2)
        return;
    const char *ids[RPS_MAX];
    int16_t sc[RPS_MAX];
    for (int i = 0; i < moleScoreN; i++) {
        ids[i] = moleScores[i].id;
        sc[i] = moleScores[i].score;
    }
    if (gameTryFinishMatch(GAME_KIND_MOLE, ids, sc, moleScoreN))
        gameUiDirty = true;
}

static void moleAdvance()
{
    if (matchEnded)
        return;
    moleActive = (uint8_t)(moleRand() % MOLE_CELLS);
    moleNextFlip = millis() + 700UL + (moleRand() % 600UL);
    if (moleIsHost) {
        sendGamePacket(GAME_KIND_MOLE, GAME_OP_STATE, (int16_t)moleActive,
                       (int16_t)(moleSeed & 0xFFFF), (int16_t)moleMyScore, 1);
    }
}


// ===== Radar =====
static float radarSweepDeg = 0.0f;
static float radarPrevSweepDeg = -1.0f;
static unsigned long radarLastBlipMs = 0;
static unsigned long radarLastPanelMs = 0;
static unsigned long radarLastLayoutMs = 0;
static unsigned long radarLastLinkTxMs = 0;
static bool radarFrameReady = false;
static bool radarCalibrating = false;
static unsigned long radarCalibStart = 0;
static const int RADAR_CX = 112;
static const int RADAR_CY = 126;
static const int RADAR_R = 88;
static const float RADAR_RANGE_M = 20.0f;

// 方位约定：0°=屏幕上方(设备正前方)，顺时针；屏坐标 bx=cx+sin*r, by=cy-cos*r
static const int RADAR_CAL_BINS = 36;          // 10°/格
static const unsigned long RADAR_CAL_MS = 10000UL; // 慢转 10 秒一圈

struct RadarContact_t {
    char id[DEVICE_ID_MAX_LEN + 1];
    int8_t rssi;
    uint16_t latencyMs;
    float distM;       // displayed (smoothed) distance
    float targetDistM; // latest RSSI estimate
    float angleDeg;    // 0=up/forward, CW
    float posX, posY;  // +X=right, +Y=forward (meters)
    unsigned long lastMs;
    bool azValid;
    bool azLocked; // CAL 后锁定，不被弹簧布局改写
    uint8_t azConf; // 0-100 方位置信度
    int8_t rssiMin, rssiMax; // 会话内波动
    int16_t calibSum[RADAR_CAL_BINS];
    uint8_t calibN[RADAR_CAL_BINS];
    int lastBx, lastBy;
};
static const int RADAR_MAX = 8;
static RadarContact_t radarContacts[RADAR_MAX];
static int radarCount = 0;
static int radarFocusIdx = 0; // 点选设定方位的目标

// Cross-link RSSI: observer -> target (by contact index); -128 = unknown
static int8_t radarLink[RADAR_MAX][RADAR_MAX];

static int8_t radarFreshRssi(const char *id, int8_t fallback)
{
    if (!id || !id[0])
        return fallback;
    for (auto const &kv : peerInfoMap) {
        const PeerInfo_t &p = kv.second;
        if (p.deviceId[0] && strcmp(p.deviceId, id) == 0 && p.rssi != 0)
            return p.rssi;
    }
    return fallback;
}

static void radarSetAz(int i, float angDeg, uint8_t conf, bool lock)
{
    if (i < 0 || i >= radarCount)
        return;
    while (angDeg < 0)
        angDeg += 360.0f;
    while (angDeg >= 360.0f)
        angDeg -= 360.0f;
    radarContacts[i].angleDeg = angDeg;
    radarContacts[i].azValid = true;
    radarContacts[i].azLocked = lock;
    radarContacts[i].azConf = conf;
    float a = angDeg * 0.01745329252f;
    float d = radarContacts[i].distM > 0.1f ? radarContacts[i].distM : 1.0f;
    radarContacts[i].posX = sinf(a) * d;
    radarContacts[i].posY = cosf(a) * d;
}

static int radarFind(const char *id)
{
    for (int i = 0; i < radarCount; i++) {
        if (strcmp(radarContacts[i].id, id) == 0)
            return i;
    }
    return -1;
}

static void radarUpsert(const char *id, int8_t rssi, uint16_t latMs)
{
    if (!id || !id[0])
        return;
    int i = radarFind(id);
    if (i < 0) {
        if (radarCount >= RADAR_MAX)
            return;
        i = radarCount++;
        memset(&radarContacts[i], 0, sizeof(radarContacts[i]));
        strncpy(radarContacts[i].id, id, DEVICE_ID_MAX_LEN);
        radarContacts[i].id[DEVICE_ID_MAX_LEN] = '\0';
        // 临时方位用 id hash，避免永远钉在正前方(0°)
        radarContacts[i].angleDeg = (float)(idHash16(id) % 360);
        radarContacts[i].azValid = false;
        radarContacts[i].azLocked = false;
        radarContacts[i].azConf = 0;
        radarContacts[i].rssiMin = 0;
        radarContacts[i].rssiMax = -127;
        radarContacts[i].lastBx = -1;
        for (int b = 0; b < RADAR_CAL_BINS; b++) {
            radarContacts[i].calibSum[b] = 0;
            radarContacts[i].calibN[b] = 0;
        }
        for (int j = 0; j < RADAR_MAX; j++) {
            radarLink[i][j] = -128;
            radarLink[j][i] = -128;
        }
    }
    // EMA rssi → 目标距离；显示距离在 update 里向目标平滑滑动
    if (radarContacts[i].rssi == 0)
        radarContacts[i].rssi = rssi;
    else
        radarContacts[i].rssi = (int8_t)((radarContacts[i].rssi * 2 + rssi) / 3);
    radarContacts[i].latencyMs = latMs;
    float d = estimateDistanceMeters(radarContacts[i].rssi);
    if (d < 0)
        d = 15.0f;
    radarContacts[i].targetDistM = d;
    if (radarContacts[i].distM <= 0.01f)
        radarContacts[i].distM = d;
    if (radarContacts[i].rssiMin == 0 || rssi < radarContacts[i].rssiMin)
        radarContacts[i].rssiMin = radarContacts[i].rssi;
    if (rssi > radarContacts[i].rssiMax)
        radarContacts[i].rssiMax = radarContacts[i].rssi;
    radarContacts[i].lastMs = millis();
}

static void radarSetLink(const char *observerId, const char *targetId, int8_t rssi)
{
    int o = radarFind(observerId);
    int t = radarFind(targetId);
    if (t < 0 && targetId && targetId[0]) {
        radarUpsert(targetId, rssi ? rssi : (int8_t)-75, 0);
        t = radarFind(targetId);
    }
    if (o < 0 || t < 0 || o == t)
        return;
    radarLink[o][t] = rssi;
}

static float radarDistPair(int a, int b)
{
    if (a == b)
        return 0.0f;
    // prefer cross-link if known
    if (a >= 0 && b >= 0 && a < radarCount && b < radarCount) {
        int8_t l1 = radarLink[a][b];
        int8_t l2 = radarLink[b][a];
        int8_t use = (l1 != -128) ? l1 : l2;
        if (use != -128) {
            float d = estimateDistanceMeters(use);
            if (d > 0)
                return d;
        }
    }
    return -1.0f;
}

static const char *radarCardinal(float az)
{
    // 0=N, 45=NE, ...
    static const char *names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int idx = (int)((az + 22.5f) / 45.0f) % 8;
    if (idx < 0)
        idx += 8;
    return names[idx];
}

// 两圆交点：圆心 O(0,0) r=r0，圆心 C(cx,cy) r=r1；选与 hint 角更近的解
static bool radarCircleIntersect(float cx, float cy, float r0, float r1,
                                 float hintAngDeg, float &outX, float &outY)
{
    float d = sqrtf(cx * cx + cy * cy);
    if (d < 0.05f || r0 < 0.05f || r1 < 0.05f)
        return false;
    if (d > r0 + r1 + 0.3f || d < fabsf(r0 - r1) - 0.3f)
        return false;
    float a = (r0 * r0 - r1 * r1 + d * d) / (2.0f * d);
    float h2 = r0 * r0 - a * a;
    if (h2 < 0)
        h2 = 0;
    float h = sqrtf(h2);
    float ux = cx / d, uy = cy / d;
    float px = ux * a, py = uy * a;
    float x1 = px + (-uy) * h, y1 = py + ux * h;
    float x2 = px - (-uy) * h, y2 = py - ux * h;
    float ha = hintAngDeg * 0.01745329252f;
    float hx = sinf(ha) * r0, hy = cosf(ha) * r0;
    float d1 = (x1 - hx) * (x1 - hx) + (y1 - hy) * (y1 - hy);
    float d2 = (x2 - hx) * (x2 - hx) + (y2 - hy) * (y2 - hy);
    if (d1 <= d2) {
        outX = x1;
        outY = y1;
    } else {
        outX = x2;
        outY = y2;
    }
    return true;
}

// Relative map: self at origin; +Y forward / +X right; locked AZ keeps bearing
static void radarRelayout()
{
    if (radarCount <= 0)
        return;

    int lockedN = 0;
    for (int i = 0; i < radarCount; i++) {
        if (radarContacts[i].azLocked)
            lockedN++;
    }

    // 未锁定：有锚点则用互距求交，否则均分槽位
    for (int i = 0; i < radarCount; i++) {
        float want = radarContacts[i].distM;
        if (want < 0.1f)
            want = 0.1f;

        if (radarContacts[i].azLocked) {
            float a = radarContacts[i].angleDeg * 0.01745329252f;
            radarContacts[i].posX = sinf(a) * want;
            radarContacts[i].posY = cosf(a) * want;
            continue;
        }

        bool placed = false;
        if (lockedN > 0) {
            for (int j = 0; j < radarCount; j++) {
                if (!radarContacts[j].azLocked)
                    continue;
                float linkD = radarDistPair(i, j);
                if (linkD < 0)
                    continue;
                float ox, oy;
                if (radarCircleIntersect(radarContacts[j].posX, radarContacts[j].posY,
                                         want, linkD, radarContacts[i].angleDeg, ox, oy)) {
                    radarContacts[i].posX = ox;
                    radarContacts[i].posY = oy;
                    placed = true;
                    // 软锁定：由锚点推得的方位标 valid，置信度中等
                    float ang = atan2f(ox, oy) * 57.2957795f;
                    if (ang < 0)
                        ang += 360.0f;
                    radarContacts[i].angleDeg = ang;
                    radarContacts[i].azValid = true;
                    if (radarContacts[i].azConf < 55)
                        radarContacts[i].azConf = 55;
                    break;
                }
            }
        }
        if (!placed) {
            // 未锁定：只按当前 angle 更新半径，绝不强行拉回 0°/正前方
            float a = radarContacts[i].angleDeg * 0.01745329252f;
            radarContacts[i].posX = sinf(a) * want;
            radarContacts[i].posY = cosf(a) * want;
        }
    }

    // 弹簧微调未锁定节点（有互距时）
    for (int iter = 0; iter < 8; iter++) {
        for (int i = 0; i < radarCount; i++) {
            if (radarContacts[i].azLocked)
                continue;
            float want = radarContacts[i].distM;
            float dx = radarContacts[i].posX;
            float dy = radarContacts[i].posY;
            float cur = sqrtf(dx * dx + dy * dy);
            if (cur < 0.05f)
                continue;
            float scale = want / cur;
            radarContacts[i].posX *= (0.85f + 0.15f * scale);
            radarContacts[i].posY *= (0.85f + 0.15f * scale);
        }
        for (int i = 0; i < radarCount; i++) {
            for (int j = i + 1; j < radarCount; j++) {
                if (radarContacts[i].azLocked && radarContacts[j].azLocked)
                    continue;
                float want = radarDistPair(i, j);
                if (want < 0)
                    continue;
                float dx = radarContacts[j].posX - radarContacts[i].posX;
                float dy = radarContacts[j].posY - radarContacts[i].posY;
                float cur = sqrtf(dx * dx + dy * dy);
                if (cur < 0.05f) {
                    dx = 0.1f;
                    dy = 0.0f;
                    cur = 0.1f;
                }
                float err = (want - cur) * 0.1f;
                float ux = dx / cur, uy = dy / cur;
                if (!radarContacts[i].azLocked) {
                    radarContacts[i].posX -= ux * err * 0.5f;
                    radarContacts[i].posY -= uy * err * 0.5f;
                }
                if (!radarContacts[j].azLocked) {
                    radarContacts[j].posX += ux * err * 0.5f;
                    radarContacts[j].posY += uy * err * 0.5f;
                }
            }
        }
    }

    // 仅当未锁定且有互距几何时，才从 pos 回写 angle
    for (int i = 0; i < radarCount; i++) {
        if (radarContacts[i].azLocked)
            continue;
        if (lockedN == 0)
            continue; // 无锚点时保持原 angle，避免被算回 0°
        float ang = atan2f(radarContacts[i].posX, radarContacts[i].posY) * 57.2957795f;
        if (ang < 0)
            ang += 360.0f;
        float prev = radarContacts[i].angleDeg;
        float diff = ang - prev;
        if (diff > 180)
            diff -= 360;
        if (diff < -180)
            diff += 360;
        radarContacts[i].angleDeg = prev + diff * 0.3f;
        if (radarContacts[i].angleDeg < 0)
            radarContacts[i].angleDeg += 360.0f;
        if (radarContacts[i].angleDeg >= 360.0f)
            radarContacts[i].angleDeg -= 360.0f;
    }
}

static void radarSyncFromPeers()
{
    unsigned long now = millis();
    for (auto const &kv : peerInfoMap) {
        const PeerInfo_t &p = kv.second;
        if (!peerVisibleForLocalMode(p))
            continue;
        if (!p.deviceId[0])
            continue;
        if (strcmp(p.deviceId, localDeviceId) == 0)
            continue;
        unsigned long age = (p.lastSeenMs && now >= p.lastSeenMs) ? (now - p.lastSeenMs) : 9999UL;
        int8_t rssi = p.rssi ? p.rssi : (int8_t)-75;
        radarUpsert(p.deviceId, rssi, (uint16_t)min(9999UL, age ? age : (unsigned long)p.latencyMs));
    }
    for (int i = 0; i < radarCount;) {
        if (now - radarContacts[i].lastMs > 20000UL) {
            // compact remove
            for (int r = 0; r < RADAR_MAX; r++) {
                radarLink[i][r] = radarLink[radarCount - 1][r];
                radarLink[r][i] = radarLink[r][radarCount - 1];
            }
            radarContacts[i] = radarContacts[radarCount - 1];
            radarCount--;
            if (radarFocusIdx >= radarCount)
                radarFocusIdx = radarCount ? radarCount - 1 : 0;
        } else {
            i++;
        }
    }
    if (!radarCalibrating)
        radarRelayout();
}

static void radarBroadcastLinks()
{
    // Tell others our RSSI to each contact (for their relative map)
    int n = min(radarCount, 4);
    for (int i = 0; i < n; i++) {
        sendGamePacket(GAME_KIND_RADAR, GAME_OP_LINK,
                       (int16_t)idHash16(radarContacts[i].id),
                       (int16_t)radarContacts[i].rssi, 0, 0);
    }
    sendGamePacket(GAME_KIND_RADAR, GAME_OP_PING, (int16_t)radarCount, 0, 0, 0);
}

// ===== Reaction =====
enum ReactPhase_e { REACT_IDLE = 0, REACT_WAIT, REACT_GO, REACT_DONE };
static ReactPhase_e reactPhase = REACT_IDLE;
static unsigned long reactGoAt = 0;
static unsigned long reactGoShownAt = 0;
static int16_t reactMyMs = -1; // -1 none, -2 foul
static int reactMyScore = 0;
static bool reactIsHost = false;
static unsigned long reactRoundId = 0;
static bool reactTapped = false;

static void reactStartWait()
{
    reactPhase = REACT_WAIT;
    reactMyMs = -1;
    reactTapped = false;
    reactGoAt = millis() + 1200UL + (esp_random() % 2800UL);
    reactRoundId = millis();
    sendGamePacket(GAME_KIND_REACT, GAME_OP_STATE, (int16_t)REACT_WAIT,
                   (int16_t)(reactGoAt - millis()), (int16_t)(reactRoundId & 0xFFFF),
                   (int16_t)((reactRoundId >> 16) & 0xFFFF));
}

// ===== Bridge (Stick Hero) =====
enum BridgePhase_e {
    BRIDGE_AIM = 0,    // hold to grow stick
    BRIDGE_FALL,       // stick falling
    BRIDGE_WALK,       // dino walking
    BRIDGE_OVER        // game over
};
static BridgePhase_e bridgePhase = BRIDGE_AIM;
static int bridgeScore = 0;
static int platL = 20;      // left platform left x
static int platLW = 56;     // left width
static int platR = 140;     // right platform left x
static int platRW = 50;     // right width
static int stickLen = 0;
static bool stickGrowing = false;
static unsigned long stickGrowLast = 0;
static float stickAngle = 90.0f; // 90=up, 0=flat
static float dinoX = 0;
static int groundY = 175;
static uint32_t bridgeSeed = 1;
static bool bridgeDirty = true;

static uint32_t bridgeRand()
{
    bridgeSeed ^= bridgeSeed << 13;
    bridgeSeed ^= bridgeSeed >> 17;
    bridgeSeed ^= bridgeSeed << 5;
    if (!bridgeSeed)
        bridgeSeed = 1;
    return bridgeSeed;
}

static void bridgeGenNext(bool first)
{
    if (first) {
        platL = 8;
        platLW = 50 + (int)(bridgeRand() % 25);
        platR = platL + platLW + 35 + (int)(bridgeRand() % 70);
        platRW = 36 + (int)(bridgeRand() % 40);
    } else {
        // shift: old right becomes new left
        platL = 8;
        platLW = platRW;
        int gap = 28 + (int)(bridgeRand() % 75);
        platRW = 32 + (int)(bridgeRand() % 45);
        platR = platL + platLW + gap;
        if (platR + platRW > SCREEN_WIDTH - 8) {
            platRW = SCREEN_WIDTH - 8 - platR;
            if (platRW < 28)
                platRW = 28;
        }
    }
    stickLen = 0;
    stickAngle = 90.0f;
    stickGrowing = false;
    dinoX = (float)(platL + platLW - 14);
    bridgePhase = BRIDGE_AIM;
    bridgeDirty = true;
    // Sync layout for multiplayer fairness: v0=platLW, v1=gap, v2=platRW, v3=score
    int gap = platR - (platL + platLW);
    sendGamePacket(GAME_KIND_BRIDGE, GAME_OP_STATE, (int16_t)platLW, (int16_t)gap,
                   (int16_t)platRW, (int16_t)bridgeScore);
}

static void bridgeApplyLayout(int16_t lw, int16_t gap, int16_t rw, int16_t score)
{
    platL = 8;
    platLW = lw;
    platR = platL + platLW + gap;
    platRW = rw;
    bridgeScore = score;
    stickLen = 0;
    stickAngle = 90.0f;
    stickGrowing = false;
    dinoX = (float)(platL + platLW - 14);
    bridgePhase = BRIDGE_AIM;
    bridgeDirty = true;
    liveSet(localDeviceId, (int16_t)bridgeScore, 1);
}

static void bridgeRestart()
{
    bridgeSeed = millis() ^ esp_random();
    bridgeScore = 0;
    liveSet(localDeviceId, 0, 1);
    sendGamePacket(GAME_KIND_BRIDGE, GAME_OP_SCORE, 0, 0, 0, 0);
    bridgeGenNext(true);
}

// ===== UI helpers =====
static void drawBackChip()
{
    tft.fillRoundRect(SCREEN_WIDTH - 54, 4, 50, 18, 3, tft.color565(50, 60, 80));
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, tft.color565(50, 60, 80));
    tft.drawString("BACK", SCREEN_WIDTH - 29, 13, 1);
    tft.setTextDatum(TL_DATUM);
}

static bool hitBack(int x, int y)
{
    return x >= SCREEN_WIDTH - 54 && x <= SCREEN_WIDTH - 4 && y >= 4 && y <= 22;
}

void drawGameJoinButton()
{
    if (!isScreenOn || inCustomColorMode)
        return;
    tft.fillRect(GAME_BUTTON_X, GAME_BUTTON_Y, GAME_BUTTON_W, GAME_BUTTON_H, tft.color565(180, 40, 90));
    cnDrawUtf8(tft, GAME_BUTTON_X + 2, GAME_BUTTON_Y + 2, "娱", TFT_WHITE);
}

bool isGameJoinButtonPressed(int x, int y)
{
    return x >= GAME_BUTTON_X && x <= GAME_BUTTON_X + GAME_BUTTON_W &&
           y >= GAME_BUTTON_Y && y <= GAME_BUTTON_Y + GAME_BUTTON_H;
}

static void drawRpsScreen();
static void drawRpsDynamic(bool force);
static void drawMoleScreen();
static void drawMoleDynamic(bool force);
static void drawRadarScreen(bool full);
static void drawReactScreen();
static void drawBridgeScreen();
static void redrawArcade();
static void leaveCurrentGame();
static void updateRecruitToast();
static void drawRecruitToast();
static bool hitInvChip(int x, int y);
static uint8_t arcadeRecruitKind();

void drawGameLobby()
{
    tft.fillScreen(tft.color565(10, 12, 20));
    tft.fillRect(0, 0, SCREEN_WIDTH, 26, tft.color565(24, 32, 52));
    cnDrawUtf8(tft, 8, 7, "娱乐大厅", TFT_WHITE);

    // 我的 / 榜
    tft.fillRoundRect(148, 4, 44, 18, 3, tft.color565(60, 90, 50));
    cnDrawUtf8(tft, 154, 7, "我的", TFT_YELLOW);
    tft.fillRoundRect(196, 4, 36, 18, 3, tft.color565(70, 60, 110));
    cnDrawUtf8(tft, 204, 7, "榜", TFT_CYAN);
    drawBackChip();

    struct Item {
        const char *en;
        const char *cn;
        uint16_t accent;
    };
    const Item items[LOBBY_N] = {
        {"MULTI WHACK-A-MOLE", "多人打地鼠", tft.color565(40, 160, 90)},
        {"MULTI RPS ARENA", "多人剪刀石头布", tft.color565(40, 120, 200)},
        {"RF DEVICE RADAR", "设备雷达", tft.color565(200, 80, 40)},
        {"REACTION RACE", "比反应", tft.color565(220, 160, 40)},
        {"DINO BRIDGE", "恐龙搭桥", tft.color565(120, 80, 200)},
    };
    int listTop = 28;
    int viewH = SCREEN_HEIGHT - listTop;
    int maxScroll = LOBBY_N * LOBBY_ITEM_H - viewH;
    if (maxScroll < 0)
        maxScroll = 0;
    if (lobbyScrollY > maxScroll)
        lobbyScrollY = maxScroll;
    if (lobbyScrollY < 0)
        lobbyScrollY = 0;

    for (int i = 0; i < LOBBY_N; i++) {
        int y = listTop + i * LOBBY_ITEM_H - lobbyScrollY;
        if (y + LOBBY_ITEM_H < listTop || y > SCREEN_HEIGHT)
            continue;
        tft.fillRoundRect(10, y + 2, SCREEN_WIDTH - 20, LOBBY_ITEM_H - 4, 5, tft.color565(22, 28, 42));
        tft.drawRoundRect(10, y + 2, SCREEN_WIDTH - 20, LOBBY_ITEM_H - 4, 5, items[i].accent);
        tft.fillRoundRect(16, y + 8, 6, LOBBY_ITEM_H - 16, 2, items[i].accent);
        tft.setTextColor(TFT_WHITE, tft.color565(22, 28, 42));
        tft.setTextDatum(TL_DATUM);
        tft.drawString(items[i].en, 30, y + 6, 1);
        cnDrawUtf8(tft, 30, y + 20, items[i].cn, TFT_LIGHTGREY);
        tft.fillRoundRect(SCREEN_WIDTH - 78, y + 10, 36, 18, 3, items[i].accent);
        tft.setTextColor(TFT_BLACK, items[i].accent);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("INV", SCREEN_WIDTH - 60, y + 19, 1);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(items[i].accent, tft.color565(22, 28, 42));
        tft.drawString(">", SCREEN_WIDTH - 28, y + 14, 2);
    }
    overlayGameInviteIfAny();
}

static void drawStatsProfile(const char *id, bool showPeersHint)
{
    tft.fillScreen(tft.color565(12, 14, 22));
    tft.fillRect(0, 0, SCREEN_WIDTH, 26, tft.color565(30, 40, 60));
    cnDrawUtf8(tft, 8, 7, showPeersHint ? "我的战绩" : "战绩", TFT_WHITE);
    drawBackChip();

    PlayerStats_t *ps = statsLookup(id);
    PlayerStats_t tmp;
    if (!ps) {
        statsClearPlayer(tmp, id);
        ps = &tmp;
    }

    int tw = cnTextWidth(id);
    cnDrawUtf8(tft, (SCREEN_WIDTH - tw) / 2, 32, id, TFT_CYAN);

    char line[48];
    char kd[12];
    uint16_t tw_ = statsTotalWins(*ps);
    uint16_t tl = statsTotalLosses(*ps);
    formatKd(kd, sizeof(kd), tw_, tl);
    snprintf(line, sizeof(line), "KD %s  W%d L%d", kd, (int)tw_, (int)tl);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_YELLOW, tft.color565(12, 14, 22));
    tft.drawString(line, SCREEN_WIDTH / 2, 50, 2);
    tft.setTextDatum(TL_DATUM);

    tft.setTextColor(TFT_LIGHTGREY, tft.color565(12, 14, 22));
    tft.drawString("GAME     W   L   G    KD", 16, 74, 1);
    int y = 90;
    for (int i = 0; i < GAME_STAT_KIND_N; i++) {
        formatKd(kd, sizeof(kd), ps->k[i].wins, ps->k[i].losses);
        snprintf(line, sizeof(line), "%-6s  %3u %3u %3u  %s",
                 statsKindShort(i),
                 (unsigned)ps->k[i].wins, (unsigned)ps->k[i].losses,
                 (unsigned)ps->k[i].games, kd);
        tft.setTextColor(TFT_WHITE, tft.color565(12, 14, 22));
        tft.drawString(line, 16, y, 1);
        y += 16;
    }

    if (showPeersHint) {
        tft.setTextColor(TFT_DARKGREY, tft.color565(12, 14, 22));
        cnDrawUtf8(tft, 16, SCREEN_HEIGHT - 20, "点榜查看附近对手", TFT_DARKGREY);
    }
    overlayGameInviteIfAny();
}

static void drawLeaderboard()
{
    tft.fillScreen(tft.color565(12, 14, 22));
    tft.fillRect(0, 0, SCREEN_WIDTH, 26, tft.color565(40, 35, 70));
    cnDrawUtf8(tft, 8, 7, "排行榜", TFT_CYAN);
    drawBackChip();

    // gather: local + peers
    struct Row {
        const char *id;
        uint16_t w, l;
    };
    Row rows[PEER_STATS_MAX + 1];
    int n = 0;
    rows[n].id = localDeviceId;
    rows[n].w = statsTotalWins(localStats);
    rows[n].l = statsTotalLosses(localStats);
    n++;
    for (int i = 0; i < peerStatsN && n < PEER_STATS_MAX + 1; i++) {
        rows[n].id = peerStats[i].id;
        rows[n].w = statsTotalWins(peerStats[i]);
        rows[n].l = statsTotalLosses(peerStats[i]);
        n++;
    }
    // also pull online peers with empty stats
    for (auto const &kv : peerInfoMap) {
        if (n >= PEER_STATS_MAX + 1)
            break;
        const PeerInfo_t &p = kv.second;
        if (!peerVisibleForLocalMode(p) || !p.deviceId[0])
            continue;
        if (strcmp(p.deviceId, localDeviceId) == 0)
            continue;
        bool exists = false;
        for (int i = 0; i < n; i++) {
            if (strcmp(rows[i].id, p.deviceId) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            rows[n].id = p.deviceId;
            rows[n].w = 0;
            rows[n].l = 0;
            n++;
        }
    }
    // sort by wins desc
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (rows[j].w > rows[i].w || (rows[j].w == rows[i].w && rows[j].l < rows[i].l)) {
                Row t = rows[i];
                rows[i] = rows[j];
                rows[j] = t;
            }
        }
    }

    tft.setTextColor(TFT_LIGHTGREY, tft.color565(12, 14, 22));
    tft.drawString("#  NAME          W/L   KD", 10, 32, 1);
    int y = 48 - statsScrollY;
    for (int i = 0; i < n; i++) {
        if (y < 40) {
            y += 22;
            continue;
        }
        if (y > SCREEN_HEIGHT - 8)
            break;
        char kd[12];
        formatKd(kd, sizeof(kd), rows[i].w, rows[i].l);
        uint16_t rowBg = (i % 2) ? tft.color565(18, 20, 30) : tft.color565(14, 16, 26);
        tft.fillRect(8, y - 2, SCREEN_WIDTH - 16, 20, rowBg);
        char line[40];
        snprintf(line, sizeof(line), "%d", i + 1);
        tft.setTextColor(TFT_ORANGE, rowBg);
        tft.drawString(line, 12, y, 1);
        cnDrawUtf8Ellipsis(tft, 32, y, rows[i].id, TFT_WHITE, 100);
        snprintf(line, sizeof(line), "%u/%u", (unsigned)rows[i].w, (unsigned)rows[i].l);
        tft.setTextColor(TFT_CYAN, rowBg);
        tft.drawString(line, 150, y, 1);
        tft.setTextColor(TFT_YELLOW, rowBg);
        tft.drawString(kd, 220, y, 1);
        y += 22;
    }
    if (n == 0) {
        tft.setTextColor(TFT_DARKGREY, tft.color565(12, 14, 22));
        tft.drawString("NO DATA", 120, 100, 2);
    }
    tft.setTextColor(TFT_DARKGREY, tft.color565(12, 14, 22));
    cnDrawUtf8(tft, 10, SCREEN_HEIGHT - 16, "点击查看详情", TFT_DARKGREY);
    overlayGameInviteIfAny();
}

static void enterRps()
{
    arcadeView = ARCADE_RPS;
    matchResetForNewGame();
    rpsCount = 0;
    rpsEnsure(localDeviceId);
    if (rpsCount > 0)
        rpsPlayers[0].score = 0;
    rpsResetRound();
    rpsSoftDirty = false;
    rpsLastDrawnChoice = 0xFF;
    rpsLastDrawnCount = -1;
    sendGamePacket(GAME_KIND_RPS, GAME_OP_JOIN, 0, 0, 0, 0);
    rpsBroadcastRoster();
    statsBroadcast();
    drawRpsScreen();
}

static void enterMole()
{
    arcadeView = ARCADE_MOLE;
    matchResetForNewGame();
    moleMyScore = 0;
    moleScoreN = 0;
    moleFrameReady = false;
    moleDrawnActive = 0xFE;
    moleDrawnMyScore = -999;
    moleDrawnScoreN = -1;
    moleDrawnWait = -1;
    moleScoreSig = 0;
    moleSetScore(localDeviceId, 0);
    moleIsHost = true;
    moleSeed = (uint32_t)(millis() ^ (uint32_t)esp_random());
    moleAdvance();
    sendGamePacket(GAME_KIND_MOLE, GAME_OP_JOIN, 0, 0, 0, 0);
    statsBroadcast();
    drawMoleScreen();
}

static void enterRadar()
{
    arcadeView = ARCADE_RADAR;
    radarCount = 0;
    radarFocusIdx = 0;
    radarSweepDeg = 0;
    radarPrevSweepDeg = -1;
    radarFrameReady = false;
    radarCalibrating = false;
    memset(radarLink, -128, sizeof(radarLink));
    radarSyncFromPeers();
    radarBroadcastLinks();
    drawRadarScreen(true);
}

static void enterReact()
{
    arcadeView = ARCADE_REACT;
    matchResetForNewGame();
    liveClear();
    reactMyScore = 0;
    reactPhase = REACT_IDLE;
    reactMyMs = -1;
    reactTapped = false;
    reactIsHost = true;
    liveSet(localDeviceId, 0, 0);
    sendGamePacket(GAME_KIND_REACT, GAME_OP_JOIN, 0, 0, 0, 0);
    statsBroadcast();
    drawReactScreen();
}

static void enterBridge()
{
    arcadeView = ARCADE_BRIDGE;
    matchResetForNewGame();
    liveClear();
    bridgeSeed = millis() ^ esp_random();
    bridgeScore = 0;
    liveSet(localDeviceId, 0, 1);
    sendGamePacket(GAME_KIND_BRIDGE, GAME_OP_JOIN, 0, 0, 0, 0);
    statsBroadcast();
    bridgeGenNext(true);
    drawBridgeScreen();
}

static void leaveCurrentGame()
{
    if (arcadeView == ARCADE_RPS)
        sendGamePacket(GAME_KIND_RPS, GAME_OP_LEAVE, 0, 0, 0, 0);
    else if (arcadeView == ARCADE_MOLE)
        sendGamePacket(GAME_KIND_MOLE, GAME_OP_LEAVE, 0, 0, 0, 0);
    else if (arcadeView == ARCADE_REACT)
        sendGamePacket(GAME_KIND_REACT, GAME_OP_LEAVE, 0, 0, 0, 0);
    else if (arcadeView == ARCADE_BRIDGE)
        sendGamePacket(GAME_KIND_BRIDGE, GAME_OP_LEAVE, 0, 0, 0, 0);
}

static void drawRpsScreen()
{
    tft.fillScreen(tft.color565(12, 14, 24));
    tft.fillRect(0, 0, SCREEN_WIDTH, 24, tft.color565(30, 40, 70));
    tft.setTextColor(TFT_WHITE, tft.color565(30, 40, 70));
    tft.drawString("MULTI RPS ARENA", 8, 6, 2);
    tft.fillRoundRect(200, 4, 40, 16, 3, tft.color565(200, 120, 40));
    tft.setTextColor(TFT_BLACK, tft.color565(200, 120, 40));
    tft.setTextDatum(MC_DATUM);
    tft.drawString("INV", 220, 12, 1);
    tft.setTextDatum(TL_DATUM);
    drawBackChip();
    rpsLastDrawnChoice = 0xFF;
    rpsLastDrawnCount = -1;
    // rest via soft redraw
    drawRpsDynamic(true);
}

static void drawRpsDynamic(bool force)
{
    uint16_t bg = tft.color565(12, 14, 24);
    bool choiceChanged = force || (rpsMyChoice != rpsLastDrawnChoice);
    bool listChanged = force || rpsSoftDirty || (rpsCount != rpsLastDrawnCount);

    if (choiceChanged) {
        const char *labels[3] = {"ROCK", "PAPER", "SCISSORS"};
        uint16_t cols[3] = {tft.color565(180, 70, 70), tft.color565(70, 140, 200), tft.color565(70, 170, 100)};
        uint8_t choices[3] = {RPS_ROCK, RPS_PAPER, RPS_SCISSORS};
        for (int i = 0; i < 3; i++) {
            int x = 12 + i * 102;
            bool sel = (rpsMyChoice == choices[i]);
            uint16_t b = sel ? cols[i] : tft.color565(28, 34, 50);
            tft.fillRoundRect(x, 32, 96, 36, 5, b);
            tft.drawRoundRect(x, 32, 96, 36, 5, cols[i]);
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(TFT_WHITE, b);
            tft.drawString(labels[i], x + 48, 50, 2);
        }
        tft.setTextDatum(TL_DATUM);
        rpsLastDrawnChoice = rpsMyChoice;
    }

    // wait banner — 独立条，不挡住按钮
    if (rpsCount < 2) {
        tft.fillRect(0, 70, SCREEN_WIDTH, 16, tft.color565(70, 50, 18));
        tft.setTextColor(TFT_YELLOW, tft.color565(70, 50, 18));
        tft.drawString("NEED 2+ PLAYERS - waiting...", 8, 74, 1);
    } else if (listChanged) {
        tft.fillRect(0, 70, SCREEN_WIDTH, 16, bg);
    }

    // status row + list (y>=88)
    static unsigned long lastTimerDraw = 0;
    bool timerTick = (millis() - lastTimerDraw > 1000UL);
    if (listChanged || timerTick || force) {
        lastTimerDraw = millis();
        tft.fillRect(0, 88, SCREEN_WIDTH, SCREEN_HEIGHT - 88, bg);

        tft.fillRoundRect(12, 90, 140, 22, 4, tft.color565(60, 50, 100));
        tft.setTextColor(TFT_WHITE, tft.color565(60, 50, 100));
        tft.drawString(rpsRevealed ? "NEXT ROUND" : "WAITING...", 20, 96, 1);

        long left = (long)rpsRoundDeadline - (long)millis();
        if (left < 0)
            left = 0;
        char tbuf[24];
        snprintf(tbuf, sizeof(tbuf), "T-%lds  P:%d  /%d", left / 1000L, rpsCount, GAME_WIN_SCORE);
        tft.setTextColor(TFT_CYAN, bg);
        tft.drawString(tbuf, 170, 96, 1);

        int y = 118;
        tft.setTextColor(TFT_LIGHTGREY, bg);
        tft.drawString("PLAYER          PICK     SCORE", 12, y, 1);
        y += 14;
        for (int i = 0; i < rpsCount && y < SCREEN_HEIGHT - 8; i++) {
            const char *pick;
            if (!rpsRevealed && strcmp(rpsPlayers[i].id, localDeviceId) != 0 &&
                rpsPlayers[i].choice != RPS_NONE)
                pick = "LOCKED";
            else
                pick = rpsName(rpsPlayers[i].choice);
            tft.fillRect(8, y - 1, SCREEN_WIDTH - 16, 14, tft.color565(18, 22, 34));
            cnDrawUtf8Ellipsis(tft, 12, y, rpsPlayers[i].id, TFT_WHITE, 90);
            tft.setTextColor(TFT_YELLOW, tft.color565(18, 22, 34));
            tft.drawString(pick, 110, y, 1);
            char sc[8];
            snprintf(sc, sizeof(sc), "%u/%d", (unsigned)rpsPlayers[i].score, GAME_WIN_SCORE);
            tft.setTextColor(TFT_GREENYELLOW, tft.color565(18, 22, 34));
            tft.drawString(sc, 210, y, 1);
            y += 16;
        }
        rpsSoftDirty = false;
        rpsLastDrawnCount = rpsCount;
    }
    drawMatchEndBannerIfAny();
}

static void moleCellCenter(int i, int &cx, int &cy)
{
    const int cell = 52, gap = 6;
    int r = i / 3, c = i % 3;
    cx = 50 + c * (cell + gap) + cell / 2;
    cy = 62 + r * (cell + gap) + cell / 2;
}

static void molePaintHole(int i, bool active)
{
    const int cell = 52;
    int cx, cy;
    moleCellCenter(i, cx, cy);
    uint16_t hole = tft.color565(40, 30, 20);
    uint16_t bg = tft.color565(14, 18, 16);
    // 擦掉旧 mole 光晕
    tft.fillCircle(cx, cy, cell / 2 - 1, bg);
    tft.fillCircle(cx, cy, cell / 2 - 2, hole);
    if (active) {
        tft.fillCircle(cx, cy - 4, 14, tft.color565(160, 90, 50));
        tft.fillCircle(cx - 5, cy - 8, 2, TFT_BLACK);
        tft.fillCircle(cx + 5, cy - 8, 2, TFT_BLACK);
    }
}

static uint16_t moleComputeScoreSig()
{
    uint16_t s = (uint16_t)moleScoreN * 31u + (uint16_t)moleMyScore;
    for (int i = 0; i < moleScoreN; i++)
        s = (uint16_t)(s * 131u + (uint16_t)moleScores[i].score + (uint16_t)moleScores[i].id[0]);
    return s;
}

static void drawMoleDynamic(bool force)
{
    uint16_t bg = tft.color565(14, 18, 16);
    uint16_t hdr = tft.color565(30, 55, 40);

    if (force || !moleFrameReady) {
        tft.fillScreen(bg);
        tft.fillRect(0, 0, SCREEN_WIDTH, 24, hdr);
        tft.setTextColor(TFT_WHITE, hdr);
        tft.drawString("MULTI WHACK-A-MOLE", 8, 6, 2);
        tft.fillRoundRect(200, 4, 40, 16, 3, tft.color565(200, 120, 40));
        tft.setTextColor(TFT_BLACK, tft.color565(200, 120, 40));
        tft.setTextDatum(MC_DATUM);
        tft.drawString("INV", 220, 12, 1);
        tft.setTextDatum(TL_DATUM);
        drawBackChip();
        for (int i = 0; i < MOLE_CELLS; i++)
            molePaintHole(i, false);
        moleFrameReady = true;
        moleDrawnActive = 0xFE;
        moleDrawnMyScore = -999;
        moleDrawnScoreN = -1;
        moleDrawnWait = -1;
        moleScoreSig = 0;
    }

    int needWait = (moleScoreN < 2) ? 1 : 0;
    if (force || needWait != moleDrawnWait) {
        if (needWait) {
            tft.fillRect(0, 24, SCREEN_WIDTH, 18, tft.color565(70, 50, 18));
            tft.setTextColor(TFT_YELLOW, tft.color565(70, 50, 18));
            tft.drawString("NEED 2+ PLAYERS - tap INV to recruit", 4, 28, 1);
        } else {
            tft.fillRect(0, 24, SCREEN_WIDTH, 18, bg);
        }
        moleDrawnWait = needWait;
        moleDrawnMyScore = -999; // force score redraw below banner
    }

    if (force || moleMyScore != moleDrawnMyScore) {
        int sy = needWait ? 44 : 28;
        tft.fillRect(0, sy, 120, 18, bg);
        char sc[32];
        snprintf(sc, sizeof(sc), "YOU %d/%d", moleMyScore, GAME_WIN_SCORE);
        tft.setTextColor(TFT_GREENYELLOW, bg);
        tft.drawString(sc, 12, sy, 2);
        moleDrawnMyScore = moleMyScore;
    }

    if (force || moleActive != moleDrawnActive) {
        if (moleDrawnActive < MOLE_CELLS)
            molePaintHole(moleDrawnActive, false);
        if (moleActive < MOLE_CELLS)
            molePaintHole(moleActive, true);
        moleDrawnActive = moleActive;
    }

    uint16_t sig = moleComputeScoreSig();
    if (force || sig != moleScoreSig || moleScoreN != moleDrawnScoreN) {
        int fy = SCREEN_HEIGHT - 28;
        tft.fillRect(0, fy, SCREEN_WIDTH, 28, tft.color565(20, 28, 24));
        tft.setTextColor(TFT_LIGHTGREY, tft.color565(20, 28, 24));
        tft.drawString("SCORES:", 6, fy + 8, 1);
        int sx = 60;
        for (int i = 0; i < moleScoreN && sx < SCREEN_WIDTH - 10; i++) {
            char b[28];
            snprintf(b, sizeof(b), ":%d/%d", (int)moleScores[i].score, GAME_WIN_SCORE);
            cnDrawUtf8Ellipsis(tft, sx, fy + 8, moleScores[i].id, TFT_CYAN, 40);
            tft.setTextColor(TFT_CYAN, tft.color565(20, 28, 24));
            tft.drawString(b, sx + 42, fy + 8, 1);
            sx += 78;
        }
        moleScoreSig = sig;
        moleDrawnScoreN = moleScoreN;
    }
    drawMatchEndBannerIfAny();
}

static void drawMoleScreen()
{
    drawMoleDynamic(true);
}


static void radarDrawStaticFrame()
{
    const int cx = RADAR_CX, cy = RADAR_CY, R = RADAR_R;
    uint16_t bg = tft.color565(2, 10, 8);
    uint16_t grid = tft.color565(18, 78, 55);
    uint16_t dim = tft.color565(6, 28, 22);
    uint16_t panel = tft.color565(4, 14, 12);
    uint16_t hdr = tft.color565(6, 32, 26);

    tft.fillScreen(bg);
    tft.fillRect(0, 0, SCREEN_WIDTH, 22, hdr);
    tft.setTextColor(TFT_GREENYELLOW, hdr);
    tft.drawString("RF GEO RADAR", 6, 4, 2);
    tft.setTextColor(TFT_DARKGREY, hdr);
    tft.drawString("0=FWD", 100, 8, 1);
    // INV chip (left of CAL)
    tft.fillRoundRect(120, 3, 40, 16, 3, tft.color565(200, 120, 40));
    tft.setTextColor(TFT_BLACK, tft.color565(200, 120, 40));
    tft.setTextDatum(MC_DATUM);
    tft.drawString("INV", 140, 11, 1);
    // CALIB chip
    tft.fillRoundRect(164, 3, 44, 16, 3, tft.color565(30, 70, 50));
    tft.setTextColor(TFT_GREEN, tft.color565(30, 70, 50));
    tft.drawString("CAL", 186, 11, 1);
    tft.setTextDatum(TL_DATUM);
    drawBackChip();

    // scope face
    tft.fillCircle(cx, cy, R + 4, dim);
    for (int ring = 1; ring <= 4; ring++) {
        int rr = (R * ring) / 4;
        tft.drawCircle(cx, cy, rr, grid);
    }
    // degree ticks — 0° at top
    for (int deg = 0; deg < 360; deg += 15) {
        float a = deg * 0.01745329252f;
        int r0 = (deg % 90 == 0) ? R - 10 : R - 5;
        int x0 = cx + (int)(sinf(a) * r0);
        int y0 = cy - (int)(cosf(a) * r0);
        int x1 = cx + (int)(sinf(a) * R);
        int y1 = cy - (int)(cosf(a) * R);
        tft.drawLine(x0, y0, x1, y1, grid);
    }
    tft.drawFastHLine(cx - R, cy, R * 2, grid);
    tft.drawFastVLine(cx, cy - R, R * 2, grid);
    // N/E/S/W：0=前方(上)
    tft.setTextColor(tft.color565(80, 220, 120), dim);
    tft.drawString("N", cx - 3, cy - R + 2, 1);
    tft.drawString("E", cx + R - 10, cy - 4, 1);
    tft.drawString("S", cx - 3, cy + R - 10, 1);
    tft.drawString("W", cx - R + 2, cy - 4, 1);
    tft.setTextColor(tft.color565(40, 100, 70), bg);
    tft.drawString("5m", cx + 4, cy - R / 4 - 6, 1);
    tft.drawString("10m", cx + 4, cy - R / 2 - 6, 1);
    tft.drawString("20m", cx + 4, cy - R + 2, 1);
    // 本机朝向箭头（朝上）
    tft.fillTriangle(cx, cy - 10, cx - 5, cy + 4, cx + 5, cy + 4, TFT_GREENYELLOW);
    tft.fillCircle(cx, cy, 2, TFT_BLACK);

    // side intel panel
    tft.fillRect(214, 22, 106, SCREEN_HEIGHT - 22, panel);
    tft.drawFastVLine(214, 22, SCREEN_HEIGHT - 22, grid);
    tft.setTextColor(TFT_GREEN, panel);
    tft.drawString("TRACK FILE", 220, 26, 1);
    tft.drawString("----------------", 218, 36, 1);

    // footer
    tft.fillRect(0, SCREEN_HEIGHT - 18, 214, 18, hdr);
    tft.setTextColor(TFT_GREENYELLOW, hdr);
    tft.drawString("MODE SWEEP | TAP=AZ", 4, SCREEN_HEIGHT - 14, 1);

    radarFrameReady = true;
}

static void radarDrawPanel()
{
    uint16_t panel = tft.color565(4, 14, 12);
    tft.fillRect(216, 40, 102, SCREEN_HEIGHT - 60, panel);
    int ly = 42;
    unsigned long now = millis();
    for (int i = 0; i < radarCount && ly < SCREEN_HEIGHT - 44; i++) {
        RadarContact_t &c = radarContacts[i];
        uint16_t nameCol = (i == radarFocusIdx) ? TFT_YELLOW : TFT_WHITE;
        cnDrawUtf8Ellipsis(tft, 218, ly, c.id, nameCol, 98);

        char dbuf[8];
        if (c.distM < 10.0f)
            snprintf(dbuf, sizeof(dbuf), "%.1fm", (double)c.distM);
        else
            snprintf(dbuf, sizeof(dbuf), "%.0fm", (double)c.distM);

        char line[36];
        if (c.azLocked || c.azValid)
            snprintf(line, sizeof(line), "%s %03.0f  %s", radarCardinal(c.angleDeg),
                     (double)c.angleDeg, dbuf);
        else
            snprintf(line, sizeof(line), "AZ---  %s", dbuf);
        tft.setTextColor(c.azLocked ? TFT_GREENYELLOW : TFT_CYAN, panel);
        tft.drawString(line, 218, ly + 11, 1);

        int ageS = (int)((now - c.lastMs) / 1000UL);
        if (ageS > 99)
            ageS = 99;
        int q = constrain((int)c.rssi + 100, 0, 50);
        int qPct = q * 2;
        if (qPct > 100)
            qPct = 100;
        snprintf(line, sizeof(line), "%ddBm Q%d%%", (int)c.rssi, qPct);
        tft.setTextColor(TFT_LIGHTGREY, panel);
        tft.drawString(line, 218, ly + 21, 1);

        int links = 0;
        for (int j = 0; j < radarCount; j++) {
            if (i != j && (radarLink[i][j] != -128 || radarLink[j][i] != -128))
                links++;
        }
        snprintf(line, sizeof(line), "L%d Age%ds C%d%%", links, ageS, (int)c.azConf);
        tft.setTextColor(TFT_DARKGREY, panel);
        tft.drawString(line, 218, ly + 31, 1);

        tft.fillRect(218, ly + 41, 90, 3, tft.color565(20, 40, 30));
        tft.fillRect(218, ly + 41, qPct * 90 / 100, 3,
                     c.azLocked ? TFT_GREEN : tft.color565(180, 160, 40));
        ly += 52;
    }
    if (radarCount == 0) {
        tft.setTextColor(TFT_DARKGREY, panel);
        tft.drawString("NO CONTACTS", 222, 60, 1);
        tft.drawString("WAIT RF...", 222, 74, 1);
    }
    tft.setTextColor(TFT_ORANGE, panel);
    if (radarCalibrating)
        tft.drawString("TURN: face peer", 218, SCREEN_HEIGHT - 34, 1);
    else if (radarCount && radarContacts[0].azLocked)
        tft.drawString("AZ OK tap=adj", 218, SCREEN_HEIGHT - 34, 1);
    else
        tft.drawString("CAL/TAP SCOPE", 218, SCREEN_HEIGHT - 34, 1);
}

static void radarRestoreScopeGrid()
{
    const int cx = RADAR_CX, cy = RADAR_CY, R = RADAR_R;
    uint16_t grid = tft.color565(18, 78, 55);
    for (int ring = 1; ring <= 4; ring++)
        tft.drawCircle(cx, cy, (R * ring) / 4, grid);
    tft.drawFastHLine(cx - R, cy, R * 2, grid);
    tft.drawFastVLine(cx, cy - R, R * 2, grid);
    tft.fillTriangle(cx, cy - 10, cx - 5, cy + 4, cx + 5, cy + 4, TFT_GREENYELLOW);
    tft.fillCircle(cx, cy, 2, TFT_BLACK);
}

static void radarEraseBlip(int bx, int by)
{
    if (bx < 0)
        return;
    uint16_t dim = tft.color565(6, 28, 22);
    tft.fillCircle(bx, by, 6, dim);
}

static void radarAzToScreen(float angleDeg, float distM, int &bx, int &by)
{
    float norm = distM / RADAR_RANGE_M;
    if (norm > 1.0f)
        norm = 1.0f;
    float br = norm * (float)RADAR_R;
    float a = angleDeg * 0.01745329252f;
    bx = RADAR_CX + (int)(sinf(a) * br);
    by = RADAR_CY - (int)(cosf(a) * br);
}

static void radarDrawSweepAndBlips()
{
    const int cx = RADAR_CX, cy = RADAR_CY, R = RADAR_R;
    uint16_t dim = tft.color565(6, 28, 22);
    uint16_t grid = tft.color565(18, 78, 55);
    uint16_t beam = tft.color565(60, 255, 150);

    // 只擦掉上一条扫线（不留拖尾、不填充扫过面积）
    if (radarPrevSweepDeg >= 0.0f) {
        float prad = radarPrevSweepDeg * 0.01745329252f;
        int px = cx + (int)(sinf(prad) * R);
        int py = cy - (int)(cosf(prad) * R);
        tft.drawLine(cx, cy, px, py, dim);
        // 恢复十字与环线被擦掉的像素
        tft.drawFastHLine(cx - R, cy, R * 2, grid);
        tft.drawFastVLine(cx, cy - R, R * 2, grid);
        for (int ring = 1; ring <= 4; ring++)
            tft.drawCircle(cx, cy, (R * ring) / 4, grid);
        tft.fillTriangle(cx, cy - 10, cx - 5, cy + 4, cx + 5, cy + 4, TFT_GREENYELLOW);
        tft.fillCircle(cx, cy, 2, TFT_BLACK);
    }

    float rad = radarSweepDeg * 0.01745329252f;
    int x2 = cx + (int)(sinf(rad) * R);
    int y2 = cy - (int)(cosf(rad) * R);
    tft.drawLine(cx, cy, x2, y2, beam);
    radarPrevSweepDeg = radarSweepDeg;

    // 光点始终可见（扫线只是指示，不遮挡）
    bool needRestore = false;
    for (int i = 0; i < radarCount; i++) {
        RadarContact_t &c = radarContacts[i];
        int bx, by;
        radarAzToScreen(c.angleDeg, c.distM, bx, by);
        if (c.lastBx >= 0 && (c.lastBx != bx || c.lastBy != by)) {
            radarEraseBlip(c.lastBx, c.lastBy);
            needRestore = true;
        }
        c.lastBx = bx;
        c.lastBy = by;
    }
    if (needRestore)
        radarRestoreScopeGrid();

    for (int i = 0; i < radarCount; i++) {
        RadarContact_t &c = radarContacts[i];
        uint16_t col = c.azLocked ? tft.color565(40, 255, 90)
                                  : tft.color565(220, 180, 40);
        tft.fillCircle(c.lastBx, c.lastBy, 3, col);
        tft.drawCircle(c.lastBx, c.lastBy, 5, tft.color565(20, 120, 70));
    }
}

static void drawRadarScreen(bool full)
{
    if (full || !radarFrameReady) {
        radarDrawStaticFrame();
        radarDrawPanel();
    }
    radarDrawSweepAndBlips();
    if (full)
        radarDrawPanel();
}

static void drawReactScreen()
{
    uint16_t bg = tft.color565(18, 18, 28);
    if (reactPhase == REACT_WAIT)
        bg = tft.color565(90, 20, 20);
    else if (reactPhase == REACT_GO)
        bg = tft.color565(20, 110, 40);
    else if (reactPhase == REACT_DONE)
        bg = tft.color565(20, 28, 50);

    tft.fillScreen(bg);
    tft.fillRect(0, 0, SCREEN_WIDTH, 24, tft.color565(30, 30, 48));
    tft.setTextColor(TFT_WHITE, tft.color565(30, 30, 48));
    tft.drawString("REACTION RACE", 8, 6, 2);
    tft.fillRoundRect(200, 4, 40, 16, 3, tft.color565(200, 120, 40));
    tft.setTextColor(TFT_BLACK, tft.color565(200, 120, 40));
    tft.setTextDatum(MC_DATUM);
    tft.drawString("INV", 220, 12, 1);
    tft.setTextDatum(TL_DATUM);
    drawBackChip();

    char pc[24];
    snprintf(pc, sizeof(pc), "P:%d/2+", liveScoreN);
    tft.setTextColor(liveScoreN >= 2 ? TFT_GREEN : TFT_ORANGE, tft.color565(30, 30, 48));
    tft.drawString(pc, 160, 8, 1);

    tft.setTextDatum(MC_DATUM);
    if (liveScoreN < 2) {
        tft.setTextColor(TFT_YELLOW, bg);
        tft.drawString("NEED 2+ PLAYERS", SCREEN_WIDTH / 2, 90, 2);
        tft.setTextColor(TFT_LIGHTGREY, bg);
        tft.drawString("Waiting for peer...", SCREEN_WIDTH / 2, 118, 2);
    } else if (reactPhase == REACT_IDLE) {
        tft.setTextColor(TFT_WHITE, bg);
        tft.drawString("TAP START", SCREEN_WIDTH / 2, 90, 4);
        cnDrawUtf8(tft, SCREEN_WIDTH / 2 - 40, 120, "比反应", TFT_LIGHTGREY);
    } else if (reactPhase == REACT_WAIT) {
        tft.setTextColor(TFT_YELLOW, bg);
        tft.drawString("WAIT...", SCREEN_WIDTH / 2, 100, 4);
    } else if (reactPhase == REACT_GO) {
        tft.setTextColor(TFT_WHITE, bg);
        tft.drawString("GO!", SCREEN_WIDTH / 2, 100, 4);
    } else {
        char b[32];
        if (reactMyMs == -2)
            snprintf(b, sizeof(b), "FOUL");
        else if (reactMyMs >= 0)
            snprintf(b, sizeof(b), "%d ms", (int)reactMyMs);
        else
            snprintf(b, sizeof(b), "--");
        tft.setTextColor(TFT_CYAN, bg);
        tft.drawString(b, SCREEN_WIDTH / 2, 80, 4);
        tft.setTextColor(TFT_WHITE, bg);
        tft.drawString("TAP NEXT", SCREEN_WIDTH / 2, 120, 2);
    }
    tft.setTextDatum(TL_DATUM);

    char sc[24];
    snprintf(sc, sizeof(sc), "YOU %d/%d", reactMyScore, GAME_WIN_SCORE);
    tft.setTextColor(TFT_GREENYELLOW, bg);
    tft.drawString(sc, 10, 32, 2);

    drawLiveScoresStrip(SCREEN_HEIGHT - 28, tft.color565(16, 16, 28));
    drawMatchEndBannerIfAny();
}

static void drawBridgeScreen()
{
    uint16_t sky = tft.color565(135, 206, 235);
    uint16_t dirt = tft.color565(60, 40, 20);
    uint16_t wood = tft.color565(160, 110, 50);
    uint16_t stickC = tft.color565(40, 30, 20);

    tft.fillRect(0, 0, SCREEN_WIDTH, groundY, sky);
    tft.fillRect(0, groundY, SCREEN_WIDTH, SCREEN_HEIGHT - groundY, dirt);
    tft.fillRect(0, 0, SCREEN_WIDTH, 24, tft.color565(40, 50, 70));
    tft.setTextColor(TFT_WHITE, tft.color565(40, 50, 70));
    tft.drawString("DINO BRIDGE", 8, 6, 2);
    tft.fillRoundRect(200, 4, 40, 16, 3, tft.color565(200, 120, 40));
    tft.setTextColor(TFT_BLACK, tft.color565(200, 120, 40));
    tft.setTextDatum(MC_DATUM);
    tft.drawString("INV", 220, 12, 1);
    tft.setTextDatum(TL_DATUM);
    drawBackChip();
    if (liveScoreN < 2) {
        tft.fillRect(40, 50, 240, 40, tft.color565(40, 30, 20));
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_YELLOW, tft.color565(40, 30, 20));
        tft.drawString("NEED 2+ PLAYERS", SCREEN_WIDTH / 2, 70, 2);
        tft.setTextDatum(TL_DATUM);
        drawLiveScoresStrip(SCREEN_HEIGHT - 28, tft.color565(40, 30, 20));
        bridgeDirty = false;
        return;
    }

    char sc[24];
    snprintf(sc, sizeof(sc), "SCORE %d/%d", bridgeScore, GAME_WIN_SCORE);
    tft.setTextColor(TFT_YELLOW, sky);
    tft.drawString(sc, 10, 30, 2);
    cnDrawUtf8(tft, 120, 30, "长按", TFT_DARKGREY);

    // platforms
    tft.fillRect(platL, groundY - 18, platLW, 18, wood);
    tft.fillRect(platR, groundY - 18, platRW, 18, wood);
    tft.drawRect(platL, groundY - 18, platLW, 18, TFT_BLACK);
    tft.drawRect(platR, groundY - 18, platRW, 18, TFT_BLACK);

    // stick pivot at right edge of left platform
    int px = platL + platLW;
    int py = groundY - 18;
    float rad = stickAngle * 0.01745329252f;
    int sx = px + (int)(cosf(rad) * stickLen);
    int sy = py - (int)(sinf(rad) * stickLen);
    if (stickLen > 0)
        tft.drawLine(px, py, sx, sy, stickC);

    // dino (simple)
    int dx = (int)dinoX;
    int dy = groundY - 30;
    if (bridgePhase == BRIDGE_OVER)
        dy = groundY + 10; // fallen
    tft.fillRoundRect(dx, dy, 16, 12, 3, tft.color565(50, 140, 60));
    tft.fillCircle(dx + 14, dy + 2, 5, tft.color565(50, 140, 60));
    tft.fillCircle(dx + 16, dy, 2, TFT_BLACK);
    tft.drawLine(dx + 2, dy + 12, dx, dy + 18, TFT_BLACK);
    tft.drawLine(dx + 10, dy + 12, dx + 12, dy + 18, TFT_BLACK);

    if (bridgePhase == BRIDGE_OVER) {
        tft.fillRoundRect(50, 70, 220, 70, 8, tft.color565(30, 20, 20));
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_RED, tft.color565(30, 20, 20));
        tft.drawString("GAME OVER", SCREEN_WIDTH / 2, 92, 4);
        tft.setTextColor(TFT_WHITE, tft.color565(30, 20, 20));
        tft.drawString("TAP TO RETRY", SCREEN_WIDTH / 2, 120, 2);
        tft.setTextDatum(TL_DATUM);
    }

    drawLiveScoresStrip(SCREEN_HEIGHT - 28, tft.color565(40, 30, 20));
    drawMatchEndBannerIfAny();
    bridgeDirty = false;
}

static void redrawArcade()
{
    switch (arcadeView) {
    case ARCADE_RPS: drawRpsScreen(); break;
    case ARCADE_MOLE: drawMoleScreen(); break;
    case ARCADE_RADAR: drawRadarScreen(true); break;
    case ARCADE_REACT: drawReactScreen(); break;
    case ARCADE_BRIDGE: drawBridgeScreen(); break;
    case ARCADE_MY_STATS:
        drawStatsProfile(localDeviceId, true);
        break;
    case ARCADE_PEER_STATS:
        drawStatsProfile(statsPeerId[0] ? statsPeerId : localDeviceId, false);
        break;
    case ARCADE_BOARD:
        drawLeaderboard();
        break;
    default: drawGameLobby(); break;
    }
    overlayGameInviteIfAny();
}

void redrawGameArcade()
{
    if (currentUIState != UI_STATE_ARCADE)
        return;
    redrawArcade();
}

void showGameLobby()
{
    if (!isScreenOn || inCustomColorMode)
        return;
    arcadeView = ARCADE_LOBBY;
    arcadeFingerDown = false;
    lobbyScrollY = 0;
    currentUIState = UI_STATE_ARCADE;
    statsLoadLocal();
    statsBroadcast();
    drawGameLobby();
}

void hideGameArcade()
{
    if (currentUIState != UI_STATE_ARCADE)
        return;
    leaveCurrentGame();
    arcadeView = ARCADE_LOBBY;
    currentUIState = UI_STATE_MAIN;
    redrawMainScreen();
}

void updateGameArcade()
{
    if (currentUIState != UI_STATE_ARCADE)
        return;

    // 多人在场广播：始终同步花名册（不只是等人满）
    static unsigned long lastPresence = 0;
    if (millis() - lastPresence > 1200UL) {
        lastPresence = millis();
        if (arcadeView == ARCADE_RPS) {
            sendGamePacket(GAME_KIND_RPS, GAME_OP_JOIN, 0, 0, 0, 0);
            rpsBroadcastRoster();
        } else if (arcadeView == ARCADE_MOLE) {
            sendGamePacket(GAME_KIND_MOLE, GAME_OP_JOIN, 0, 0, 0, 0);
            sendGamePacket(GAME_KIND_MOLE, GAME_OP_SCORE, (int16_t)moleMyScore, 0, 0, 0);
        } else if (arcadeView == ARCADE_REACT) {
            sendGamePacket(GAME_KIND_REACT, GAME_OP_JOIN, (int16_t)reactMyScore, 0, 0, 0);
            sendGamePacket(GAME_KIND_REACT, GAME_OP_SCORE, (int16_t)reactMyScore,
                           (reactMyMs > 0) ? reactMyMs : (int16_t)0, 0, 0);
        } else if (arcadeView == ARCADE_BRIDGE) {
            sendGamePacket(GAME_KIND_BRIDGE, GAME_OP_JOIN, (int16_t)bridgeScore, 0, 0, 0);
            sendGamePacket(GAME_KIND_BRIDGE, GAME_OP_SCORE, (int16_t)bridgeScore, 1, 0, 0);
        } else if (arcadeView == ARCADE_RADAR) {
            radarBroadcastLinks();
        }
    }

    if (arcadeView == ARCADE_RPS) {
        rpsTryReveal();
        if (rpsSoftDirty || rpsMyChoice != rpsLastDrawnChoice) {
            drawRpsDynamic(false);
            overlayGameInviteIfAny();
        } else {
            // 仅刷新倒计时，约 1s 一次（drawRpsDynamic 内部节流）
            static unsigned long last = 0;
            if (millis() - last > 1000UL) {
                last = millis();
                drawRpsDynamic(false);
                overlayGameInviteIfAny();
            }
        }
    } else if (arcadeView == ARCADE_MOLE) {
        if (moleIsHost && millis() >= moleNextFlip)
            moleAdvance();
        static unsigned long last = 0;
        static uint8_t lastMole = 0xFE;
        static int lastMoleN = -1;
        static int lastMy = -999;
        if (gameUiDirty || moleActive != lastMole || moleScoreN != lastMoleN ||
            moleMyScore != lastMy) {
            last = millis();
            lastMole = moleActive;
            lastMoleN = moleScoreN;
            lastMy = moleMyScore;
            gameUiDirty = false;
            drawMoleDynamic(false);
            overlayGameInviteIfAny();
        }
    } else if (arcadeView == ARCADE_RADAR) {
        // calibration: 转身采样 RSSI → 峰值方位；也可点盘面手动设方位
        if (radarCalibrating) {
            unsigned long elapsed = millis() - radarCalibStart;
            // 校准中高频刷新 RSSI
            radarSyncFromPeers();

            if (elapsed >= RADAR_CAL_MS) {
                radarCalibrating = false;
                for (int i = 0; i < radarCount; i++) {
                    float avgs[RADAR_CAL_BINS];
                    int filled = 0;
                    float sumAll = 0;
                    float peakAvg = -999.0f;
                    int peakBin = 0;
                    for (int b = 0; b < RADAR_CAL_BINS; b++) {
                        if (radarContacts[i].calibN[b] == 0) {
                            avgs[b] = -999.0f;
                            continue;
                        }
                        avgs[b] = (float)radarContacts[i].calibSum[b] /
                                  (float)radarContacts[i].calibN[b];
                        sumAll += avgs[b];
                        filled++;
                        if (avgs[b] > peakAvg) {
                            peakAvg = avgs[b];
                            peakBin = b;
                        }
                    }
                    if (filled < 3 || peakAvg < -200.0f)
                        continue;

                    // 主算法：峰值 bin + 邻域抛物线插值（比圆均值更稳）
                    float peak = (float)peakBin;
                    int bm = (peakBin + RADAR_CAL_BINS - 1) % RADAR_CAL_BINS;
                    int bp = (peakBin + 1) % RADAR_CAL_BINS;
                    float ym1 = (avgs[bm] > -200.0f) ? avgs[bm] : peakAvg - 5.0f;
                    float yp1 = (avgs[bp] > -200.0f) ? avgs[bp] : peakAvg - 5.0f;
                    float y0 = peakAvg;
                    float denom = (ym1 - 2.0f * y0 + yp1);
                    if (fabsf(denom) > 0.05f)
                        peak += 0.5f * (ym1 - yp1) / denom;
                    if (peak < 0)
                        peak += (float)RADAR_CAL_BINS;
                    if (peak >= (float)RADAR_CAL_BINS)
                        peak -= (float)RADAR_CAL_BINS;

                    float angDeg = peak * (360.0f / (float)RADAR_CAL_BINS);
                    float mean = sumAll / (float)filled;
                    float prominence = peakAvg - mean;
                    int conf = (int)(prominence * 12.0f + (float)filled * 1.2f);
                    if (conf < 25)
                        conf = 25;
                    if (conf > 99)
                        conf = 99;
                    // 有峰值就锁定（ESP 天线弱方向性时仍给可用方位）
                    radarSetAz(i, angDeg, (uint8_t)conf, true);
                }
                radarRelayout();
                radarDrawPanel();
                radarDrawSweepAndBlips();
            } else {
                int bin = (int)((elapsed * (unsigned long)RADAR_CAL_BINS) / RADAR_CAL_MS);
                if (bin < 0)
                    bin = 0;
                if (bin >= RADAR_CAL_BINS)
                    bin = RADAR_CAL_BINS - 1;
                float faceAng = ((float)bin + 0.5f) * (360.0f / (float)RADAR_CAL_BINS);

                for (int i = 0; i < radarCount; i++) {
                    // 用对端原始 RSSI 入仓，避免 EMA 抹平峰值
                    int8_t raw = radarFreshRssi(radarContacts[i].id, radarContacts[i].rssi);
                    if (raw != 0) {
                        radarContacts[i].rssi = raw;
                        int16_t sum = radarContacts[i].calibSum[bin] + (int16_t)raw;
                        uint8_t n = radarContacts[i].calibN[bin];
                        if (n < 250) {
                            radarContacts[i].calibSum[bin] = sum;
                            radarContacts[i].calibN[bin] = (uint8_t)(n + 1);
                        }
                    }
                    // 实时预览：当前最强 bin 的方位（光点会跟着跳）
                    int bestB = -1;
                    float bestA = -999.0f;
                    for (int b = 0; b < RADAR_CAL_BINS; b++) {
                        if (radarContacts[i].calibN[b] == 0)
                            continue;
                        float a = (float)radarContacts[i].calibSum[b] /
                                  (float)radarContacts[i].calibN[b];
                        if (a > bestA) {
                            bestA = a;
                            bestB = b;
                        }
                    }
                    if (bestB >= 0) {
                        float preview = ((float)bestB + 0.5f) * (360.0f / (float)RADAR_CAL_BINS);
                        radarSetAz(i, preview, 40, false);
                        radarContacts[i].azValid = true;
                    }
                }

                tft.fillRect(0, SCREEN_HEIGHT - 18, 214, 18, tft.color565(6, 32, 26));
                char cb[44];
                snprintf(cb, sizeof(cb), "FACE FWD=%03.0f  %lu%%", (double)faceAng,
                         elapsed * 100UL / RADAR_CAL_MS);
                tft.setTextColor(TFT_YELLOW, tft.color565(6, 32, 26));
                tft.drawString(cb, 4, SCREEN_HEIGHT - 14, 1);
            }
        }

        if (millis() - radarLastBlipMs >= 40UL) {
            radarLastBlipMs = millis();
            radarSweepDeg += 3.0f;
            if (radarSweepDeg >= 360.0f)
                radarSweepDeg -= 360.0f;
            for (int i = 0; i < radarCount; i++) {
                RadarContact_t &c = radarContacts[i];
                float t = c.targetDistM > 0.01f ? c.targetDistM : c.distM;
                c.distM = c.distM * 0.82f + t * 0.18f;
                // 锁定后保持径向
                if (c.azLocked) {
                    float a = c.angleDeg * 0.01745329252f;
                    c.posX = sinf(a) * c.distM;
                    c.posY = cosf(a) * c.distM;
                }
            }
            if (!radarFrameReady)
                radarDrawStaticFrame();
            radarDrawSweepAndBlips();
        }
        if (millis() - radarLastLayoutMs > 1200UL) {
            radarLastLayoutMs = millis();
            radarSyncFromPeers();
            if (!radarCalibrating)
                radarRelayout();
        }
        if (millis() - radarLastLinkTxMs > 1500UL) {
            radarLastLinkTxMs = millis();
            radarBroadcastLinks();
        }
        if (millis() - radarLastPanelMs > 800UL) {
            radarLastPanelMs = millis();
            radarDrawPanel();
        }
    } else if (arcadeView == ARCADE_REACT) {
        static ReactPhase_e lastPhase = REACT_IDLE;
        // 只有房主本地计时触发 GO，并广播；其他人等 READY
        if (reactIsHost && reactPhase == REACT_WAIT && millis() >= reactGoAt) {
            reactPhase = REACT_GO;
            reactGoShownAt = millis();
            reactTapped = false;
            reactMyMs = -1;
            sendGamePacket(GAME_KIND_REACT, GAME_OP_READY, (int16_t)REACT_GO, 0, 0, 0);
            gameUiDirty = true;
        }
        if (reactPhase == REACT_GO && reactIsHost && reactGoShownAt &&
            millis() - reactGoShownAt > 2500UL) {
            int best = 9999, bestIdx = -1;
            for (int i = 0; i < liveScoreN; i++) {
                if (liveScores[i].extra > 0 && liveScores[i].extra < best) {
                    best = liveScores[i].extra;
                    bestIdx = i;
                }
            }
            if (bestIdx >= 0) {
                liveScores[bestIdx].score++;
                if (strcmp(liveScores[bestIdx].id, localDeviceId) == 0)
                    reactMyScore = liveScores[bestIdx].score;
                sendGamePacket(GAME_KIND_REACT, GAME_OP_SCORE,
                               liveScores[bestIdx].score, (int16_t)best,
                               (int16_t)idHash16(liveScores[bestIdx].id), 1);
                const char *ids[LIVE_MAX];
                int16_t sc[LIVE_MAX];
                for (int i = 0; i < liveScoreN; i++) {
                    ids[i] = liveScores[i].id;
                    sc[i] = liveScores[i].score;
                }
                gameTryFinishMatch(GAME_KIND_REACT, ids, sc, liveScoreN);
            }
            reactPhase = REACT_DONE;
            gameUiDirty = true;
        }
        if (gameUiDirty || reactPhase != lastPhase) {
            lastPhase = reactPhase;
            gameUiDirty = false;
            drawReactScreen();
            overlayGameInviteIfAny();
        }
    } else if (arcadeView == ARCADE_BRIDGE) {
        if (matchEnded) {
            static bool painted = false;
            if (bridgeDirty || !painted) {
                painted = true;
                drawBridgeScreen();
            }
            return;
        }
        if (liveScoreN < 2) {
            static unsigned long lastB = 0;
            if (millis() - lastB > 800) {
                lastB = millis();
                drawBridgeScreen();
            }
            return;
        }
        // grow stick while holding
        if (stickGrowing && bridgePhase == BRIDGE_AIM && arcadeFingerDown) {
            if (millis() - stickGrowLast >= 16) {
                stickGrowLast = millis();
                stickLen += 3;
                if (stickLen > 220)
                    stickLen = 220;
                bridgeDirty = true;
            }
        }
        if (bridgePhase == BRIDGE_FALL) {
            stickAngle -= 8.0f;
            if (stickAngle <= 0.0f) {
                stickAngle = 0.0f;
                // judge landing
                int tip = platL + platLW + stickLen;
                int leftOk = platR;
                int rightOk = platR + platRW;
                if (tip >= leftOk && tip <= rightOk) {
                    bridgePhase = BRIDGE_WALK;
                } else {
                    bridgePhase = BRIDGE_OVER;
                    liveSet(localDeviceId, (int16_t)bridgeScore, 0);
                    sendGamePacket(GAME_KIND_BRIDGE, GAME_OP_SCORE, (int16_t)bridgeScore, 0, 0, 0);
                }
            }
            bridgeDirty = true;
        }
        if (bridgePhase == BRIDGE_WALK) {
            float target = (float)(platR + platRW - 16);
            dinoX += 4.0f;
            if (dinoX >= target) {
                dinoX = target;
                bridgeScore++;
                liveSet(localDeviceId, (int16_t)bridgeScore, 1);
                sendGamePacket(GAME_KIND_BRIDGE, GAME_OP_SCORE, (int16_t)bridgeScore, 1, 0, 0);
                const char *ids[LIVE_MAX];
                int16_t sc[LIVE_MAX];
                for (int i = 0; i < liveScoreN; i++) {
                    ids[i] = liveScores[i].id;
                    sc[i] = liveScores[i].score;
                }
                if (!gameTryFinishMatch(GAME_KIND_BRIDGE, ids, sc, liveScoreN))
                    bridgeGenNext(false);
                else {
                    bridgePhase = BRIDGE_OVER;
                    bridgeDirty = true;
                }
            }
            bridgeDirty = true;
        }
        if (bridgeDirty) {
            drawBridgeScreen();
            overlayGameInviteIfAny();
        }
    }
    updateRecruitToast();
}

bool handleGameArcadeTouch(int x, int y)
{
    if (currentUIState != UI_STATE_ARCADE)
        return false;
    bool rising = !arcadeFingerDown;
    arcadeFingerDown = true;

    // 游戏内 INV：优先处理（含桥游戏按住逻辑之前）
    if (rising && hitInvChip(x, y)) {
        uint8_t k = arcadeRecruitKind();
        if (k != GAME_KIND_NONE) {
            sendGameRecruit(k);
            return true;
        }
    }

    // bridge: allow hold (not only rising) for grow start
    if (arcadeView == ARCADE_BRIDGE) {
        if (hitBack(x, y) && rising) {
            leaveCurrentGame();
            arcadeView = ARCADE_LOBBY;
            drawGameLobby();
            return true;
        }
        if (matchEnded && rising) {
            matchResetForNewGame();
            bridgeRestart();
            drawBridgeScreen();
            return true;
        }
        if (bridgePhase == BRIDGE_OVER && rising) {
            if (liveScoreN < 2)
                return true;
            bridgeRestart();
            drawBridgeScreen();
            return true;
        }
        if (bridgePhase == BRIDGE_AIM) {
            if (liveScoreN < 2)
                return true;
            // 避开顶栏 INV/BACK
            if (y < 28)
                return true;
            if (rising) {
                stickGrowing = true;
                stickGrowLast = millis();
                if (stickLen < 2)
                    stickLen = 2;
            }
            return true;
        }
        return true;
    }

    if (!rising)
        return true;

    if (hitBack(x, y)) {
        if (arcadeView == ARCADE_LOBBY)
            hideGameArcade();
        else if (arcadeView == ARCADE_MY_STATS || arcadeView == ARCADE_BOARD ||
                 arcadeView == ARCADE_PEER_STATS) {
            arcadeView = ARCADE_LOBBY;
            drawGameLobby();
        } else {
            leaveCurrentGame();
            arcadeView = ARCADE_LOBBY;
            drawGameLobby();
        }
        return true;
    }

    if (arcadeView == ARCADE_MY_STATS) {
        return true;
    }

    if (arcadeView == ARCADE_PEER_STATS) {
        return true;
    }

    if (arcadeView == ARCADE_BOARD) {
        if (y > SCREEN_HEIGHT - 18) {
            statsScrollY += 44;
            drawLeaderboard();
            return true;
        }
        // rebuild ranking to resolve tap
        struct Row {
            const char *id;
            uint16_t w, l;
        };
        Row rows[PEER_STATS_MAX + 1];
        int n = 0;
        rows[n].id = localDeviceId;
        rows[n].w = statsTotalWins(localStats);
        rows[n].l = statsTotalLosses(localStats);
        n++;
        for (int i = 0; i < peerStatsN && n < PEER_STATS_MAX + 1; i++) {
            rows[n].id = peerStats[i].id;
            rows[n].w = statsTotalWins(peerStats[i]);
            rows[n].l = statsTotalLosses(peerStats[i]);
            n++;
        }
        for (auto const &kv : peerInfoMap) {
            if (n >= PEER_STATS_MAX + 1)
                break;
            const PeerInfo_t &p = kv.second;
            if (!peerVisibleForLocalMode(p) || !p.deviceId[0])
                continue;
            if (strcmp(p.deviceId, localDeviceId) == 0)
                continue;
            bool exists = false;
            for (int i = 0; i < n; i++) {
                if (strcmp(rows[i].id, p.deviceId) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                rows[n].id = p.deviceId;
                rows[n].w = 0;
                rows[n].l = 0;
                n++;
            }
        }
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                if (rows[j].w > rows[i].w || (rows[j].w == rows[i].w && rows[j].l < rows[i].l)) {
                    Row t = rows[i];
                    rows[i] = rows[j];
                    rows[j] = t;
                }
            }
        }
        int ry = 48 - statsScrollY;
        for (int i = 0; i < n; i++) {
            if (y >= ry - 2 && y <= ry + 18) {
                strncpy(statsPeerId, rows[i].id, DEVICE_ID_MAX_LEN);
                statsPeerId[DEVICE_ID_MAX_LEN] = '\0';
                arcadeView = (strcmp(statsPeerId, localDeviceId) == 0)
                                 ? ARCADE_MY_STATS
                                 : ARCADE_PEER_STATS;
                if (arcadeView == ARCADE_MY_STATS)
                    drawStatsProfile(localDeviceId, true);
                else
                    drawStatsProfile(statsPeerId, false);
                return true;
            }
            ry += 22;
        }
        return true;
    }

    if (arcadeView == ARCADE_LOBBY) {
        // 我的 / 榜
        if (y >= 4 && y <= 22) {
            if (x >= 148 && x <= 192) {
                arcadeView = ARCADE_MY_STATS;
                statsScrollY = 0;
                drawStatsProfile(localDeviceId, true);
                return true;
            }
            if (x >= 196 && x <= 232) {
                arcadeView = ARCADE_BOARD;
                statsScrollY = 0;
                statsBroadcast();
                drawLeaderboard();
                return true;
            }
        }
        if (y > SCREEN_HEIGHT - 20) {
            lobbyScrollY += LOBBY_ITEM_H;
            drawGameLobby();
            return true;
        }
        if (y < 28)
            return true;
        int listTop = 28;
        int idx = (y - listTop + lobbyScrollY) / LOBBY_ITEM_H;
        if (idx >= 0 && idx < LOBBY_N) {
            int rowY = listTop + idx * LOBBY_ITEM_H - lobbyScrollY;
            if (x >= SCREEN_WIDTH - 78 && x <= SCREEN_WIDTH - 42 &&
                y >= rowY + 10 && y <= rowY + 28) {
                static const uint8_t kinds[5] = {
                    GAME_KIND_MOLE, GAME_KIND_RPS, GAME_KIND_RADAR,
                    GAME_KIND_REACT, GAME_KIND_BRIDGE};
                sendGameRecruit(kinds[idx]);
                return true;
            }
            if (idx == 0)
                enterMole();
            else if (idx == 1)
                enterRps();
            else if (idx == 2)
                enterRadar();
            else if (idx == 3)
                enterReact();
            else
                enterBridge();
        }
        return true;
    }

    // Radar: CAL + INV + panel recruit + 点盘面设方位
    if (arcadeView == ARCADE_RADAR) {
        if (x >= 164 && x <= 208 && y >= 3 && y <= 22) {
            radarCalibrating = true;
            radarCalibStart = millis();
            radarSyncFromPeers();
            for (int i = 0; i < radarCount; i++) {
                for (int b = 0; b < RADAR_CAL_BINS; b++) {
                    radarContacts[i].calibSum[b] = 0;
                    radarContacts[i].calibN[b] = 0;
                }
                radarContacts[i].azLocked = false;
                radarContacts[i].azValid = false;
                radarContacts[i].azConf = 0;
            }
            return true;
        }
        // 右侧列表点选目标
        if (x >= 216) {
            int ly = 42;
            for (int i = 0; i < radarCount; i++) {
                if (y >= ly && y < ly + 52) {
                    radarFocusIdx = i;
                    radarDrawPanel();
                    return true;
                }
                ly += 52;
            }
            if (y >= SCREEN_HEIGHT - 40) {
                sendGameRecruit(GAME_KIND_RADAR);
                return true;
            }
            return true;
        }
        // 点雷达盘面 → 把当前焦点目标设到该方位（可手动获得方位）
        {
            int dx = x - RADAR_CX;
            int dy = RADAR_CY - y; // 屏上=前方
            int rr = dx * dx + dy * dy;
            int R2 = (RADAR_R + 6) * (RADAR_R + 6);
            if (rr <= R2 && rr >= 36 && radarCount > 0) {
                float ang = atan2f((float)dx, (float)dy) * 57.2957795f;
                if (ang < 0)
                    ang += 360.0f;
                if (radarFocusIdx < 0 || radarFocusIdx >= radarCount)
                    radarFocusIdx = 0;
                radarSetAz(radarFocusIdx, ang, 98, true);
                radarCalibrating = false;
                radarDrawPanel();
                radarDrawSweepAndBlips();
                // 底部提示
                tft.fillRect(0, SCREEN_HEIGHT - 18, 214, 18, tft.color565(6, 32, 26));
                char tip[40];
                snprintf(tip, sizeof(tip), "SET %s %03.0f",
                         radarCardinal(ang), (double)ang);
                tft.setTextColor(TFT_GREENYELLOW, tft.color565(6, 32, 26));
                tft.drawString(tip, 4, SCREEN_HEIGHT - 14, 1);
                return true;
            }
        }
        return true;
    }

    // 旧 INV 条已由 hitInvChip 统一处理
    if (arcadeView == ARCADE_RPS) {
        if (matchEnded) {
            // rematch: reset scores
            matchResetForNewGame();
            for (int i = 0; i < rpsCount; i++)
                rpsPlayers[i].score = 0;
            rpsResetRound();
            drawRpsDynamic(true);
            overlayGameInviteIfAny();
            return true;
        }
        if (rpsCount < 2)
            return true;
        uint8_t choices[3] = {RPS_ROCK, RPS_PAPER, RPS_SCISSORS};
        for (int i = 0; i < 3; i++) {
            int bx = 12 + i * 102;
            if (x >= bx && x <= bx + 96 && y >= 32 && y <= 68) {
                if (!rpsRevealed) {
                    rpsMyChoice = choices[i];
                    int me = rpsEnsure(localDeviceId);
                    if (me >= 0) {
                        rpsPlayers[me].choice = rpsMyChoice;
                        sendGamePacket(GAME_KIND_RPS, GAME_OP_ACTION, (int16_t)rpsMyChoice,
                                       (int16_t)rpsPlayers[me].score, 0, 0);
                    }
                    rpsBroadcastRoster();
                    rpsTryReveal();
                    drawRpsDynamic(false);
                    overlayGameInviteIfAny();
                }
                return true;
            }
        }
        if (x >= 12 && x <= 152 && y >= 90 && y <= 112 && rpsRevealed) {
            rpsResetRound();
            sendGamePacket(GAME_KIND_RPS, GAME_OP_READY, 0, 0, 0, 0);
            drawRpsDynamic(true);
            overlayGameInviteIfAny();
            return true;
        }
        return true;
    }

    if (arcadeView == ARCADE_MOLE) {
        if (matchEnded) {
            matchResetForNewGame();
            moleMyScore = 0;
            for (int i = 0; i < moleScoreN; i++)
                moleScores[i].score = 0;
            moleDrawnMyScore = -999;
            moleScoreSig = 0;
            if (moleIsHost)
                moleAdvance();
            drawMoleDynamic(true);
            overlayGameInviteIfAny();
            return true;
        }
        if (moleScoreN < 2)
            return true;
        for (int i = 0; i < MOLE_CELLS; i++) {
            int cx, cy;
            moleCellCenter(i, cx, cy);
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= 22 * 22) {
                if (moleActive == i) {
                    moleMyScore++;
                    moleSetScore(localDeviceId, (int16_t)moleMyScore);
                    moleActive = 0xFF;
                    sendGamePacket(GAME_KIND_MOLE, GAME_OP_SCORE, (int16_t)moleMyScore, (int16_t)i, 0, 0);
                    moleCheckWin();
                    if (!matchEnded && moleIsHost)
                        moleAdvance();
                    drawMoleDynamic(false);
                    overlayGameInviteIfAny();
                }
                return true;
            }
        }
        return true;
    }

    if (arcadeView == ARCADE_REACT) {
        if (matchEnded) {
            matchResetForNewGame();
            reactMyScore = 0;
            for (int i = 0; i < liveScoreN; i++) {
                liveScores[i].score = 0;
                liveScores[i].extra = 0;
            }
            reactPhase = REACT_IDLE;
            drawReactScreen();
            return true;
        }
        if (liveScoreN < 2)
            return true;
        if (reactPhase == REACT_IDLE || reactPhase == REACT_DONE) {
            static unsigned long lastStart = 0;
            if (millis() - lastStart < 400)
                return true; // 防连点闪退
            lastStart = millis();
            reactIsHost = true;
            for (int i = 0; i < liveScoreN; i++)
                liveScores[i].extra = 0;
            reactMyMs = -1;
            reactTapped = false;
            reactStartWait();
            drawReactScreen();
            return true;
        }
        if (reactPhase == REACT_WAIT) {
            reactTapped = true;
            reactMyMs = -2;
            reactPhase = REACT_DONE;
            liveSet(localDeviceId, (int16_t)reactMyScore, -2);
            sendGamePacket(GAME_KIND_REACT, GAME_OP_ACTION, -2, (int16_t)reactMyScore, 0, 0);
            drawReactScreen();
            return true;
        }
        if (reactPhase == REACT_GO && !reactTapped) {
            reactTapped = true;
            unsigned long dt = (reactGoShownAt > 0) ? (millis() - reactGoShownAt) : 0;
            if (dt > 9999UL)
                dt = 9999UL;
            reactMyMs = (int16_t)dt;
            liveSet(localDeviceId, (int16_t)reactMyScore, reactMyMs);
            sendGamePacket(GAME_KIND_REACT, GAME_OP_ACTION, reactMyMs, (int16_t)reactMyScore, 0, 0);
            drawReactScreen();
            return true;
        }
        return true;
    }

    return true;
}

void gameArcadeTouchReleased()
{
    if (arcadeView == ARCADE_BRIDGE && stickGrowing && bridgePhase == BRIDGE_AIM) {
        stickGrowing = false;
        if (stickLen < 4)
            stickLen = 4;
        bridgePhase = BRIDGE_FALL;
        bridgeDirty = true;
    }
    arcadeFingerDown = false;
}

void processIncomingGameQueue()
{
    while (gameQTail != gameQHead) {
        GameIncoming_t item = gameQ[gameQTail];
        gameQTail = (uint8_t)((gameQTail + 1) % GAME_Q_MAX);
        applyIncomingGamePacket(item.pkt, item.rssi);
    }
    // 禁止整屏 redrawArcade 造成闪频：按游戏软刷新
    if (gameUiDirty && currentUIState == UI_STATE_ARCADE) {
        gameUiDirty = false;
        if (arcadeView == ARCADE_RPS) {
            rpsSoftDirty = true;
            drawRpsDynamic(false);
        } else if (arcadeView == ARCADE_MOLE) {
            drawMoleDynamic(false);
        } else if (arcadeView == ARCADE_REACT) {
            drawReactScreen();
        } else if (arcadeView == ARCADE_BRIDGE) {
            drawBridgeScreen();
        } else if (arcadeView == ARCADE_BOARD) {
            drawLeaderboard();
        } else if (arcadeView == ARCADE_PEER_STATS) {
            drawStatsProfile(statsPeerId[0] ? statsPeerId : localDeviceId, false);
        } else if (arcadeView == ARCADE_RADAR) {
            radarLastPanelMs = 0; // next tick panel only
        } else {
            redrawArcade();
        }
        if (arcadeView != ARCADE_RADAR)
            overlayGameInviteIfAny();
    }
}

// 仅更新状态；禁止在此函数内假设可安全画屏（由队列在主循环调用）
static void applyIncomingGamePacket(const GamePacket_t &pkt, int8_t rssi)
{
    if (pkt.magic != GAME_PKT_MAGIC)
        return;
    if (!pkt.senderId[0])
        return;
    if (strcmp(pkt.senderId, localDeviceId) == 0)
        return;

    if (pkt.opcode == GAME_OP_INVITE) {
        // v0 可冗余带 kind；以 pkt.gameKind 为准
        showGameInviteDialog(pkt.senderId, pkt.gameKind, millis() + GAME_INVITE_TIMEOUT_MS);
        return;
    }

    if (pkt.opcode == GAME_OP_STATS) {
        statsApplyRemote(pkt.senderId, pkt.v0, pkt.v1, pkt.v2, pkt.v3);
        if (arcadeView == ARCADE_BOARD || arcadeView == ARCADE_PEER_STATS)
            gameUiDirty = true;
        return;
    }

    if (pkt.gameKind == GAME_KIND_RADAR) {
        if (pkt.opcode == GAME_OP_LINK) {
            // sender reports RSSI to targetHash in v0, rssi in v1
            // resolve target id among known contacts / peers
            uint16_t th = (uint16_t)pkt.v0;
            const char *tid = nullptr;
            for (int i = 0; i < radarCount; i++) {
                if (idHash16(radarContacts[i].id) == th) {
                    tid = radarContacts[i].id;
                    break;
                }
            }
            if (!tid) {
                for (auto const &kv : peerInfoMap) {
                    if (kv.second.deviceId[0] && idHash16(kv.second.deviceId) == th) {
                        tid = kv.second.deviceId;
                        break;
                    }
                }
            }
            radarUpsert(pkt.senderId, rssi ? rssi : (int8_t)-70, 0);
            if (tid)
                radarSetLink(pkt.senderId, tid, (int8_t)pkt.v1);
            if (arcadeView == ARCADE_RADAR) {
                radarRelayout();
                // panel only — avoid full flicker
                radarLastPanelMs = 0;
            }
            return;
        }
        radarUpsert(pkt.senderId, rssi ? rssi : (int8_t)-70, 0);
        if (arcadeView == ARCADE_RADAR)
            radarLastPanelMs = 0; // refresh panel next tick
        return;
    }

    if (pkt.gameKind == GAME_KIND_RPS) {
        // 未进入 RPS 时仍接受 JOIN/SCORE，便于进房后立刻对齐；其它包忽略
        if (arcadeView != ARCADE_RPS &&
            pkt.opcode != GAME_OP_JOIN && pkt.opcode != GAME_OP_SCORE)
            return;
        int i = rpsEnsure(pkt.senderId);
        if (i < 0)
            return;
        rpsPlayers[i].lastMs = millis();
        if (pkt.opcode == GAME_OP_LEAVE) {
            if (rpsCount > 0) {
                rpsPlayers[i] = rpsPlayers[rpsCount - 1];
                rpsCount--;
            }
            rpsSoftDirty = true;
        } else if (pkt.opcode == GAME_OP_JOIN) {
            rpsSoftDirty = true;
            if (arcadeView == ARCADE_RPS)
                rpsBroadcastRoster(); // 回告花名册，双方列表同步
        } else if (pkt.opcode == GAME_OP_ACTION) {
            rpsPlayers[i].choice = (uint8_t)pkt.v0;
            if (pkt.v1 >= 0)
                rpsPlayers[i].score = (uint8_t)pkt.v1;
            rpsSoftDirty = true;
            rpsTryReveal();
        } else if (pkt.opcode == GAME_OP_SCORE) {
            rpsApplyPeerRoster(pkt.senderId, pkt.v0, pkt.v1, pkt.v2);
            rpsTryReveal();
            rpsCheckWin();
        } else if (pkt.opcode == GAME_OP_READY) {
            rpsResetRound();
        }
        if (arcadeView == ARCADE_RPS)
            gameUiDirty = true;
        return;
    }

    if (pkt.gameKind == GAME_KIND_MOLE) {
        if (pkt.opcode == GAME_OP_JOIN) {
            moleSetScore(pkt.senderId, 0);
            if (moleIsHost && arcadeView == ARCADE_MOLE) {
                sendGamePacket(GAME_KIND_MOLE, GAME_OP_STATE, (int16_t)moleActive,
                               (int16_t)(moleSeed & 0xFFFF), (int16_t)moleMyScore, 1);
                sendGamePacket(GAME_KIND_MOLE, GAME_OP_SCORE, (int16_t)moleMyScore, 0, 0, 0);
            }
        } else if (pkt.opcode == GAME_OP_STATE) {
            moleActive = (uint8_t)pkt.v0;
            moleSeed = ((uint32_t)(uint16_t)pkt.v1) | 0x10000u;
            moleIsHost = false;
            moleNextFlip = millis() + 2000;
        } else if (pkt.opcode == GAME_OP_SCORE) {
            moleSetScore(pkt.senderId, pkt.v0);
            if ((uint8_t)pkt.v1 == moleActive)
                moleActive = 0xFF;
            moleCheckWin();
        } else if (pkt.opcode == GAME_OP_LEAVE) {
            int si = moleFindScore(pkt.senderId);
            if (si >= 0 && moleScoreN > 0) {
                moleScores[si] = moleScores[moleScoreN - 1];
                moleScoreN--;
            }
        }
        if (arcadeView == ARCADE_MOLE)
            gameUiDirty = true;
        return;
    }

    if (pkt.gameKind == GAME_KIND_REACT) {
        if (pkt.opcode == GAME_OP_JOIN) {
            liveSet(pkt.senderId, pkt.v0, 0);
            if (arcadeView == ARCADE_REACT) {
                // 回告在场分数，勿回 JOIN（防死循环）
                sendGamePacket(GAME_KIND_REACT, GAME_OP_SCORE, (int16_t)reactMyScore,
                               (reactMyMs > 0) ? reactMyMs : (int16_t)0, 0, 0);
            }
        } else if (pkt.opcode == GAME_OP_LEAVE) {
            liveRemove(pkt.senderId);
        } else if (pkt.opcode == GAME_OP_STATE) {
            // 对方开局 WAIT：跟随时钟，勿在回调风暴里重入
            if (arcadeView != ARCADE_REACT)
                return;
            reactIsHost = false;
            reactPhase = REACT_WAIT;
            reactTapped = false;
            reactMyMs = -1;
            int16_t delayMs = pkt.v1;
            if (delayMs < 400)
                delayMs = 400;
            if (delayMs > 6000)
                delayMs = 6000;
            reactGoAt = millis() + (unsigned long)delayMs;
            liveSet(pkt.senderId, 0, 0);
            liveSet(localDeviceId, (int16_t)reactMyScore, 0);
        } else if (pkt.opcode == GAME_OP_READY) {
            if (arcadeView != ARCADE_REACT)
                return;
            // 仅跟随 host 的 GO，避免每人各发 READY 互相打断
            if (!reactIsHost) {
                reactPhase = REACT_GO;
                reactGoShownAt = millis();
                reactTapped = false;
                reactMyMs = -1;
            }
        } else if (pkt.opcode == GAME_OP_ACTION) {
            liveSet(pkt.senderId, pkt.v1, pkt.v0);
        } else if (pkt.opcode == GAME_OP_SCORE) {
            if (pkt.v3 == 1) {
                uint16_t wh = (uint16_t)pkt.v2;
                for (int i = 0; i < liveScoreN; i++) {
                    if (idHash16(liveScores[i].id) == wh) {
                        liveScores[i].score = pkt.v0;
                        if (strcmp(liveScores[i].id, localDeviceId) == 0)
                            reactMyScore = pkt.v0;
                        break;
                    }
                }
            } else {
                liveSet(pkt.senderId, pkt.v0, pkt.v1);
            }
            {
                const char *ids[LIVE_MAX];
                int16_t sc[LIVE_MAX];
                for (int i = 0; i < liveScoreN; i++) {
                    ids[i] = liveScores[i].id;
                    sc[i] = liveScores[i].score;
                }
                gameTryFinishMatch(GAME_KIND_REACT, ids, sc, liveScoreN);
            }
        }
        if (arcadeView == ARCADE_REACT)
            gameUiDirty = true;
        return;
    }

    if (pkt.gameKind == GAME_KIND_BRIDGE) {
        if (pkt.opcode == GAME_OP_JOIN) {
            liveSet(pkt.senderId, pkt.v0, 1);
            if (arcadeView == ARCADE_BRIDGE) {
                sendGamePacket(GAME_KIND_BRIDGE, GAME_OP_SCORE, (int16_t)bridgeScore, 1, 0, 0);
            }
        } else if (pkt.opcode == GAME_OP_LEAVE) {
            liveRemove(pkt.senderId);
        } else if (pkt.opcode == GAME_OP_SCORE) {
            liveSet(pkt.senderId, pkt.v0, pkt.v1);
            const char *ids[LIVE_MAX];
            int16_t sc[LIVE_MAX];
            for (int i = 0; i < liveScoreN; i++) {
                ids[i] = liveScores[i].id;
                sc[i] = liveScores[i].score;
            }
            if (gameTryFinishMatch(GAME_KIND_BRIDGE, ids, sc, liveScoreN))
                bridgeDirty = true;
        }
        if (arcadeView == ARCADE_BRIDGE)
            gameUiDirty = true;
    }
}

// ===== Game recruit / invite (all-page overlay) =====
static bool gameInviteVisible = false;
static char gameInviteFromId[DEVICE_ID_MAX_LEN + 1] = {0};
static uint8_t gameInviteKind = GAME_KIND_NONE;
static unsigned long gameInviteDeadlineMs = 0;
static unsigned long gameInviteLastSec = 0;
static bool gameInviteFingerDown = false;

const char *gameKindDisplayName(uint8_t kind)
{
    switch (kind) {
    case GAME_KIND_MOLE: return "WHACK-A-MOLE";
    case GAME_KIND_RPS: return "RPS ARENA";
    case GAME_KIND_RADAR: return "RF RADAR";
    case GAME_KIND_REACT: return "REACTION";
    case GAME_KIND_BRIDGE: return "DINO BRIDGE";
    default: return "GAME";
    }
}

static bool recruitToastVisible = false;
static unsigned long recruitToastUntilMs = 0;

static void drawRecruitToast()
{
    if (!recruitToastVisible || !isScreenOn)
        return;
    tft.fillRect(0, 0, SCREEN_WIDTH, 20, tft.color565(90, 70, 20));
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_YELLOW, tft.color565(90, 70, 20));
    tft.drawString("RECRUIT SENT", SCREEN_WIDTH / 2, 10, 1);
    tft.setTextDatum(TL_DATUM);
}

static void clearRecruitToast()
{
    if (!recruitToastVisible)
        return;
    recruitToastVisible = false;
    // 恢复当前界面顶栏（软重绘）
    if (currentUIState == UI_STATE_ARCADE)
        redrawArcade();
    else if (isScreenOn)
        redrawMainScreen();
}

static void updateRecruitToast()
{
    if (!recruitToastVisible)
        return;
    if ((long)(millis() - recruitToastUntilMs) >= 0) {
        clearRecruitToast();
        return;
    }
    // 约 250ms 重贴一次，避免被游戏层盖住又不过度闪
    static unsigned long lastDraw = 0;
    if (millis() - lastDraw > 250UL) {
        lastDraw = millis();
        drawRecruitToast();
    }
}

static bool hitInvChip(int x, int y)
{
    if (y < 2 || y > 26)
        return false;
    if (arcadeView == ARCADE_RADAR)
        return x >= 118 && x <= 162; // 雷达顶栏 INV
    // 其它游戏：INV 在标题右侧
    return x >= 196 && x <= 248;
}

static uint8_t arcadeRecruitKind()
{
    switch (arcadeView) {
    case ARCADE_MOLE: return GAME_KIND_MOLE;
    case ARCADE_RPS: return GAME_KIND_RPS;
    case ARCADE_RADAR: return GAME_KIND_RADAR;
    case ARCADE_REACT: return GAME_KIND_REACT;
    case ARCADE_BRIDGE: return GAME_KIND_BRIDGE;
    default: return GAME_KIND_NONE;
    }
}

void sendGameRecruit(uint8_t gameKind)
{
    if (gameKind == GAME_KIND_NONE)
        return;
    sendGamePacket(gameKind, GAME_OP_INVITE, (int16_t)gameKind, 0, 0, 0);
    recruitToastVisible = true;
    recruitToastUntilMs = millis() + 2200UL;
    drawRecruitToast();
    overlayGameInviteIfAny();
}

void showGameInviteDialog(const char *fromId, uint8_t gameKind, unsigned long deadlineMs)
{
    strncpy(gameInviteFromId, fromId ? fromId : "Peer", DEVICE_ID_MAX_LEN);
    gameInviteFromId[DEVICE_ID_MAX_LEN] = '\0';
    gameInviteKind = gameKind;
    gameInviteDeadlineMs = deadlineMs;
    gameInviteVisible = true;
    gameInviteFingerDown = false;
    gameInviteLastSec = 0;
    drawGameInviteDialog();
}

void hideGameInviteDialog()
{
    if (!gameInviteVisible)
        return;
    gameInviteVisible = false;
    gameInviteFingerDown = false;
    if (currentUIState == UI_STATE_ARCADE)
        redrawArcade();
    else
        redrawMainScreen();
}

bool isGameInviteDialogVisible()
{
    return gameInviteVisible;
}

void drawGameInviteDialog()
{
    if (!gameInviteVisible || !isScreenOn)
        return;
    long remain = (long)(gameInviteDeadlineMs - millis());
    if (remain < 0)
        remain = 0;
    unsigned long sec = (unsigned long)(remain / 1000UL);

    const uint16_t panelBg = tft.color565(28, 36, 52);
    const uint16_t rejectBg = tft.color565(100, 45, 45);
    const uint16_t acceptBg = tft.color565(35, 130, 85);
    const uint16_t warnBg = tft.color565(180, 120, 20);

    // 半透明感：先压暗中部区域，再画弹窗，保证盖住游戏动态层
    tft.fillRect(GAME_INVITE_POPUP_X - 8, GAME_INVITE_POPUP_Y - 4,
                 GAME_INVITE_POPUP_W + 16, GAME_INVITE_POPUP_H + 8,
                 tft.color565(0, 0, 0));

    tft.fillRoundRect(GAME_INVITE_POPUP_X, GAME_INVITE_POPUP_Y,
                      GAME_INVITE_POPUP_W, GAME_INVITE_POPUP_H, 8, panelBg);
    tft.drawRoundRect(GAME_INVITE_POPUP_X, GAME_INVITE_POPUP_Y,
                      GAME_INVITE_POPUP_W, GAME_INVITE_POPUP_H, 8, TFT_YELLOW);
    tft.drawRoundRect(GAME_INVITE_POPUP_X + 1, GAME_INVITE_POPUP_Y + 1,
                      GAME_INVITE_POPUP_W - 2, GAME_INVITE_POPUP_H - 2, 7, TFT_ORANGE);

    // 醒目黄条标题
    tft.fillRoundRect(GAME_INVITE_POPUP_X + 2, GAME_INVITE_POPUP_Y + 2,
                      GAME_INVITE_POPUP_W - 4, 22, 6, warnBg);
    tft.setTextColor(TFT_BLACK, warnBg);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("GAME INVITE", SCREEN_WIDTH / 2, GAME_INVITE_POPUP_Y + 6, 2);

    int tw = cnTextWidth(gameInviteFromId);
    cnDrawUtf8(tft, GAME_INVITE_POPUP_X + (GAME_INVITE_POPUP_W - tw) / 2,
               GAME_INVITE_POPUP_Y + 30, gameInviteFromId, TFT_WHITE);

    char line[48];
    snprintf(line, sizeof(line), "-> %s", gameKindDisplayName(gameInviteKind));
    tft.setTextColor(TFT_CYAN, panelBg);
    tft.drawString(line, SCREEN_WIDTH / 2, GAME_INVITE_POPUP_Y + 50, 1);

    snprintf(line, sizeof(line), "%lus left", sec);
    tft.setTextColor(TFT_YELLOW, panelBg);
    tft.drawString(line, SCREEN_WIDTH / 2, GAME_INVITE_POPUP_Y + 64, 1);
    tft.setTextDatum(TL_DATUM);

    int btnY = GAME_INVITE_POPUP_Y + GAME_INVITE_POPUP_H - 34;
    int rejectX = GAME_INVITE_POPUP_X + 16;
    int acceptX = GAME_INVITE_POPUP_X + GAME_INVITE_POPUP_W - CONFIRM_BTN_W - 16;
    tft.fillRoundRect(rejectX, btnY, CONFIRM_BTN_W, CONFIRM_BTN_H, 4, rejectBg);
    tft.fillRoundRect(acceptX, btnY, CONFIRM_BTN_W, CONFIRM_BTN_H, 4, acceptBg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, rejectBg);
    tft.drawString("DECLINE", rejectX + CONFIRM_BTN_W / 2, btnY + CONFIRM_BTN_H / 2, 1);
    tft.setTextColor(TFT_WHITE, acceptBg);
    tft.drawString("JOIN", acceptX + CONFIRM_BTN_W / 2, btnY + CONFIRM_BTN_H / 2, 1);
    tft.setTextDatum(TL_DATUM);
    gameInviteLastSec = sec;
}

void updateGameInviteDialog()
{
    if (!gameInviteVisible)
        return;
    long remain = (long)(gameInviteDeadlineMs - millis());
    if (remain <= 0) {
        hideGameInviteDialog();
        return;
    }
    unsigned long sec = (unsigned long)(remain / 1000UL);
    // 定时重绘置顶，防止被游戏软刷新盖住
    static unsigned long lastForce = 0;
    if (sec != gameInviteLastSec || millis() - lastForce > 450UL) {
        lastForce = millis();
        drawGameInviteDialog();
    }
}

bool handleGameInviteTouch(int x, int y)
{
    if (!gameInviteVisible)
        return false;
    if (gameInviteFingerDown)
        return true;
    int btnY = GAME_INVITE_POPUP_Y + GAME_INVITE_POPUP_H - 34;
    int rejectX = GAME_INVITE_POPUP_X + 16;
    int acceptX = GAME_INVITE_POPUP_X + GAME_INVITE_POPUP_W - CONFIRM_BTN_W - 16;
    if (y >= btnY && y <= btnY + CONFIRM_BTN_H) {
        if (x >= rejectX && x <= rejectX + CONFIRM_BTN_W) {
            gameInviteFingerDown = true;
            hideGameInviteDialog();
            return true;
        }
        if (x >= acceptX && x <= acceptX + CONFIRM_BTN_W) {
            gameInviteFingerDown = true;
            uint8_t kind = gameInviteKind;
            gameInviteVisible = false;
            gameInviteFingerDown = false;
            if (currentUIState != UI_STATE_ARCADE)
                currentUIState = UI_STATE_ARCADE;
            if (kind == GAME_KIND_MOLE)
                enterMole();
            else if (kind == GAME_KIND_RPS)
                enterRps();
            else if (kind == GAME_KIND_RADAR)
                enterRadar();
            else if (kind == GAME_KIND_REACT)
                enterReact();
            else if (kind == GAME_KIND_BRIDGE)
                enterBridge();
            else
                showGameLobby();
            return true;
        }
    }
    return true;
}

void overlayGameInviteIfAny()
{
    if (gameInviteVisible)
        drawGameInviteDialog();
}
