#include "drawing_history.h" // 包含自定义绘图历史头文件和 TouchData_t 的定义
#include "esp_now_handler.h"
#include "config.h"     // 包含项目配置常量
#include "ui_manager.h" // << 添加对 UI 管理器的引用
#include "transport_manager.h"
#include "game_arcade.h"
#include <Arduino.h>    // For Serial, millis, etc.
#include <cstring>      // For memcpy, memset, snprintf
#include <TFT_eSPI.h> // 需要 TFT_eSPI::color565 等，以及 tft 对象
#include "touch_handler.h" // For TS_Point type
#include <vector> // 用于 getPeerInfoList 返回值
#include <map> // 用于 std::map
#include <set>
#include <queue>

// TFT_eSPI tft 对象和 drawMainInterface 函数在 Project-ESPNow.ino 中定义
// 通过 extern 声明来在此文件中使用它们
extern TFT_eSPI tft;
extern void drawMainInterface(); // 用于清屏后重绘UI骨架
// 如果 clearScreenAndCache 也需要从这里调用，也需要 extern
extern void clearScreenAndCache();

// 来自 ui_manager 的外部变量
extern int eraserRadius; // 当前橡皮擦半径
extern char localDeviceId[DEVICE_ID_MAX_LEN + 1];
extern UIState_t currentUIState;
extern bool inCustomColorMode;
extern bool pendingCanvasRedrawAfterChat; // 非花瓣画板界面时延后笔迹/restore 重绘

static std::set<String> canvasSyncedPeers;
static std::map<String, unsigned long> lastCanvasSyncMs;
static std::map<String, unsigned long> peerLastRawUptimeSeen;

// 对端链路质量：差→好时触发一次画面全量重同步
struct PeerSignalState_t {
    bool markedBad = false;
    unsigned long badCandidateSince = 0;
    unsigned long goodCandidateSince = 0;
    unsigned long lastRecoveryResyncMs = 0;
};
static std::map<String, PeerSignalState_t> peerSignalState;
static bool pendingSignalRecoveryResync = false;
static String pendingSignalRecoveryPeer;
static unsigned long pendingSignalRecoveryPeerUptime = 0;
static long pendingSignalRecoveryPeerOffset = 0;
static unsigned long signalRecoveryFallbackAtMs = 0;
static String signalRecoveryFallbackPeer;
static bool signalRecoveryAllowLargerMac = false;

static void xorDecryptIncomingSync(SyncMessage_t *msg);
static bool isPrivEncryptedDrawType(MessageType_t t);
static bool privActivePeerMacEquals(const uint8_t mac[6]);
static bool privPeerKeyMatches(const String &peerKey);
static void offerPrivateCanvasResume();

static String macKeyFromLastPeer()
{
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             lastPeerMac[0], lastPeerMac[1], lastPeerMac[2],
             lastPeerMac[3], lastPeerMac[4], lastPeerMac[5]);
    return String(macStr);
}

static bool localIsSyncSourceVsMac(unsigned long localEff, unsigned long peerEff,
                                   const uint8_t peerMac[6])
{
    if (localEff > peerEff)
        return true;
    if (localEff < peerEff)
        return false;
    uint8_t myMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    return memcmp(myMac, peerMac, 6) > 0;
}

static bool localIsSyncSource(unsigned long localEff, unsigned long peerEff)
{
    return localIsSyncSourceVsMac(localEff, peerEff, lastPeerMac);
}

static bool parseMacKey(const String &macKey, uint8_t outMac[6])
{
    unsigned int b[6] = {0};
    if (sscanf(macKey.c_str(), "%02X:%02X:%02X:%02X:%02X:%02X",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++)
        outMac[i] = (uint8_t)b[i];
    return true;
}

bool parseMacString(const String &macKey, uint8_t outMac[6])
{
    return parseMacKey(macKey, outMac);
}

static bool syncBusy()
{
    return iamRequestingAllData || isReceivingDrawingData ||
           isSendingDrawingData || isAwaitingSyncStartResponse;
}

// 笔迹与 restore 只允许出现在花瓣画板主界面
static bool isCanvasBoardActive()
{
    return currentUIState == UI_STATE_MAIN && !inCustomColorMode;
}

static bool shouldSkipCanvasPaint()
{
    return !isCanvasBoardActive();
}

static void markPeerCanvasSynced(const String &peerKey)
{
    canvasSyncedPeers.insert(peerKey);
    lastCanvasSyncMs[peerKey] = millis();
}

static void clearPeerCanvasSyncState(const String &peerKey)
{
    canvasSyncedPeers.erase(peerKey);
    lastCanvasSyncMs.erase(peerKey);
}

static bool peerNeedsCanvasSync(const String &peerKey)
{
    // 仅在「已成功同步」后短冷却；未成功完成的不算
    if (canvasSyncedPeers.count(peerKey)) {
        auto it = lastCanvasSyncMs.find(peerKey);
        if (it != lastCanvasSyncMs.end() && (millis() - it->second) < CANVAS_RESYNC_COOLDOWN_MS)
            return false;
        // 冷却过期：允许再次评估（例如对端 reboot 后）
        clearPeerCanvasSyncState(peerKey);
    }
    return true;
}

// 检测对端 reboot：uptime 明显回落则强制允许恢复
static void detectPeerRebootAndAllowResync(const String &peerKey, unsigned long peerRawUptime)
{
    auto it = peerLastRawUptimeSeen.find(peerKey);
    if (it != peerLastRawUptimeSeen.end()) {
        unsigned long prev = it->second;
        // 对端曾运行较久，突然变成很小 uptime → 视为重启
        if (prev > 20000UL && peerRawUptime + 3000UL < prev) {
            clearPeerCanvasSyncState(peerKey);
            Serial.println("检测到对端 reboot，允许立即画板恢复");
        }
    }
    peerLastRawUptimeSeen[peerKey] = peerRawUptime;
}

static void applySenderId(SyncMessage_t *msg)
{
    if (!msg)
        return;
    strncpy(msg->senderId, localDeviceId, DEVICE_ID_MAX_LEN);
    msg->senderId[DEVICE_ID_MAX_LEN] = '\0';
}

// 定义在 esp_now_handler.h 中声明的全局变量
esp_now_peer_info_t broadcastPeerInfo;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // ESP-NOW 广播地址
std::queue<SyncMessage_t> incomingMessageQueue;                    // ESP-NOW 接收消息队列
DrawingHistory allDrawingHistory;                        // 所有绘图操作的历史记录

struct PrivIncoming_t {
    PrivCanvasPacket_t pkt;
    uint8_t srcMac[6];
};
static std::queue<PrivIncoming_t> incomingPrivQueue;
std::set<String> macSet;                                           // 已发现的对端设备 MAC 地址
std::map<String, unsigned long> peerLastHeartbeat; // 存储每个对端的最后心跳时间

// PeerInfo_t 结构体定义已移至 esp_now_handler.h
// 已移除重复的 PeerInfo_s 结构体定义和 PeerInfo_t typedef

// 新增：存储所有已知对端详细信息的 map
std::map<String, PeerInfo_t> peerInfoMap;


unsigned long lastKnownPeerUptime = 0;
long lastKnownPeerOffset = 0;
uint8_t lastPeerMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool initialSyncLogicProcessed = false;
bool iamEffectivelyMoreUptimeDevice = false;
bool iamRequestingAllData = false;
bool isAwaitingSyncStartResponse = false;
bool isReceivingDrawingData = false; // 修正：这里之前少了一个 bool 关键字
bool isSendingDrawingData = false;
size_t currentHistorySendIndex = 0; // 定义新增的全局变量
long relativeBootTimeOffset = 0;
unsigned long uptimeOfLastPeerSyncedFrom = 0;
unsigned long timeRequestSentForAllDrawings = 0; // 新增：记录请求所有绘图数据的时间戳

// 用于接收进度条的变量
static size_t receivedHistoryPointCount = 0;
static uint16_t totalPointsExpectedFromPeer = 0;

// 触摸点处理相关 (用于远程点绘制)
TS_Point lastRemotePoint = {0, 0, 0}; // 远程最后一点
unsigned long lastRemoteDrawTime = 0; // 远程最后绘制时间
static uint32_t lastRemoteColor = 0;  // 用于橡皮/画笔分段，避免跨笔触连线误擦
static uint8_t lastRemotePage = 0;    // 远程最后一点所在页，跨页不断笔

static void bumpCanvasPageCountFromPoint(uint8_t page)
{
    if (page >= CANVAS_MAX_PAGES)
        return;
    uint8_t need = (uint8_t)(page + 1);
    if (need > canvasPageCount)
        canvasPageCount = need;
}

static void refreshCanvasPageCountFromHistory()
{
    uint8_t maxP = 0;
    const size_t n = allDrawingHistory.size();
    for (size_t i = 0; i < n; i++) {
        const TouchData_t &d = allDrawingHistory[i];
        if (!d.isReset && d.page > maxP)
            maxP = d.page;
    }
    uint8_t need = (uint8_t)(maxP + 1);
    if (need < 1)
        need = 1;
    if (need > CANVAS_MAX_PAGES)
        need = CANVAS_MAX_PAGES;
    if (need > canvasPageCount)
        canvasPageCount = need;
    if (currentCanvasPage >= canvasPageCount)
        currentCanvasPage = (uint8_t)(canvasPageCount - 1);
}
// unsigned long touchInterval = 50;     // 触摸笔划间隔阈值 (毫秒) -> 已移至 config.h 作为 TOUCH_STROKE_INTERVAL

static void cancelSignalRecoveryFallback()
{
    signalRecoveryFallbackAtMs = 0;
    signalRecoveryFallbackPeer = "";
    signalRecoveryAllowLargerMac = false;
}

static void beginRequestAllDrawings()
{
    // 私聊画板中禁止拉公屏历史（会清空私聊笔迹）
    if (isPrivateCanvasActive())
        return;

    unsigned long now = millis();
    iamEffectivelyMoreUptimeDevice = false;
    iamRequestingAllData = true;
    isAwaitingSyncStartResponse = true;
    allDrawingHistory.clear();
    clearCanvasRedoStack();

    SyncMessage_t syncStartMsg;
    memset(&syncStartMsg, 0, sizeof(syncStartMsg));
    syncStartMsg.type = MSG_TYPE_SYNC_START;
    syncStartMsg.senderUptime = now;
    syncStartMsg.senderOffset = relativeBootTimeOffset;
    sendSyncMessage(&syncStartMsg);

    SyncMessage_t requestMsg;
    memset(&requestMsg, 0, sizeof(requestMsg));
    requestMsg.type = MSG_TYPE_REQUEST_ALL_DRAWINGS;
    requestMsg.senderUptime = now;
    requestMsg.senderOffset = relativeBootTimeOffset;
    requestMsg.touch_data.x = CANVAS_FORCE_RESYNC_FLAG;
    sendSyncMessage(&requestMsg);
    timeRequestSentForAllDrawings = millis();
}

static void beginPromptPeerToRequest(const String &peerKey, bool force)
{
    unsigned long now = millis();
    iamEffectivelyMoreUptimeDevice = true;
    iamRequestingAllData = false;

    SyncMessage_t promptMsg;
    memset(&promptMsg, 0, sizeof(promptMsg));
    promptMsg.type = MSG_TYPE_CLEAR_AND_REQUEST_UPDATE;
    promptMsg.senderUptime = now;
    promptMsg.senderOffset = relativeBootTimeOffset;
    if (force)
        promptMsg.touch_data.x = CANVAS_FORCE_RESYNC_FLAG;
    sendSyncMessage(&promptMsg);
    markPeerCanvasSynced(peerKey);
}

static void queueSignalRecoveryResync(const String &peerKey,
                                     unsigned long peerRawUptime,
                                     long peerOffset)
{
    pendingSignalRecoveryResync = true;
    pendingSignalRecoveryPeer = peerKey;
    if (peerRawUptime != 0) {
        pendingSignalRecoveryPeerUptime = peerRawUptime;
        pendingSignalRecoveryPeerOffset = peerOffset;
    } else {
        auto pit = peerInfoMap.find(peerKey);
        if (pit != peerInfoMap.end() && pit->second.effectiveUptime != 0) {
            pendingSignalRecoveryPeerUptime = pit->second.effectiveUptime;
            pendingSignalRecoveryPeerOffset = 0;
        } else {
            pendingSignalRecoveryPeerUptime = lastKnownPeerUptime;
            pendingSignalRecoveryPeerOffset = lastKnownPeerOffset;
        }
    }
}

static void updatePeerSignalQuality(const String &peerKey, int8_t rssi,
                                    unsigned long peerRawUptime, long peerOffset)
{
    if (rssi == 0 || peerKey.length() == 0)
        return;

    PeerSignalState_t &st = peerSignalState[peerKey];
    unsigned long now = millis();

    if (rssi <= SIGNAL_BAD_RSSI_DBM) {
        st.goodCandidateSince = 0;
        if (st.badCandidateSince == 0)
            st.badCandidateSince = now;
        else if (!st.markedBad && (now - st.badCandidateSince) >= SIGNAL_BAD_HOLD_MS) {
            st.markedBad = true;
            Serial.print("对端信号变差: ");
            Serial.print(peerKey);
            Serial.print(" RSSI=");
            Serial.println((int)rssi);
        }
        return;
    }

    if (rssi >= SIGNAL_GOOD_RSSI_DBM) {
        st.badCandidateSince = 0;
        if (!st.markedBad) {
            st.goodCandidateSince = 0;
            return;
        }
        if (st.goodCandidateSince == 0)
            st.goodCandidateSince = now;
        else if ((now - st.goodCandidateSince) >= SIGNAL_GOOD_HOLD_MS) {
            st.markedBad = false;
            st.goodCandidateSince = 0;
            if (st.lastRecoveryResyncMs != 0 &&
                (now - st.lastRecoveryResyncMs) < SIGNAL_RECOVERY_RESYNC_COOLDOWN_MS) {
                Serial.println("信号已恢复，但重同步仍在冷却中");
                return;
            }
            st.lastRecoveryResyncMs = now;
            Serial.print("对端信号恢复，排队画面重同步: ");
            Serial.println(peerKey);
            queueSignalRecoveryResync(peerKey, peerRawUptime, peerOffset);
        }
        return;
    }

    // 中间区：不推进计时，避免临界来回抖动
    st.badCandidateSince = 0;
    st.goodCandidateSince = 0;
}

static unsigned long lastDestructiveEditMs = 0;

void noteLocalDestructiveCanvasEdit()
{
    lastDestructiveEditMs = millis();
}

static bool recentlyDestructivelyEdited(unsigned long windowMs = 25000UL)
{
    if (lastDestructiveEditMs == 0)
        return false;
    return (millis() - lastDestructiveEditMs) < windowMs;
}

void forcePushDrawingHistoryToPeers()
{
    // 忙于接收时不要强推，避免双边互踩
    if (iamRequestingAllData || isReceivingDrawingData || isAwaitingSyncStartResponse)
        return;

    noteLocalDestructiveCanvasEdit();

    SyncMessage_t syncStart;
    memset(&syncStart, 0, sizeof(syncStart));
    syncStart.type = MSG_TYPE_SYNC_START;
    syncStart.senderUptime = millis();
    syncStart.senderOffset = relativeBootTimeOffset;
    size_t pts = allDrawingHistory.size();
    syncStart.totalPointsForSync = (pts > 65535) ? 65535 : (uint16_t)pts;
    syncStart.touch_data.x = CANVAS_FORCE_RESYNC_FLAG;
    sendSyncMessage(&syncStart);

    iamEffectivelyMoreUptimeDevice = true;
    isSendingDrawingData = true;
    currentHistorySendIndex = 0;
    if (!allDrawingHistory.empty())
        updateSendProgress(0, allDrawingHistory.size());
    else {
        // 空历史也要发完成包，让对端清空
        SyncMessage_t completeMsg;
        memset(&completeMsg, 0, sizeof(completeMsg));
        completeMsg.type = MSG_TYPE_ALL_DRAWINGS_COMPLETE;
        completeMsg.senderUptime = millis();
        completeMsg.senderOffset = relativeBootTimeOffset;
        sendSyncMessage(&completeMsg);
        isSendingDrawingData = false;
        hideSendProgress();
    }
    Serial.println("强制推送本机笔迹历史到对端（清页/权威同步）");
}

void processPendingSignalRecoveryResync()
{
    // 私聊中：信号恢复只触发私聊 RESUME，不做公屏全量同步
    if (isPrivateCanvasActive()) {
        if (pendingSignalRecoveryResync) {
            const String peerKey = pendingSignalRecoveryPeer;
            pendingSignalRecoveryResync = false;
            if (privPeerKeyMatches(peerKey) || peerKey.length() == 0)
                offerPrivateCanvasResume();
        }
        if (signalRecoveryFallbackAtMs != 0 &&
            (long)(millis() - signalRecoveryFallbackAtMs) >= 0) {
            signalRecoveryFallbackAtMs = 0;
            signalRecoveryAllowLargerMac = false;
            offerPrivateCanvasResume();
        }
        return;
    }

    // 大 MAC 等待对端发起超时后，自己兜底发起
    if (!pendingSignalRecoveryResync && signalRecoveryFallbackAtMs != 0 &&
        (long)(millis() - signalRecoveryFallbackAtMs) >= 0) {
        if (!syncBusy()) {
            pendingSignalRecoveryResync = true;
            pendingSignalRecoveryPeer = signalRecoveryFallbackPeer;
            auto pit = peerInfoMap.find(signalRecoveryFallbackPeer);
            if (pit != peerInfoMap.end()) {
                pendingSignalRecoveryPeerUptime = pit->second.effectiveUptime;
                pendingSignalRecoveryPeerOffset = 0;
            }
            signalRecoveryFallbackAtMs = 0;
            signalRecoveryAllowLargerMac = true;
            Serial.println("信号恢复兜底：对端未发起，本机启动同步");
        }
    }

    if (!pendingSignalRecoveryResync)
        return;
    if (syncBusy())
        return; // 忙则保留 pending，下一轮再试

    const String peerKey = pendingSignalRecoveryPeer;
    pendingSignalRecoveryResync = false;

    uint8_t peerMac[6];
    bool haveMac = parseMacKey(peerKey, peerMac);
    bool iAmLargerMac = false;
    if (haveMac) {
        uint8_t myMac[6];
        esp_wifi_get_mac(WIFI_IF_STA, myMac);
        iAmLargerMac = (memcmp(myMac, peerMac, 6) > 0);
    }

    // 仅 MAC 较小方立刻发起；较大方等 4s，避免双边同时 clear
    if (iAmLargerMac && !signalRecoveryAllowLargerMac) {
        clearPeerCanvasSyncState(peerKey);
        if (signalRecoveryFallbackAtMs == 0) {
            signalRecoveryFallbackAtMs = millis() + 4000UL;
            signalRecoveryFallbackPeer = peerKey;
        }
        Serial.println("信号恢复：本机 MAC 较大，等待对端发起同步");
        showStatusToast("信号恢复，等待同步…", 1000);
        return;
    }

    signalRecoveryAllowLargerMac = false;
    signalRecoveryFallbackAtMs = 0;
    signalRecoveryFallbackPeer = "";
    clearPeerCanvasSyncState(peerKey);

    unsigned long localEff = millis() + relativeBootTimeOffset;
    unsigned long peerEff = pendingSignalRecoveryPeerUptime + pendingSignalRecoveryPeerOffset;

    bool amSource = haveMac
        ? localIsSyncSourceVsMac(localEff, peerEff, peerMac)
        : localIsSyncSource(localEff, peerEff);

    // 优先以笔迹更多的一侧为源，减少信号差期间本机独有笔迹被清空
    auto pit = peerInfoMap.find(peerKey);
    if (pit != peerInfoMap.end() && pit->second.historyPoints > 0) {
        size_t localPts = allDrawingHistory.size();
        uint16_t peerPts = pit->second.historyPoints;
        if (localPts > peerPts + 8)
            amSource = true;
        else if (peerPts > localPts + 8)
            amSource = false;
    }
    // 刚清页/清空：绝不能因为点数变少去向对端拉旧历史
    if (recentlyDestructivelyEdited(25000UL))
        amSource = true;

    showStatusToast("信号恢复，同步画面…", 1200);
    Serial.print("执行信号恢复画面重同步 vs ");
    Serial.println(peerKey);

    if (amSource)
        beginPromptPeerToRequest(peerKey, true);
    else
        beginRequestAllDrawings();
}

// ESP-NOW 初始化函数
void espNowInit()
{
    if (esp_now_init() != ESP_OK)
    {
        Serial.println("错误：ESP-NOW 初始化失败");
        return;
    }

    esp_now_register_send_cb(OnSyncDataSent);
    esp_now_register_recv_cb(OnSyncDataRecv);

    memcpy(broadcastPeerInfo.peer_addr, broadcastAddress, 6);
    broadcastPeerInfo.channel = 0;
    broadcastPeerInfo.ifidx = WIFI_IF_STA;
    broadcastPeerInfo.encrypt = false;
    if (esp_now_add_peer(&broadcastPeerInfo) != ESP_OK)
    {
        Serial.println("添加广播对端失败");
        esp_now_del_peer(broadcastPeerInfo.peer_addr); // 尝试删除后重新添加
        if (esp_now_add_peer(&broadcastPeerInfo) != ESP_OK)
        {
            Serial.println("尝试删除后重新添加广播对端仍然失败");
            return;
        }
        Serial.println("初次失败后成功重新添加广播对端。");
    }
    else
    {
        Serial.println("广播对端添加成功。");
    }
}

// ESP-NOW 数据发送回调函数 (ESP32 Arduino Core 3.x: wifi_tx_info_t)
void OnSyncDataSent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS)
    {
        const uint8_t *mac_addr = (tx_info && tx_info->des_addr) ? tx_info->des_addr : nullptr;
        char macStr[18] = "??:??:??:??:??:??";
        if (mac_addr)
        {
            snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        }
        Serial.print("发送到 ");
        Serial.print(macStr);
        Serial.print(" 失败。状态: ");
        Serial.println(status == ESP_NOW_SEND_SUCCESS ? "成功" : "失败");
    }
}

void OnSyncDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingDataPtr, int len)
{
    if (!info || !incomingDataPtr)
        return;
    // 仅 WiFi 模式不吃 ESP-NOW，避免列表出现纯 ESP-NOW 设备
    if (linkModeIsWifiOnly())
        return;
    // Drop if same payload already arrived via WiFi (concurrent dual-path)
    if (transportIsDuplicatePacket(incomingDataPtr, len))
        return;
    int8_t rssi = (info->rx_ctrl) ? info->rx_ctrl->rssi : 0;
    ingestIncomingPacketEx(info->src_addr, incomingDataPtr, len, rssi, false);
}

bool peerVisibleForLocalMode(const PeerInfo_t &p)
{
    if (!linkModeIsWifiOnly())
        return true;
    if ((p.linkCaps & PEER_CAP_WIFI) == 0)
        return false;
    // 明确宣称仅 ESP-NOW 的不显示（即使误标了 WiFi）
    if (p.peerLinkMode == LINK_MODE_ESPNOW_ONLY)
        return false;
    return true;
}

void purgePeersNotVisibleForWifiOnly()
{
    if (!linkModeIsWifiOnly())
        return;
    for (auto it = peerInfoMap.begin(); it != peerInfoMap.end();) {
        if ((it->second.linkCaps & PEER_CAP_WIFI) == 0) {
            macSet.erase(it->first);
            peerLastHeartbeat.erase(it->first);
            it = peerInfoMap.erase(it);
        } else {
            ++it;
        }
    }
}

void notePeerWifiPresence(const uint8_t srcMac[6], const char *deviceId, uint8_t peerMode)
{
    if (!srcMac)
        return;
    if (peerMode == LINK_MODE_ESPNOW_ONLY)
        return;
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             srcMac[0], srcMac[1], srcMac[2], srcMac[3], srcMac[4], srcMac[5]);
    String macKey = String(macStr);
    unsigned long nowMs = millis();
    PeerInfo_t &pi = peerInfoMap[macKey];
    if (pi.firstSeenMs == 0) {
        pi.firstSeenMs = nowMs;
        pi.peerLinkMode = 0xFF;
    }
    pi.lastSeenMs = nowMs;
    pi.macAddress = macKey;
    pi.linkCaps = (uint8_t)(pi.linkCaps | PEER_CAP_WIFI);
    if (peerMode <= LINK_MODE_WIFI_ONLY)
        pi.peerLinkMode = peerMode;
    if (deviceId && deviceId[0]) {
        strncpy(pi.deviceId, deviceId, DEVICE_ID_MAX_LEN);
        pi.deviceId[DEVICE_ID_MAX_LEN] = '\0';
    }
    macSet.insert(macKey);
    peerLastHeartbeat[macKey] = nowMs;
}

// 统一收包入口（ESP-NOW / WiFi UDP / 蓝牙）
void ingestIncomingPacket(const uint8_t srcMac[6], const uint8_t *incomingDataPtr, int len, int8_t rssi)
{
    ingestIncomingPacketEx(srcMac, incomingDataPtr, len, rssi, false);
}

