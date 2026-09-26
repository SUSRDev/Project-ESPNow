#include "drawing_history.h" // 包含自定义绘图历史头文件和 TouchData_t 的定义
#include "esp_now_handler.h"
#include "config.h"     // 包含项目配置常量
#include "ui_manager.h" // << 添加对 UI 管理器的引用
#include <Arduino.h>    // For Serial, millis, etc.
#include <cstring>      // For memcpy, memset, snprintf
#include <TFT_eSPI.h> // 需要 TFT_eSPI::color565 等，以及 tft 对象
#include "touch_handler.h" // For TS_Point type
#include <vector> // 用于 getPeerInfoList 返回值
#include <map> // 用于 std::map
#include <set>

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
        showStatusToast("信号恢复，等待同步…", 2000);
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

    showStatusToast("信号恢复，同步画面…", 2800);
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

// ESP-NOW 数据接收回调函数
void OnSyncDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingDataPtr, int len)
{
    if (len == sizeof(SyncMessage_t))
    {
        SyncMessage_t receivedMsg;
        memcpy(&receivedMsg, incomingDataPtr, sizeof(receivedMsg));
        receivedMsg.senderId[DEVICE_ID_MAX_LEN] = '\0';
        memcpy(lastPeerMac, info->src_addr, 6); // 更新最后通信的对端 MAC

        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 info->src_addr[0], info->src_addr[1], info->src_addr[2],
                 info->src_addr[3], info->src_addr[4], info->src_addr[5]);
        String macKey = String(macStr);
        bool isNewPeer = (peerInfoMap.find(macKey) == peerInfoMap.end());

        macSet.insert(macKey); // 添加到 MAC 地址集合中用于计数
        peerLastHeartbeat[macKey] = millis(); // 更新对端的最后心跳时间

        // 更新或添加对端详细信息
        peerInfoMap[macKey].macAddress = macKey;
        peerInfoMap[macKey].effectiveUptime = receivedMsg.senderUptime + receivedMsg.senderOffset;
        peerInfoMap[macKey].usedMemory = receivedMsg.usedMemory;
        peerInfoMap[macKey].totalMemory = receivedMsg.totalMemory;
        if (receivedMsg.type == MSG_TYPE_HEARTBEAT ||
            receivedMsg.type == MSG_TYPE_UPTIME_INFO ||
            receivedMsg.type == MSG_TYPE_SYNC_START ||
            receivedMsg.type == MSG_TYPE_ALL_DRAWINGS_COMPLETE) {
            peerInfoMap[macKey].historyPoints = receivedMsg.totalPointsForSync;
        }
        if (info->rx_ctrl) {
            peerInfoMap[macKey].rssi = info->rx_ctrl->rssi;
            updatePeerSignalQuality(macKey, info->rx_ctrl->rssi,
                                    receivedMsg.senderUptime, receivedMsg.senderOffset);
        }
        if (receivedMsg.senderId[0]) {
            strncpy(peerInfoMap[macKey].deviceId, receivedMsg.senderId, DEVICE_ID_MAX_LEN);
            peerInfoMap[macKey].deviceId[DEVICE_ID_MAX_LEN] = '\0';
            // 对端 ID 与本机相同 → 冲突提示
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
        } else if (peerInfoMap[macKey].deviceId[0] == '\0') {
            snprintf(peerInfoMap[macKey].deviceId, sizeof(peerInfoMap[macKey].deviceId),
                     "%02X%02X", info->src_addr[4], info->src_addr[5]);
        }

        if (isNewPeer) {
            // 对端重新出现：清同步标记，马上发一次 UPTIME 触发恢复
            clearPeerCanvasSyncState(macKey);
            peerJoinedNotify(peerInfoMap[macKey].deviceId);
            SyncMessage_t urgent;
            memset(&urgent, 0, sizeof(urgent));
            urgent.type = MSG_TYPE_UPTIME_INFO;
            urgent.senderUptime = millis();
            urgent.senderOffset = relativeBootTimeOffset;
            applySenderId(&urgent);
            sendSyncMessage(&urgent);
        }

        incomingMessageQueue.push(receivedMsg); // 将消息放入队列等待处理
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
                 info->src_addr[0], info->src_addr[1], info->src_addr[2],
                 info->src_addr[3], info->src_addr[4], info->src_addr[5]);
        String macKey = String(macStr);
        macSet.insert(macKey);
        peerLastHeartbeat[macKey] = millis();
        if (info->rx_ctrl) {
            peerInfoMap[macKey].rssi = info->rx_ctrl->rssi;
            updatePeerSignalQuality(macKey, info->rx_ctrl->rssi, 0, 0);
        }
        peerInfoMap[macKey].macAddress = macKey;
        if (chatPkt.senderId[0]) {
            strncpy(peerInfoMap[macKey].deviceId, chatPkt.senderId, DEVICE_ID_MAX_LEN);
            peerInfoMap[macKey].deviceId[DEVICE_ID_MAX_LEN] = '\0';
        }

        processIncomingChatPacket(chatPkt);
    }
    else if (len == strlen("XX:XX:XX:XX:XX:XX") && incomingDataPtr[0] != '{')
    {
        // 处理旧版或特定的 MAC 地址广播 (如果项目中有这种逻辑)
        char macStr[18];
        memcpy(macStr, incomingDataPtr, len);
        macStr[len] = '\0';
        String macKey = String(macStr);
        bool isNewPeer = (peerInfoMap.find(macKey) == peerInfoMap.end());
        macSet.insert(macKey);
        peerLastHeartbeat[macKey] = millis(); // 更新对端的最后心跳时间
        if (isNewPeer) {
            peerInfoMap[macKey].macAddress = macKey;
            peerInfoMap[macKey].deviceId[0] = '\0';
            peerJoinedNotify(macStr);
        }
        // 对于旧版消息，我们没有内存信息，只更新心跳
    }
    else
    {
        Serial.print("收到意外长度的数据: ");
        Serial.print(len);
        Serial.print(", 期望长度: ");
        Serial.println(sizeof(SyncMessage_t));
    }
}

