#ifndef ESP_NOW_HANDLER_H
#define ESP_NOW_HANDLER_H

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h> // 用于 esp_wifi_get_mac()
#include <queue>
#include <set>
#include <map>        // 新增：用于 std::map
#include <string>     // For std::string if used by macSet, though it's std::set<String>
#include "config.h"   // 项目配置文件
#include <TFT_eSPI.h> // 需要 TFT_eSPI::color565 等，以及 tft 对象
#include "touch_handler.h" // For TS_Point type
#include "drawing_history.h" // 包含自定义绘图历史头文件和 TouchData_t 的定义

// ESP-NOW 相关数据结构定义
// TouchData_t 的定义已移至 drawing_history.h

enum MessageType_e // 使用 _e 后缀表示 enum
{
    MSG_TYPE_UPTIME_INFO,
    MSG_TYPE_DRAW_POINT,
    MSG_TYPE_REQUEST_ALL_DRAWINGS,
    MSG_TYPE_ALL_DRAWINGS_COMPLETE,
    MSG_TYPE_CLEAR_AND_REQUEST_UPDATE,
    MSG_TYPE_RESET_CANVAS,
    MSG_TYPE_SYNC_START, // 新增：同步开始信号
    MSG_TYPE_HEARTBEAT,  // 新增：心跳包
    MSG_TYPE_CHAT,       // 聊天文本（大厅/私聊/群）
    MSG_TYPE_CHAT_JOIN,  // 加入通知
    MSG_TYPE_CHAT_GROUP, // 群组控制：创建/解散（text 携带协议）
    MSG_TYPE_CANVAS_PAGE, // 画布翻页/新建/删空页（touch_data 携带页信息）
    MSG_TYPE_PRIV_INVITE, // 私聊画板邀请
    MSG_TYPE_PRIV_ACCEPT, // 同意私聊画板
    MSG_TYPE_PRIV_REJECT, // 拒绝私聊画板
    MSG_TYPE_PRIV_LEAVE   // 退出私聊画板
};
typedef enum MessageType_e MessageType_t; // Typedef for the enum

// 私聊画板控制包（单播+加密会话协商）
typedef struct PrivCanvasPacket_s {
    uint8_t magic; // PRIV_CANVAS_MAGIC
    MessageType_t type;
    char senderId[DEVICE_ID_MAX_LEN + 1];
    char targetId[DEVICE_ID_MAX_LEN + 1];
    uint8_t senderMac[6];
    uint8_t nonce[8];
    uint32_t timestamp;
    uint8_t _sizeTag[11]; // 保证与 SyncMessage/ChatPacket 线长不同
} PrivCanvasPacket_t;

// 独立聊天包 (与 SyncMessage 分开发送，避免拖大绘图包)
typedef struct ChatPacket_s {
    MessageType_t type;
    uint8_t mode;          // CHAT_MODE_PUBLIC / PRIVATE / GROUP
    uint16_t textColor;    // RGB565 文字颜色
    char senderId[DEVICE_ID_MAX_LEN + 1];
    char targetId[DEVICE_ID_MAX_LEN + 1]; // 私聊=对方ID；群聊=群ID；大厅可空
    char text[CHAT_TEXT_MAX + 1];
    uint32_t timestamp;
} ChatPacket_t;

typedef struct SyncMessage_s
{
    MessageType_t type;
    unsigned long senderUptime;
    long senderOffset;
    TouchData_t touch_data;
    uint16_t totalPointsForSync; // 新增：用于同步开始时告知总点数
    uint32_t usedMemory;         // 新增：发送方已用内存 (字节)
    uint32_t totalMemory;        // 新增：发送方总内存 (字节)
    char senderId[DEVICE_ID_MAX_LEN + 1]; // 设备短标识，如 "WZL"
} SyncMessage_t;

// 新增：存储对端详细信息的结构体
typedef struct PeerInfo_s {
    String macAddress;
    unsigned long effectiveUptime;
    uint32_t usedMemory;
    uint32_t totalMemory;
    char deviceId[DEVICE_ID_MAX_LEN + 1]; // 对端短标识
    int8_t rssi;                          // 最近一次收到该对端包的 RSSI (dBm)
    uint16_t historyPoints;               // 对端最近通报的笔迹点数（心跳/同步）
    uint8_t batteryPercent;               // 0-100，来自心跳
    unsigned long firstSeenMs;            // 本会话首次发现时间
    unsigned long lastSeenMs;             // 最近收包时间
    uint16_t latencyMs;                   // 粗测延迟（心跳间隔抖动近似）
    uint8_t linkCaps;                     // PEER_CAP_ESPNOW / PEER_CAP_WIFI
    uint8_t peerLinkMode;                 // 对端宣称的链路模式，0xFF=未知
} PeerInfo_t;