void ingestIncomingPacketEx(const uint8_t srcMac[6], const uint8_t *incomingDataPtr, int len, int8_t rssi, bool viaWifi)
{
    if (!srcMac || !incomingDataPtr || len <= 0)
        return;
    // 仅 WiFi：丢弃非 WiFi 路径（防御）
    if (linkModeIsWifiOnly() && !viaWifi)
        return;

    uint8_t macUse[6];
    memcpy(macUse, srcMac, 6);
    // 蓝牙可能无 MAC：用全 0，后续用 senderId 补登记
    bool macAllZero = true;
    for (int i = 0; i < 6; i++) {
        if (macUse[i]) { macAllZero = false; break; }
    }

    auto markCaps = [&](PeerInfo_t &pi) {
        if (viaWifi)
            pi.linkCaps = (uint8_t)(pi.linkCaps | PEER_CAP_WIFI);
        else
            pi.linkCaps = (uint8_t)(pi.linkCaps | PEER_CAP_ESPNOW);
        if (pi.peerLinkMode == 0)
            pi.peerLinkMode = 0xFF;
    };

    if (len == sizeof(PrivCanvasPacket_t))
    {
        PrivCanvasPacket_t privPkt;
        memcpy(&privPkt, incomingDataPtr, sizeof(privPkt));
        privPkt.senderId[DEVICE_ID_MAX_LEN] = '\0';
        privPkt.targetId[DEVICE_ID_MAX_LEN] = '\0';
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 macUse[0], macUse[1], macUse[2], macUse[3], macUse[4], macUse[5]);
        String macKey = String(macStr);
        if (macAllZero && privPkt.senderId[0])
            macKey = String("ID:") + privPkt.senderId;
        macSet.insert(macKey);
        peerLastHeartbeat[macKey] = millis();
        unsigned long nowMs = millis();
        PeerInfo_t &pi = peerInfoMap[macKey];
        if (pi.firstSeenMs == 0) {
            pi.firstSeenMs = nowMs;
            pi.peerLinkMode = 0xFF;
        }
        if (pi.lastSeenMs > 0 && nowMs >= pi.lastSeenMs)
            pi.latencyMs = (uint16_t)min(9999UL, nowMs - pi.lastSeenMs);
        pi.lastSeenMs = nowMs;
        pi.macAddress = macKey;
        markCaps(pi);
        if (rssi)
            pi.rssi = rssi;
        if (privPkt.senderId[0]) {
            strncpy(pi.deviceId, privPkt.senderId, DEVICE_ID_MAX_LEN);
            pi.deviceId[DEVICE_ID_MAX_LEN] = '\0';
        }
        // 勿在 WiFi/ESP-NOW 回调里画 TFT：入队，主循环再处理
        PrivIncoming_t item;
        item.pkt = privPkt;
        if (!macAllZero)
            memcpy(item.srcMac, macUse, 6);
        else if (privPkt.senderMac[0] || privPkt.senderMac[5])
            memcpy(item.srcMac, privPkt.senderMac, 6);
        else
            memcpy(item.srcMac, macUse, 6);
        if (incomingPrivQueue.size() < 8)
            incomingPrivQueue.push(item);
        return;
    }
    if (len == sizeof(SyncMessage_t))
    {
        SyncMessage_t receivedMsg;
        memcpy(&receivedMsg, incomingDataPtr, sizeof(receivedMsg));
        receivedMsg.senderId[DEVICE_ID_MAX_LEN] = '\0';
        if (!macAllZero)
            memcpy(lastPeerMac, macUse, 6);

        if (isPrivEncryptedDrawType(receivedMsg.type)) {
            if (isPrivateCanvasActive()) {
                bool peerOk = privActivePeerMacEquals(macUse) || macAllZero;
                if (peerOk)
                    xorDecryptIncomingSync(&receivedMsg);
                else
                    return;
            }
        }

        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 macUse[0], macUse[1], macUse[2], macUse[3], macUse[4], macUse[5]);
        String macKey = String(macStr);
        if (macAllZero && receivedMsg.senderId[0])
            macKey = String("ID:") + receivedMsg.senderId;
        bool isNewPeer = (peerInfoMap.find(macKey) == peerInfoMap.end());
        unsigned long nowMs = millis();

        macSet.insert(macKey);
        peerLastHeartbeat[macKey] = nowMs;

        PeerInfo_t &pi = peerInfoMap[macKey];
        if (pi.firstSeenMs == 0) {
            pi.firstSeenMs = nowMs;
            pi.peerLinkMode = 0xFF;
        }
        if (pi.lastSeenMs > 0 && nowMs >= pi.lastSeenMs)
            pi.latencyMs = (uint16_t)min(9999UL, nowMs - pi.lastSeenMs);
        pi.lastSeenMs = nowMs;
        pi.macAddress = macKey;
        markCaps(pi);
        pi.effectiveUptime = receivedMsg.senderUptime + receivedMsg.senderOffset;
        pi.usedMemory = receivedMsg.usedMemory;
        pi.totalMemory = receivedMsg.totalMemory;
        if (receivedMsg.type == MSG_TYPE_HEARTBEAT ||
            receivedMsg.type == MSG_TYPE_UPTIME_INFO ||
            receivedMsg.type == MSG_TYPE_SYNC_START ||
            receivedMsg.type == MSG_TYPE_ALL_DRAWINGS_COMPLETE) {
            pi.historyPoints = receivedMsg.totalPointsForSync;
        }
        if (receivedMsg.type == MSG_TYPE_HEARTBEAT) {
            int bat = receivedMsg.touch_data.x;
            if (bat < 0) bat = 0;
            if (bat > 100) bat = 100;
            pi.batteryPercent = (uint8_t)bat;
        }
        if (rssi) {
            pi.rssi = rssi;
            updatePeerSignalQuality(macKey, rssi,
                                    receivedMsg.senderUptime, receivedMsg.senderOffset);
        }
        if (receivedMsg.senderId[0]) {
            strncpy(pi.deviceId, receivedMsg.senderId, DEVICE_ID_MAX_LEN);
            pi.deviceId[DEVICE_ID_MAX_LEN] = '\0';
            if (localDeviceId[0]) {
                const char *a = localDeviceId;
                const char *b = receivedMsg.senderId;
                bool conflict = true;
                while (*a && *b) {
                    char ca = (*a >= 'a' && *a <= 'z') ? (*a - 'a' + 'A') : *a;
                    char cb = (*b >= 'a' && *b <= 'z') ? (*b - 'a' + 'A') : *b;
                    if (ca != cb) { conflict = false; break; }
                    a++;
                    b++;
                }
                if (conflict && (*a || *b))
                    conflict = false;
                if (conflict) {
                    char tip[40];
                    snprintf(tip, sizeof(tip), "标识冲突:%s", receivedMsg.senderId);
                    showStatusToast(tip, 3000);
                    if (currentUIState == UI_STATE_NAME_EDIT)
                        drawNameEditScreen();
                }
            }
        } else if (pi.deviceId[0] == '\0' && !macAllZero) {
            snprintf(pi.deviceId, sizeof(pi.deviceId),
                     "%02X%02X", macUse[4], macUse[5]);
        }

        if (isNewPeer) {
            clearPeerCanvasSyncState(macKey);
            peerJoinedNotify(pi.deviceId);
            SyncMessage_t urgent;
            memset(&urgent, 0, sizeof(urgent));
            urgent.type = MSG_TYPE_UPTIME_INFO;
            urgent.senderUptime = millis();
            urgent.senderOffset = relativeBootTimeOffset;
            applySenderId(&urgent);
            sendSyncMessage(&urgent);
        }

        incomingMessageQueue.push(receivedMsg);
    }
    else if (len == sizeof(ChatPacket_t))
    {
        ChatPacket_t chatPkt;
        memcpy(&chatPkt, incomingDataPtr, sizeof(chatPkt));
        chatPkt.senderId[DEVICE_ID_MAX_LEN] = '\0';
        chatPkt.targetId[DEVICE_ID_MAX_LEN] = '\0';
        chatPkt.text[CHAT_TEXT_MAX] = '\0';

        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 macUse[0], macUse[1], macUse[2], macUse[3], macUse[4], macUse[5]);
        String macKey = String(macStr);
        if (macAllZero && chatPkt.senderId[0])
            macKey = String("ID:") + chatPkt.senderId;
        macSet.insert(macKey);
        peerLastHeartbeat[macKey] = millis();
        PeerInfo_t &pi = peerInfoMap[macKey];
        if (pi.firstSeenMs == 0) {
            pi.firstSeenMs = millis();
            pi.peerLinkMode = 0xFF;
        }
        pi.lastSeenMs = millis();
        markCaps(pi);
        if (rssi) {
            pi.rssi = rssi;
            updatePeerSignalQuality(macKey, rssi, 0, 0);
        }
        pi.macAddress = macKey;
        if (chatPkt.senderId[0]) {
            strncpy(pi.deviceId, chatPkt.senderId, DEVICE_ID_MAX_LEN);
            pi.deviceId[DEVICE_ID_MAX_LEN] = '\0';
        }

        processIncomingChatPacket(chatPkt);
    }
    else if (len == sizeof(GamePacket_t))
    {
        GamePacket_t gamePkt;
        memcpy(&gamePkt, incomingDataPtr, sizeof(gamePkt));
        gamePkt.senderId[DEVICE_ID_MAX_LEN] = '\0';
        if (gamePkt.magic != GAME_PKT_MAGIC)
            return;

        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 macUse[0], macUse[1], macUse[2], macUse[3], macUse[4], macUse[5]);
        String macKey = String(macStr);
        if (macAllZero && gamePkt.senderId[0])
            macKey = String("ID:") + gamePkt.senderId;
        macSet.insert(macKey);
        peerLastHeartbeat[macKey] = millis();
        PeerInfo_t &pi = peerInfoMap[macKey];
        if (pi.firstSeenMs == 0) {
            pi.firstSeenMs = millis();
            pi.peerLinkMode = 0xFF;
        }
        unsigned long nowMs = millis();
        if (pi.lastSeenMs > 0 && nowMs >= pi.lastSeenMs)
            pi.latencyMs = (uint16_t)min(9999UL, nowMs - pi.lastSeenMs);
        pi.lastSeenMs = nowMs;
        pi.macAddress = macKey;
        markCaps(pi);
        if (rssi)
            pi.rssi = rssi;
        if (gamePkt.senderId[0]) {
            strncpy(pi.deviceId, gamePkt.senderId, DEVICE_ID_MAX_LEN);
            pi.deviceId[DEVICE_ID_MAX_LEN] = '\0';
        }
        enqueueGamePacket(gamePkt, rssi); // 勿在回调里 process/画屏
    }
    else if (len == strlen("XX:XX:XX:XX:XX:XX") && incomingDataPtr[0] != '{')
    {
        char macStr[18];
        memcpy(macStr, incomingDataPtr, len);
        macStr[len] = '\0';
        String macKey = String(macStr);
        bool isNewPeer = (peerInfoMap.find(macKey) == peerInfoMap.end());
        macSet.insert(macKey);
        peerLastHeartbeat[macKey] = millis();
        if (isNewPeer) {
            peerInfoMap[macKey].macAddress = macKey;
            peerInfoMap[macKey].deviceId[0] = '\0';
            peerInfoMap[macKey].peerLinkMode = 0xFF;
            markCaps(peerInfoMap[macKey]);
            peerJoinedNotify(macStr);
        } else {
            markCaps(peerInfoMap[macKey]);
        }
    }
    else
    {
        Serial.print("收到意外长度的数据: ");
        Serial.print(len);
        Serial.print(", 期望长度: ");
        Serial.println(sizeof(SyncMessage_t));
    }
}

extern uint8_t currentCanvasPage;
extern uint8_t canvasPageCount;
extern void paintCurrentCanvasPage();
extern void showStatusToast(const char *msg, unsigned long durationMs);
extern void showPrivInviteDialog(const char *fromId, unsigned long deadlineMs);
extern void hidePrivInviteDialog();
extern void onPrivateCanvasSessionChanged();

enum PrivCanvasPhase_e {
    PRIV_PHASE_IDLE = 0,
    PRIV_PHASE_OUTGOING, // 已发出邀请
    PRIV_PHASE_INCOMING, // 收到邀请待确认
    PRIV_PHASE_ACTIVE
};

static PrivCanvasPhase_e privPhase = PRIV_PHASE_IDLE;
static char privPeerId[DEVICE_ID_MAX_LEN + 1] = {0};
static uint8_t privPeerMac[6] = {0};
static uint8_t privSessionKey[8] = {0};
static uint8_t privLocalNonce[8] = {0};
static unsigned long privInviteDeadlineMs = 0;
static DrawingHistory publicHistoryBackup;
static uint8_t publicPageBackup = 0;
static uint8_t publicPageCountBackup = 1;
static bool publicHistorySaved = false;
// 私聊对方掉线/重启：保留本机私聊笔迹，对方上线后 RESUME 恢复
static bool privPeerMarkedOffline = false;
static unsigned long lastPrivResumeOfferMs = 0;
static bool privResumeAwaitingAck = false;

bool isPrivateCanvasActive() { return privPhase == PRIV_PHASE_ACTIVE; }
bool isPrivateCanvasInvitePending()
{
    return privPhase == PRIV_PHASE_OUTGOING || privPhase == PRIV_PHASE_INCOMING;
}
const char *getPrivateCanvasPeerId() { return privPeerId; }

static void fillRandomNonce(uint8_t *n, size_t len)
{
    for (size_t i = 0; i < len; i++)
        n[i] = (uint8_t)(esp_random() & 0xFF);
}

static void deriveSessionKey(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 8; i++)
        privSessionKey[i] = a[i] ^ b[i];
}

