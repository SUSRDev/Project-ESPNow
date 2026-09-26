#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "config.h"
#include <TFT_eSPI.h>
#include <set>               // For std::set
#include <string>            // For String (used in std::set<String> macSet)
#include "esp_now_handler.h" // For TouchData_t, macSet, allDrawingHistory, relativeBootTimeOffset, replayAllDrawings, PeerInfo_t, peerInfoMap
#include "power_manager.h" // 包含电源管理器头文件，用于 isScreenOn
#include "drawing_history.h" // 包含自定义绘图历史头文件
#include <map> // For std::map
#include <vector> // For std::vector (if needed for peer list display)

// UI 状态枚举
enum UIState_e {
    UI_STATE_MAIN,         // 主绘图界面
    UI_STATE_COLOR_PICKER, // 颜色选择器界面
    UI_STATE_POPUP,        // 弹窗界面 (例如项目信息或 Coffee)
    UI_STATE_PEER_INFO,    // 对端信息界面
    UI_STATE_NAME_EDIT,    // 设备标识编辑界面
    UI_STATE_CHAT          // 局域网群聊界面
};
typedef enum UIState_e UIState_t;

// 当前 UI 状态变量，将在 ui_manager.cpp 中定义
extern UIState_t currentUIState;

// UI state variables that will be defined in ui_manager.cpp
extern uint32_t currentColor;      // 当前画笔颜色
extern bool inCustomColorMode;     // 是否处于自定义颜色模式
extern bool isEraserMode;          // 是否处于橡皮擦模式
extern bool isEraserSliderVisible; // 是否显示橡皮擦滑块
extern int eraserRadius;          // 当前橡皮擦半径
extern int brushRadius;           // 当前画笔半径
extern bool isBrushSliderVisible; // 是否显示笔粗细滑块
extern bool isDebugInfoVisible;    // 调试信息框是否可见
extern bool showDebugToggleButton; // 是否显示调试信息切换按钮
extern bool isPeerInfoScreenVisible; // 对端信息界面是否可见
extern char localDeviceId[DEVICE_ID_MAX_LEN + 1]; // 本机短标识

// 进度条状态变量
extern int sendProgressTotal;      // 发送总数
extern int sendProgressCurrent;    // 当前发送进度
extern int receiveProgressTotal;   // 接收总数
extern int receiveProgressCurrent; // 当前接收进度
extern bool showSendProgress;      // 是否显示发送进度条
extern bool showReceiveProgress;   // 是否显示接收进度条
extern bool isProjectInfoPopupVisible; // 项目信息弹窗是否可见
extern bool isCoffeePopupVisible;    // "Coffee" 弹窗是否可见
extern bool isClearConfirmVisible;   // 清空确认弹窗是否可见

// 清空确认类型
enum ClearConfirmKind_e {
    CLEAR_CONFIRM_NONE = 0,
    CLEAR_CONFIRM_ALL,   // 左上角清空全部
    CLEAR_CONFIRM_PAGE   // 右下角清本页
};
typedef enum ClearConfirmKind_e ClearConfirmKind_t;
extern ClearConfirmKind_t clearConfirmKind;
extern int screenshotCounter; // 截屏文件计数器

extern int redValue;                // 红色通道值 (0-255)
extern int greenValue;              // 绿色通道值 (0-255)
extern int blueValue;               // 蓝色通道值 (0-255)
extern uint16_t *savedScreenBuffer; // 用于保存调色界面覆盖区域的屏幕缓冲

// Variables from other modules needed by UI functions
extern std::set<String> macSet;                    // 来自 esp_now_handler.h (用于设备计数，对端详细信息存储在 peerInfoMap 中)
extern std::map<String, PeerInfo_t> peerInfoMap; // 来自 esp_now_handler.h (存储对端详细信息)
extern DrawingHistory allDrawingHistory; // 来自 esp_now_handler.h (用于调试信息)
extern long relativeBootTimeOffset;                // 来自 esp_now_handler.h (用于调试信息)
// isScreenOn (如果 drawDebugInfo 需要) 会通过包含 power_manager.h 在 ui_manager.cpp 中获得

// --- 函数声明 ---

// 初始化函数
void uiManagerInit(); // UI 相关初始化占位符 (TFT 初始化之外)