// 发送同步消息的辅助函数
void sendSyncMessage(const SyncMessage_t *msg)
{
    SyncMessage_t out = *msg;
    applySenderId(&out);
    // 调用前应确保 msg->senderUptime 和 msg->senderOffset 已正确设置
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&out, sizeof(SyncMessage_t));
    if (result != ESP_OK)
    {
        Serial.print("发送 SyncMessage 类型 ");
        Serial.print(out.type);
        Serial.print(" 错误: ");
        Serial.println(esp_err_to_name(result));
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
            int mapX = currentPointData.x;
            int mapY = currentPointData.y;
            const char *drawerId = msg.senderId[0] ? msg.senderId : "?";
            bumpCanvasPageCountFromPoint(currentPointData.page);
            const bool onViewPage = (currentPointData.page == currentCanvasPage);
            const bool strokeContinue = (lastRemotePoint.z != 0 && lastRemotePage == currentPointData.page);

            if (isReceivingDrawingData)
            {
                // 场景1: 历史同步 — 聊天界面打开时只收数据不画布，避免叠在聊天室上
                allDrawingHistory.push_back(currentPointData);
                receivedHistoryPointCount++;
                if ((receivedHistoryPointCount & 15) == 0 || receivedHistoryPointCount >= totalPointsExpectedFromPeer) {
                    if (!shouldSkipCanvasPaint())
                        updateReceiveProgress(receivedHistoryPointCount, totalPointsExpectedFromPeer);
                }
                if (!shouldSkipCanvasPaint() && onViewPage) {
                    if (currentPointData.color == TFT_BLACK) {
                        int r = resolveEraserRadius(currentPointData.brushR);
                        bool hitUi = false;
                        if (strokeContinue && lastRemoteColor == TFT_BLACK &&
                            (currentPointData.timestamp - lastRemoteDrawTime <= TOUCH_STROKE_INTERVAL)) {
                            hitUi = applyEraserSegment(lastRemotePoint.x, lastRemotePoint.y,
                                                       mapX, mapY, r);
                        } else {
                            hitUi = applyEraserDot(mapX, mapY, r);
                        }
                        if (hitUi)
                            redrawUiChrome();
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
                    // 其它页笔迹只入库，同步结束后按当前页重放
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
                if ((receivedHistoryPointCount & 63) == 0)
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
                        bool hitUi = false;
                        if (strokeContinue && lastRemoteColor == TFT_BLACK &&
                            (currentPointData.timestamp - lastRemoteDrawTime <= TOUCH_STROKE_INTERVAL)) {
                            hitUi = applyEraserSegment(lastRemotePoint.x, lastRemotePoint.y,
                                                       mapX, mapY, r);
                        } else {
                            hitUi = applyEraserDot(mapX, mapY, r);
                        }
                        if (hitUi)
                            redrawUiChrome();
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
            applyRemoteCanvasPage(action, page, count);
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

            if (!allDrawingHistory.empty())
                updateSendProgress(0, allDrawingHistory.size());
            else
                hideSendProgress();
            isSendingDrawingData = true;
            currentHistorySendIndex = 0;
            showStatusToast("正在广播笔迹…");
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
                    historyRestoredNotify(msg.senderId[0] ? msg.senderId : nullptr);
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
            else
                clearScreenAndCache();
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
                if (shouldSkipCanvasPaint()) {
                    pendingCanvasRedrawAfterChat = true;
                } else {
                    clearScreenAndCache();
                }
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
                if (!shouldSkipCanvasPaint()) {
                    char buf[40];
                    snprintf(buf, sizeof(buf), "同步自 %s…", msg.senderId[0] ? msg.senderId : "对端");
                    showStatusToast(buf, 4000);
                    if (totalPointsExpectedFromPeer > 0)
                        updateReceiveProgress(receivedHistoryPointCount, totalPointsExpectedFromPeer);
                    else
                        hideReceiveProgress();
                }

                lastKnownPeerUptime = peerRawUptime;
                lastKnownPeerOffset = peerReceivedOffset;
                Serial.print("  屏幕已清空。准备从对端 (raw uptime: ");
                Serial.print(peerRawUptime);
                Serial.print(", total points: ");
                Serial.print(totalPointsExpectedFromPeer);
                Serial.println(") 同步数据。");
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
            delay(5); // 在每个点发送后加入一个小的 delay(1)

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
            updateSendProgress(currentHistorySendIndex, allDrawingHistory.size()); // 最后更新一次确保是100%
            // hideSendProgress(); // 或者在 updateSendProgress 内部处理完成后的隐藏
            isSendingDrawingData = false;
            // currentHistorySendIndex = 0;
        }
        else if (pointsSentThisCycle > 0)
        {
            // 当前批次已发送，但还有更多数据
            updateSendProgress(currentHistorySendIndex, allDrawingHistory.size()); // 更新发送进度
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

    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&pkt, sizeof(pkt));
    if (result != ESP_OK) {
        Serial.print("发送聊天包失败: ");
        Serial.println(esp_err_to_name(result));
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
    // 笔迹只在花瓣画板重放；其它界面只记 pending，返回后再画
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode) {
        pendingCanvasRedrawAfterChat = true;
        return;
    }

    lastRemotePoint.x = 0;
    lastRemotePoint.y = 0;
    lastRemotePoint.z = 0;
    lastRemoteDrawTime = 0;
    lastRemoteColor = 0;
    lastRemotePage = currentCanvasPage;

    const size_t total = allDrawingHistory.size();
    // 历史过大时降采样绘制，避免同步恢复卡死白屏
    const size_t stride = (total > 4000) ? 2 : 1;

    for (size_t i = 0; i < total; i += stride)
    {
        const auto &drawData = allDrawingHistory[i];
        if (drawData.isReset)
        {
            // 只清画布，不在循环里反复重绘整套 UI（原先此处最卡）
            tft.fillScreen(TFT_BLACK);
            lastRemotePoint.x = 0;
            lastRemotePoint.y = 0;
            lastRemotePoint.z = 0;
            lastRemoteDrawTime = drawData.timestamp;
            lastRemoteColor = 0;
            continue;
        }
        // 只重放当前页，其它页互不影响
        if (drawData.page != currentCanvasPage)
            continue;
        int mapX = drawData.x;
        int mapY = drawData.y;
        if (drawData.color == TFT_BLACK) {
            int r = resolveEraserRadius(drawData.brushR);
            // 降采样时强制用段擦，避免漏点；同色连续才连段
            if (lastRemotePoint.z != 0 && lastRemoteColor == TFT_BLACK &&
                (drawData.timestamp - lastRemoteDrawTime <= TOUCH_STROKE_INTERVAL * 2 || stride > 1)) {
                applyEraserSegment(lastRemotePoint.x, lastRemotePoint.y, mapX, mapY, r);
            } else {
                applyEraserDot(mapX, mapY, r);
            }
        } else {
            int r = resolveBrushRadius(drawData.brushR);
            if (drawData.timestamp - lastRemoteDrawTime > TOUCH_STROKE_INTERVAL || lastRemotePoint.z == 0 ||
                lastRemoteColor == TFT_BLACK)
                applyBrushDot(mapX, mapY, drawData.color, r);
            else
                applyBrushSegment(lastRemotePoint.x, lastRemotePoint.y, mapX, mapY, drawData.color, r);
        }
        lastRemotePoint.x = mapX;
        lastRemotePoint.y = mapY;
        lastRemotePoint.z = 1;
        lastRemoteDrawTime = drawData.timestamp;
        lastRemoteColor = drawData.color;

        // 每 64 点让出 CPU，避免看门狗/触摸无响应
        if ((i & 63) == 0) {
            yield();
        }
    }
    redrawUiChrome();
}