static bool isPrivEncryptedDrawType(MessageType_t t)
{
    return t == MSG_TYPE_DRAW_POINT || t == MSG_TYPE_CANVAS_PAGE ||
           t == MSG_TYPE_SYNC_START || t == MSG_TYPE_ALL_DRAWINGS_COMPLETE ||
           t == MSG_TYPE_REQUEST_ALL_DRAWINGS || t == MSG_TYPE_RESET_CANVAS ||
           t == MSG_TYPE_CLEAR_AND_REQUEST_UPDATE;
}

static bool privActivePeerMacEquals(const uint8_t mac[6])
{
    return privPhase == PRIV_PHASE_ACTIVE && mac && memcmp(mac, privPeerMac, 6) == 0;
}

static bool privPeerKeyMatches(const String &peerKey)
{
    if (privPhase != PRIV_PHASE_ACTIVE || peerKey.length() == 0)
        return false;
    uint8_t mac[6];
    if (!parseMacKey(peerKey, mac))
        return false;
    return memcmp(mac, privPeerMac, 6) == 0;
}

static String privPeerMacKey()
{
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             privPeerMac[0], privPeerMac[1], privPeerMac[2],
             privPeerMac[3], privPeerMac[4], privPeerMac[5]);
    return String(macStr);
}

static void clearPrivResumeFlags()
{
    privPeerMarkedOffline = false;
    lastPrivResumeOfferMs = 0;
    privResumeAwaitingAck = false;
}

static void offerPrivateCanvasResume()
{
    if (privPhase != PRIV_PHASE_ACTIVE || !privPeerId[0])
        return;
    unsigned long now = millis();
    if (lastPrivResumeOfferMs != 0 &&
        (now - lastPrivResumeOfferMs) < PRIV_CANVAS_RESUME_OFFER_MS)
        return;
    lastPrivResumeOfferMs = now;
    privResumeAwaitingAck = true;

    PrivCanvasPacket_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = PRIV_CANVAS_MAGIC;
    pkt.type = MSG_TYPE_PRIV_RESUME;
    strncpy(pkt.senderId, localDeviceId, DEVICE_ID_MAX_LEN);
    strncpy(pkt.targetId, privPeerId, DEVICE_ID_MAX_LEN);
    esp_wifi_get_mac(WIFI_IF_STA, pkt.senderMac);
    // nonce 槽复用为会话密钥，供重启方直接恢复解密
    memcpy(pkt.nonce, privSessionKey, 8);
    pkt.timestamp = now;
    ensureUnicastPeer(privPeerMac);
    for (int i = 0; i < 3; i++) {
        sendPrivCanvasPacket(&pkt, privPeerMac);
        sendPrivCanvasPacket(&pkt, nullptr);
        delay(12);
    }
    Serial.println("已发送私聊画板 RESUME（等待对方重连确认）");
}

static void sendPrivResumeAck()
{
    PrivCanvasPacket_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = PRIV_CANVAS_MAGIC;
    pkt.type = MSG_TYPE_PRIV_ACCEPT; // 复用 ACCEPT：在线方 ACTIVE 时视为重连确认
    strncpy(pkt.senderId, localDeviceId, DEVICE_ID_MAX_LEN);
    strncpy(pkt.targetId, privPeerId, DEVICE_ID_MAX_LEN);
    esp_wifi_get_mac(WIFI_IF_STA, pkt.senderMac);
    memcpy(pkt.nonce, privSessionKey, 8);
    pkt.timestamp = millis();
    for (int i = 0; i < 3; i++) {
        sendPrivCanvasPacket(&pkt, privPeerMac);
        sendPrivCanvasPacket(&pkt, nullptr);
        delay(12);
    }
}

static void pushPrivateHistoryAfterResume()
{
    if (privPhase != PRIV_PHASE_ACTIVE)
        return;
    String key = privPeerMacKey();
    clearPeerCanvasSyncState(key);
    privPeerMarkedOffline = false;
    privResumeAwaitingAck = false;
    forcePushDrawingHistoryToPeers();
    showStatusToast("恢复私聊笔迹…", 2000);
}

static void xorEncryptSyncMessage(SyncMessage_t *msg)
{
    if (!msg || privPhase != PRIV_PHASE_ACTIVE)
        return;
    uint8_t *p = (uint8_t *)msg;
    // 保留 type 明文便于路由；其余加密
    for (size_t i = sizeof(MessageType_t); i < sizeof(SyncMessage_t); i++)
        p[i] ^= privSessionKey[i % 8];
}

static void xorDecryptIncomingSync(SyncMessage_t *msg)
{
    xorEncryptSyncMessage(msg); // XOR 对称
}

bool ensureUnicastPeer(const uint8_t mac[6])
{
    if (!mac)
        return false;
    if (esp_now_is_peer_exist(mac))
        return true;
    esp_now_peer_info_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;
    p.encrypt = false;
    p.ifidx = WIFI_IF_STA;
    return esp_now_add_peer(&p) == ESP_OK;
}

static void savePublicHistoryForPrivate()
{
    publicHistoryBackup.clear();
    const size_t n = allDrawingHistory.size();
    for (size_t i = 0; i < n; i++)
        publicHistoryBackup.push_back(allDrawingHistory[i]);
    publicPageBackup = currentCanvasPage;
    publicPageCountBackup = canvasPageCount ? canvasPageCount : 1;
    publicHistorySaved = true;
    allDrawingHistory.clear();
    clearCanvasRedoStack();
    currentCanvasPage = 0;
    canvasPageCount = 1;
}

static void restorePublicHistoryFromPrivate()
{
    allDrawingHistory.clear();
    clearCanvasRedoStack();
    if (publicHistorySaved) {
        const size_t n = publicHistoryBackup.size();
        for (size_t i = 0; i < n; i++)
            allDrawingHistory.push_back(publicHistoryBackup[i]);
        publicHistoryBackup.clear();
        currentCanvasPage = publicPageBackup;
        canvasPageCount = publicPageCountBackup ? publicPageCountBackup : 1;
        publicHistorySaved = false;
    } else {
        currentCanvasPage = 0;
        canvasPageCount = 1;
    }
}

static bool macEqual(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static void enterPrivateActive(const char *peerId, const uint8_t peerMac[6],
                               const uint8_t *nonceA, const uint8_t *nonceB)
{
    strncpy(privPeerId, peerId ? peerId : "Peer", DEVICE_ID_MAX_LEN);
    privPeerId[DEVICE_ID_MAX_LEN] = '\0';
    if (peerMac)
        memcpy(privPeerMac, peerMac, 6);
    deriveSessionKey(nonceA, nonceB);
    ensureUnicastPeer(privPeerMac);

    // 备份公屏，进入全新空白私聊画板（页 1、无笔迹）
    if (!publicHistorySaved)
        savePublicHistoryForPrivate();
    else {
        allDrawingHistory.clear();
        clearCanvasRedoStack();
        currentCanvasPage = 0;
        canvasPageCount = 1;
    }

    privPhase = PRIV_PHASE_ACTIVE;
    privInviteDeadlineMs = 0;
    clearPrivResumeFlags();
    hidePrivInviteDialog();

    // 无论当前在列表/设置/聊天，都切回主界面私聊画板
    extern bool inCustomColorMode;
    inCustomColorMode = false;
    currentUIState = UI_STATE_MAIN;
    paintCurrentCanvasPage(); // 空画板 + 显示「退」
    onPrivateCanvasSessionChanged();

    char tip[40];
    snprintf(tip, sizeof(tip), "私聊画板:%s", privPeerId);
    showStatusToast(tip, 2500);
}

// 重启方：用在线方下发的会话密钥直接重进私聊，等待对方推送笔迹
static void enterPrivateResume(const char *peerId, const uint8_t peerMac[6],
                               const uint8_t sessionKey[8])
{
    strncpy(privPeerId, peerId ? peerId : "Peer", DEVICE_ID_MAX_LEN);
    privPeerId[DEVICE_ID_MAX_LEN] = '\0';
    if (peerMac)
        memcpy(privPeerMac, peerMac, 6);
    if (sessionKey)
        memcpy(privSessionKey, sessionKey, 8);
    ensureUnicastPeer(privPeerMac);

    // 中止可能正在进行的公屏同步，避免空/乱历史盖住即将恢复的私聊笔迹
    iamRequestingAllData = false;
    isReceivingDrawingData = false;
    isAwaitingSyncStartResponse = false;
    isSendingDrawingData = false;
    hideReceiveProgress();
    hideSendProgress();

    if (!publicHistorySaved)
        savePublicHistoryForPrivate();
    else {
        allDrawingHistory.clear();
        clearCanvasRedoStack();
        currentCanvasPage = 0;
        canvasPageCount = 1;
    }

    clearPrivResumeFlags();
    privPhase = PRIV_PHASE_ACTIVE;
    privInviteDeadlineMs = 0;
    hidePrivInviteDialog();

    extern bool inCustomColorMode;
    inCustomColorMode = false;
    currentUIState = UI_STATE_MAIN;
    paintCurrentCanvasPage();
    onPrivateCanvasSessionChanged();
    showStatusToast("已重连私聊画板", 2200);
}

static void sendPrivAcceptWithRetries()
{
    PrivCanvasPacket_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = PRIV_CANVAS_MAGIC;
    pkt.type = MSG_TYPE_PRIV_ACCEPT;
    strncpy(pkt.senderId, localDeviceId, DEVICE_ID_MAX_LEN);
    strncpy(pkt.targetId, privPeerId, DEVICE_ID_MAX_LEN);
    esp_wifi_get_mac(WIFI_IF_STA, pkt.senderMac);
    memcpy(pkt.nonce, privLocalNonce, 8);
    pkt.timestamp = millis();
    // 多发几次，避免邀请方仍停在「邀请中」
    for (int i = 0; i < 3; i++) {
        sendPrivCanvasPacket(&pkt, privPeerMac);
        sendPrivCanvasPacket(&pkt, nullptr);
        delay(15);
    }
}

void sendPrivCanvasPacket(const PrivCanvasPacket_t *pkt, const uint8_t *destMacOrNull)
{
    if (!pkt)
        return;
    static_assert(sizeof(PrivCanvasPacket_t) != sizeof(SyncMessage_t), "Priv/Sync size collide");
    static_assert(sizeof(PrivCanvasPacket_t) != sizeof(ChatPacket_t), "Priv/Chat size collide");
    // 私聊控制包优先走 ESP-NOW（单播 peer + 广播兜底由调用方决定）
    if (destMacOrNull)
        ensureUnicastPeer(destMacOrNull);
    if (!transportSend((const uint8_t *)pkt, sizeof(PrivCanvasPacket_t), destMacOrNull)) {
        Serial.println("发送私聊包失败(全通道)");
        if (!linkModeIsWifiOnly()) {
            const uint8_t *dest = destMacOrNull ? destMacOrNull : broadcastAddress;
            if (esp_now_send(dest, (uint8_t *)pkt, sizeof(PrivCanvasPacket_t)) != ESP_OK)
                Serial.println("ESP-NOW 私聊包重试仍失败");
        }
    }
}

void invitePrivateCanvas(const char *peerId, const String &peerMac)
{
    if (!peerId || !peerId[0])
        return;
    if (privPhase != PRIV_PHASE_IDLE) {
        showStatusToast("已在私聊流程中", 1500);
        return;
    }
    uint8_t mac[6];
    if (!parseMacKey(peerMac, mac)) {
        showStatusToast("MAC无效", 1200);
        return;
    }
    fillRandomNonce(privLocalNonce, 8);
    strncpy(privPeerId, peerId, DEVICE_ID_MAX_LEN);
    privPeerId[DEVICE_ID_MAX_LEN] = '\0';
    memcpy(privPeerMac, mac, 6);
    privPhase = PRIV_PHASE_OUTGOING;
    privInviteDeadlineMs = millis() + PRIV_CANVAS_INVITE_TIMEOUT_MS;

    PrivCanvasPacket_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = PRIV_CANVAS_MAGIC;
    pkt.type = MSG_TYPE_PRIV_INVITE;
    strncpy(pkt.senderId, localDeviceId, DEVICE_ID_MAX_LEN);
    strncpy(pkt.targetId, peerId, DEVICE_ID_MAX_LEN);
    esp_wifi_get_mac(WIFI_IF_STA, pkt.senderMac);
    memcpy(pkt.nonce, privLocalNonce, 8);
    pkt.timestamp = millis();
    ensureUnicastPeer(mac);
    sendPrivCanvasPacket(&pkt, mac);
    // 广播一份兜底（目标按 targetId 过滤）
    sendPrivCanvasPacket(&pkt, nullptr);

    char tip[40];
    snprintf(tip, sizeof(tip), "邀请%s中…", peerId);
    showStatusToast(tip, 2000);
    onPrivateCanvasSessionChanged();
}

void acceptPrivateCanvasInvite()
{
    if (privPhase != PRIV_PHASE_INCOMING)
        return;
    fillRandomNonce(privLocalNonce, 8);

    // 邀请方 nonce 暂存在 privSessionKey
    uint8_t peerNonce[8];
    memcpy(peerNonce, privSessionKey, 8);

    sendPrivAcceptWithRetries();
    // 被邀请人：先发 ACCEPT，再进入空白私聊画板
    enterPrivateActive(privPeerId, privPeerMac, peerNonce, privLocalNonce);
}

void rejectPrivateCanvasInvite()
{
    if (privPhase != PRIV_PHASE_INCOMING)
        return;
    PrivCanvasPacket_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = PRIV_CANVAS_MAGIC;
    pkt.type = MSG_TYPE_PRIV_REJECT;
    strncpy(pkt.senderId, localDeviceId, DEVICE_ID_MAX_LEN);
    strncpy(pkt.targetId, privPeerId, DEVICE_ID_MAX_LEN);
    esp_wifi_get_mac(WIFI_IF_STA, pkt.senderMac);
    pkt.timestamp = millis();
    sendPrivCanvasPacket(&pkt, privPeerMac);
    sendPrivCanvasPacket(&pkt, nullptr);

    privPhase = PRIV_PHASE_IDLE;
    privPeerId[0] = 0;
    memset(privPeerMac, 0, 6);
    privInviteDeadlineMs = 0;
    hidePrivInviteDialog();
    showStatusToast("已拒绝私聊", 1500);
    onPrivateCanvasSessionChanged();
}

void leavePrivateCanvas()
{
    if (privPhase == PRIV_PHASE_IDLE)
        return;
    if (privPhase == PRIV_PHASE_ACTIVE) {
        PrivCanvasPacket_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.magic = PRIV_CANVAS_MAGIC;
        pkt.type = MSG_TYPE_PRIV_LEAVE;
        strncpy(pkt.senderId, localDeviceId, DEVICE_ID_MAX_LEN);
        strncpy(pkt.targetId, privPeerId, DEVICE_ID_MAX_LEN);
        esp_wifi_get_mac(WIFI_IF_STA, pkt.senderMac);
        pkt.timestamp = millis();
        sendPrivCanvasPacket(&pkt, privPeerMac);
        sendPrivCanvasPacket(&pkt, nullptr);
        restorePublicHistoryFromPrivate();
    }
    privPhase = PRIV_PHASE_IDLE;
    privPeerId[0] = 0;
    memset(privPeerMac, 0, 6);
    memset(privSessionKey, 0, 8);
    privInviteDeadlineMs = 0;
    clearPrivResumeFlags();
    hidePrivInviteDialog();
    showStatusToast("已退出私聊画板", 1800);
    onPrivateCanvasSessionChanged();
    if (currentUIState == UI_STATE_MAIN)
        paintCurrentCanvasPage();
}

void checkPrivateCanvasTimeouts()
{
    if (privInviteDeadlineMs != 0 &&
        (privPhase == PRIV_PHASE_OUTGOING || privPhase == PRIV_PHASE_INCOMING) &&
        (long)(millis() - privInviteDeadlineMs) >= 0) {
        if (privPhase == PRIV_PHASE_INCOMING)
            hidePrivInviteDialog();
        privPhase = PRIV_PHASE_IDLE;
        privPeerId[0] = 0;
        memset(privPeerMac, 0, 6);
        privInviteDeadlineMs = 0;
        clearPrivResumeFlags();
        showStatusToast("私聊邀请超时", 2000);
        onPrivateCanvasSessionChanged();
        return;
    }

    // 私聊对方掉线后重新出现：主动发 RESUME，让对方重进并拉回笔迹
    if (privPhase == PRIV_PHASE_ACTIVE && privPeerMarkedOffline) {
        String key = privPeerMacKey();
        auto hb = peerLastHeartbeat.find(key);
        if (hb != peerLastHeartbeat.end() &&
            (millis() - hb->second) < HEARTBEAT_TIMEOUT_MS)
            offerPrivateCanvasResume();
    }
}

static bool privIdEquals(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return false;
    }
    return *a == *b;
}