// ESP-NOW 相关全局变量 (声明为 extern)
extern esp_now_peer_info_t broadcastPeerInfo;
extern uint8_t broadcastAddress[];
extern std::queue<SyncMessage_t> incomingMessageQueue;
extern DrawingHistory allDrawingHistory;
extern std::set<String> macSet; // 用于设备计数，由 ESP-NOW 填充
extern std::map<String, unsigned long> peerLastHeartbeat; // 新增：存储每个对端的最后心跳时间
extern std::map<String, PeerInfo_t> peerInfoMap; // 新增：存储所有已知对端详细信息的 map

extern unsigned long lastKnownPeerUptime;
extern long lastKnownPeerOffset;
extern uint8_t lastPeerMac[6];
extern bool initialSyncLogicProcessed;
extern bool iamEffectivelyMoreUptimeDevice;
extern bool iamRequestingAllData;
extern bool isAwaitingSyncStartResponse;
extern bool isReceivingDrawingData;
extern bool isSendingDrawingData;
extern size_t currentHistorySendIndex;   // 新增：用于分批发送历史记录的当前索引
extern long relativeBootTimeOffset;
extern unsigned long uptimeOfLastPeerSyncedFrom;

// 触摸点处理相关 (用于远程点绘制)
extern TS_Point lastRemotePoint;      // 远程最后一点 (用于以正确的连续性重播历史记录)
extern unsigned long lastRemoteDrawTime; // 远程最后绘制时间 (用于以正确的时间/连续性重播历史记录)
// touchInterval 定义已移至 config.h 作为 TOUCH_STROKE_INTERVAL


// 函数声明
void espNowInit(); // ESP-NOW 初始化
void OnSyncDataSent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status); // 发送回调 (ESP32 Arduino 3.x)
void OnSyncDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingDataPtr, int len);
void ingestIncomingPacket(const uint8_t srcMac[6], const uint8_t *data, int len, int8_t rssi);
// viaWifi=true：来自 UDP；false：来自 ESP-NOW
void ingestIncomingPacketEx(const uint8_t srcMac[6], const uint8_t *data, int len, int8_t rssi, bool viaWifi);
bool peerVisibleForLocalMode(const PeerInfo_t &p);
void notePeerWifiPresence(const uint8_t srcMac[6], const char *deviceId, uint8_t peerMode);
void purgePeersNotVisibleForWifiOnly();
void sendSyncMessage(const SyncMessage_t *msg); // 发送同步消息的辅助函数
void processIncomingMessages(); // 处理接收到的消息队列
void replayAllDrawings();       // 重播所有绘图历史 (需要 tft 对象)
void sendHeartbeat(); // 新增：发送心跳包
void checkPeerHeartbeatTimeout(); // 新增：检查对端心跳超时
void processPendingSignalRecoveryResync(); // 信号恢复后补一次全量画面同步
void noteLocalDestructiveCanvasEdit(); // 清页/清空/撤销后标记，防止对端旧历史盖回来
void forcePushDrawingHistoryToPeers(); // 强制把本机历史推给对端
std::vector<PeerInfo_t> getPeerInfoList(); // 新增：获取对端信息列表
void sendChatPacket(MessageType_t type, const char *text); // 兼容：发到大厅
void sendChatEx(MessageType_t type, uint8_t mode, const char *targetId, const char *text, uint16_t color);
void processIncomingChatPacket(const ChatPacket_t &pkt); // 处理聊天包

// 私聊画板
bool isPrivateCanvasActive();
bool isPrivateCanvasInvitePending(); // 本机发出或收到待确认
const char *getPrivateCanvasPeerId();
bool parseMacString(const String &macKey, uint8_t outMac[6]);
bool ensureUnicastPeer(const uint8_t mac[6]);
void sendPrivCanvasPacket(const PrivCanvasPacket_t *pkt, const uint8_t *destMacOrNull);
void invitePrivateCanvas(const char *peerId, const String &peerMac);
void acceptPrivateCanvasInvite();
void rejectPrivateCanvasInvite();
void leavePrivateCanvas();
void checkPrivateCanvasTimeouts();
void processIncomingPrivQueue(); // 主循环处理私聊包（勿在 recv 回调里画屏）
void processIncomingPrivPacket(const PrivCanvasPacket_t &pkt, const uint8_t srcMac[6]);

// 注意: replayAllDrawings 函数依赖于在 esp_now_handler.cpp 中可访问的全局 tft 对象和 drawMainInterface 函数。

#endif // ESP_NOW_HANDLER_H