// 主界面绘制函数
void drawMainInterface();
void drawResetButton();
void drawColorButtons();
void drawEraserButton();       // 新增：绘制橡皮擦按钮
void drawEraserSlider();      // 新增：绘制橡皮擦滑块
void drawPeerInfoButton();    // 显示对端信息按钮 (可能显示连接设备数)
void drawCustomColorButton(); // 显示当前颜色
void drawStarButton();        // 显示当前颜色, 自定义颜色入口的占位符
void drawScreenshotButton();  // 绘制截屏按钮 (仅 SD 存在时)
void drawCanvasPageButtons(); // 右下角画布翻页 / 新建 / 清页
bool isCanvasPagePrevPressed(int x, int y);
bool isCanvasPageNextPressed(int x, int y);
bool isCanvasPageClearPressed(int x, int y);
void handleCanvasPagePrev();  // 上一页；当前页空则删除
void handleCanvasPageNext();  // 下一页或新建
void handleCanvasPageClear(); // 仅清空当前页笔迹（广播）
void showCanvasPage(uint8_t page, bool broadcastInfo);
void applyRemoteCanvasPage(uint8_t action, uint8_t page, uint8_t pageCount);
uint8_t getCurrentCanvasPage();
uint8_t getCanvasPageCount();
bool canvasPageHasContent(uint8_t page);
void paintCurrentCanvasPage(); // 清屏并重放当前页笔迹 + UI
extern uint8_t currentCanvasPage;
extern uint8_t canvasPageCount;
void drawSignalStrengthInfo(); // 左侧显示对端信号强度
void updateSignalStrengthDisplay(); // 轮换刷新信号显示
void updateOnlinePanelLive(); // 在线列表延迟/信号动态刷新
bool detectSdCardPresent();    // 检测是否插入 SD 卡（轻量，勿在画屏前阻塞调用）
bool isSdCardAvailable();      // 当前是否可用 SD
void deferSdCardDetection();   // 标记：稍后在 loop 里探测 SD
void processDeferredSdDetect(); // loop 中调用的非阻塞调度
extern bool sdCardAvailable;   // SD 卡可用标志
extern bool sdDetectPending;   // 是否还有待完成的 SD 探测
extern bool pendingCanvasRedrawAfterChat; // 非花瓣画板时延后笔迹重绘

// 调试信息函数
void drawDebugInfo();         // 显示历史记录大小、运行时间、偏移量、内存
void toggleDebugInfo();       // 切换调试信息框的显示状态
void drawDebugToggleButton(); // 绘制调试信息切换按钮
void drawInfoButton();        // 绘制项目信息按钮
void showProjectInfoPopup();  // 显示项目信息弹窗
void hideProjectInfoPopup();  // 隐藏项目信息弹窗

// "Coffee" 按钮相关函数
void drawCoffeeButton();      // 绘制 "Coffee" 按钮
void drawChatJoinButton();    // 绘制加入聊天室按钮
void showCoffeePopup();       // 显示 "Coffee" 弹窗
void hideCoffeePopup();       // 隐藏 "Coffee" 弹窗
void showClearConfirm(ClearConfirmKind_t kind); // 清空确认弹窗
void hideClearConfirm(bool redraw);             // 关闭确认弹窗
void drawClearConfirmPopup();                   // 绘制确认弹窗
bool handleClearConfirmTouch(int x, int y);     // true=已处理
void performFullCanvasReset();                  // 执行全部清空（含广播）
void showChatRoom();          // 进入群聊
void hideChatRoom();          // 退出群聊回主界面
void showChatJoinToast(const char *msg); // 聊天室内短暂加入提示
void chatTouchReleased();     // 抬手时复位滑动状态
void nameEditTouchReleased(); // 抬手时复位 ID 编辑按键
void drawChatRoom();          // 绘制群聊界面
bool handleChatTouch(int x, int y); // 群聊触摸
void appendChatMessage(const char *senderId, const char *targetId, const char *text,
                       bool isSelf, uint8_t mode, uint16_t color);
bool chatHasGroup(const char *gid); // 本机是否已加入该群
bool isChatJoinButtonPressed(int x, int y);

// 对端信息界面函数
void drawPeerInfoScreen();
void updatePeerInfoScreen();
void showPeerInfoScreen();
void hidePeerInfoScreen();


