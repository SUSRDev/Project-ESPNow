// ============================================================================
// 【编译必看】若报 Sketch too big / Maximum is 1310720：
//   工具(Tools) → Partition Scheme → 选 「Huge APP (3MB No OTA)」
//   （或 Custom，使用本目录 partitions.csv）
//   改完后必须重新编译上传！中文字库需要约 2MB+ 应用分区。
// ============================================================================
// 版本更新记录:
// 2025.1.5: 右侧新增RGB色彩自定义功能 (左侧四个固定颜色按钮暂保留)。感谢群友 xiao_hj909。
// 2025.3.23: 新增息屏功能。短按BOOT键息屏/亮屏，长按2秒进入深度睡眠。
//            息屏状态下：红色LED为电源指示，蓝色LED为连接指示，绿色LED为远程更新指示。
//            LED亮度可通过PWM调节。感谢群友 2093416185 (shapaper@126.com)。
// 2025.5.9: 新增调试信息区域，显示设备数量、Uptime、MAC地址、内存使用情况等。
//            调试信息区域在屏幕底部，包含4行信息。
//            1. 绘图历史数量
//            2. Uptime (毫秒)
//            3. Compared Uptime (相对启动时间)
//            4. 内存使用情况 (已用/总内存)
//            重构了所有代码，解耦分离了UI、触摸、ESP-NOW等模块。
//            代码结构更清晰，便于后续维护和扩展。
//            添加历史记录同步功能，支持多设备间的绘图历史同步。
//            感谢群友 2093416185 (shapaper@126.com)。
// 2025.5.10: 修复了清屏bug,同步bug,并且优化了debug按钮，添加了嵌入式的coffee按钮（已获得Kurio Reiko授权）。
// 2025.5.17: 支持触摸点超过2048个的情况
// 2025.5.17: 新增对端信息界面和心跳包逻辑，10s无心跳认为对端下线。
// 2026.9.26: 设备短标识(ID)、进出线/掉线/恢复字迹提示、画画状态条；修复橡皮擦擦掉操作按钮。
// 2026.9.26b: 无SD隐藏S；左侧RSSI轮显；ESP-NOW局域网群聊打字(聊按钮)。
// 2026.9.26c: 修复启动白屏卡顿——SD延迟探测；恢复字迹降负载。
// 2026.9.26d: 中文显示/拼音输入；私聊/群组管理；聊天文字颜色。
// 2026.9.26e: 禁加入刷屏；聊天中不叠 restore；修误触发同步；音节级大词库+标点。

#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <TFT_eSPI.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h> // 用于 esp_wifi_get_mac()
#include <queue>
#include <set>
#include <vector>
#include <cmath>      // 用于 abs()
#include <map>        // For std::map (needed for peerInfoMap)

#include "src/config.h" // 引入配置文件
#include "src/esp_now_handler.h" // 引入 ESP-NOW 处理模块
#include "src/ui_manager.h"   // 引入 UI 管理模块
#include "src/touch_handler.h" // 引入触摸处理模块
#include "src/power_manager.h" // 引入电源管理模块

// XY_structure (now XY_TouchPoint_t) 已移至 touch_handler.h

// 呼吸灯相关变量 (breathBrightness, breathDirection, lastBreathTime, hasNewUpdateWhileScreenOff)
// 已移至 power_manager.cpp (作为 static 或 extern)

// 创建 SPI 和触摸屏对象 (这些是硬件相关的，通常在主文件初始化)
SPIClass mySpi = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ); // 触摸屏对象
TFT_eSPI tft = TFT_eSPI();                     // TFT 显示对象

// 全局变量，部分已移至 esp_now_handler.cpp 并通过 esp_now_handler.h extern 声明
// 此处保留那些尚未模块化或确实需要在主文件直接访问的变量

unsigned long deviceInitialBootMillis = 0; // 本机启动时的 millis() 值 (仅供调试参考)

// 与ESP-NOW同步逻辑相关的常量 (如果只在esp_now_handler中使用，可以移至其.cpp文件)
// const unsigned long MIN_UPTIME_DIFF_FOR_NEW_SYNC_TARGET = 200UL; // 已移至 esp_now_handler.h/cpp (作为定义)
// const unsigned long EFFECTIVE_UPTIME_SYNC_THRESHOLD = 1000UL; // 已移至 esp_now_handler.h/cpp