void processIncomingPrivQueue()
{
    while (!incomingPrivQueue.empty()) {
        PrivIncoming_t item = incomingPrivQueue.front();
        incomingPrivQueue.pop();
        processIncomingPrivPacket(item.pkt, item.srcMac);
    }
}

void processIncomingPrivPacket(const PrivCanvasPacket_t &pkt, const uint8_t srcMac[6])
{
    if (pkt.magic != PRIV_CANVAS_MAGIC)
        return;
    // 仅目标为本机的邀请/应答才处理（LEAVE/RESUME 也校验）
    if (pkt.type == MSG_TYPE_PRIV_INVITE || pkt.type == MSG_TYPE_PRIV_ACCEPT ||
        pkt.type == MSG_TYPE_PRIV_REJECT || pkt.type == MSG_TYPE_PRIV_RESUME) {
        if (!privIdEquals(pkt.targetId, localDeviceId))
            return;
    }

    switch (pkt.type) {
    case MSG_TYPE_PRIV_INVITE:
        if (privPhase != PRIV_PHASE_IDLE) {
            // 忙：自动拒绝
            PrivCanvasPacket_t rej;
            memset(&rej, 0, sizeof(rej));
            rej.magic = PRIV_CANVAS_MAGIC;
            rej.type = MSG_TYPE_PRIV_REJECT;
            strncpy(rej.senderId, localDeviceId, DEVICE_ID_MAX_LEN);
            strncpy(rej.targetId, pkt.senderId, DEVICE_ID_MAX_LEN);
            esp_wifi_get_mac(WIFI_IF_STA, rej.senderMac);
            rej.timestamp = millis();
            sendPrivCanvasPacket(&rej, srcMac);
            return;
        }
        strncpy(privPeerId, pkt.senderId, DEVICE_ID_MAX_LEN);
        privPeerId[DEVICE_ID_MAX_LEN] = '\0';
        memcpy(privPeerMac, srcMac, 6);
        // 暂存对方 nonce 到 sessionKey 槽
        memcpy(privSessionKey, pkt.nonce, 8);
        privPhase = PRIV_PHASE_INCOMING;
        privInviteDeadlineMs = millis() + PRIV_CANVAS_INVITE_TIMEOUT_MS;
        // 先刷新列表/按钮，再画弹窗，避免 ONLINE_LIST 全屏重绘盖掉邀请框
        onPrivateCanvasSessionChanged();
        showPrivInviteDialog(privPeerId, privInviteDeadlineMs);
        break;

    case MSG_TYPE_PRIV_ACCEPT:
        if (privPhase == PRIV_PHASE_OUTGOING) {
            if (!privIdEquals(pkt.senderId, privPeerId))
                return;
            // 优先用包内 MAC（WiFi/MQTT 路径 srcMac 可能不准）
            if (pkt.senderMac[0] || pkt.senderMac[1] || pkt.senderMac[2] ||
                pkt.senderMac[3] || pkt.senderMac[4] || pkt.senderMac[5])
                memcpy(privPeerMac, pkt.senderMac, 6);
            else if (srcMac)
                memcpy(privPeerMac, srcMac, 6);
            // 邀请人：对方同意后同样进入空白私聊画板
            enterPrivateActive(privPeerId, privPeerMac, privLocalNonce, pkt.nonce);
            break;
        }
        // 本机仍在私聊：对方重启后回 ACK → 推送私聊笔迹
        if (privPhase == PRIV_PHASE_ACTIVE &&
            (privIdEquals(pkt.senderId, privPeerId) || macEqual(srcMac, privPeerMac))) {
            if (pkt.senderMac[0] || pkt.senderMac[1] || pkt.senderMac[2] ||
                pkt.senderMac[3] || pkt.senderMac[4] || pkt.senderMac[5])
                memcpy(privPeerMac, pkt.senderMac, 6);
            else if (srcMac)
                memcpy(privPeerMac, srcMac, 6);
            ensureUnicastPeer(privPeerMac);
            pushPrivateHistoryAfterResume();
            break;
        }
        break;

    case MSG_TYPE_PRIV_REJECT:
        if (privPhase != PRIV_PHASE_OUTGOING)
            return;
        privPhase = PRIV_PHASE_IDLE;
        privInviteDeadlineMs = 0;
        showStatusToast("对方拒绝私聊", 2000);
        privPeerId[0] = 0;
        clearPrivResumeFlags();
        onPrivateCanvasSessionChanged();
        break;

    case MSG_TYPE_PRIV_LEAVE:
        if (privPhase == PRIV_PHASE_ACTIVE &&
            (macEqual(srcMac, privPeerMac) || strcmp(pkt.senderId, privPeerId) == 0)) {
            restorePublicHistoryFromPrivate();
            privPhase = PRIV_PHASE_IDLE;
            privPeerId[0] = 0;
            memset(privPeerMac, 0, 6);
            memset(privSessionKey, 0, 8);
            clearPrivResumeFlags();
            showStatusToast("对方退出私聊", 2000);
            onPrivateCanvasSessionChanged();
            if (currentUIState == UI_STATE_MAIN)
                paintCurrentCanvasPage();
        }
        break;

    case MSG_TYPE_PRIV_RESUME: {
        // 重启方收到：自动加入私聊并回 ACK，等待在线方推送笔迹
        uint8_t peerMac[6];
        if (pkt.senderMac[0] || pkt.senderMac[1] || pkt.senderMac[2] ||
            pkt.senderMac[3] || pkt.senderMac[4] || pkt.senderMac[5])
            memcpy(peerMac, pkt.senderMac, 6);
        else if (srcMac)
            memcpy(peerMac, srcMac, 6);
        else
            break;

        if (privPhase == PRIV_PHASE_ACTIVE &&
            (privIdEquals(pkt.senderId, privPeerId) || macEqual(peerMac, privPeerMac))) {
            // 已在同一私聊：刷新密钥并回 ACK
            memcpy(privSessionKey, pkt.nonce, 8);
            memcpy(privPeerMac, peerMac, 6);
            ensureUnicastPeer(privPeerMac);
            sendPrivResumeAck();
            break;
        }

        if (privPhase == PRIV_PHASE_INCOMING)
            hidePrivInviteDialog();
        // 邀请中/空闲/其它：直接恢复进私聊
        enterPrivateResume(pkt.senderId, peerMac, pkt.nonce);
        sendPrivResumeAck();
        break;
    }
    default:
        break;
    }
}

void sendSyncMessage(const SyncMessage_t *msg)
{
    SyncMessage_t out = *msg;
    applySenderId(&out);

    // 心跳/UPTIME/发现类始终广播；绘图类在私聊时单播并加密
    bool privDraw = isPrivateCanvasActive() &&
                    (out.type == MSG_TYPE_DRAW_POINT || out.type == MSG_TYPE_CANVAS_PAGE ||
                     out.type == MSG_TYPE_SYNC_START || out.type == MSG_TYPE_ALL_DRAWINGS_COMPLETE ||
                     out.type == MSG_TYPE_REQUEST_ALL_DRAWINGS || out.type == MSG_TYPE_RESET_CANVAS ||
                     out.type == MSG_TYPE_CLEAR_AND_REQUEST_UPDATE);

    if (privDraw) {
        xorEncryptSyncMessage(&out);
        ensureUnicastPeer(privPeerMac);
        if (!transportSend((const uint8_t *)&out, sizeof(SyncMessage_t), privPeerMac)) {
            Serial.println("私聊发送失败(全通道)");
        }
        return;
    }

    if (!transportSend((const uint8_t *)&out, sizeof(SyncMessage_t), nullptr)) {
        Serial.print("发送 SyncMessage 类型 ");
        Serial.print(out.type);
        Serial.println(" 失败(全通道)");
    }
}