// 按钮按下检测函数 (基于坐标)
bool isResetButtonPressed(int x, int y);
bool isColorButtonPressed(int x, int y, uint32_t &selectedColor); // 输出选中的颜色
bool isEraserButtonPressed(int x, int y); // 新增：检测橡皮擦按钮是否被按下
bool isEraserSliderPressed(int x, int y); // 新增：检测橡皮擦滑块是否被按下
bool isBrushButtonPressed(int x, int y);
bool isBrushSliderPressed(int x, int y);
bool isPeerInfoButtonPressed(int x, int y); // 检测对端信息按钮是否被按下
bool isPeerInfoScreenBackButtonPressed(int x, int y); // 检测对端信息界面返回按钮是否被按下 - 新增声明
bool isCustomColorButtonPressed(int x, int y); // 用于进入自定义颜色模式
bool isBackButtonPressed(int x, int y);        // 用于退出自定义颜色模式 (主要用于调色盘)
bool isDebugToggleButtonPressed(int x, int y); // 检测调试信息切换按钮是否被按下
bool isInfoButtonPressed(int x, int y);    // 检测项目信息按钮是否被按下
bool isCoffeeButtonPressed(int x, int y);  // 检测 "Coffee" 按钮是否被按下
bool isScreenshotButtonPressed(int x, int y); // 检测截屏按钮是否被按下

// 自定义颜色选择器 UI 函数
void handleCustomColorTouch(int x, int y); // 处理颜色选择器内的触摸
void handleEraserSliderTouch(int x, int y); // 新增：处理橡皮擦滑块触摸
void handleBrushSliderTouch(int x, int y);
void drawBrushButton();
void drawBrushSlider();
void redrawBrushButton();
void updateSingleColorSlider(int yPos, uint32_t sliderColor, int &channelValue);
void drawColorSelectors();       // 绘制 RGB 滑块和返回按钮
void updateCustomColorPreview(); // 更新颜色预览框
void refreshAllColorSliders();   // 重绘所有滑块 (例如触摸后)
void closeColorSelectors();      // 恢复屏幕，退出自定义颜色模式

// 屏幕缓冲区管理 (用于自定义颜色选择器)
void saveScreenArea();         // 保存颜色选择器将覆盖的屏幕区域
void restoreSavedScreenArea(); // 恢复保存的屏幕区域

// UI 工具函数
void updateCurrentColor(uint32_t newColor); // 设置全局当前颜色
void updateConnectedDevicesCount();         // 更新休眠按钮上的设备计数
void clearScreenAndCache();                 // 清屏、重绘UI、重置相关触摸点 (影响广泛)
void redrawMainScreen();                    // 重绘整个主屏幕
void redrawMainScreenWithoutMessage();       // 重绘整个主屏幕（不显示消息）

// 进度条绘制和更新函数
void drawSendProgressIndicator();
void drawReceiveProgressIndicator();
void updateSendProgress(int current, int total);
void updateReceiveProgress(int current, int total);
void hideSendProgress();
void hideReceiveProgress();

void hideStarButton();
void showStarButton();
void redrawStarButton();

void redrawEraserButton(); // 新增：重绘橡皮擦按钮

// 截屏功能函数
void initScreenshotCounter(); // 初始化截屏计数器
bool saveScreenshotToSD(); // 保存截屏到SD卡
void showScreenshotError(const char* errorMsg); // 显示截屏错误消息

// 设备标识 / 状态提示
void loadLocalDeviceId();
void saveLocalDeviceId(const char *id);
bool isDeviceIdTakenByNearbyPeer(const char *id); // 附近是否已有相同 ID
const char *getLocalDeviceId();
void showStatusToast(const char *msg, unsigned long durationMs = STATUS_TOAST_MS);
void setDrawingStatus(const char *deviceId); // 兼容：等同 setActivityStatus(id,"drawing")
void setActivityStatus(const char *deviceId, const char *action); // 谁在干什么
void clearDrawingStatus();
void updateStatusOverlays(); // loop 中周期性调用
void redrawUiChrome();       // 仅重绘操作按钮，不擦画布
bool eraserOverlapsUi(int cx, int cy, int r);
bool safeEraserFill(int cx, int cy, int r); // 兼容：擦除后若碰 UI 返回 false
bool applyEraserDot(int cx, int cy, int r); // 擦一点，碰 UI 返回 true
bool applyEraserSegment(int x0, int y0, int x1, int y1, int r); // 沿路径擦
void applyBrushDot(int cx, int cy, uint32_t color, int r);
void applyBrushSegment(int x0, int y0, int x1, int y1, uint32_t color, int r);
int resolveEraserRadius(uint8_t brushR);
int resolveBrushRadius(uint8_t brushR);
void showNameEditScreen();
void hideNameEditScreen();
void drawNameEditScreen();
bool handleNameEditTouch(int x, int y); // true=已处理
void peerJoinedNotify(const char *idOrMac);
void peerLeftNotify(const char *idOrMac);
void historyRestoredNotify(const char *fromId);

// 可能需要从其他模块获取的函数
extern float readBatteryVoltagePercentage(); // drawResetButton 使用，已移至 power_manager

#endif // UI_MANAGER_H