// 其他全局变量
// TS_Point lastLocalPoint = {0, 0, 0};  // 已移至 touch_handler.cpp (作为 static)
// unsigned long lastLocalTouchTime = 0; // 已移至 touch_handler.cpp (作为 static)

// macSet, allDrawingHistory, incomingMessageQueue 等已移至 esp_now_handler
// currentColor, inCustomColorMode, redValue, greenValue, blueValue, savedScreenBuffer 已移至 ui_manager

// 彩蛋相关变量 (lastResetTime, resetPressCount) 已移至 touch_handler.cpp

// 定时器相关变量
unsigned long lastBroadcastTime = 0;         // 上次广播 MAC 地址发现消息的时间戳 (用于UI更新设备数)
unsigned long lastUptimeInfoBroadcastTime = 0; // 上次广播 Uptime 信息的时间戳 (ESP-NOW模块也用)
unsigned long lastDebugInfoUpdateTime = 0;   // 上次更新调试信息区域的时间戳 (UI模块用)
unsigned long lastHeartbeatSendTime = 0;     // 新增：上次发送心跳包的时间戳
unsigned long lastPeerInfoUpdateTime = 0;    // 新增：上次更新对端信息界面的时间戳


// 屏幕状态变量 (isScreenOn) 已移至 power_manager.cpp (作为 extern)

// UI绘制函数已移至 ui_manager.cpp

extern void redrawMainScreenWithoutMessage();
extern void initScreenshotCounter(); // 初始化截屏计数器

void setup()
{
    Serial.begin(115200);

    // 1. 初始化自定义模块
    powerManagerInit();  // 初始化电源管理 (引脚设置, LED, 按钮)
    uiManagerInit();     // 初始化 UI 管理器 (如果需要特定设置)
    touchHandlerInit();  // 初始化触摸处理器 (如果需要特定设置)

    // 2. 初始化硬件接口 (SPI, 触摸屏, TFT) —— 立刻清屏，避免白屏
    initTouchSPI();
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    // 3. 初始化 WiFi 和 ESP-NOW
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    loadLocalDeviceId();
    espNowInit();

    // 4. 记录启动时间
    deviceInitialBootMillis = millis();
    Serial.print("设备初始启动毫秒数: ");
    Serial.println(deviceInitialBootMillis);

    // 5. 先画主界面（SD 探测推迟到 loop，避免 SD.begin 阻塞导致白屏卡顿）
    sdCardAvailable = false;
    screenshotCounter = 1;
    deferSdCardDetection();
    drawMainInterface();

    // 6. 初始 UPTIME_INFO 广播
    SyncMessage_t initialUptimeMsg;
    initialUptimeMsg.type = MSG_TYPE_UPTIME_INFO;
    initialUptimeMsg.senderUptime = millis();
    initialUptimeMsg.senderOffset = relativeBootTimeOffset;
    memset(&initialUptimeMsg.touch_data, 0, sizeof(TouchData_t));
    sendSyncMessage(&initialUptimeMsg);
    lastUptimeInfoBroadcastTime = millis();
}

// updateBreathLED, readBatteryVoltagePercentage 已移至 power_manager.cpp
// UI 绘制及按钮检测函数已移至 ui_manager.cpp
// averageXY 和 handleLocalTouch 函数已移至 touch_handler.cpp