// 处理接收到的消息队列
void processIncomingMessages()
{
    // 声明外部变量/函数，如果它们在 .ino 或其他模块中定义
    extern bool isScreenOn;                 // 来自主 .ino 或 power_manager
    extern bool hasNewUpdateWhileScreenOff; // 来自主 .ino 或 power_manager

    while (!incomingMessageQueue.empty())
    {
        SyncMessage_t msg = incomingMessageQueue.front();
        incomingMessageQueue.pop();

        unsigned long localCurrentRawUptime = millis();
        long localCurrentOffset = relativeBootTimeOffset;
        unsigned long localEffectiveUptime = localCurrentRawUptime + localCurrentOffset;

        unsigned long peerRawUptime = msg.senderUptime;
        long peerReceivedOffset = msg.senderOffset;
        unsigned long peerEffectiveUptime = peerRawUptime + peerReceivedOffset;

        switch (msg.type)
        {
        case MSG_TYPE_UPTIME_INFO:
        {
            // 例行心跳式 UPTIME：只更新对端信息；reboot 检测后允许立即恢复
            lastKnownPeerUptime = peerRawUptime;
            lastKnownPeerOffset = peerReceivedOffset;
            initialSyncLogicProcessed = true;

            String peerKey = macKeyFromLastPeer();
            detectPeerRebootAndAllowResync(peerKey, peerRawUptime);

            // 私聊画板进行中：禁止公屏同步清掉私聊笔迹；对端重连则发 RESUME
            if (isPrivateCanvasActive()) {
                if (privPeerKeyMatches(peerKey)) {
                    // reboot 检测已清 sync 标记，或曾掉线 → 发 RESUME
                    if (privPeerMarkedOffline || peerNeedsCanvasSync(peerKey) ||
                        privResumeAwaitingAck) {
                        if (peerNeedsCanvasSync(peerKey))
                            privPeerMarkedOffline = true;
                        offerPrivateCanvasResume();
                    }
                    markPeerCanvasSynced(peerKey);
                } else {
                    markPeerCanvasSynced(peerKey);
                }
                break;
            }

            if (!peerNeedsCanvasSync(peerKey)) {
                break;
            }

            // 刚清页后点数变少：绝不能再向对端「拉回」旧笔迹
            if (recentlyDestructivelyEdited(25000UL)) {
                Serial.println("  本机刚清页/清空：推送本机历史，拒绝拉取对端旧数据");
                if (!iamRequestingAllData && !isReceivingDrawingData && !isAwaitingSyncStartResponse)
                    forcePushDrawingHistoryToPeers();
                markPeerCanvasSynced(peerKey);
                break;
            }

            Serial.println("收到 MSG_TYPE_UPTIME_INFO (首次与该对端同步评估)");

            {
                Serial.println("  处理 UPTIME_INFO 进行同步决策。");
                Serial.print("  本地有效运行时间: ");
                Serial.print(localEffectiveUptime);
                Serial.print("  对端有效运行时间: ");
                Serial.println(peerEffectiveUptime);

                if (localEffectiveUptime == peerEffectiveUptime)
                {
                    uint8_t myMacAddr[6];
                    esp_wifi_get_mac(WIFI_IF_STA, myMacAddr);
                    if (memcmp(myMacAddr, lastPeerMac, 6) < 0)
                    {
                        Serial.println("  决策 (有效运行时间相同, MAC较小): 本机请求数据。");
                        if (iamRequestingAllData || isReceivingDrawingData || isSendingDrawingData)
                            break; // 忙则下次 UPTIME 再试，绝不提前标记成功
                        iamEffectivelyMoreUptimeDevice = false;
                        iamRequestingAllData = true;
                        isAwaitingSyncStartResponse = true;
                        allDrawingHistory.clear();
                        SyncMessage_t syncStartMsgBeforeRequest;
                        syncStartMsgBeforeRequest.type = MSG_TYPE_SYNC_START;
                        syncStartMsgBeforeRequest.senderUptime = localCurrentRawUptime;
                        syncStartMsgBeforeRequest.senderOffset = localCurrentOffset;
                        memset(&syncStartMsgBeforeRequest.touch_data, 0, sizeof(TouchData_t));
                        sendSyncMessage(&syncStartMsgBeforeRequest);
                        SyncMessage_t requestMsg;
                        requestMsg.type = MSG_TYPE_REQUEST_ALL_DRAWINGS;
                        requestMsg.senderUptime = localCurrentRawUptime;
                        requestMsg.senderOffset = localCurrentOffset;
                        memset(&requestMsg.touch_data, 0, sizeof(TouchData_t));
                        sendSyncMessage(&requestMsg);
                        timeRequestSentForAllDrawings = millis();
                    }
                    else
                    {
                        Serial.println("  决策 (有效运行时间相同, MAC较大): 通知对端向我请求。");
                        iamEffectivelyMoreUptimeDevice = true;
                        iamRequestingAllData = false;
                        SyncMessage_t promptMsg;
                        promptMsg.type = MSG_TYPE_CLEAR_AND_REQUEST_UPDATE;
                        promptMsg.senderUptime = localCurrentRawUptime;
                        promptMsg.senderOffset = localCurrentOffset;
                        memset(&promptMsg.touch_data, 0, sizeof(TouchData_t));
                        sendSyncMessage(&promptMsg);
                        markPeerCanvasSynced(peerKey); // 本机作为数据源侧
                    }
                }
                else
                {
                    unsigned long effectiveUptimeDifference = (localEffectiveUptime > peerEffectiveUptime)
                        ? (localEffectiveUptime - peerEffectiveUptime)
                        : (peerEffectiveUptime - localEffectiveUptime);

                    if (effectiveUptimeDifference <= EFFECTIVE_UPTIME_SYNC_THRESHOLD)
                    {
                        Serial.println("  决策: 有效运行时间差在阈值内，不同步。");
                        markPeerCanvasSynced(peerKey);
                    }
                    else if (localEffectiveUptime < peerEffectiveUptime)
                    {
                        Serial.println("  决策: 本机较新，向对端请求历史。");
                        if (iamRequestingAllData || isReceivingDrawingData || isSendingDrawingData)
                            break;
                        iamEffectivelyMoreUptimeDevice = false;
                        iamRequestingAllData = true;
                        isAwaitingSyncStartResponse = true;
                        allDrawingHistory.clear();
                        SyncMessage_t syncStartMsgBeforeRequest2;
                        syncStartMsgBeforeRequest2.type = MSG_TYPE_SYNC_START;
                        syncStartMsgBeforeRequest2.senderUptime = localCurrentRawUptime;
                        syncStartMsgBeforeRequest2.senderOffset = localCurrentOffset;
                        memset(&syncStartMsgBeforeRequest2.touch_data, 0, sizeof(TouchData_t));
                        sendSyncMessage(&syncStartMsgBeforeRequest2);
                        SyncMessage_t requestMsg;
                        requestMsg.type = MSG_TYPE_REQUEST_ALL_DRAWINGS;
                        requestMsg.senderUptime = localCurrentRawUptime;
                        requestMsg.senderOffset = localCurrentOffset;
                        memset(&requestMsg.touch_data, 0, sizeof(TouchData_t));
                        sendSyncMessage(&requestMsg);
                        timeRequestSentForAllDrawings = millis();
                    }
                    else
                    {
                        Serial.println("  决策: 本机较旧，提示对端向我同步。");
                        iamEffectivelyMoreUptimeDevice = true;
                        iamRequestingAllData = false;
                        SyncMessage_t promptMsg;
                        promptMsg.type = MSG_TYPE_CLEAR_AND_REQUEST_UPDATE;
                        promptMsg.senderUptime = localCurrentRawUptime;
                        promptMsg.senderOffset = localCurrentOffset;
                        memset(&promptMsg.touch_data, 0, sizeof(TouchData_t));
                        sendSyncMessage(&promptMsg);
                        markPeerCanvasSynced(peerKey);
                    }
                }
            }
            break;
        }
        case MSG_TYPE_DRAW_POINT:
        {
            TouchData_t currentPointData = msg.touch_data; // Declare once at the beginning of the case
            if (currentPointData.ownerHash == 0 && msg.senderId[0])
                currentPointData.ownerHash = deviceIdToOwnerHash(msg.senderId);
            int mapX = currentPointData.x;
            int mapY = currentPointData.y;
            const char *drawerId = msg.senderId[0] ? msg.senderId : "?";
            bumpCanvasPageCountFromPoint(currentPointData.page);
            const bool onViewPage = (currentPointData.page == currentCanvasPage);
            const bool strokeContinue = (lastRemotePoint.z != 0 && lastRemotePage == currentPointData.page);

            if (isReceivingDrawingData)
            {
                // 空闲/全量同步：只入库，不逐点动画；结束后一次性 paintCurrentCanvasPage
                allDrawingHistory.push_back(currentPointData);
                receivedHistoryPointCount++;
                lastRemotePoint.x = mapX;
                lastRemotePoint.y = mapY;
                lastRemotePoint.z = 1;
                lastRemoteDrawTime = currentPointData.timestamp;
                lastRemoteColor = currentPointData.color;
                lastRemotePage = currentPointData.page;
                if (!isScreenOn)
                    hasNewUpdateWhileScreenOff = true;
                if ((receivedHistoryPointCount & 127) == 0)
                    yield();
            }
            else if (!iamRequestingAllData && !isSendingDrawingData && !isAwaitingSyncStartResponse)
            {
                clearCanvasRedoStack();
                allDrawingHistory.push_back(currentPointData);
                if (!shouldSkipCanvasPaint() && onViewPage) {
                    setActivityStatus(drawerId,
                                      currentPointData.color == TFT_BLACK ? "在擦" : "在画");
                    if (currentPointData.color == TFT_BLACK) {
                        int r = resolveEraserRadius(currentPointData.brushR);
                        uint16_t oh = currentPointData.ownerHash
                                          ? currentPointData.ownerHash
                                          : deviceIdToOwnerHash(drawerId);
                        bool erased = false;
                        if (strokeContinue && lastRemoteColor == TFT_BLACK &&
                            (currentPointData.timestamp - lastRemoteDrawTime <= TOUCH_STROKE_INTERVAL)) {
                            erased = eraseOwnerInkSegment(lastRemotePoint.x, lastRemotePoint.y,
                                                          mapX, mapY, r, currentPointData.page, oh);
                        } else {
                            erased = eraseOwnerInkNear(mapX, mapY, r, currentPointData.page, oh);
                        }
                        // 历史里仍保留橡皮事件；实心点已删
                        // 即时黑圆，少做整屏重绘防闪
                        if (strokeContinue && lastRemoteColor == TFT_BLACK &&
                            (currentPointData.timestamp - lastRemoteDrawTime <= TOUCH_STROKE_INTERVAL))
                            applyEraserSegment(lastRemotePoint.x, lastRemotePoint.y, mapX, mapY, r);
                        else
                            applyEraserDot(mapX, mapY, r);
                        if (erased)
                            paintCanvasAfterOwnerErase(); // 内部已强节流
                    } else {
                        int r = resolveBrushRadius(currentPointData.brushR);
                        if (!strokeContinue ||
                            currentPointData.timestamp - lastRemoteDrawTime > TOUCH_STROKE_INTERVAL ||
                            lastRemoteColor == TFT_BLACK)
                            applyBrushDot(mapX, mapY, currentPointData.color, r);
                        else
                            applyBrushSegment(lastRemotePoint.x, lastRemotePoint.y, mapX, mapY,
                                              currentPointData.color, r);
                    }
                } else if (!onViewPage) {
                    // 对端在别的页编辑：只存历史，不污染当前页画面
                    if (currentPointData.color == TFT_BLACK) {
                        uint16_t oh = currentPointData.ownerHash
                                          ? currentPointData.ownerHash
                                          : deviceIdToOwnerHash(drawerId);
                        int r = resolveEraserRadius(currentPointData.brushR);
                        eraseOwnerInkNear(mapX, mapY, r, currentPointData.page, oh);
                    }
                } else {
                    pendingCanvasRedrawAfterChat = true;
                }
                lastRemotePoint.x = mapX;
                lastRemotePoint.y = mapY;
                lastRemotePoint.z = 1;
                lastRemoteDrawTime = currentPointData.timestamp;
                lastRemoteColor = currentPointData.color;
                lastRemotePage = currentPointData.page;
                if (!isScreenOn)
                    hasNewUpdateWhileScreenOff = true;
            }
            else
            {
                // 中间同步状态，忽略
            }
            break;
        }
        case MSG_TYPE_CANVAS_PAGE:
        {
            uint8_t action = (uint8_t)msg.touch_data.color;
            uint8_t page = msg.touch_data.page;
            uint8_t count = (uint8_t)msg.touch_data.y;
            if (count == 0)
                count = (uint8_t)msg.touch_data.x; // 兜底
            applyRemoteCanvasPage(action, page, count, msg.senderId);
            {
                const char *who = msg.senderId[0] ? msg.senderId : "Peer";
                if (action == CANVAS_PAGE_ACT_CREATE)
                    setActivityStatus(who, "新建页");
                else if (action == CANVAS_PAGE_ACT_DELETE)
                    setActivityStatus(who, "删页");
                else if (action == CANVAS_PAGE_ACT_CLEAR)
                    setActivityStatus(who, "清页");
                else if (action == CANVAS_PAGE_ACT_UNDO)
                    setActivityStatus(who, "撤销");
                else if (action == CANVAS_PAGE_ACT_INFO)
                    setActivityStatus(who, "翻页");
            }
            if (!isScreenOn)
                hasNewUpdateWhileScreenOff = true;
            break;
        }
        case MSG_TYPE_REQUEST_ALL_DRAWINGS:
        {
            Serial.println("收到 MSG_TYPE_REQUEST_ALL_DRAWINGS.");
            const bool forceResync = (msg.touch_data.x == CANVAS_FORCE_RESYNC_FLAG);

            // 私聊中：不走公屏请求路径（加密历史对方可能尚未恢复密钥）
            // 对端重连由 PRIV_RESUME → ACCEPT → forcePush 完成
            if (isPrivateCanvasActive()) {
                if (privPeerKeyMatches(macKeyFromLastPeer()) ||
                    privActivePeerMacEquals(lastPeerMac)) {
                    offerPrivateCanvasResume();
                }
                break;
            }

            if (forceResync) {
                clearPeerCanvasSyncState(macKeyFromLastPeer());
                cancelSignalRecoveryFallback();
            }
            if (!localIsSyncSource(localEffectiveUptime, peerEffectiveUptime))
            {
                Serial.println("  决策: 收到 REQUEST_ALL_DRAWINGS，但本机并非同步源。忽略。");
                if (peerRawUptime != 0)
                    lastKnownPeerUptime = peerRawUptime;
                if (peerReceivedOffset != 0 || lastKnownPeerOffset != 0)
                    lastKnownPeerOffset = peerReceivedOffset;
                initialSyncLogicProcessed = true;
                break;
            }

            if (iamRequestingAllData || isReceivingDrawingData || isSendingDrawingData)
            {
                Serial.println("  但本机正在进行其他同步操作，忽略此新的 REQUEST_ALL_DRAWINGS 请求。");
                if (peerRawUptime != 0)
                    lastKnownPeerUptime = peerRawUptime;
                if (peerReceivedOffset != 0 || lastKnownPeerOffset != 0)
                    lastKnownPeerOffset = peerReceivedOffset;
                initialSyncLogicProcessed = true;
                break;
            }

            Serial.print("  决策: 本机作为同步源。发送所有 ");
            Serial.print(allDrawingHistory.size());
            Serial.println(" 个点。");
            iamEffectivelyMoreUptimeDevice = true;
            lastKnownPeerUptime = peerRawUptime;
            lastKnownPeerOffset = peerReceivedOffset;
            initialSyncLogicProcessed = true;

            SyncMessage_t syncStartMsgBeforeSending;
            syncStartMsgBeforeSending.type = MSG_TYPE_SYNC_START;
            syncStartMsgBeforeSending.senderUptime = localCurrentRawUptime;
            syncStartMsgBeforeSending.senderOffset = localCurrentOffset;
            memset(&syncStartMsgBeforeSending.touch_data, 0, sizeof(TouchData_t));
            syncStartMsgBeforeSending.totalPointsForSync = allDrawingHistory.size();
            sendSyncMessage(&syncStartMsgBeforeSending);
            Serial.println("  发送 MSG_TYPE_SYNC_START (准备发送历史数据，将开始分批发送)");

            // 空闲同步：不播发送进度圈 / toast（接收端也静默一次性重绘）
            hideSendProgress();
            isSendingDrawingData = true;
            currentHistorySendIndex = 0;
            break;
        }
        case MSG_TYPE_ALL_DRAWINGS_COMPLETE:
        {
            Serial.println("收到 MSG_TYPE_ALL_DRAWINGS_COMPLETE.");
            if (isReceivingDrawingData)
            {
                Serial.println("  同步完成 (本机为较新设备，已接收完数据)。");
                // 确保接收进度条更新到100% (如果需要，但现在隐藏由外部控制)
                // if (totalPointsExpectedFromPeer > 0) {
                //     updateReceiveProgress(receivedHistoryPointCount, totalPointsExpectedFromPeer);
                // }

                // 使用 SNTP-like 的时间同步方法：
                // Offset = (服务器发送时间戳 + 服务器已知偏移) - 客户端接收时间戳
                // localCurrentRawUptime 是在处理此消息时本机的 millis()
                relativeBootTimeOffset = (long)peerRawUptime + (long)peerReceivedOffset - localCurrentRawUptime;
                Serial.println("  使用 localCurrentRawUptime (SNTP-like) 计算 offset");

                // 记录一下 timeRequestSentForAllDrawings 的状态，但它不再用于主要计算
                if (timeRequestSentForAllDrawings == 0)
                {
                    Serial.println("  警告: timeRequestSentForAllDrawings 为 0 (本应在请求时设置).");
                }
                // else {
                //    long rtt_approx = localCurrentRawUptime - timeRequestSentForAllDrawings;
                //    Serial.print("  近似 RTT: "); Serial.println(rtt_approx);
                // }

                iamRequestingAllData = false;
                isReceivingDrawingData = false;
                isAwaitingSyncStartResponse = false;
                timeRequestSentForAllDrawings = 0;
                uptimeOfLastPeerSyncedFrom = peerRawUptime;

                hideReceiveProgress();

                Serial.print("  relativeBootTimeOffset 计算并设置为: ");
                Serial.println(relativeBootTimeOffset);
                lastKnownPeerUptime = peerRawUptime;
                lastKnownPeerOffset = peerReceivedOffset;

                markPeerCanvasSynced(macKeyFromLastPeer());
                refreshCanvasPageCountFromHistory();
                if (shouldSkipCanvasPaint()) {
                    pendingCanvasRedrawAfterChat = true;
                } else {
                    // 空闲同步结束：一次性重绘，无逐点动画
                    pendingCanvasRedrawAfterChat = false;
                    paintCurrentCanvasPage();
                }

                Serial.println("  同步完成。");
                Serial.print("  本地有效运行时间: ");
                Serial.print(millis() + relativeBootTimeOffset);
                Serial.print(" (原始: ");
                Serial.print(millis());
                Serial.print(", 偏移: ");
                Serial.print(relativeBootTimeOffset);
                Serial.println(")");
                Serial.print("  对端有效运行时间: ");
                Serial.print(peerEffectiveUptime);
                Serial.print(" (原始: ");
                Serial.print(peerRawUptime);
                Serial.print(", 偏移: ");
                Serial.print(peerReceivedOffset);
                Serial.println(")");
                initialSyncLogicProcessed = true;
            }
            else if (iamRequestingAllData && isAwaitingSyncStartResponse) // 异常：等待SYNC_START却收到COMPLETE
            {
                Serial.println("  异常：正在等待 SYNC_START，却收到了 ALL_DRAWINGS_COMPLETE。可能同步中断。重新发起请求。");
                // 状态: iamRequestingAllData = true, isAwaitingSyncStartResponse = true (保持)

                // 重新发送 SYNC_START (表明本机意图，因为我们仍想同步)
                SyncMessage_t syncStartMsgRetry;
                syncStartMsgRetry.type = MSG_TYPE_SYNC_START;
                syncStartMsgRetry.senderUptime = localCurrentRawUptime; // 使用当前时间
                syncStartMsgRetry.senderOffset = localCurrentOffset;
                memset(&syncStartMsgRetry.touch_data, 0, sizeof(TouchData_t));
                sendSyncMessage(&syncStartMsgRetry);
                Serial.println("  重新发送 MSG_TYPE_SYNC_START (在重新请求所有绘图前)");

                // 重新发送 REQUEST_ALL_DRAWINGS
                SyncMessage_t requestMsgRetry;
                requestMsgRetry.type = MSG_TYPE_REQUEST_ALL_DRAWINGS;
                requestMsgRetry.senderUptime = localCurrentRawUptime; // 使用当前时间
                requestMsgRetry.senderOffset = localCurrentOffset;
                memset(&requestMsgRetry.touch_data, 0, sizeof(TouchData_t));
                sendSyncMessage(&requestMsgRetry);
                timeRequestSentForAllDrawings = millis(); // 更新请求时间戳
                Serial.println("  重新发送 MSG_TYPE_REQUEST_ALL_DRAWINGS.");

                // 更新对端信息，因为这仍然是来自对端的最新（尽管可能是异常的）消息
                lastKnownPeerUptime = peerRawUptime;
                lastKnownPeerOffset = peerReceivedOffset;
                initialSyncLogicProcessed = true; // 标记已处理此消息
            }
            else
            {
                Serial.println("  收到 ALL_DRAWINGS_COMPLETE，但本机状态不符 (非正常完成，也非等待SYNC_START时收到)。忽略。");
                // 考虑是否也更新对端信息
                // lastKnownPeerUptime = peerRawUptime;
                // lastKnownPeerOffset = peerReceivedOffset;
            }
            break;
        }
        case MSG_TYPE_CLEAR_AND_REQUEST_UPDATE:
        {
            Serial.println("收到 MSG_TYPE_CLEAR_AND_REQUEST_UPDATE.");
            String peerKeyClear = macKeyFromLastPeer();

            if (isPrivateCanvasActive()) {
                if (privPeerKeyMatches(peerKeyClear))
                    offerPrivateCanvasResume();
                markPeerCanvasSynced(peerKeyClear);
                break;
            }

            const bool forceResync = (msg.touch_data.x == CANVAS_FORCE_RESYNC_FLAG);
            if (forceResync) {
                clearPeerCanvasSyncState(peerKeyClear);
                cancelSignalRecoveryFallback();
                Serial.println("  强制重同步（信号恢复）。");
            } else if (!peerNeedsCanvasSync(peerKeyClear)) {
                // 已与该对端同步过：忽略重复 CLEAR，杜绝无故 restore
                Serial.println("  忽略：该对端已同步/冷却中。");
                lastKnownPeerUptime = peerRawUptime;
                lastKnownPeerOffset = peerReceivedOffset;
                initialSyncLogicProcessed = true;
                break;
            }

            unsigned long effectiveUptimeDifference =
                (localEffectiveUptime > peerEffectiveUptime)
                    ? (localEffectiveUptime - peerEffectiveUptime)
                    : (peerEffectiveUptime - localEffectiveUptime);
            const bool shouldRequest = forceResync
                ? !localIsSyncSource(localEffectiveUptime, peerEffectiveUptime)
                : (localEffectiveUptime < peerEffectiveUptime &&
                   effectiveUptimeDifference > EFFECTIVE_UPTIME_SYNC_THRESHOLD);

            if (shouldRequest)
            {
                Serial.println("  决策: 本机向对端请求全量笔迹。");
                if (iamRequestingAllData || isReceivingDrawingData || isSendingDrawingData)
                {
                    Serial.println("  但当前已有同步正在进行，忽略新的 CLEAR_AND_REQUEST_UPDATE 触发的同步请求。");
                    lastKnownPeerUptime = peerRawUptime;
                    lastKnownPeerOffset = peerReceivedOffset;
                    initialSyncLogicProcessed = true;
                    break;
                }
                beginRequestAllDrawings();
                lastKnownPeerUptime = peerRawUptime;
                lastKnownPeerOffset = peerReceivedOffset;
                initialSyncLogicProcessed = true;
            }
            else
            {
                Serial.println("  决策: 收到 CLEAR_AND_REQUEST_UPDATE，但本机应为同步源。忽略请求。");
                markPeerCanvasSynced(peerKeyClear);
                if (peerRawUptime != 0)
                    lastKnownPeerUptime = peerRawUptime;
                if (peerReceivedOffset != 0 || lastKnownPeerOffset != 0)
                    lastKnownPeerOffset = peerReceivedOffset;
                initialSyncLogicProcessed = true;
            }
            break;
        }
        case MSG_TYPE_RESET_CANVAS:
        {
            Serial.println("收到 MSG_TYPE_RESET_CANVAS.");
            allDrawingHistory.clear();
            clearCanvasRedoStack();
            if (shouldSkipCanvasPaint())
                pendingCanvasRedrawAfterChat = true;
            else {
                pendingCanvasRedrawAfterChat = false;
                clearScreenAndCache();
            }
            relativeBootTimeOffset = 0;
            iamEffectivelyMoreUptimeDevice = false;
            iamRequestingAllData = false;
            isAwaitingSyncStartResponse = false;
            isReceivingDrawingData = false;
            isSendingDrawingData = false;
            initialSyncLogicProcessed = true;
            lastKnownPeerUptime = 0;
            lastKnownPeerOffset = 0;
            uptimeOfLastPeerSyncedFrom = 0;
            canvasSyncedPeers.clear();
            lastCanvasSyncMs.clear();
            SyncMessage_t uptimeInfoMsg;
            uptimeInfoMsg.type = MSG_TYPE_UPTIME_INFO;
            uptimeInfoMsg.senderUptime = localCurrentRawUptime;
            uptimeInfoMsg.senderOffset = relativeBootTimeOffset;
            memset(&uptimeInfoMsg.touch_data, 0, sizeof(TouchData_t));
            sendSyncMessage(&uptimeInfoMsg);
            if (!isScreenOn)
                hasNewUpdateWhileScreenOff = true;
            break;
        }
        case MSG_TYPE_SYNC_START:
        {
            Serial.println("收到 MSG_TYPE_SYNC_START");
            cancelSignalRecoveryFallback();
            const bool forcePush = (msg.touch_data.x == CANVAS_FORCE_RESYNC_FLAG);
            // 本机刚清页/清空：若对端点数更多，多半是过期历史，拒绝被盖回
            if (forcePush && recentlyDestructivelyEdited(20000UL) &&
                msg.totalPointsForSync > allDrawingHistory.size() + 8) {
                Serial.println("  忽略强制同步：本机刚做过清页，对端历史可能过期");
                lastKnownPeerUptime = peerRawUptime;
                lastKnownPeerOffset = peerReceivedOffset;
                break;
            }

            const bool acceptSync =
                forcePush ||
                (iamRequestingAllData && isAwaitingSyncStartResponse && !iamEffectivelyMoreUptimeDevice);

            if (acceptSync)
            {
                if (forcePush)
                    Serial.println("  接受对端强制推送历史（如清页后权威同步）。");
                else
                    Serial.println("  本机作为请求方，收到响应方的 SYNC_START。准备清空并接收数据。");

                // 正在发送则停掉，改收对端权威数据
                isSendingDrawingData = false;
                hideSendProgress();

                allDrawingHistory.clear();
                clearCanvasRedoStack();
                // 空闲同步：不清屏、不播进度动画，收完后一次性 paint
                // （避免逐点“动画”和中间闪屏）
                if (shouldSkipCanvasPaint())
                    pendingCanvasRedrawAfterChat = true;

                lastRemotePoint.x = 0;
                lastRemotePoint.y = 0;
                lastRemotePoint.z = 0;
                lastRemoteDrawTime = 0;
                lastRemoteColor = 0;
                lastRemotePage = 0;

                iamRequestingAllData = false;
                isAwaitingSyncStartResponse = false;
                isReceivingDrawingData = true;

                totalPointsExpectedFromPeer = msg.totalPointsForSync;
                receivedHistoryPointCount = 0;
                hideReceiveProgress();

                lastKnownPeerUptime = peerRawUptime;
                lastKnownPeerOffset = peerReceivedOffset;
                Serial.print("  静默同步：准备从对端 (raw uptime: ");
                Serial.print(peerRawUptime);
                Serial.print(", total points: ");
                Serial.print(totalPointsExpectedFromPeer);
                Serial.println(") 接收数据。");
            }
            else if (isSendingDrawingData)
            {
                Serial.println("  本机正在发送数据，但收到了一个 SYNC_START。可能是对方也想发起同步。忽略此 SYNC_START，继续本机发送流程。");
            }
            else if (isReceivingDrawingData)
            {
                Serial.println("  本机正在接收数据，又收到了一个 SYNC_START。可能是重复信号或来自不同设备的干扰。忽略。");
            }
            else
            {
                Serial.println("  收到 SYNC_START，但本机状态不符 (非明确的请求/响应流程中)。可能是一个错序或无关的信号。");
                lastKnownPeerUptime = peerRawUptime;
                lastKnownPeerOffset = peerReceivedOffset;
            }
            break;
        }
        case MSG_TYPE_HEARTBEAT:
        {
            // 收到心跳包，OnSyncDataRecv 中已经更新了 peerLastHeartbeat，这里可以根据需要添加调试信息
            // Serial.print("收到心跳包，来自对端 MAC (最后通信): "); // 调试信息，如果频繁可能会刷屏
            // char macStr[18];
            // snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
            //          lastPeerMac[0], lastPeerMac[1], lastPeerMac[2],
            //          lastPeerMac[3], lastPeerMac[4], lastPeerMac[5]);
            // Serial.println(macStr);
            break;
        }
        default:
        {
            Serial.print("收到未知消息类型: ");
            Serial.println(msg.type);
            break;
        }
        } // End of switch (msg.type)
    } // End of while (!incomingMessageQueue.empty())

    // --- 分批发送历史数据逻辑 ---
    if (isSendingDrawingData)
    {
        const size_t BATCH_SIZE = 50; // 每批发送15个点
        size_t pointsSentThisCycle = 0;
        unsigned long batchSendStartTime = millis();        // 用于该批次消息的时间戳
        unsigned long currentSenderUptimeForMsg = millis(); // 获取一次，用于本批次所有消息
        long currentSenderOffsetForMsg = relativeBootTimeOffset;

        while (currentHistorySendIndex < allDrawingHistory.size() && pointsSentThisCycle < BATCH_SIZE)
        {
            const auto &drawData = allDrawingHistory[currentHistorySendIndex];

            SyncMessage_t historyPointMsg;
            historyPointMsg.type = MSG_TYPE_DRAW_POINT;
            historyPointMsg.senderUptime = currentSenderUptimeForMsg;
            historyPointMsg.senderOffset = currentSenderOffsetForMsg;
            historyPointMsg.touch_data = drawData;

            sendSyncMessage(&historyPointMsg);
            // WiFi 并发辅助时少延时；纯 ESP-NOW 略延时防刷屏丢包
            delay(wifiAssistActive() ? 1 : 3);

            currentHistorySendIndex++;
            pointsSentThisCycle++;
        }

        if (currentHistorySendIndex >= allDrawingHistory.size())
        {
            // 所有数据点已发送完毕
            SyncMessage_t completeMsg;
            completeMsg.type = MSG_TYPE_ALL_DRAWINGS_COMPLETE;
            completeMsg.senderUptime = millis();
            completeMsg.senderOffset = relativeBootTimeOffset;
            memset(&completeMsg.touch_data, 0, sizeof(TouchData_t));
            sendSyncMessage(&completeMsg);

            Serial.println("  所有历史绘图数据已分批发送完毕。发送了 ALL_DRAWINGS_COMPLETE。");
            hideSendProgress();
            isSendingDrawingData = false;
            // currentHistorySendIndex = 0;
        }
        else if (pointsSentThisCycle > 0)
        {
            // 空闲同步不刷新进度圈，只打日志
            Serial.print("  分批发送：已发送 ");
            Serial.print(pointsSentThisCycle);
            Serial.print(" 个点，总计已发送 ");
            Serial.print(currentHistorySendIndex);
            Serial.print("/");
            Serial.print(allDrawingHistory.size());
            Serial.println(" 个点。");
        }
        // 如果仍在发送过程中 (isSendingDrawingData 仍为 true 且还有数据)，让出CPU
        if (isSendingDrawingData && currentHistorySendIndex < allDrawingHistory.size())
        {
            yield();
        }
    }
} // End of processIncomingMessages()

// 新增：发送心跳包
void sendHeartbeat()
{
    SyncMessage_t heartbeatMsg;
    heartbeatMsg.type = MSG_TYPE_HEARTBEAT;
    heartbeatMsg.senderUptime = millis();
    heartbeatMsg.senderOffset = relativeBootTimeOffset;
    memset(&heartbeatMsg.touch_data, 0, sizeof(TouchData_t));
    // 电量塞进 touch_data.x（0-100），供在线列表显示
    extern float readBatteryVoltagePercentage();
    int bat = (int)(readBatteryVoltagePercentage() + 0.5f);
    if (bat < 0) bat = 0;
    if (bat > 100) bat = 100;
    heartbeatMsg.touch_data.x = bat;
    size_t pts = allDrawingHistory.size();
    heartbeatMsg.totalPointsForSync = (pts > 65535) ? 65535 : (uint16_t)pts;
    // 获取并添加内存信息
    heartbeatMsg.usedMemory = esp_get_free_heap_size(); // 使用 esp_get_free_heap_size 获取可用堆内存
    heartbeatMsg.totalMemory = ESP.getHeapSize(); // 使用 ESP.getHeapSize 获取总堆内存

    sendSyncMessage(&heartbeatMsg);
    // Serial.println("发送心跳包."); // 调试信息，如果频繁发送可能会刷屏
}