void loop()
{
    // 处理输入和通信
    handleLocalTouch();         // from touch_handler.cpp
    processIncomingMessages();  // from esp_now_handler.cpp
    handleBootButton();         // from power_manager.cpp
    processDeferredSdDetect();  // 启动后延迟轻量探测 SD，不堵第一帧

    unsigned long currentTimeForLoop = millis();

    // 定期任务
    // 1. 广播 UPTIME_INFO（聊天室内跳过，减负）
    if (currentUIState != UI_STATE_CHAT &&
        currentTimeForLoop - lastUptimeInfoBroadcastTime >= UPTIME_INFO_BROADCAST_INTERVAL) {
        SyncMessage_t uptimeMsgLoop; // 已重命名以避免与 setup 中的 uptimeMsg 冲突
        uptimeMsgLoop.type = MSG_TYPE_UPTIME_INFO;
        uptimeMsgLoop.senderUptime = currentTimeForLoop;
        uptimeMsgLoop.senderOffset = relativeBootTimeOffset; // 来自 esp_now_handler 的 extern 变量
        memset(&uptimeMsgLoop.touch_data, 0, sizeof(TouchData_t));
        // 新增：填充内存信息
        uptimeMsgLoop.usedMemory = esp_get_free_heap_size();
        uptimeMsgLoop.totalMemory = ESP.getHeapSize();
        sendSyncMessage(&uptimeMsgLoop); // 来自 esp_now_handler.cpp
        lastUptimeInfoBroadcastTime = currentTimeForLoop;
    }

    // 2. 发送心跳包
    // 心跳包现在也包含内存信息，并且用于心跳超时检测。
    // UPTIME_INFO 消息现在也包含内存信息，并且用于同步决策。
    // 两个消息都包含内存信息，确保对端能收到最新的内存数据。
    // 我们可以保留心跳包，因为它有专门的心跳超时检测逻辑。
    if (currentTimeForLoop - lastHeartbeatSendTime >= HEARTBEAT_SEND_INTERVAL_MS) {
        sendHeartbeat(); // 来自 esp_now_handler.cpp
        lastHeartbeatSendTime = currentTimeForLoop;
    }

    // 3. 检查对端心跳超时
    checkPeerHeartbeatTimeout(); // 来自 esp_now_handler.cpp

    // 4. 更新调试信息 (如果屏幕亮且不在调色模式)
    // isScreenOn 和 inCustomColorMode 分别是来自 power_manager 和 ui_manager 的 extern 变量
    if (isScreenOn && !inCustomColorMode && currentUIState == UI_STATE_MAIN &&
        (currentTimeForLoop - lastDebugInfoUpdateTime >= DEBUG_INFO_UPDATE_INTERVAL)) {
        drawDebugInfo(); // 来自 ui_manager.cpp
        updateStatusOverlays(); // 状态条 / toast / drawing
        updateSignalStrengthDisplay(); // 左侧对端信号（多台每5秒轮换）
        lastDebugInfoUpdateTime = currentTimeForLoop;
    }

    // 5. 更新连接设备计数 (如果不在调色模式且在主界面)
    // inCustomColorMode 和 currentUIState 是来自 ui_manager 的 extern 变量
    if (!inCustomColorMode && currentUIState == UI_STATE_MAIN && (currentTimeForLoop - lastBroadcastTime >= BROADCAST_INTERVAL)) { // BROADCAST_INTERVAL 也用于设备数量的UI更新
        updateConnectedDevicesCount(); // 来自 ui_manager.cpp
        lastBroadcastTime = currentTimeForLoop;
    }

    // 6. 管理屏幕关闭时的 LED 状态 (包括呼吸灯)
    // isScreenOn 和 hasNewUpdateWhileScreenOff 是来自 power_manager 的 extern 变量
    if (!isScreenOn && hasNewUpdateWhileScreenOff) {
        updateBreathLED(); // 来自 power_manager.cpp
    }
    manageScreenStateLEDs(); // 来自 power_manager.cpp (根据 isScreenOn 处理其他 LED)

    // 7. 周期性更新对端信息界面 (如果当前处于该界面)
    // currentUIState 和 isPeerInfoScreenVisible 是来自 ui_manager 的 extern 变量
    if (currentUIState == UI_STATE_PEER_INFO && isPeerInfoScreenVisible && (currentTimeForLoop - lastPeerInfoUpdateTime >= PEER_INFO_UPDATE_INTERVAL)) {
        updatePeerInfoScreen(); // 来自 ui_manager.cpp
        lastPeerInfoUpdateTime = currentTimeForLoop;
    }

    // 8. 在线列表：延迟 / 信号条动态刷新
    static unsigned long lastOnlinePanelRefresh = 0;
    if (currentUIState == UI_STATE_CHAT &&
        (currentTimeForLoop - lastOnlinePanelRefresh >= ONLINE_PANEL_REFRESH_MS)) {
        updateOnlinePanelLive();
        lastOnlinePanelRefresh = currentTimeForLoop;
    }

    // 短暂延时，避免过于频繁的循环，给其他任务（如WiFi栈）一些时间
    // delay(1); // 可选，根据实际情况调整
}