// 新增：检查对端心跳超时
void checkPeerHeartbeatTimeout()
{
    unsigned long currentTime = millis();
    // 使用一个临时的 vector 来存储需要移除的 MAC 地址，避免在迭代时修改 map
    std::vector<String> macsToRemove;

    for (auto const& [mac, lastHeartbeatTime] : peerLastHeartbeat)
    {
        if (currentTime - lastHeartbeatTime > HEARTBEAT_TIMEOUT_MS) // HEARTBEAT_TIMEOUT_MS 定义在 config.h
        {
            Serial.print("对端 ");
            Serial.print(mac);
            Serial.println(" 心跳超时，认为已下线。");
            macsToRemove.push_back(mac);
        }
    }

    // 移除超时的对端
    for (const auto& mac : macsToRemove)
    {
        const char *leaveId = mac.c_str();
        auto it = peerInfoMap.find(mac);
        if (it != peerInfoMap.end() && it->second.deviceId[0])
            leaveId = it->second.deviceId;

        // 私聊对方掉线：保留私聊笔迹与会话，等待重启后 RESUME
        if (privPhase == PRIV_PHASE_ACTIVE && privPeerKeyMatches(mac)) {
            privPeerMarkedOffline = true;
            privResumeAwaitingAck = false;
            lastPrivResumeOfferMs = 0;
            showStatusToast("私聊对方掉线", 2000);
        }

        peerLeftNotify(leaveId);

        peerLastHeartbeat.erase(mac);
        macSet.erase(mac);
        peerInfoMap.erase(mac);
        peerSignalState.erase(mac);
        if (pendingSignalRecoveryResync && pendingSignalRecoveryPeer == mac)
            pendingSignalRecoveryResync = false;
        if (signalRecoveryFallbackPeer == mac) {
            signalRecoveryFallbackAtMs = 0;
            signalRecoveryFallbackPeer = "";
            signalRecoveryAllowLargerMac = false;
        }
        // 保留 canvasSyncedPeers / lastCanvasSyncMs：短暂掉线后再上线不得再次全量 restore
        // （信号变差→恢复由 RSSI 状态机单独触发重同步）
        updateConnectedDevicesCount();
    }
}

// 新增：获取对端信息列表
std::vector<PeerInfo_t> getPeerInfoList() {
    std::vector<PeerInfo_t> peerList;
    int count = 0;
    for (auto const& [mac, peerInfo] : peerInfoMap) {
        if (count < MAX_PEERS_TO_DISPLAY) { // 限制返回的数量
            peerList.push_back(peerInfo);
            count++;
        } else {
            break;
        }
    }
    return peerList;
}

void sendChatEx(MessageType_t type, uint8_t mode, const char *targetId, const char *text, uint16_t color)
{
    ChatPacket_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = type;
    pkt.mode = mode;
    pkt.textColor = color ? color : TFT_WHITE;
    strncpy(pkt.senderId, localDeviceId, DEVICE_ID_MAX_LEN);
    pkt.senderId[DEVICE_ID_MAX_LEN] = '\0';
    if (targetId) {
        strncpy(pkt.targetId, targetId, DEVICE_ID_MAX_LEN);
        pkt.targetId[DEVICE_ID_MAX_LEN] = '\0';
    }
    if (text) {
        strncpy(pkt.text, text, CHAT_TEXT_MAX);
        pkt.text[CHAT_TEXT_MAX] = '\0';
    }
    pkt.timestamp = millis();

    if (!transportSend((const uint8_t *)&pkt, sizeof(pkt), nullptr)) {
        Serial.println("发送聊天包失败(全通道)");
    }
}

void sendChatPacket(MessageType_t type, const char *text)
{
    sendChatEx(type, CHAT_MODE_PUBLIC, "", text, TFT_WHITE);
}

void processIncomingChatPacket(const ChatPacket_t &pkt)
{
    if (pkt.type == MSG_TYPE_CHAT_GROUP) {
        // INVITE：仅目标设备处理；DISBAND：成员同步删除
        if (strncmp(pkt.text, "INVITE:", 7) == 0) {
            if (strcmp(pkt.targetId, localDeviceId) != 0)
                return;
        }
        if (strncmp(pkt.text, "DISBAND:", 8) == 0) {
            char tip[40];
            snprintf(tip, sizeof(tip), "%s disbanded group", pkt.senderId[0] ? pkt.senderId : "?");
            if (currentUIState != UI_STATE_CHAT)
                showStatusToast(tip, 2500);
        }
        appendChatMessage(pkt.senderId[0] ? pkt.senderId : "Peer", pkt.targetId,
                          pkt.text, false, CHAT_MODE_GROUP, TFT_ORANGE);
        return;
    }
    if (pkt.type == MSG_TYPE_CHAT_JOIN) {
        char tip[40];
        const char *who = pkt.senderId[0] ? pkt.senderId : (pkt.text[0] ? pkt.text : "?");
        snprintf(tip, sizeof(tip), "%s joined chat", who);
        if (currentUIState == UI_STATE_CHAT)
            showChatJoinToast(tip); // 聊天室内用 cn 字体横幅；英文无方框
        else
            showStatusToast(tip, 2500);
        return;
    }
    if (pkt.type == MSG_TYPE_CHAT) {
        if (pkt.mode == CHAT_MODE_PRIVATE) {
            if (strcmp(pkt.targetId, localDeviceId) != 0 && strcmp(pkt.senderId, localDeviceId) != 0) {
                return;
            }
        }
        if (pkt.mode == CHAT_MODE_GROUP && pkt.targetId[0] && !chatHasGroup(pkt.targetId)) {
            return; // 未加入该群，不可见
        }
        appendChatMessage(pkt.senderId[0] ? pkt.senderId : "Peer", pkt.targetId,
                          pkt.text[0] ? pkt.text : "", false, pkt.mode,
                          pkt.textColor ? pkt.textColor : TFT_WHITE);
        if (currentUIState != UI_STATE_CHAT) {
            char tip[48];
            snprintf(tip, sizeof(tip), "%s: %.16s", pkt.senderId[0] ? pkt.senderId : "?", pkt.text);
            showStatusToast(tip, 2200);
        }
    }
}


// 重播所有绘图历史 (在屏幕上重新绘制所有点和线)
void replayAllDrawings()
{
    // 与 paintCurrentCanvasPage 同一路径：橡皮只清本笔主墨迹
    paintCurrentCanvasPage();
}
