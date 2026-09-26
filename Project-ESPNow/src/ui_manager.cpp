#include "ui_manager.h"
#include "config.h"
#include <Arduino.h> // For Serial, millis, ESP, sprintf, etc.
#include <cstring>   // For memset, strncpy if used
#include <TFT_eSPI.h> // 包含 TFT_eSPI 库
#include <cmath>      // 包含 cmath 库，用于 round 函数
#include <algorithm>  // 包含 algorithm 库，用于 min/max 函数
#include "drawing_history.h" // 包含自定义绘图历史头文件
#include "esp_now_handler.h" // 包含 esp_now_handler.h 以访问 PeerInfo_t 和 peerInfoMap
#include "transport_manager.h"
#include <esp_wifi.h> // 用于获取本机 MAC 地址
#include "SD.h"       // 包含 SD 卡库
#include "FS.h"        // 包含文件系统库
#include <Preferences.h>
#include "cn_text.h"
#include <vector>
#include <XPT2046_Touchscreen.h>

static void loadChatHistory();
static void saveChatHistory();
static void clearChatHistoryPersistent();
static void broadcastCanvasPage(uint8_t action, uint8_t page, uint8_t count);

// 来自 Project-ESPNow.ino 的外部变量
extern SPIClass mySpi; // SPI对象
extern XPT2046_Touchscreen ts;
extern TFT_eSPI tft;

// --- 全局 UI 状态变量 (在此定义) ---
UIState_t currentUIState = UI_STATE_MAIN; // 当前 UI 状态
uint32_t currentColor = TFT_BLUE; // Default to blue, consistent with .ino // 默认为蓝色, 与 .ino 文件一致
bool inCustomColorMode = false;
bool isEraserMode = false; // 橡皮擦模式，默认关闭
bool isEraserSliderVisible = false; // 橡皮擦滑块是否可见
int eraserRadius = 8; // 当前橡皮擦半径，默认为8
int brushRadius = 2; // 画笔半径，默认 2
bool isBrushSliderVisible = false; // 笔粗细滑块是否可见
bool isDebugInfoVisible = false;   // 调试信息框默认关闭
bool showDebugToggleButton = true; // 调试信息切换按钮默认显示
bool isProjectInfoPopupVisible = false; // 项目信息弹窗默认关闭
bool isCoffeePopupVisible = false;    // "Coffee" 弹窗默认关闭
bool isClearConfirmVisible = false;   // 清空确认弹窗
ClearConfirmKind_t clearConfirmKind = CLEAR_CONFIRM_NONE;
bool isPeerInfoScreenVisible = false; // 对端信息界面默认关闭
int screenshotCounter = 1; // 截屏文件计数器
char localDeviceId[DEVICE_ID_MAX_LEN + 1] = "ESP";
bool sdCardAvailable = false;
bool sdDetectPending = false;
bool pendingCanvasRedrawAfterChat = false;
uint8_t currentCanvasPage = 0;
uint8_t canvasPageCount = 1;
static unsigned long sdDetectStartMs = 0;
static bool screenshotCounterInited = false;

// 状态条 / Toast
static char statusToastMsg[48] = {0};
static unsigned long statusToastUntil = 0;
static char drawingStatusMsg[48] = {0};
static unsigned long drawingStatusUntil = 0;
static char nameEditBuffer[DEVICE_ID_MAX_LEN + 1] = {0};
static bool nameEditFingerDown = false;
static size_t signalPeerRotateIndex = 0;
static unsigned long lastSignalPeerRotateTime = 0;
static String signalFocusMac;

// 画板在线列表 / 私聊邀请
static int onlineListScrollY = 0;
static int onlineListPressX = 0;
static int onlineListPressY = 0;
static int onlineListDragLastY = -1;
static bool onlineListDragging = false;
static bool onlineListFingerDown = false;
static bool privInviteVisible = false;
static char privInviteFromId[DEVICE_ID_MAX_LEN + 1] = {0};
static unsigned long privInviteDeadlineMs = 0;
static unsigned long privInviteLastDrawnSec = 0;
static bool privInviteFingerDown = false;

// 进度条状态变量定义
int sendProgressTotal = 0;
int sendProgressCurrent = 0;
int receiveProgressTotal = 0;
int receiveProgressCurrent = 0;
bool showSendProgress = false;
bool showReceiveProgress = false;

int redValue = 255;
int greenValue = 255;
int blueValue = 255;
uint16_t *savedScreenBuffer = nullptr;

// --- 来自其他模块/主 .ino 文件的 Extern 变量 ---
extern bool isScreenOn; // 来自 power_manager 模块 (通过 ui_manager.h 间接包含 power_manager.h)
// lastLocalPoint 和 lastLocalTouchTime 是 touch_handler 模块的内部状态, 不应在此 extern 或修改

static uint8_t screenRotation = SCREEN_ROT_DEFAULT;

// macSet, allDrawingHistory, relativeBootTimeOffset, peerInfoMap 已在 ui_manager.h 中 extern 声明
// replayAllDrawings() 已在 esp_now_handler.h 中声明
// lastRemotePoint, lastRemoteDrawTime 已在 esp_now_handler.h 中 extern 声明
// getPeerInfoList() 已在 esp_now_handler.h 中声明

// --- 函数实现 ---

void uiManagerInit()
{
    loadLocalDeviceId();
    loadChatHistory();
}

void drawMainInterface()
{
    tft.fillScreen(TFT_BLACK);
    drawResetButton();
    drawColorButtons();
    drawEraserButton(); // 绘制橡皮擦按钮
    if (isEraserSliderVisible) {
        drawEraserSlider(); // 绘制橡皮擦滑块
    }
    drawPeerInfoButton(); // 此函数内部会调用 updateConnectedDevicesCount
    drawStarButton(); // 绘制颜色框和 "*"
    drawUndoRedoButtons();
    drawBrushButton();
    if (isBrushSliderVisible)
        drawBrushSlider();
    drawScreenshotButton(); // 有 SD 才显示 S
    drawCanvasPageButtons();
    drawSignalStrengthInfo(); // 左侧信号强度
    if (isScreenOn && !inCustomColorMode)
    {
        if (isDebugInfoVisible)
        {
            drawDebugInfo();
            if (!isProjectInfoPopupVisible && !isCoffeePopupVisible && !isClearConfirmVisible) { // 仅在弹窗未显示时绘制按钮
                drawInfoButton();
            }
        }
        // C/D 已移除；仅保留左下角聊天入口
        drawChatJoinButton();
        drawSettingsButton();
        if (showSendProgress)
        {
            drawSendProgressIndicator();
        }
        if (showReceiveProgress)
        {
            drawReceiveProgressIndicator();
        }
        updateStatusOverlays();
    }
}

void redrawMainScreen()
{
    tft.fillScreen(TFT_BLACK);
    if (currentUIState == UI_STATE_MAIN) {
        drawMainInterface(); // 这会根据 isDebugInfoVisible 和 showDebugToggleButton 绘制正确的状态
        if (isProjectInfoPopupVisible) { // 如果项目信息弹窗之前是可见的，重绘它
            showProjectInfoPopup();
        } else if (isCoffeePopupVisible) { // 如果 Coffee 弹窗之前是可见的，重绘它
            showCoffeePopup();
        } else if (isClearConfirmVisible) {
            drawClearConfirmPopup();
        } else {
            replayAllDrawings(); // 否则重绘历史笔迹
        }
        if (privInviteVisible)
            drawPrivInviteDialog();
    } else if (currentUIState == UI_STATE_COLOR_PICKER) {
        drawColorSelectors();
    } else if (currentUIState == UI_STATE_PEER_INFO) {
        drawPeerInfoScreen();
    } else if (currentUIState == UI_STATE_NAME_EDIT) {
        drawNameEditScreen();
    } else if (currentUIState == UI_STATE_CHAT) {
        drawChatRoom();
    } else if (currentUIState == UI_STATE_ONLINE_LIST) {
        drawOnlineListScreen();
    } else if (currentUIState == UI_STATE_SETTINGS) {
        drawSettingsScreen();
    } else if (currentUIState == UI_STATE_POPUP) {
        drawMainInterface();
        replayAllDrawings();
        if (isClearConfirmVisible)
            drawClearConfirmPopup();
        else if (isCoffeePopupVisible)
            showCoffeePopup();
        else if (isProjectInfoPopupVisible)
            showProjectInfoPopup();
        if (isPrivInviteDialogVisible())
            drawPrivInviteDialog();
    }
}

void redrawMainScreenWithoutMessage()
{
    tft.fillScreen(TFT_BLACK);
    if (currentUIState == UI_STATE_MAIN) {
        drawMainInterface(); // 这会根据 isDebugInfoVisible 和 showDebugToggleButton 绘制正确的状态
        if (isProjectInfoPopupVisible) { // 如果项目信息弹窗之前是可见的，重绘它
            showProjectInfoPopup();
        } else if (isCoffeePopupVisible) { // 如果 Coffee 弹窗之前是可见的，重绘它
            showCoffeePopup();
        } else {
            replayAllDrawings(); // 否则重绘历史笔迹
        }
    } else if (currentUIState == UI_STATE_COLOR_PICKER) {
        drawColorSelectors();
    } else if (currentUIState == UI_STATE_PEER_INFO) {
        drawPeerInfoScreen();
    } else if (currentUIState == UI_STATE_NAME_EDIT) {
        drawNameEditScreen();
    } else if (currentUIState == UI_STATE_CHAT) {
        drawChatRoom();
    } else if (currentUIState == UI_STATE_ONLINE_LIST) {
        drawOnlineListScreen();
    } else if (currentUIState == UI_STATE_SETTINGS) {
        drawSettingsScreen();
    }
    if (privInviteVisible)
        drawPrivInviteDialog();
}

void drawResetButton()
{
    tft.fillRect(RESET_BUTTON_X, RESET_BUTTON_Y, RESET_BUTTON_W, RESET_BUTTON_H, TFT_RED);
    float batteryPercentage = readBatteryVoltagePercentage();
    tft.setTextColor(TFT_WHITE, TFT_RED); // 设置文本背景为按钮颜色
    tft.setTextDatum(MC_DATUM);           // 居中对齐
    tft.drawString(String(batteryPercentage, 0) + "%",
                   RESET_BUTTON_X + RESET_BUTTON_W / 2,
                   RESET_BUTTON_Y + RESET_BUTTON_H / 2,
                   1);          // 使用1号字体以显示较小文本
    tft.setTextDatum(TL_DATUM); // 重置对齐方式
}

void drawColorButtons() {
    uint32_t colors[] = {TFT_RED, TFT_YELLOW, TFT_BLUE, TFT_GREEN};
    for (int i = 0; i < 4; i++)
    {
        int buttonY = COLOR_BUTTON_START_Y + (COLOR_BUTTON_HEIGHT + COLOR_BUTTON_SPACING) * i;
        tft.fillRect(RESET_BUTTON_X, buttonY, COLOR_BUTTON_WIDTH, COLOR_BUTTON_HEIGHT, colors[i]);
    }
}

void drawEraserButton()
{
    uint16_t buttonColor = isEraserMode ? ERASER_ACTIVE_COLOR : ERASER_COLOR;
    tft.fillCircle(ERASER_BUTTON_X, ERASER_BUTTON_Y, ERASER_BUTTON_RADIUS, buttonColor);
    tft.drawCircle(ERASER_BUTTON_X, ERASER_BUTTON_Y, ERASER_BUTTON_RADIUS, TFT_WHITE);
}

void drawEraserSlider()
{
    if (!isEraserSliderVisible)
        return;

    int sliderTop = ERASER_SLIDER_Y - ERASER_SLIDER_HEIGHT / 2;
    int sliderBottom = ERASER_SLIDER_Y + ERASER_SLIDER_HEIGHT / 2;
    int trackX = ERASER_SLIDER_X + (ERASER_SLIDER_HANDLE_W - ERASER_SLIDER_WIDTH) / 2;
    int panelX = ERASER_SLIDER_X - 4;
    int panelY = sliderTop - ERASER_PM_BTN_H - 6;
    int panelW = ERASER_SLIDER_HANDLE_W + ERASER_PM_BTN_W + 20;
    int panelH = ERASER_SLIDER_HEIGHT + ERASER_PM_BTN_H * 2 + 28;

    // 底板清干净，避免拖动残影
    tft.fillRect(panelX, panelY, panelW, panelH, TFT_BLACK);

    // 轨道
    tft.fillRoundRect(trackX, sliderTop, ERASER_SLIDER_WIDTH, ERASER_SLIDER_HEIGHT, 3, tft.color565(50, 50, 60));
    tft.drawRoundRect(trackX, sliderTop, ERASER_SLIDER_WIDTH, ERASER_SLIDER_HEIGHT, 3, TFT_WHITE);

    // 手柄（大）
    int handleY = map(eraserRadius, ERASER_MIN_RADIUS, ERASER_MAX_RADIUS, sliderBottom, sliderTop);
    handleY = constrain(handleY, sliderTop + ERASER_SLIDER_HANDLE_H / 2,
                        sliderBottom - ERASER_SLIDER_HANDLE_H / 2);
    tft.fillRoundRect(ERASER_SLIDER_X, handleY - ERASER_SLIDER_HANDLE_H / 2,
                      ERASER_SLIDER_HANDLE_W, ERASER_SLIDER_HANDLE_H, 3, TFT_CYAN);
    tft.drawRoundRect(ERASER_SLIDER_X, handleY - ERASER_SLIDER_HANDLE_H / 2,
                      ERASER_SLIDER_HANDLE_W, ERASER_SLIDER_HANDLE_H, 3, TFT_WHITE);

    // + / - 大按钮
    int plusY = sliderTop - ERASER_PM_BTN_H - 2;
    int minusY = sliderBottom + 2;
    tft.fillRoundRect(ERASER_PM_BTN_X, plusY, ERASER_PM_BTN_W, ERASER_PM_BTN_H, 3, TFT_GREEN);
    tft.fillRoundRect(ERASER_PM_BTN_X, minusY, ERASER_PM_BTN_W, ERASER_PM_BTN_H, 3, TFT_ORANGE);
    tft.setTextColor(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("+", ERASER_PM_BTN_X + ERASER_PM_BTN_W / 2, plusY + ERASER_PM_BTN_H / 2, 2);
    tft.drawString("-", ERASER_PM_BTN_X + ERASER_PM_BTN_W / 2, minusY + ERASER_PM_BTN_H / 2, 2);

    // 预览圆 + 数值
    int prevCx = ERASER_PM_BTN_X + ERASER_PM_BTN_W / 2;
    int prevCy = ERASER_SLIDER_Y;
    int prevR = eraserRadius;
    if (prevR > 12)
        prevR = 12;
    tft.fillCircle(prevCx, prevCy, prevR + 1, TFT_DARKGREY);
    tft.drawCircle(prevCx, prevCy, prevR, TFT_WHITE);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawNumber(eraserRadius, trackX + ERASER_SLIDER_WIDTH / 2, minusY + ERASER_PM_BTN_H + 10, 1);
    tft.setTextDatum(TL_DATUM);
}

bool isEraserSliderPressed(int x, int y)
{
    if (!isEraserSliderVisible)
        return false;

    int sliderTop = ERASER_SLIDER_Y - ERASER_SLIDER_HEIGHT / 2;
    int sliderBottom = ERASER_SLIDER_Y + ERASER_SLIDER_HEIGHT / 2;
    int plusY = sliderTop - ERASER_PM_BTN_H - 2;
    int minusY = sliderBottom + 2;

    // 滑条胖触控区
    if (x >= ERASER_SLIDER_X - ERASER_SLIDER_HIT_PAD &&
        x <= ERASER_SLIDER_X + ERASER_SLIDER_HANDLE_W + ERASER_SLIDER_HIT_PAD &&
        y >= sliderTop - 4 && y <= sliderBottom + 4)
        return true;
    // +
    if (x >= ERASER_PM_BTN_X - 2 && x <= ERASER_PM_BTN_X + ERASER_PM_BTN_W + 2 &&
        y >= plusY - 2 && y <= plusY + ERASER_PM_BTN_H + 2)
        return true;
    // -
    if (x >= ERASER_PM_BTN_X - 2 && x <= ERASER_PM_BTN_X + ERASER_PM_BTN_W + 2 &&
        y >= minusY - 2 && y <= minusY + ERASER_PM_BTN_H + 2)
        return true;
    return false;
}

void handleEraserSliderTouch(int x, int y)
{
    if (!isEraserSliderVisible)
        return;

    int sliderTop = ERASER_SLIDER_Y - ERASER_SLIDER_HEIGHT / 2;
    int sliderBottom = ERASER_SLIDER_Y + ERASER_SLIDER_HEIGHT / 2;
    int plusY = sliderTop - ERASER_PM_BTN_H - 2;
    int minusY = sliderBottom + 2;
    int oldR = eraserRadius;
    static unsigned long lastPmMs = 0;

    // +/- 点按（防抖，避免按住连跳）
    if (x >= ERASER_PM_BTN_X - 2 && x <= ERASER_PM_BTN_X + ERASER_PM_BTN_W + 2) {
        if (y >= plusY - 2 && y <= plusY + ERASER_PM_BTN_H + 2) {
            if (millis() - lastPmMs < 160)
                return;
            lastPmMs = millis();
            eraserRadius = constrain(eraserRadius + 1, ERASER_MIN_RADIUS, ERASER_MAX_RADIUS);
        } else if (y >= minusY - 2 && y <= minusY + ERASER_PM_BTN_H + 2) {
            if (millis() - lastPmMs < 160)
                return;
            lastPmMs = millis();
            eraserRadius = constrain(eraserRadius - 1, ERASER_MIN_RADIUS, ERASER_MAX_RADIUS);
        } else {
            // 预览区：点一下不改，可拖到旁边滑条
        }
    } else {
        // 拖动滑条：Y 映射半径，X 放宽
        int yy = constrain(y, sliderTop, sliderBottom);
        eraserRadius = map(yy, sliderBottom, sliderTop, ERASER_MIN_RADIUS, ERASER_MAX_RADIUS);
        eraserRadius = constrain(eraserRadius, ERASER_MIN_RADIUS, ERASER_MAX_RADIUS);
    }

    if (eraserRadius != oldR)
        drawEraserSlider();
}

void drawPeerInfoButton()
{
    tft.fillRect(PEER_INFO_BUTTON_X, PEER_INFO_BUTTON_Y, PEER_INFO_BUTTON_W, PEER_INFO_BUTTON_H, TFT_BLUE);
    updateConnectedDevicesCount(); // 更新按钮上的设备计数
}

void drawCustomColorButton()
{ // 星星按钮的颜色预览部分
    tft.fillRect(CUSTOM_COLOR_BUTTON_X, CUSTOM_COLOR_BUTTON_Y, CUSTOM_COLOR_BUTTON_W, CUSTOM_COLOR_BUTTON_H, currentColor);
}

void drawStarButton()
{
    drawCustomColorButton(); // 绘制颜色部分
    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("*", CUSTOM_COLOR_BUTTON_X + CUSTOM_COLOR_BUTTON_W / 2, CUSTOM_COLOR_BUTTON_Y + CUSTOM_COLOR_BUTTON_H / 2, 1);
    tft.setTextDatum(TL_DATUM);
}

void drawBrushButton()
{
    tft.fillRect(BRUSH_BUTTON_X, BRUSH_BUTTON_Y, BRUSH_BUTTON_W, BRUSH_BUTTON_H,
                 isBrushSliderVisible ? tft.color565(60, 120, 200) : tft.color565(50, 50, 60));
    tft.drawRect(BRUSH_BUTTON_X, BRUSH_BUTTON_Y, BRUSH_BUTTON_W, BRUSH_BUTTON_H, TFT_WHITE);
    // 预览当前笔粗
    int r = brushRadius;
    if (r > 6)
        r = 6;
    tft.fillCircle(BRUSH_BUTTON_X + BRUSH_BUTTON_W / 2, BRUSH_BUTTON_Y + BRUSH_BUTTON_H / 2, r, currentColor);
}

static std::vector<std::vector<TouchData_t>> canvasRedoStack;

uint16_t deviceIdToOwnerHash(const char *id)
{
    if (!id || !id[0])
        return 0;
    uint16_t h = 5381;
    for (const char *p = id; *p; ++p)
        h = (uint16_t)(((h << 5) + h) ^ (uint8_t)(*p));
    return h ? h : 1;
}

static uint16_t localOwnerHash()
{
    return deviceIdToOwnerHash(localDeviceId);
}

void clearCanvasRedoStack()
{
    canvasRedoStack.clear();
}

void drawUndoRedoButtons()
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return;
    bool canUndo = false;
    const uint16_t mine = localOwnerHash();
    const size_t n = allDrawingHistory.size();
    for (size_t i = 0; i < n; i++) {
        const TouchData_t &d = allDrawingHistory[i];
        if (!d.isReset && d.page == currentCanvasPage && d.ownerHash == mine && mine != 0) {
            canUndo = true;
            break;
        }
    }
    bool canRedo = !canvasRedoStack.empty();

    tft.fillRect(UNDO_BUTTON_X, UNDO_REDO_BTN_Y, UNDO_REDO_BTN_W, UNDO_REDO_BTN_H,
                 canUndo ? tft.color565(70, 70, 100) : tft.color565(40, 40, 48));
    tft.fillRect(REDO_BUTTON_X, UNDO_REDO_BTN_Y, UNDO_REDO_BTN_W, UNDO_REDO_BTN_H,
                 canRedo ? tft.color565(70, 70, 100) : tft.color565(40, 40, 48));
    tft.drawRect(UNDO_BUTTON_X, UNDO_REDO_BTN_Y, UNDO_REDO_BTN_W, UNDO_REDO_BTN_H, TFT_DARKGREY);
    tft.drawRect(REDO_BUTTON_X, UNDO_REDO_BTN_Y, UNDO_REDO_BTN_W, UNDO_REDO_BTN_H, TFT_DARKGREY);

    uint16_t uc = canUndo ? TFT_WHITE : TFT_DARKGREY;
    uint16_t rc = canRedo ? TFT_WHITE : TFT_DARKGREY;
    int uw = cnTextWidth("撤");
    int rw = cnTextWidth("重");
    cnDrawUtf8(tft, UNDO_BUTTON_X + (UNDO_REDO_BTN_W - uw) / 2, UNDO_REDO_BTN_Y + 1, "撤", uc);
    cnDrawUtf8(tft, REDO_BUTTON_X + (UNDO_REDO_BTN_W - rw) / 2, UNDO_REDO_BTN_Y + 1, "重", rc);
}

bool isUndoButtonPressed(int x, int y)
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return false;
    return x >= UNDO_BUTTON_X - 2 && x <= UNDO_BUTTON_X + UNDO_REDO_BTN_W + 2 &&
           y >= UNDO_REDO_BTN_Y - 2 && y <= UNDO_REDO_BTN_Y + UNDO_REDO_BTN_H + 2;
}

bool isRedoButtonPressed(int x, int y)
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return false;
    return x >= REDO_BUTTON_X - 2 && x <= REDO_BUTTON_X + UNDO_REDO_BTN_W + 2 &&
           y >= UNDO_REDO_BTN_Y - 2 && y <= UNDO_REDO_BTN_Y + UNDO_REDO_BTN_H + 2;
}

// 找出当前页、指定笔主的最后一连续笔划 [start, end)
static bool findLastStrokeOnPageByOwner(uint8_t page, uint16_t ownerHash, size_t &start, size_t &end)
{
    if (ownerHash == 0)
        return false;
    const size_t n = allDrawingHistory.size();
    if (n == 0)
        return false;
    int endIdx = -1;
    for (int i = (int)n - 1; i >= 0; --i) {
        const TouchData_t &d = allDrawingHistory[i];
        if (d.isReset)
            continue;
        if (d.page == page && d.ownerHash == ownerHash) {
            endIdx = i;
            break;
        }
    }
    if (endIdx < 0)
        return false;
    int startIdx = endIdx;
    const unsigned long gapLim = TOUCH_STROKE_INTERVAL * 2UL;
    while (startIdx > 0) {
        const TouchData_t &cur = allDrawingHistory[startIdx];
        const TouchData_t &prev = allDrawingHistory[startIdx - 1];
        if (prev.isReset || prev.page != page || prev.ownerHash != ownerHash)
            break;
        unsigned long dt = (cur.timestamp >= prev.timestamp)
                               ? (cur.timestamp - prev.timestamp)
                               : gapLim + 1;
        if (dt > gapLim)
            break;
        startIdx--;
    }
    start = (size_t)startIdx;
    end = (size_t)endIdx + 1;
    return true;
}

static bool extractLastStrokeOnPageByOwner(uint8_t page, uint16_t ownerHash,
                                           std::vector<TouchData_t> &stroke)
{
    size_t start = 0, end = 0;
    if (!findLastStrokeOnPageByOwner(page, ownerHash, start, end))
        return false;
    stroke.clear();
    stroke.reserve(end - start);
    for (size_t i = start; i < end; i++)
        stroke.push_back(allDrawingHistory[i]);

    std::vector<TouchData_t> kept;
    const size_t n = allDrawingHistory.size();
    kept.reserve(n - (end - start));
    for (size_t i = 0; i < n; i++) {
        if (i >= start && i < end)
            continue;
        kept.push_back(allDrawingHistory[i]);
    }
    allDrawingHistory.clear();
    for (const auto &d : kept)
        allDrawingHistory.push_back(d);
    return true;
}

void applyRemoteCanvasUndo(uint8_t page, const char *ownerId)
{
    uint16_t oh = deviceIdToOwnerHash(ownerId);
    std::vector<TouchData_t> discarded;
    if (!extractLastStrokeOnPageByOwner(page, oh, discarded))
        return;
    if (page == currentCanvasPage && currentUIState == UI_STATE_MAIN && !inCustomColorMode)
        paintCurrentCanvasPage();
    else if (page == currentCanvasPage)
        pendingCanvasRedrawAfterChat = true;
}

void handleCanvasUndo()
{
    std::vector<TouchData_t> stroke;
    if (!extractLastStrokeOnPageByOwner(currentCanvasPage, localOwnerHash(), stroke)) {
        showStatusToast("无可撤销(仅本机)", 1200);
        drawUndoRedoButtons();
        return;
    }
    if (canvasRedoStack.size() >= CANVAS_REDO_STACK_MAX)
        canvasRedoStack.erase(canvasRedoStack.begin());
    canvasRedoStack.push_back(std::move(stroke));
    broadcastCanvasPage(CANVAS_PAGE_ACT_UNDO, currentCanvasPage, canvasPageCount);
    noteLocalDestructiveCanvasEdit();
    paintCurrentCanvasPage();
    drawUndoRedoButtons();
    showStatusToast("已撤销", 800);
}

void handleCanvasRedo()
{
    if (canvasRedoStack.empty()) {
        showStatusToast("无可重做", 900);
        drawUndoRedoButtons();
        return;
    }
    std::vector<TouchData_t> stroke = std::move(canvasRedoStack.back());
    canvasRedoStack.pop_back();
    const uint16_t mine = localOwnerHash();
    for (auto &d : stroke) {
        d.ownerHash = mine;
        allDrawingHistory.push_back(d);
        SyncMessage_t drawMsg;
        memset(&drawMsg, 0, sizeof(drawMsg));
        drawMsg.type = MSG_TYPE_DRAW_POINT;
        drawMsg.senderUptime = millis();
        drawMsg.senderOffset = relativeBootTimeOffset;
        drawMsg.touch_data = d;
        sendSyncMessage(&drawMsg);
        delay(2);
    }
    paintCurrentCanvasPage();
    drawUndoRedoButtons();
    showStatusToast("已重做", 800);
}

void redrawBrushButton()
{
    drawBrushButton();
}

void drawBrushSlider()
{
    if (!isBrushSliderVisible)
        return;
    int sliderTop = BRUSH_SLIDER_Y - BRUSH_SLIDER_HEIGHT / 2;
    int sliderBottom = BRUSH_SLIDER_Y + BRUSH_SLIDER_HEIGHT / 2;
    tft.fillRect(BRUSH_SLIDER_X, sliderTop, BRUSH_SLIDER_WIDTH, BRUSH_SLIDER_HEIGHT, TFT_DARKGREY);
    tft.drawRect(BRUSH_SLIDER_X, sliderTop, BRUSH_SLIDER_WIDTH, BRUSH_SLIDER_HEIGHT, TFT_WHITE);
    int handleY = map(brushRadius, BRUSH_MIN_RADIUS, BRUSH_MAX_RADIUS, sliderBottom, sliderTop);
    int handleX = BRUSH_SLIDER_X + (BRUSH_SLIDER_WIDTH - BRUSH_SLIDER_HANDLE_W) / 2;
    tft.fillRect(handleX, handleY - BRUSH_SLIDER_HANDLE_H / 2, BRUSH_SLIDER_HANDLE_W, BRUSH_SLIDER_HANDLE_H, TFT_CYAN);
    tft.drawRect(handleX, handleY - BRUSH_SLIDER_HANDLE_H / 2, BRUSH_SLIDER_HANDLE_W, BRUSH_SLIDER_HANDLE_H, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawNumber(brushRadius, BRUSH_SLIDER_X + BRUSH_SLIDER_WIDTH / 2, sliderBottom + 10, 1);
    tft.setTextDatum(TL_DATUM);
}

void drawDebugInfo()
{
    if (!isScreenOn || inCustomColorMode || !isDebugInfoVisible)
        return;
    if (currentUIState != UI_STATE_MAIN)
        return;

    int startX = 2;
    int startY = SCREEN_HEIGHT - 42;
    int lineHeight = 10;
    uint16_t bgColor = TFT_GRAY;
    uint16_t textColor = TFT_WHITE;
    int rectHeight = 4 * lineHeight + 2;

    // 如果弹窗可见，则不绘制调试信息背景，避免覆盖弹窗
    if (!isProjectInfoPopupVisible && !isCoffeePopupVisible) {
        tft.fillRect(startX, startY, 120, rectHeight, bgColor);
    }

    tft.setTextColor(textColor, bgColor); // 背景色用于文本背景，使其在弹窗上也可见
    tft.setTextSize(1);
    tft.setTextFont(1); // 默认字体

    char buffer[50];

    sprintf(buffer, "Hist: %u", allDrawingHistory.size());
    tft.setCursor(startX + 2, startY + 2);
    tft.print(buffer);

    sprintf(buffer, "Uptime: %lu", millis());
    tft.setCursor(startX + 2, startY + 2 + lineHeight);
    tft.print(buffer);

    sprintf(buffer, "Comp: %ld", relativeBootTimeOffset);
    tft.setCursor(startX + 2, startY + 2 + 2 * lineHeight);
    tft.print(buffer);

    sprintf(buffer, "Mem: %u/%uKB", ESP.getFreeHeap() / 1024, ESP.getHeapSize() / 1024);
    tft.setCursor(startX + 2, startY + 2 + 3 * lineHeight);
    tft.print(buffer);
}

bool isResetButtonPressed(int x, int y)
{
    return x >= RESET_BUTTON_X && x <= RESET_BUTTON_X + RESET_BUTTON_W &&
           y >= RESET_BUTTON_Y && y <= RESET_BUTTON_Y + RESET_BUTTON_H;
}

bool isColorButtonPressed(int x, int y, uint32_t &selectedColor) {
    uint32_t colors[] = {TFT_RED, TFT_YELLOW, TFT_BLUE, TFT_GREEN};
    for (int i = 0; i < 4; i++)
    {
        int buttonY = COLOR_BUTTON_START_Y + (COLOR_BUTTON_HEIGHT + COLOR_BUTTON_SPACING) * i;
        // 触控容差 +4，避免点不中
        if (x >= RESET_BUTTON_X - 2 && x <= RESET_BUTTON_X + COLOR_BUTTON_WIDTH + 6 &&
            y >= buttonY - 2 && y <= buttonY + COLOR_BUTTON_HEIGHT + 2)
        {
            selectedColor = colors[i];
            return true;
        }
    }
    return false;
}

bool isEraserButtonPressed(int x, int y)
{
    int dx = x - ERASER_BUTTON_X;
    int dy = y - ERASER_BUTTON_Y;
    return (dx * dx + dy * dy) <= (ERASER_BUTTON_RADIUS * ERASER_BUTTON_RADIUS);
}

bool isBrushButtonPressed(int x, int y)
{
    return x >= BRUSH_BUTTON_X - 4 && x <= BRUSH_BUTTON_X + BRUSH_BUTTON_W + 6 &&
           y >= BRUSH_BUTTON_Y - 2 && y <= BRUSH_BUTTON_Y + BRUSH_BUTTON_H + 4;
}

bool isBrushSliderPressed(int x, int y)
{
    if (!isBrushSliderVisible)
        return false;
    int sliderTop = BRUSH_SLIDER_Y - BRUSH_SLIDER_HEIGHT / 2;
    int sliderBottom = BRUSH_SLIDER_Y + BRUSH_SLIDER_HEIGHT / 2;
    return x >= BRUSH_SLIDER_X - 4 && x <= BRUSH_SLIDER_X + BRUSH_SLIDER_WIDTH + BRUSH_SLIDER_HANDLE_W &&
           y >= sliderTop - 4 && y <= sliderBottom + 4;
}

bool isPeerInfoButtonPressed(int x, int y)
{
    return x >= PEER_INFO_BUTTON_X && x <= PEER_INFO_BUTTON_X + PEER_INFO_BUTTON_W &&
           y >= PEER_INFO_BUTTON_Y && y <= PEER_INFO_BUTTON_Y + PEER_INFO_BUTTON_H;
}


bool isCustomColorButtonPressed(int x, int y)
{
    // 扩大触控容差，右上角边缘更好点
    return x >= CUSTOM_COLOR_BUTTON_X - 6 && x <= CUSTOM_COLOR_BUTTON_X + CUSTOM_COLOR_BUTTON_W + 8 &&
           y >= CUSTOM_COLOR_BUTTON_Y - 4 && y <= CUSTOM_COLOR_BUTTON_Y + CUSTOM_COLOR_BUTTON_H + 8;
}

bool isBackButtonPressed(int x, int y)
{
    return x >= BACK_BUTTON_X - 8 && x <= BACK_BUTTON_X + BACK_BUTTON_W + 10 &&
           y >= BACK_BUTTON_Y - 6 && y <= BACK_BUTTON_Y + BACK_BUTTON_H + 8;
}

bool isDebugToggleButtonPressed(int x, int y)
{
    (void)x; (void)y;
    return false; // D 按钮已移除
}

void drawDebugToggleButton()
{
    // C/D 已移除，保留空实现以免旧调用报错
}

void toggleDebugInfo()
{
    isDebugInfoVisible = !isDebugInfoVisible;
    showDebugToggleButton = !isDebugInfoVisible;
    if (isProjectInfoPopupVisible && !isDebugInfoVisible) { // 如果关闭调试信息时弹窗是开的，也关掉弹窗
        hideProjectInfoPopup(); // 这会触发 redrawMainScreen
    } else {
        redrawMainScreen();
    }
}

void handleCustomColorTouch(int x, int y)
{
    // 返回键优先（底部独立区域，不再被蓝条挡住）
    if (isBackButtonPressed(x, y)) {
        updateCurrentColor(tft.color565(redValue, greenValue, blueValue));
        closeColorSelectors();
        return;
    }

    const int sliderLeft = SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4 - COLOR_SLIDER_TOUCH_PAD;
    const int sliderRight = SCREEN_WIDTH - 2;
    if (x < sliderLeft || x > sliderRight)
        return;

    if (y >= 0 && y < COLOR_SLIDER_HEIGHT) {
        updateSingleColorSlider(y, TFT_RED, redValue);
    } else if (y >= COLOR_SLIDER_HEIGHT && y < 2 * COLOR_SLIDER_HEIGHT) {
        updateSingleColorSlider(y - COLOR_SLIDER_HEIGHT, TFT_GREEN, greenValue);
    } else if (y >= 2 * COLOR_SLIDER_HEIGHT && y < 3 * COLOR_SLIDER_HEIGHT) {
        updateSingleColorSlider(y - (2 * COLOR_SLIDER_HEIGHT), TFT_BLUE, blueValue);
    } else {
        return;
    }
    // 拖动时不整屏三滑条全刷，只刷预览+当前值（refresh 已含三栏，但比整屏快）
    refreshAllColorSliders();
    updateCurrentColor(tft.color565(redValue, greenValue, blueValue));
}

void handleBrushSliderTouch(int x, int y)
{
    if (!isBrushSliderVisible)
        return;
    int sliderTop = BRUSH_SLIDER_Y - BRUSH_SLIDER_HEIGHT / 2;
    int sliderBottom = BRUSH_SLIDER_Y + BRUSH_SLIDER_HEIGHT / 2;
    if (x >= BRUSH_SLIDER_X - 4 && x <= BRUSH_SLIDER_X + BRUSH_SLIDER_WIDTH + BRUSH_SLIDER_HANDLE_W &&
        y >= sliderTop && y <= sliderBottom) {
        brushRadius = map(y, sliderBottom, sliderTop, BRUSH_MIN_RADIUS, BRUSH_MAX_RADIUS);
        brushRadius = constrain(brushRadius, BRUSH_MIN_RADIUS, BRUSH_MAX_RADIUS);
        drawBrushSlider();
        redrawBrushButton();
    }
}

void updateSingleColorSlider(int yPos, uint32_t sliderColor, int &channelValue)
{
    channelValue = constrain(map(yPos, 0, COLOR_SLIDER_HEIGHT - 1, 255, 0), 0, 255);
    // 绘制由 refreshAllColorSliders 处理
}

void drawColorSelectors()
{
    refreshAllColorSliders();

    tft.fillRect(BACK_BUTTON_X - 4, BACK_BUTTON_Y - 2, BACK_BUTTON_W + 8, BACK_BUTTON_H + 4, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("OK", BACK_BUTTON_X + BACK_BUTTON_W / 2, BACK_BUTTON_Y + BACK_BUTTON_H / 2, 1);
    tft.setTextDatum(TL_DATUM);
}

void updateCustomColorPreview()
{
    uint32_t previewColor = tft.color565(redValue, greenValue, blueValue);
    int previewY = 3 * COLOR_SLIDER_HEIGHT + 2;
    int previewBoxHeight = BACK_BUTTON_Y - previewY - 4;
    if (previewBoxHeight < 12)
        previewBoxHeight = 12;

    tft.fillRect(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4, previewY, COLOR_SLIDER_WIDTH, previewBoxHeight, previewColor);
    tft.drawRect(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4, previewY, COLOR_SLIDER_WIDTH, previewBoxHeight, TFT_WHITE);
}

void refreshAllColorSliders()
{
    float barHeightRed = redValue * COLOR_SLIDER_HEIGHT / 255.0;
    float barHeightGreen = greenValue * COLOR_SLIDER_HEIGHT / 255.0;
    float barHeightBlue = blueValue * COLOR_SLIDER_HEIGHT / 255.0;

    // 清除旧的数值显示区域 - 减小宽度
    //tft.fillRect(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4 - 40, 0, 40, 3 * COLOR_SLIDER_HEIGHT, TFT_BLACK); // 减小清除区域宽度

    // 绘制红色滑块
    tft.fillRect(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4, 0, COLOR_SLIDER_WIDTH, COLOR_SLIDER_HEIGHT, TFT_BLACK);
    tft.drawRect(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4, 0, COLOR_SLIDER_WIDTH, COLOR_SLIDER_HEIGHT, TFT_RED);
    tft.fillRect(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4, COLOR_SLIDER_HEIGHT - barHeightRed, COLOR_SLIDER_WIDTH, barHeightRed, TFT_RED);
    // 显示红色数值
    char redBuffer[10];
    sprintf(redBuffer, "%d(#%02X)", redValue, redValue); // 十进制(十六进制)
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4 - 40, 2); // 调整位置以适应新的清除区域宽度
    tft.print(redBuffer);

    // 绘制绿色滑块
    tft.fillRect(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4, COLOR_SLIDER_HEIGHT, COLOR_SLIDER_WIDTH, COLOR_SLIDER_HEIGHT, TFT_BLACK);
    tft.drawRect(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4, COLOR_SLIDER_HEIGHT, COLOR_SLIDER_WIDTH, COLOR_SLIDER_HEIGHT, TFT_GREEN);
    tft.fillRect(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4, 2 * COLOR_SLIDER_HEIGHT - barHeightGreen, COLOR_SLIDER_WIDTH, barHeightGreen, TFT_GREEN);
    // 显示绿色数值
    char greenBuffer[10];
    sprintf(greenBuffer, "%d(#%02X)", greenValue, greenValue); // 十进制(十六进制)
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4 - 40, COLOR_SLIDER_HEIGHT + 2); // 调整位置
    tft.print(greenBuffer);

    // 绘制蓝色滑块
    tft.fillRect(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4, 2 * COLOR_SLIDER_HEIGHT, COLOR_SLIDER_WIDTH, COLOR_SLIDER_HEIGHT, TFT_BLACK);
    tft.drawRect(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4, 2 * COLOR_SLIDER_HEIGHT, COLOR_SLIDER_WIDTH, COLOR_SLIDER_HEIGHT, TFT_BLUE);
    tft.fillRect(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4, 3 * COLOR_SLIDER_HEIGHT - barHeightBlue, COLOR_SLIDER_WIDTH, barHeightBlue, TFT_BLUE);
    // 显示蓝色数值
    char blueBuffer[10];
    sprintf(blueBuffer, "%d(#%02X)", blueValue, blueValue); // 十进制(十六进制)
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4 - 40, 2 * COLOR_SLIDER_HEIGHT + 2); // 调整位置
    tft.print(blueBuffer);

    updateCustomColorPreview();
}

void closeColorSelectors()
{
    updateCurrentColor(tft.color565(redValue, greenValue, blueValue));
    inCustomColorMode = false;
    currentUIState = UI_STATE_MAIN;

    if (savedScreenBuffer != nullptr)
    {
        restoreSavedScreenArea();
    }

    tft.fillScreen(TFT_BLACK);
    drawMainInterface();
    replayAllDrawings();
    redrawStarButton();
}

void updateCurrentColor(uint32_t newColor)
{
    currentColor = newColor;
}

void updateConnectedDevicesCount()
{
    char deviceCountBuffer[10];
    size_t n = 0;
    for (auto const &kv : peerInfoMap) {
        if (peerVisibleForLocalMode(kv.second))
            n++;
    }
    sprintf(deviceCountBuffer, "%d", (int)n);

    tft.fillRect(PEER_INFO_BUTTON_X, PEER_INFO_BUTTON_Y, PEER_INFO_BUTTON_W, PEER_INFO_BUTTON_H, TFT_BLUE);
    tft.setTextColor(TFT_WHITE, TFT_BLUE);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(String(deviceCountBuffer),
                   PEER_INFO_BUTTON_X + PEER_INFO_BUTTON_W / 2,
                   PEER_INFO_BUTTON_Y + PEER_INFO_BUTTON_H / 2,
                   1);
    tft.setTextDatum(TL_DATUM);
}

void saveScreenArea()
{
    if (savedScreenBuffer == nullptr)
    {
        int bufferHeight = 4 * COLOR_SLIDER_HEIGHT;
        if (bufferHeight > SCREEN_HEIGHT)
            bufferHeight = SCREEN_HEIGHT;
        savedScreenBuffer = new uint16_t[COLOR_SLIDER_WIDTH * bufferHeight];
    }
    int readHeight = 4 * COLOR_SLIDER_HEIGHT;
    if (readHeight > SCREEN_HEIGHT)
        readHeight = SCREEN_HEIGHT;
    tft.readRect(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4, 0, COLOR_SLIDER_WIDTH, readHeight, savedScreenBuffer);
}

void restoreSavedScreenArea()
{
    if (savedScreenBuffer != nullptr)
    {
        int pushHeight = 4 * COLOR_SLIDER_HEIGHT;
        if (pushHeight > SCREEN_HEIGHT > SCREEN_HEIGHT)
            pushHeight = SCREEN_HEIGHT;
        tft.pushImage(SCREEN_WIDTH - COLOR_SLIDER_WIDTH - 4, 0, COLOR_SLIDER_WIDTH, pushHeight, savedScreenBuffer);
        delete[] savedScreenBuffer;
        savedScreenBuffer = nullptr;
    }
}

void clearScreenAndCache()
{
    allDrawingHistory.clear();
    currentCanvasPage = 0;
    canvasPageCount = 1;
    // 非花瓣画板：只清历史，绝不把 restore/清屏画到聊天等界面上
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode) {
        pendingCanvasRedrawAfterChat = true;
        return;
    }
    tft.fillScreen(TFT_BLACK);
    drawMainInterface();
}

void hideStarButton()
{
    tft.fillRect(CUSTOM_COLOR_BUTTON_X, CUSTOM_COLOR_BUTTON_Y, CUSTOM_COLOR_BUTTON_W, CUSTOM_COLOR_BUTTON_H, TFT_BLACK);
}

void redrawEraserButton()
{
    tft.fillCircle(ERASER_BUTTON_X, ERASER_BUTTON_Y, ERASER_BUTTON_RADIUS + 1, TFT_BLACK);
    drawEraserButton();
}

void showStarButton()
{
    drawStarButton();
}

void redrawStarButton()
{
    hideStarButton();
    drawStarButton();
    redrawBrushButton();
}

// --- 进度条函数实现 ---
void drawArcSlice(int x, int y, int r, int thickness, int start_angle, int end_angle, uint16_t color)
{
    if (start_angle == end_angle)
        return;

    start_angle = start_angle % 360;
    end_angle = end_angle % 360;
    if (end_angle == 0 && start_angle < 359)
        end_angle = 360;

    int angle_offset = -90;
    uint32_t sa = (start_angle + angle_offset + 360) % 360;
    uint32_t ea = (end_angle + angle_offset + 360) % 360;

    tft.drawArc(x, y, r, r - thickness, sa, ea, color, color, false);
}

void drawSendProgressIndicator()
{
    if (!showSendProgress || inCustomColorMode || currentUIState != UI_STATE_MAIN)
        return;

    tft.drawCircle(SEND_PROGRESS_X, SEND_PROGRESS_Y, PROGRESS_CIRCLE_RADIUS, PROGRESS_BG_COLOR);
    tft.drawCircle(SEND_PROGRESS_X, SEND_PROGRESS_Y, PROGRESS_CIRCLE_RADIUS - PROGRESS_CIRCLE_THICKNESS, PROGRESS_BG_COLOR);

    if (sendProgressTotal > 0 && sendProgressCurrent > 0)
    {
        int progress_angle = (sendProgressCurrent * 360) / sendProgressTotal;
        if (progress_angle > 360)
            progress_angle = 360;
        if (progress_angle < 0)
            progress_angle = 0;

        for (int i = 0; i < PROGRESS_CIRCLE_THICKNESS; ++i)
        {
            tft.drawCircle(SEND_PROGRESS_X, SEND_PROGRESS_Y, PROGRESS_CIRCLE_RADIUS - i, PROGRESS_BG_COLOR);
        }
        drawArcSlice(SEND_PROGRESS_X, SEND_PROGRESS_Y, PROGRESS_CIRCLE_RADIUS, PROGRESS_CIRCLE_THICKNESS, 0, progress_angle, PROGRESS_SEND_COLOR);
    }
}

void drawReceiveProgressIndicator()
{
    if (!showReceiveProgress || inCustomColorMode || currentUIState != UI_STATE_MAIN)
        return;

    if (receiveProgressTotal > 0 && receiveProgressCurrent > 0)
    {
        int progress_angle = (receiveProgressCurrent * 360) / receiveProgressTotal;
        if (progress_angle > 360)
            progress_angle = 360;
        if (progress_angle < 0)
            progress_angle = 0;

        for (int i = 0; i < PROGRESS_CIRCLE_THICKNESS; ++i)
        {
            tft.drawCircle(RECEIVE_PROGRESS_X, RECEIVE_PROGRESS_Y, PROGRESS_CIRCLE_RADIUS - i, PROGRESS_BG_COLOR);
        }
        drawArcSlice(RECEIVE_PROGRESS_X, RECEIVE_PROGRESS_Y, PROGRESS_CIRCLE_RADIUS, PROGRESS_CIRCLE_THICKNESS, 0, progress_angle, PROGRESS_RECEIVE_COLOR);
    }
}

void updateSendProgress(int current, int total)
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return;
    if (total <= 0)
    {
        hideSendProgress();
        return;
    }
    sendProgressCurrent = current;
    sendProgressTotal = total;
    showSendProgress = true;

    if (!inCustomColorMode)
    {
        drawSendProgressIndicator();
    }

    if (current >= total)
    {
        hideSendProgress();
    }
}

void updateReceiveProgress(int current, int total)
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return;
    if (total <= 0)
    {
        hideReceiveProgress();
        return;
    }
    receiveProgressCurrent = current;
    receiveProgressTotal = total;
    showReceiveProgress = true;

    if (!inCustomColorMode)
    {
        drawReceiveProgressIndicator();
    }

    // 移除: if (current >= total) { hideReceiveProgress(); }
    // 隐藏操作将由 esp_now_handler 在接收完成后显式调用
}

void hideSendProgress()
{
    if (showSendProgress)
    {
        showSendProgress = false;
        tft.fillRect(SEND_PROGRESS_X - PROGRESS_CIRCLE_RADIUS - 1, SEND_PROGRESS_Y - PROGRESS_CIRCLE_RADIUS - 1,
                     2 * PROGRESS_CIRCLE_RADIUS + 2, 2 * PROGRESS_CIRCLE_RADIUS + 2, TFT_BLACK);
    }
}

void hideReceiveProgress()
{
    if (showReceiveProgress)
    {
        showReceiveProgress = false;
        tft.fillRect(RECEIVE_PROGRESS_X - PROGRESS_CIRCLE_RADIUS - 1, RECEIVE_PROGRESS_Y - PROGRESS_CIRCLE_RADIUS - 1,
                     2 * PROGRESS_CIRCLE_RADIUS + 2, 2 * PROGRESS_CIRCLE_RADIUS + 2, TFT_BLACK);
    }
}

// --- "Coffee" 按钮和弹窗函数 ---
void drawCoffeeButton() {
    // C 按钮已移除
}

bool isCoffeeButtonPressed(int x, int y) {
    (void)x; (void)y;
    return false; // C 按钮已移除
}

void showCoffeePopup() {
    if (!isScreenOn || inCustomColorMode || currentUIState != UI_STATE_MAIN) return;

    isCoffeePopupVisible = true;
    currentUIState = UI_STATE_POPUP;
    // isDebugInfoVisible = false; // 打开C弹窗时，可以考虑隐藏D的调试信息区域
    // showDebugToggleButton = false; // 同时隐藏D按钮

    // 弹窗区域和颜色
    int popupX = 10;
    int popupY = 10;
    int popupW = SCREEN_WIDTH - 2 * popupX;
    int popupH = SCREEN_HEIGHT - 2 * popupY;
    uint16_t popupBgColor = tft.color565(70, 70, 70); // 深灰色
    uint16_t popupBorderColor = TFT_WHITE;
    uint16_t textColor = TFT_WHITE;
    uint16_t qrPixelColor = TFT_WHITE;
    uint16_t qrBgColor = popupBgColor; // 二维码背景与弹窗背景一致

    tft.fillRect(popupX, popupY, popupW, popupH, popupBgColor);
    tft.drawRect(popupX, popupY, popupW, popupH, popupBorderColor);

    tft.setTextColor(textColor, popupBgColor);
    tft.setTextDatum(TC_DATUM);
    tft.setTextSize(1);
    tft.drawString("Coffee for Shapaper!", popupX + popupW / 2, popupY + 5, 2); // 标题

    tft.setTextDatum(TL_DATUM);
    int textX = popupX + 5;
    int textY = popupY + 25;
    int lineHeight = 9; // 英文小字体行高，稍微调小一点以容纳更多内容

    const char* line1 = "This project was largely refactored by Shapaper,";
    const char* line2 = "consuming much time and effort.";
    const char* line3 = "Welcome to buy me a coffee!";
    // const char* line4 = "a coffee!"; // 合并到上一行

    tft.setCursor(textX, textY);
    tft.print(line1);
    textY += lineHeight;
    tft.setCursor(textX, textY);
    tft.print(line2);
    textY += lineHeight;
    tft.setCursor(textX, textY);
    tft.print(line3);
    // textY += lineHeight;
    // tft.setCursor(textX, textY);
    // tft.print(line4);

    textY += lineHeight * 1.5; // 文本和二维码之间的间距

    // 支付宝二维码数据
    const char *qr_data_alipay =
        "EEEEEEE.E....EEEE.E...EEEEEEE\n"
        "E.....E.E..EEEE..E.E..E.....E\n"
        "E.EEE.E.EE..E.E..E.EE.E.EEE.E\n"
        "E.EEE.E.EEE.EEEEE.EE..E.EEE.E\n"
        "E.EEE.E..EE.E..EEEE.E.E.EEE.E\n"
        "E.....E.EEE...E.EEEE..E.....E\n"
        "EEEEEEE.E.E.E.E.E.E.E.EEEEEEE\n"
        "..........EE.....EE.E........\n"
        "EE..EEE...E.E...EE..E..E.EEEE\n"
        "EEEE.E.E.....E.E.E....EEEEEEE\n"
        "E..EE.E.EEE....EEE..E.......E\n"
        "....EE..EE..E.EE.E.EEEEE.E.EE\n"
        "EE....E.E..E..E.EEE.EE.....E.\n"
        ".E..EE.EEEE.EE.E......EEEEEEE\n"
        ".E..E.EE.EEEE..E.E..EE.EEEE.E\n"
        "..E..E.E.EEE..EE.EEE..E.E..EE\n"
        "EEEEEEEE..E.E....EEEEE.....E.\n"
        "EEEEEE.E..E..EEEE....EE.EE.EE\n"
        "..E..EEEEEE..E.E.....EEEE.E.E\n"
        "...E......E.E....E.EEEEE...EE\n"
        "EEE.EEEE...EE...EEE.EEEEEE..E\n"
        "........E.E.EEEE.E.EE...E...E\n"
        "EEEEEEE....EEE.EEE.EE.E.EEE.E\n"
        "E.....E.EEEEE..E.EE.E...E...E\n"
        "E.EEE.E.E.E.....EEEEEEEEEE.EE\n"
        "E.EEE.E..E.EE..EE...EE.E....E\n"
        "E.EEE.E..E.EEE.EE.E.EE...EEEE\n"
        "E.....E.EE.EE..EEE.EEEE..E.EE\n"
        "EEEEEEE.EEE.E..EEEE.EE..E..E.";

    // 微信二维码数据
    const char *qr_data_wechat =
        "EEEEEEE.EE.EE..E...E..EEEEEEE\n"
        "E.....E.E.E..EE...EE..E.....E\n"
        "E.EEE.E.EE.E...EEEEE..E.EEE.E\n"
        "E.EEE.E.E..EE.E.....E.E.EEE.E\n"
        "E.EEE.E...E..E.E...E..E.EEE.E\n"
        "E.....E.E.E.....EEEE..E.....E\n"
        "EEEEEEE.E.E.E.E.E.E.E.EEEEEEE\n"
        ".........E.E..EEEEEEE........\n"
        "EE..EEE...EE.E..E...E..E.EEEE\n"
        "E.E.EE..E.EE.EEEEEEE...EEE.EE\n"
        ".EEEEEEE.EEEE.EEE...EE.EE.EEE\n"
        "EEEEE...EE.EE..E.E.E..E.E..EE\n"
        "...EE.E....EEE.....EEE..EE...\n"
        "EE...E.EEE....E.E....E.EE..EE\n"
        "E...EEEEE..EEEEE....E.EEEEE.E\n"
        "...EE.....E.E...EE.E.E.....EE\n"
        ".E...EEEEEE.E..E.E.E.E.E.E..E\n"
        "EEE..........EE.E..E....E..EE\n"
        "..EEEEEEEE.EE.EE..E..E.E.EE.E\n"
        "..E.EE..E.E.E.EE.EEEEE.E.E...\n"
        "EE...EE.EEE........EEEEEEE.E.\n"
        "........E....EEEEEE.E...E.EEE\n"
        "EEEEEEE....EE..EE.E.E.E.E.E.E\n"
        "E.....E.E..EE.E.EE.EE...E...E\n"
        "E.EEE.E.E.....EE...EEEEEE..E.\n"
        "E.EEE.E...E..E..E...EE.E.EEEE\n"
        "E.EEE.E..E.E...E.EE.E.EE..EEE\n"
        "E.....E.E..E..E..E.EEE.EEE.EE\n"
        "EEEEEEE.EEEE..E.E...E.EEEE.E.";

    int qrPixelSize = 2; // 每个二维码“像素”的大小
    int qrWidthInPixels = 29 * qrPixelSize; // 二维码的屏幕宽度
    int qrHeightInPixels = 29 * qrPixelSize; // 二维码的屏幕高度
    int spacingBetweenQRs = 10; // 两个二维码之间的间距
    int totalQRWidth = 2 * qrWidthInPixels + spacingBetweenQRs;

    int qrCommonY = textY + lineHeight; // 二维码的共同起始Y坐标 (在标签下方)

    // 绘制支付宝二维码
    int qrAlipayOffsetX = popupX + (popupW - totalQRWidth) / 2;
    tft.setTextDatum(TC_DATUM);
    tft.drawString("Alipay", qrAlipayOffsetX + qrWidthInPixels / 2, textY, 1); // 标签字体大小1

    char lineBuffer[35];
    const char *p_alipay = qr_data_alipay;
    int currentY_alipay = qrCommonY;

    for (int row = 0; row < 29 && *p_alipay; ++row) { // 假设支付宝二维码是29行
        int i = 0;
        while (*p_alipay && *p_alipay != '\n' && i < 30) {
            lineBuffer[i++] = *p_alipay++;
        }
        lineBuffer[i] = '\0';
        if (*p_alipay == '\n') p_alipay++;

        for (int j = 0; j < i; ++j) {
            if (lineBuffer[j] == 'E') {
                tft.fillRect(qrAlipayOffsetX + j * qrPixelSize, currentY_alipay, qrPixelSize, qrPixelSize, qrPixelColor);
            }
        }
        currentY_alipay += qrPixelSize;
        if (currentY_alipay > popupY + popupH - 5 - qrPixelSize) break;
    }

    // 绘制微信二维码
    int qrWechatOffsetX = qrAlipayOffsetX + qrWidthInPixels + spacingBetweenQRs;
    tft.drawString("WeChat", qrWechatOffsetX + qrWidthInPixels / 2, textY, 1); // 标签字体大小1

    const char *p_wechat = qr_data_wechat;
    int currentY_wechat = qrCommonY;

    for (int row = 0; row < 29 && *p_wechat; ++row) { // 假设微信二维码也是29行
        int i = 0;
        while (*p_wechat && *p_wechat != '\n' && i < 30) {
            lineBuffer[i++] = *p_wechat++;
        }
        lineBuffer[i] = '\0';
        if (*p_wechat == '\n') p_wechat++;

        for (int j = 0; j < i; ++j) {
            if (lineBuffer[j] == 'E') {
                tft.fillRect(qrWechatOffsetX + j * qrPixelSize, currentY_wechat, qrPixelSize, qrPixelSize, qrPixelColor);
            }
        }
        currentY_wechat += qrPixelSize;
        if (currentY_wechat > popupY + popupH - 5 - qrPixelSize) break;
    }

    // --- 新增 Shapaper's Blog 二维码 ---
    textY = currentY_alipay > currentY_wechat ? currentY_alipay : currentY_wechat; // 取两个二维码中较低的Y值作为基准
    textY += lineHeight * 0.5; // 在二维码下方留出一些间距

    tft.setTextDatum(TC_DATUM);
    tft.drawString("Shapaper's Blog(https://blog.dimeta.top/)", popupX + popupW / 2, textY, 1); // 博客标题
    textY += lineHeight;                                                                  // 在二维码下方留出一些间距
    tft.setTextDatum(TL_DATUM);
    tft.drawString("Have many interesting things,Scan to visit", popupX + 5, textY, 1); // 博客二维码标签
    textY += lineHeight; // 为二维码留出空间

    const char *qr_data_blog =
        "EEEEEEE..E....E.E.EEEEEEE\n"
        "E.....E.EE.EE.EEE.E.....E\n"
        "E.EEE.E..EEE...EE.E.EEE.E\n"
        "E.EEE.E.EE.E.EEE..E.EEE.E\n"
        "E.EEE.E...E.E...E.E.EEE.E\n"
        "E.....E.E.E....EE.E.....E\n"
        "EEEEEEE.E.E.E.E.E.E.EEEEEEE\n"
        "............E.EEE........\n"
        "EEEEE.EEEEE.EE...E.E.E.E.\n"
        "EE.EEE.E.E...E..E..E...E.\n"
        "EE....E..E.EEEEEEE..EE.EE\n"
        "....E..EEEEE...E..E.....E\n"
        "..E..EE..E.E.EE..EE.E.EEE\n"
        "E.EE.E....E.E.E.E..E.E.E.\n"
        "E.EE.EEE......EE.E.EEE.EE\n"
        "E..EEE..E...E..EEE.EE...E\n"
        "E...E.E.EE..EEEEEEEEE.E..\n"
        "........EE...E.EE...EE...\n"
        "EEEEEEE.E.EEEEE.E.E.E.EEE\n"
        "E.....E....E..E.E...EE..E\n"
        "E.EEE.E.E.EEEEEEEEEEE.E..\n"
        "E.EEE.E.EEE.E.EEEEE.EEEEE\n"
        "E.EEE.E.EEE.....E....EE.E\n"
        "E.....E.E.EE...EEEEEEE..E\n"
        "EEEEEEE.E..EEE.E.E.EEEEEE";

    int qrPixelSizeBlog = 2; // 博客二维码像素大小
    int qrWidthInCharsBlog = 29; // 博客二维码字符宽度
    int qrDisplayWidthBlog = qrWidthInCharsBlog * qrPixelSizeBlog;

    int qrBlogOffsetX = popupX + (popupW - qrDisplayWidthBlog) / 2; // 单个二维码居中
    int currentY_blog = textY;
    const char *p_blog = qr_data_blog;

    for (int row = 0; row < 25 && *p_blog; ++row) { // 假设博客二维码也是25行 (根据txt内容调整)
        int i = 0;
        while (*p_blog && *p_blog != '\n' && i < qrWidthInCharsBlog) {
            lineBuffer[i++] = *p_blog++;
        }
        lineBuffer[i] = '\0';
        if (*p_blog == '\n') p_blog++;

        for (int j = 0; j < i; ++j) {
            if (lineBuffer[j] == 'E') {
                tft.fillRect(qrBlogOffsetX + j * qrPixelSizeBlog, currentY_blog, qrPixelSizeBlog, qrPixelSizeBlog, qrPixelColor);
            }
        }
        currentY_blog += qrPixelSizeBlog;
        if (currentY_blog > popupY + popupH - 5 - qrPixelSizeBlog - lineHeight) break; // 避免覆盖关闭提示
    }
    // --- 结束新增 Shapaper's Blog 二维码 ---

    tft.setTextDatum(BC_DATUM);
    tft.drawString("(Tap to close)", popupX + popupW / 2, popupY + popupH - 5, 1);
    tft.setTextDatum(TL_DATUM);
}

void hideCoffeePopup() {
    if (isCoffeePopupVisible) {
        isCoffeePopupVisible = false;
        currentUIState = UI_STATE_MAIN;
        // showDebugToggleButton = true; // 恢复D按钮的显示（如果之前隐藏了）
        redrawMainScreen();
    }
}

void drawClearConfirmPopup()
{
    if (!isClearConfirmVisible)
        return;

    tft.fillRect(CONFIRM_POPUP_X, CONFIRM_POPUP_Y, CONFIRM_POPUP_W, CONFIRM_POPUP_H,
                 tft.color565(36, 40, 48));
    tft.drawRect(CONFIRM_POPUP_X, CONFIRM_POPUP_Y, CONFIRM_POPUP_W, CONFIRM_POPUP_H, TFT_WHITE);
    tft.drawRect(CONFIRM_POPUP_X + 1, CONFIRM_POPUP_Y + 1, CONFIRM_POPUP_W - 2, CONFIRM_POPUP_H - 2,
                 tft.color565(90, 100, 120));

    const char *title = "清空本页";
    const char *hint = "仅清除当前页笔迹";
    if (clearConfirmKind == CLEAR_CONFIRM_ALL) {
        title = "清空全部";
        hint = "所有页笔迹都会清除";
    } else if (clearConfirmKind == CLEAR_CONFIRM_FLIP) {
        title = "确认翻转";
        hint = "画面转180度";
    } else if (clearConfirmKind == CLEAR_CONFIRM_PAGE) {
        title = "清空本页";
        hint = "仅清除当前页笔迹";
    }

    int titleW = cnTextWidth(title);
    cnDrawUtf8(tft, CONFIRM_POPUP_X + (CONFIRM_POPUP_W - titleW) / 2,
               CONFIRM_POPUP_Y + 18, title, TFT_WHITE);
    int hintW = cnTextWidth(hint);
    cnDrawUtf8(tft, CONFIRM_POPUP_X + (CONFIRM_POPUP_W - hintW) / 2,
               CONFIRM_POPUP_Y + 44, hint, tft.color565(180, 180, 190));

    // 取消
    tft.fillRoundRect(CONFIRM_CANCEL_X, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H, 4,
                      tft.color565(70, 74, 86));
    int cancelW = cnTextWidth("取消");
    cnDrawUtf8(tft, CONFIRM_CANCEL_X + (CONFIRM_BTN_W - cancelW) / 2,
               CONFIRM_BTN_Y + 6, "取消", TFT_WHITE);

    // 确定
    uint16_t okBg = (clearConfirmKind == CLEAR_CONFIRM_FLIP)
                        ? tft.color565(40, 120, 90)
                        : tft.color565(180, 48, 48);
    tft.fillRoundRect(CONFIRM_OK_X, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H, 4, okBg);
    int okW = cnTextWidth("确定");
    cnDrawUtf8(tft, CONFIRM_OK_X + (CONFIRM_BTN_W - okW) / 2,
               CONFIRM_BTN_Y + 6, "确定", TFT_WHITE);
}

void showClearConfirm(ClearConfirmKind_t kind)
{
    if (!isScreenOn || inCustomColorMode)
        return;
    if (kind != CLEAR_CONFIRM_ALL && kind != CLEAR_CONFIRM_PAGE && kind != CLEAR_CONFIRM_FLIP)
        return;
    if (isCoffeePopupVisible || isProjectInfoPopupVisible)
        return;

    clearConfirmKind = kind;
    isClearConfirmVisible = true;
    currentUIState = UI_STATE_POPUP;
    drawClearConfirmPopup();
}

void hideClearConfirm(bool redraw)
{
    if (!isClearConfirmVisible)
        return;
    isClearConfirmVisible = false;
    clearConfirmKind = CLEAR_CONFIRM_NONE;
    currentUIState = UI_STATE_MAIN;
    if (redraw)
        redrawMainScreen();
}

void performFullCanvasReset()
{
    unsigned long now = millis();
    allDrawingHistory.clear();
    clearCanvasRedoStack();
    noteLocalDestructiveCanvasEdit();
    clearScreenAndCache();

    relativeBootTimeOffset = 0;
    iamEffectivelyMoreUptimeDevice = false;
    iamRequestingAllData = false;
    initialSyncLogicProcessed = false;

    SyncMessage_t resetMsg;
    memset(&resetMsg, 0, sizeof(resetMsg));
    resetMsg.type = MSG_TYPE_RESET_CANVAS;
    resetMsg.senderUptime = now;
    resetMsg.senderOffset = relativeBootTimeOffset;
    resetMsg.touch_data.isReset = true;
    resetMsg.touch_data.timestamp = now;
    resetMsg.touch_data.color = currentColor;
    for (int i = 0; i < 2; i++) {
        sendSyncMessage(&resetMsg);
        delay(5);
    }
    forcePushDrawingHistoryToPeers();
    showStatusToast("已全部清空", 1200);
}

bool handleClearConfirmTouch(int x, int y)
{
    if (!isClearConfirmVisible)
        return false;

    const bool onCancel =
        (x >= CONFIRM_CANCEL_X && x <= CONFIRM_CANCEL_X + CONFIRM_BTN_W &&
         y >= CONFIRM_BTN_Y && y <= CONFIRM_BTN_Y + CONFIRM_BTN_H);
    const bool onOk =
        (x >= CONFIRM_OK_X && x <= CONFIRM_OK_X + CONFIRM_BTN_W &&
         y >= CONFIRM_BTN_Y && y <= CONFIRM_BTN_Y + CONFIRM_BTN_H);

    if (onCancel) {
        hideClearConfirm(true);
        return true;
    }

    if (onOk) {
        ClearConfirmKind_t kind = clearConfirmKind;
        hideClearConfirm(false);
        if (kind == CLEAR_CONFIRM_ALL) {
            performFullCanvasReset();
        } else if (kind == CLEAR_CONFIRM_PAGE) {
            currentUIState = UI_STATE_MAIN;
            handleCanvasPageClear();
        } else if (kind == CLEAR_CONFIRM_FLIP) {
            currentUIState = UI_STATE_MAIN;
            handleCanvasFlip();
        } else {
            redrawMainScreen();
        }
        return true;
    }

    // 点在弹窗外：忽略（避免误关）；只有「取消/确定」生效
    return true; // 点在弹窗空白处也吞掉，避免画到下面
}

// --- 新增的项目信息按钮和弹窗函数 ---
void drawInfoButton() {
    if (!isScreenOn || inCustomColorMode || !isDebugInfoVisible || isProjectInfoPopupVisible || isCoffeePopupVisible || isClearConfirmVisible || currentUIState != UI_STATE_MAIN) return; // 如果Coffee弹窗也显示，则不绘制，或不在主界面

    tft.fillRect(INFO_BUTTON_X, INFO_BUTTON_Y, INFO_BUTTON_W, INFO_BUTTON_H, TFT_DARKGREEN);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("*", INFO_BUTTON_X + INFO_BUTTON_W / 2, INFO_BUTTON_Y + INFO_BUTTON_H / 2, 2); // 字体大小2
    tft.setTextDatum(TL_DATUM);
}

bool isInfoButtonPressed(int x, int y) {
    if (!isDebugInfoVisible || isCoffeePopupVisible || currentUIState != UI_STATE_MAIN) return false; // 仅当调试信息可见且Coffee弹窗关闭时按钮才有效，且在主界面
    return x >= INFO_BUTTON_X && x <= INFO_BUTTON_X + INFO_BUTTON_W &&
           y >= INFO_BUTTON_Y && y <= INFO_BUTTON_Y + INFO_BUTTON_H;
}

void showProjectInfoPopup() {
    if (!isScreenOn || inCustomColorMode || isCoffeePopupVisible || currentUIState != UI_STATE_MAIN) return; // 如果Coffee弹窗显示，则不显示此弹窗，或不在主界面

    isProjectInfoPopupVisible = true;
    currentUIState = UI_STATE_POPUP;

    // 弹窗区域和颜色 - 增大弹窗
    int popupX = 10; // 减小边距，使弹窗更大
    int popupY = 10; // 减小边距
    int popupW = SCREEN_WIDTH - 2 * popupX;
    int popupH = SCREEN_HEIGHT - 2 * popupY; // 占据更多高度
    uint16_t popupBgColor = tft.color565(40, 40, 80); // 深蓝紫色
    uint16_t popupBorderColor = TFT_LIGHTGREY;
    uint16_t textColor = TFT_WHITE;
    uint16_t qrPixelColor = TFT_WHITE; // 二维码像素颜色

    tft.fillRect(popupX, popupY, popupW, popupH, popupBgColor);
    tft.drawRect(popupX, popupY, popupW, popupH, popupBorderColor);

    tft.setTextColor(textColor, popupBgColor);
    tft.setTextDatum(TC_DATUM); // 顶部居中对齐
    tft.setTextSize(1); // 使用小号字体
    tft.drawString("Project-ESPNow Info", popupX + popupW / 2, popupY + 5, 2); // 标题使用2号字体

    tft.setTextDatum(TL_DATUM); // 左上角对齐
    int textX = popupX + 5;
    int textY = popupY + 25;
    int lineHeight = 10; // 减小行高以容纳更多内容

    tft.setCursor(textX, textY);
    tft.print("Github: github.com/");
    textY += lineHeight;
    tft.setCursor(textX + 10, textY); // 缩进
    tft.print("Kur1oR3iko/Project-ESPNow");

    textY += lineHeight * 1.5;
    tft.setCursor(textX, textY);
    tft.print("Maintainers:");
    textY += lineHeight;
    tft.setCursor(textX + 10, textY);
    tft.print("- Kur1oR3iko");
    textY += lineHeight;
    tft.setCursor(textX + 10, textY);
    tft.print("- xiao_hj909");
    textY += lineHeight;
    tft.setCursor(textX + 10, textY);
    tft.print("- Shapaper223");
    textY += lineHeight;
    tft.setCursor(textX + 10, textY);
    tft.print("  (shapaper@126.com)");

    textY += lineHeight * 1.5;
    tft.setCursor(textX, textY);
    tft.print("Kur1oR3iko's Bilibili Space:"); // 新增内容
    textY += lineHeight;

    // 二维码数据 (原作者空间)
    const char *qr_data_author =
        "EEEEEEE.E..EE.EEE.E.E.EEEEEEE\n"
        "E.....E..EE.....EE.E..E.....E\n"
        "E.EEE.E...E.E..EE.EE..E.EEE.E\n"
        "E.EEE.E.....E..E.E.EE.E.EEE.E\n"
        "E.EEE.E..E.E..E..EEE..E.EEE.E\n"
        "E.....E...E.E...E.E...E.....E\n"
        "EEEEEEE.E.E.E.E.E.E.E.EEEEEEE\n"
        "........E.EE.E.EE..E.........\n"
        "EE.EE.E..EEE.E.E.E....E.....E\n"
        "EEEE...E.E..EE.EEEEE...EE.EE.\n"
        "...EEEE...EE.E.E..........E..\n"
        ".EE....EEEEEEE..EE...EEEEE..E\n"
        "E..E..EE.E.EEE......EEEE....E\n"
        "EEE.E........EEE.EE...EEEEEEE\n"
        ".EE.E.EEEEEEEE.EE..E...EE.E.E\n"
        "EEEEEE.EE.EE....E..EE...E.E.E\n"
        "E.E.EEE.E.E...E.E.E.E..E.E...\n"
        "E.E..E.EEEEEEEEEE.EEE...E.EE.\n"
        "EE.E.EE...EE...E...E.EEEEE..E\n"
        "EEE..E..EE..E.E....EE.E.EEE..\n"
        "EEE.EEEEE.E.EEEE.EE.EEEEEEEE.\n"
        "........EE....EEE...E...EE...\n"
        "EEEEEEE...E.E.EEE.EEE.E.EE...\n"
        "E.....E..E....E.EE.EE...E...E\n"
        "E.EEE.E.E.EEEEEEEEEEE.E..E.E.\n"
        "E.EEE.E.EEE.E.EEEEE.EEEEE.E.E\n"
        "E.EEE.E.EEE.....E....EE.E.E.E\n"
        "E.....E.E.EE...EEEEEEE..E.E.E\n"
        "EEEEEEE.E..EEE.E.E.EEEEEE.E.E";

    int qrPixelSize = 2; // 每个二维码“像素”的大小
    int qrWidthInChars = 29; // 二维码的字符宽度
    // int qrHeightInChars = 29; // 二维码的字符高度
    int qrDisplayWidth = qrWidthInChars * qrPixelSize;
    // int qrDisplayHeight = qrHeightInChars * qrPixelSize; // 未使用

    // 将二维码绘制在文本下方，居中显示
    int qrOffsetX = popupX + (popupW - qrDisplayWidth) / 2;
    int qrOffsetY = textY + lineHeight / 2; // 在文本下方留出一些空间

    char lineBuffer[35];
    const char *p_author = qr_data_author;
    int currentY_qr = qrOffsetY;

    for (int row = 0; row < 29 && *p_author; ++row) { // 假设作者二维码也是29行
        int i = 0;
        while (*p_author && *p_author != '\n' && i < qrWidthInChars) {
            lineBuffer[i++] = *p_author++;
        }
        lineBuffer[i] = '\0';
        if (*p_author == '\n') p_author++;

        for (int col = 0; col < i; ++col) {
            if (lineBuffer[col] == 'E') {
                tft.fillRect(qrOffsetX + col * qrPixelSize, currentY_qr, qrPixelSize, qrPixelSize, qrPixelColor);
            }
        }
        currentY_qr += qrPixelSize;
        if (currentY_qr > popupY + popupH - 5 - qrPixelSize - lineHeight) break; // 避免覆盖关闭提示
    }

    textY = currentY_qr + lineHeight / 2; // 更新textY到二维码下方

    // Shapaper的贡献信息移到二维码下方
    tft.setCursor(textX, textY);
    tft.print("Shapaper did a lot of");
    textY += lineHeight;
    tft.setCursor(textX, textY);
    tft.print("refactoring.");

    tft.setTextDatum(BC_DATUM); // 底部居中
    tft.drawString("(Tap to close)", popupX + popupW / 2, popupY + popupH - 5, 1); // 提示关闭
    tft.setTextDatum(TL_DATUM); // 重置
}

void hideProjectInfoPopup() {
    if (isProjectInfoPopupVisible) {
        isProjectInfoPopupVisible = false;
        currentUIState = UI_STATE_MAIN;
        redrawMainScreen(); // 重绘整个屏幕以清除弹窗并恢复UI
    }
}

// --- 对端信息界面函数实现 ---

void drawPeerInfoScreen() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("Connected Peers", SCREEN_WIDTH / 2, 5, 2); // 标题
    tft.setTextDatum(TL_DATUM);

    // 绘制返回按钮
    tft.fillRect(BACK_BUTTON_X, BACK_BUTTON_Y, BACK_BUTTON_W, BACK_BUTTON_H, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("B", BACK_BUTTON_X + BACK_BUTTON_W / 2, BACK_BUTTON_Y + BACK_BUTTON_H / 2, 2);
    tft.setTextDatum(TL_DATUM);

    // 绘制本机信息区域
    int localInfoStartX = 5;
    int localInfoStartY = 30;
    int localInfoHeight = 4 * 10 + 5; // 4行文本 + 间距
    int localInfoWidth = SCREEN_WIDTH - 10;
    int lineHeight = 10;

    tft.drawRect(localInfoStartX, localInfoStartY, localInfoWidth, localInfoHeight, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextFont(1);

    uint8_t myMacAddr[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMacAddr);
    char myMacStr[18];
    sprintf(myMacStr, "%02X:%02X:%02X:%02X:%02X:%02X",
            myMacAddr[0], myMacAddr[1], myMacAddr[2], myMacAddr[3], myMacAddr[4], myMacAddr[5]);

    tft.setCursor(localInfoStartX + 5, localInfoStartY + 5);
    tft.print("Local MAC: ");
    tft.print(myMacStr);

    tft.setCursor(localInfoStartX + 5, localInfoStartY + 5 + lineHeight);
    tft.print("Raw Uptime: ");
    tft.print(millis() / 1000);
    tft.print("s");

    tft.setCursor(localInfoStartX + 5, localInfoStartY + 5 + 2 * lineHeight);
    tft.print("Effective Uptime: ");
    tft.print((millis() + relativeBootTimeOffset) / 1000);
    tft.print("s");

    tft.setCursor(localInfoStartX + 5, localInfoStartY + 5 + 3 * lineHeight);
    tft.print("ID: ");
    tft.print(localDeviceId);
    tft.print("  Mem: ");
    tft.print(ESP.getFreeHeap() / 1024);
    tft.print("/");
    tft.print(ESP.getHeapSize() / 1024);
    tft.print("KB");


    // 绘制对端列表表头
    int peerListStartX = 5;
    int peerListStartY = localInfoStartY + localInfoHeight + 10; // 在本机信息下方留出间距
    int colWidthMac = 90;
    int colWidthId = 50;
    int colWidthUptime = 50;
    int rowHeight = 15;

    tft.drawString("MAC", peerListStartX, peerListStartY, 1);
    tft.drawString("ID", peerListStartX + colWidthMac + 5, peerListStartY, 1);
    tft.drawString("Up(s)", peerListStartX + colWidthMac + colWidthId + 10, peerListStartY, 1);
    tft.drawString("Mem", peerListStartX + colWidthMac + colWidthId + colWidthUptime + 15, peerListStartY, 1);

    // 绘制分隔线
    tft.drawLine(peerListStartX, peerListStartY + rowHeight - 2, SCREEN_WIDTH - 5, peerListStartY + rowHeight - 2, TFT_DARKGREY);

    // SET ID 按钮
    tft.fillRect(10, SCREEN_HEIGHT - 28, 80, 20, TFT_DARKCYAN);
    tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("SET ID", 50, SCREEN_HEIGHT - 18, 1);
    tft.setTextDatum(TL_DATUM);

    // 初始绘制对端列表
    updatePeerInfoScreen();
}

void updatePeerInfoScreen() {
    if (currentUIState != UI_STATE_PEER_INFO) return;

    // 更新本机信息区域
    int localInfoStartX = 5;
    int localInfoStartY = 30;
    int localInfoHeight = 4 * 10 + 5; // 4行文本 + 间距
    int localInfoWidth = SCREEN_WIDTH - 10;
    int lineHeight = 10;

    // 清除旧的本机信息区域
    tft.fillRect(localInfoStartX + 1, localInfoStartY + 1, localInfoWidth - 2, localInfoHeight - 2, TFT_BLACK);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextFont(1);

    uint8_t myMacAddr[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMacAddr);
    char myMacStr[18];
    sprintf(myMacStr, "%02X:%02X:%02X:%02X:%02X:%02X",
            myMacAddr[0], myMacAddr[1], myMacAddr[2], myMacAddr[3], myMacAddr[4], myMacAddr[5]);

    tft.setCursor(localInfoStartX + 5, localInfoStartY + 5);
    tft.print("Local MAC: ");
    tft.print(myMacStr);

    tft.setCursor(localInfoStartX + 5, localInfoStartY + 5 + lineHeight);
    tft.print("Raw Uptime: ");
    tft.print(millis() / 1000);
    tft.print("s");

    tft.setCursor(localInfoStartX + 5, localInfoStartY + 5 + 2 * lineHeight);
    tft.print("Effective Uptime: ");
    tft.print((millis() + relativeBootTimeOffset) / 1000);
    tft.print("s");

    tft.setCursor(localInfoStartX + 5, localInfoStartY + 5 + 3 * lineHeight);
    tft.print("ID: ");
    tft.print(localDeviceId);
    tft.print(" Mem:");
    tft.print(ESP.getFreeHeap() / 1024);
    tft.print("/");
    tft.print(ESP.getHeapSize() / 1024);
    tft.print("KB");


    // 更新对端列表区域
    int peerListStartX = 5;
    int peerListStartY = localInfoStartY + localInfoHeight + 10 + 15; // 在表头下方开始绘制
    int colWidthMac = 90;
    int colWidthId = 50;
    int colWidthUptime = 50;
    int rowHeight = 15;

    // 清除旧的对端列表区域 (从表头下方到返回按钮上方，避开 SET ID)
    tft.fillRect(peerListStartX, peerListStartY, SCREEN_WIDTH - 10, BACK_BUTTON_Y - peerListStartY - 32, TFT_BLACK);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextFont(1);

    std::vector<PeerInfo_t> peerList = getPeerInfoList(); // 获取对端信息列表

    int currentY = peerListStartY;
    for (const auto& peer : peerList) {
        if (currentY + rowHeight > BACK_BUTTON_Y - 32) break; // 避免覆盖 SET ID / 返回

        tft.setCursor(peerListStartX, currentY);
        tft.print(peer.macAddress.substring(0, 14));

        tft.setCursor(peerListStartX + colWidthMac + 5, currentY);
        tft.print(peer.deviceId[0] ? peer.deviceId : "--");

        tft.setCursor(peerListStartX + colWidthMac + colWidthId + 10, currentY);
        tft.print(peer.effectiveUptime / 1000); // 显示有效运行时间 (秒)

        tft.setCursor(peerListStartX + colWidthMac + colWidthId + colWidthUptime + 15, currentY);
        char memBuffer[20];
        sprintf(memBuffer, "%u/%u", peer.usedMemory / 1024, peer.totalMemory / 1024); // usedMemory 实际上是可用内存
        tft.print(memBuffer);

        currentY += rowHeight;
    }
}

void showPeerInfoScreen() {
    if (!isScreenOn || inCustomColorMode) return;

    currentUIState = UI_STATE_PEER_INFO;
    isPeerInfoScreenVisible = true;
    drawPeerInfoScreen(); // 绘制界面骨架和初始数据
}

void hidePeerInfoScreen() {
    if (currentUIState != UI_STATE_PEER_INFO) return;

    currentUIState = UI_STATE_MAIN;
    isPeerInfoScreenVisible = false;
    redrawMainScreen(); // 返回主界面并重绘
}

bool isPeerInfoScreenBackButtonPressed(int x, int y) {
    if (currentUIState != UI_STATE_PEER_INFO) return false;
     return x >= BACK_BUTTON_X && x <= BACK_BUTTON_X + BACK_BUTTON_W &&
           y >= BACK_BUTTON_Y && y <= BACK_BUTTON_Y + BACK_BUTTON_H;
}

// --- 截屏功能实现 ---

void initScreenshotCounter() {
    // 懒加载：仅在确认有卡且尚未扫描时执行；不再全盘慢扫拖垮启动
    if (screenshotCounterInited)
        return;
    screenshotCounter = 1;
    screenshotCounterInited = true;
}

bool detectSdCardPresent()
{
    // 轻量探测：低速、单次 begin，失败快速返回；始终恢复触摸 SPI
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    initSDSPI();
    mySpi.setFrequency(SD_SPI_HZ);

    bool ok = false;
    // frequency + max_files=1，减少无卡时的重试开销
    if (SD.begin(SD_CS, mySpi, SD_SPI_HZ, "/sd", 1)) {
        uint8_t cardType = SD.cardType();
        ok = (cardType != CARD_NONE);
        SD.end();
    }

    // 关键触摸 SPI，避免总线停在 SD 引脚配置导致触摸/显示异常
    initTouchSPI();
    sdCardAvailable = ok;
    return ok;
}

void deferSdCardDetection()
{
    sdDetectPending = true;
    sdDetectStartMs = millis();
    sdCardAvailable = false;
}

void processDeferredSdDetect()
{
    if (!sdDetectPending)
        return;
    if (millis() - sdDetectStartMs < SD_DETECT_DELAY_MS)
        return;

    sdDetectPending = false;
    bool ok = detectSdCardPresent();
    if (ok) {
        initScreenshotCounter();
        Serial.println("SD 延迟探测：有卡，显示 S");
    } else {
        Serial.println("SD 延迟探测：无卡/失败，隐藏 S");
    }
    if (currentUIState == UI_STATE_MAIN && !inCustomColorMode) {
        drawScreenshotButton();
    }
}

bool isSdCardAvailable()
{
    return sdCardAvailable;
}

void drawScreenshotButton()
{
    if (!sdCardAvailable) {
        // 无 SD：清掉原按钮区域，不显示 S
        tft.fillRect(SCREENSHOT_BUTTON_X, SCREENSHOT_BUTTON_Y, SCREENSHOT_BUTTON_W, SCREENSHOT_BUTTON_H, TFT_BLACK);
        return;
    }
    tft.fillRect(SCREENSHOT_BUTTON_X, SCREENSHOT_BUTTON_Y, SCREENSHOT_BUTTON_W, SCREENSHOT_BUTTON_H, TFT_WHITE);
    tft.setTextColor(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("S", SCREENSHOT_BUTTON_X + SCREENSHOT_BUTTON_W / 2, SCREENSHOT_BUTTON_Y + SCREENSHOT_BUTTON_H / 2, 1);
    tft.setTextDatum(TL_DATUM);
}

bool isScreenshotButtonPressed(int x, int y) {
    if (!sdCardAvailable) return false;
    if (currentUIState != UI_STATE_MAIN) return false;
    return x >= SCREENSHOT_BUTTON_X && x <= SCREENSHOT_BUTTON_X + SCREENSHOT_BUTTON_W &&
           y >= SCREENSHOT_BUTTON_Y && y <= SCREENSHOT_BUTTON_Y + SCREENSHOT_BUTTON_H;
}

uint8_t getCurrentCanvasPage() { return currentCanvasPage; }
uint8_t getCanvasPageCount() { return canvasPageCount; }

bool canvasPageHasContent(uint8_t page)
{
    const size_t n = allDrawingHistory.size();
    for (size_t i = 0; i < n; i++) {
        const TouchData_t &d = allDrawingHistory[i];
        if (!d.isReset && d.page == page)
            return true;
    }
    return false;
}

static void broadcastCanvasPage(uint8_t action, uint8_t page, uint8_t count)
{
    SyncMessage_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_TYPE_CANVAS_PAGE;
    msg.senderUptime = millis();
    msg.senderOffset = relativeBootTimeOffset;
    msg.touch_data.x = page;
    msg.touch_data.y = count;
    msg.touch_data.color = action;
    msg.touch_data.page = page;
    strncpy(msg.senderId, localDeviceId, DEVICE_ID_MAX_LEN);
    sendSyncMessage(&msg);
}

static void rebuildHistoryDeletePage(uint8_t delPage)
{
    // 重建历史：去掉 delPage，其后页码前移
    std::vector<TouchData_t> kept;
    kept.reserve(allDrawingHistory.size());
    const size_t n = allDrawingHistory.size();
    for (size_t i = 0; i < n; i++) {
        TouchData_t d = allDrawingHistory[i];
        if (d.isReset) {
            kept.push_back(d);
            continue;
        }
        if (d.page == delPage)
            continue;
        if (d.page > delPage)
            d.page--;
        kept.push_back(d);
    }
    allDrawingHistory.clear();
    for (const auto &d : kept)
        allDrawingHistory.push_back(d);
}

static void rebuildHistoryClearPage(uint8_t page)
{
    // 仅去掉该页笔迹，页码与页数不变
    std::vector<TouchData_t> kept;
    kept.reserve(allDrawingHistory.size());
    const size_t n = allDrawingHistory.size();
    for (size_t i = 0; i < n; i++) {
        const TouchData_t &d = allDrawingHistory[i];
        if (d.isReset || d.page != page)
            kept.push_back(d);
    }
    allDrawingHistory.clear();
    for (const auto &d : kept)
        allDrawingHistory.push_back(d);
}

static void drawRssiBars(int x, int y, int8_t rssi)
{
    int level = 0;
    if (rssi == 0)
        level = 0;
    else if (rssi >= -55)
        level = 4;
    else if (rssi >= -65)
        level = 3;
    else if (rssi >= -75)
        level = 2;
    else if (rssi >= -85)
        level = 1;
    else
        level = 0;
    uint16_t onColor = TFT_GREEN;
    if (level == 0)
        onColor = TFT_RED;
    else if (level == 1)
        onColor = TFT_YELLOW;
    for (int i = 0; i < 4; i++) {
        int h = 3 + i * 3;
        uint16_t c = (i < level) ? onColor : TFT_DARKGREY;
        tft.fillRect(x + i * 5, y + 12 - h, 4, h, c);
    }
}

// level: -1=未连接, 0=很差/高延迟, 1..3=弧段
static int wifiLevelFromLatencyMs(unsigned long latMs, bool connected)
{
    if (!connected)
        return -1;
    if (latMs == 0)
        return 2; // 已连但尚无延迟样本
    if (latMs <= 60)
        return 3;
    if (latMs <= 150)
        return 2;
    if (latMs <= 350)
        return 1;
    return 0;
}

static unsigned long focusPeerLatencyMs(const PeerInfo_t *pinfo)
{
    if (!pinfo)
        return 0;
    unsigned long lat = pinfo->latencyMs;
    unsigned long now = millis();
    if (pinfo->lastSeenMs > 0 && now >= pinfo->lastSeenMs) {
        unsigned long age = now - pinfo->lastSeenMs;
        if (age > lat)
            lat = age;
    }
    if (lat > 9999)
        lat = 9999;
    return lat;
}

// 真 WiFi 扇形图标：未连接灰+叉；已连接按延迟点亮弧
static void drawWifiIconByLevel(int x, int y, int level)
{
    int cx = x + 11;
    int cy = y + 15;
    uint16_t onC = TFT_CYAN;
    uint16_t offC = TFT_DARKGREY;
    if (level < 0) {
        onC = TFT_DARKGREY;
        // 空心扇 + 红叉
        tft.drawCircle(cx, cy - 6, 3, offC);
        tft.drawCircle(cx, cy - 6, 6, offC);
        tft.drawCircle(cx, cy - 6, 9, offC);
        tft.fillCircle(cx, cy, 2, offC);
        tft.drawLine(x + 2, y + 2, x + 20, y + 16, TFT_RED);
        tft.drawLine(x + 3, y + 2, x + 21, y + 16, TFT_RED);
        return;
    }
    if (level == 0)
        onC = TFT_RED;
    else if (level == 1)
        onC = TFT_YELLOW;
    else
        onC = TFT_CYAN;

    tft.fillCircle(cx, cy, 2, onC);
    // 三层弧（用折线近似，比单线更“真”）
    auto arc = [&](int rad, bool on) {
        uint16_t c = on ? onC : offC;
        for (int a = -60; a <= 60; a += 10) {
            float r0 = (float)rad;
            float r1 = (float)rad;
            float a0 = (float)a * 0.0174533f;
            float a1 = (float)(a + 10) * 0.0174533f;
            int x0 = cx + (int)(r0 * sinf(a0));
            int y0 = cy - 2 - (int)(r0 * cosf(a0));
            int x1 = cx + (int)(r1 * sinf(a1));
            int y1 = cy - 2 - (int)(r1 * cosf(a1));
            tft.drawLine(x0, y0, x1, y1, c);
            tft.drawLine(x0, y0 + 1, x1, y1 + 1, c);
        }
    };
    arc(4, level >= 1);
    arc(7, level >= 2);
    arc(10, level >= 3);
}

// 左半 ESP-NOW 信号格 + 右半 WiFi（按延迟）；WiFi 未连接则右半打叉
static void drawHybridLinkIcon(int x, int y, int8_t espRssi, int wifiLevel)
{
    int level = 0;
    if (espRssi == 0)
        level = 0;
    else if (espRssi >= -55)
        level = 3;
    else if (espRssi >= -65)
        level = 3;
    else if (espRssi >= -75)
        level = 2;
    else if (espRssi >= -85)
        level = 1;
    uint16_t barC = TFT_GREEN;
    if (level <= 1)
        barC = TFT_RED;
    else if (level == 2)
        barC = TFT_YELLOW;
    for (int i = 0; i < 3; i++) {
        int h = 3 + i * 3;
        uint16_t c = (i < level) ? barC : TFT_DARKGREY;
        tft.fillRect(x + i * 4, y + 12 - h, 3, h, c);
    }
    tft.drawFastVLine(x + 13, y + 1, 14, TFT_DARKGREY);
    // 右半迷你 WiFi
    int cx = x + 22;
    int cy = y + 14;
    if (wifiLevel < 0) {
        tft.fillCircle(cx, cy, 1, TFT_DARKGREY);
        tft.drawLine(cx - 4, y + 6, cx + 4, y + 14, TFT_RED);
        return;
    }
    uint16_t onC = (wifiLevel == 0) ? TFT_RED : ((wifiLevel == 1) ? TFT_YELLOW : TFT_CYAN);
    tft.fillCircle(cx, cy, 1, onC);
    auto mini = [&](int rad, bool on) {
        uint16_t c = on ? onC : TFT_DARKGREY;
        tft.drawLine(cx - rad, cy - rad / 2, cx, cy - rad, c);
        tft.drawLine(cx, cy - rad, cx + rad, cy - rad / 2, c);
    };
    mini(3, wifiLevel >= 1);
    mini(5, wifiLevel >= 2);
    mini(7, wifiLevel >= 3);
}

static void drawLinkModeIcon(int x, int y, int8_t espRssi, int wifiLevel)
{
    uint8_t mode = getLinkMode();
    if (mode == LINK_MODE_WIFI_ONLY)
        drawWifiIconByLevel(x, y, wifiLevel);
    else if (mode == LINK_MODE_DUAL || mode == LINK_MODE_WIFI_ON)
        drawHybridLinkIcon(x, y, espRssi, wifiLevel);
    else
        drawRssiBars(x, y + 2, espRssi);
}

static const char *linkModeIconLabel(int wifiLevel)
{
    uint8_t mode = getLinkMode();
    if (mode == LINK_MODE_WIFI_ONLY) {
        if (wifiLevel < 0)
            return "off";
        return "WiFi";
    }
    if (mode == LINK_MODE_DUAL)
        return "E+W";
    if (mode == LINK_MODE_WIFI_ON)
        return "E/W";
    return "ESP";
}

void paintCurrentCanvasPage()
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode) {
        pendingCanvasRedrawAfterChat = true;
        return;
    }
    tft.fillScreen(TFT_BLACK);

    // 先按时间序展开：橡皮只清掉同笔主墨迹，再绘制可见点
    std::vector<TouchData_t> visible;
    visible.reserve(256);
    const size_t n = allDrawingHistory.size();
    for (size_t i = 0; i < n; i++) {
        const TouchData_t &d = allDrawingHistory[i];
        if (d.isReset)
            continue;
        if (d.page != currentCanvasPage)
            continue;
        if (d.color == TFT_BLACK) {
            int r = resolveEraserRadius(d.brushR);
            const long r2 = (long)r * (long)r;
            uint16_t oh = d.ownerHash;
            if (oh == 0)
                continue; // 无归属的旧橡皮忽略，避免误擦他人
            std::vector<TouchData_t> kept;
            kept.reserve(visible.size());
            for (const auto &p : visible) {
                if (p.ownerHash == oh && p.color != TFT_BLACK) {
                    long dx = (long)p.x - (long)d.x;
                    long dy = (long)p.y - (long)d.y;
                    if (dx * dx + dy * dy <= r2)
                        continue;
                }
                kept.push_back(p);
            }
            visible.swap(kept);
            continue;
        }
        visible.push_back(d);
    }

    TS_Point lastPt = {0, 0, 0};
    unsigned long lastTs = 0;
    uint32_t lastCol = 0;
    for (const auto &d : visible) {
        int mapX = d.x, mapY = d.y;
        int r = resolveBrushRadius(d.brushR);
        if (d.timestamp - lastTs > TOUCH_STROKE_INTERVAL || lastPt.z == 0 || lastCol == TFT_BLACK)
            applyBrushDot(mapX, mapY, d.color, r);
        else
            applyBrushSegment(lastPt.x, lastPt.y, mapX, mapY, d.color, r);
        lastPt = {mapX, mapY, 1};
        lastTs = d.timestamp;
        lastCol = d.color;
    }
    redrawUiChrome();
}

void showCanvasPage(uint8_t page, bool broadcastInfo)
{
    if (page >= canvasPageCount)
        page = canvasPageCount ? (uint8_t)(canvasPageCount - 1) : 0;
    currentCanvasPage = page;
    paintCurrentCanvasPage();
    char tip[24];
    snprintf(tip, sizeof(tip), "第%u/%u页", (unsigned)(currentCanvasPage + 1),
             (unsigned)canvasPageCount);
    showStatusToast(tip, 1200);
    if (broadcastInfo)
        broadcastCanvasPage(CANVAS_PAGE_ACT_INFO, currentCanvasPage, canvasPageCount);
}

void drawCanvasPageButtons()
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return;
    tft.setTextDatum(MC_DATUM);
    // 退出私聊（翻左侧，仅私聊激活时显示）
    if (isPrivateCanvasActive()) {
        tft.fillRect(CANVAS_EXIT_PRIV_X, CANVAS_PAGE_BTN_Y, CANVAS_PAGE_BTN_W, CANVAS_PAGE_BTN_H,
                     tft.color565(160, 50, 50));
        cnDrawUtf8(tft, CANVAS_EXIT_PRIV_X + (CANVAS_PAGE_BTN_W - cnTextWidth("退")) / 2,
                   CANVAS_PAGE_BTN_Y + 1, "退", TFT_WHITE);
    } else {
        tft.fillRect(CANVAS_EXIT_PRIV_X, CANVAS_PAGE_BTN_Y, CANVAS_PAGE_BTN_W, CANVAS_PAGE_BTN_H, TFT_BLACK);
    }
    // 屏幕翻转（C 左侧）
    tft.fillRect(CANVAS_FLIP_X, CANVAS_PAGE_BTN_Y, CANVAS_PAGE_BTN_W, CANVAS_PAGE_BTN_H,
                 screenRotation == SCREEN_ROT_FLIPPED ? tft.color565(40, 120, 90)
                                                      : tft.color565(50, 70, 100));
    cnDrawUtf8(tft, CANVAS_FLIP_X + (CANVAS_PAGE_BTN_W - cnTextWidth("翻")) / 2,
               CANVAS_PAGE_BTN_Y + 1, "翻", TFT_WHITE);
    // 清当前页（不删页、不清其它页）
    tft.fillRect(CANVAS_PAGE_CLEAR_X, CANVAS_PAGE_BTN_Y, CANVAS_PAGE_BTN_W, CANVAS_PAGE_BTN_H,
                 tft.color565(140, 40, 40));
    tft.setTextColor(TFT_WHITE);
    tft.drawString("C", CANVAS_PAGE_CLEAR_X + CANVAS_PAGE_BTN_W / 2,
                   CANVAS_PAGE_BTN_Y + CANVAS_PAGE_BTN_H / 2, 1);
    // 上一页
    tft.fillRect(CANVAS_PAGE_PREV_X, CANVAS_PAGE_BTN_Y, CANVAS_PAGE_BTN_W, CANVAS_PAGE_BTN_H,
                 currentCanvasPage > 0 ? tft.color565(40, 80, 140) : tft.color565(40, 40, 48));
    tft.drawString("<", CANVAS_PAGE_PREV_X + CANVAS_PAGE_BTN_W / 2,
                   CANVAS_PAGE_BTN_Y + CANVAS_PAGE_BTN_H / 2, 1);
    // 下一页 / 新建
    bool canNew = (canvasPageCount < CANVAS_MAX_PAGES);
    bool atLast = (currentCanvasPage + 1 >= canvasPageCount);
    tft.fillRect(CANVAS_PAGE_NEXT_X, CANVAS_PAGE_BTN_Y, CANVAS_PAGE_BTN_W, CANVAS_PAGE_BTN_H,
                 (atLast ? canNew : true) ? tft.color565(40, 80, 140) : tft.color565(40, 40, 48));
    tft.drawString(atLast ? "+" : ">", CANVAS_PAGE_NEXT_X + CANVAS_PAGE_BTN_W / 2,
                   CANVAS_PAGE_BTN_Y + CANVAS_PAGE_BTN_H / 2, 1);
    // 页码小标记
    char pg[8];
    snprintf(pg, sizeof(pg), "%u/%u", (unsigned)(currentCanvasPage + 1), (unsigned)canvasPageCount);
    tft.setTextFont(1);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(pg, CANVAS_PAGE_PREV_X + CANVAS_PAGE_BTN_W,
                   CANVAS_PAGE_BTN_Y - 8, 1);
    tft.setTextDatum(TL_DATUM);
}

bool isCanvasPagePrevPressed(int x, int y)
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return false;
    return x >= CANVAS_PAGE_PREV_X && x <= CANVAS_PAGE_PREV_X + CANVAS_PAGE_BTN_W &&
           y >= CANVAS_PAGE_BTN_Y && y <= CANVAS_PAGE_BTN_Y + CANVAS_PAGE_BTN_H;
}

bool isCanvasPageNextPressed(int x, int y)
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return false;
    return x >= CANVAS_PAGE_NEXT_X && x <= CANVAS_PAGE_NEXT_X + CANVAS_PAGE_BTN_W &&
           y >= CANVAS_PAGE_BTN_Y && y <= CANVAS_PAGE_BTN_Y + CANVAS_PAGE_BTN_H;
}

bool isCanvasPageClearPressed(int x, int y)
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return false;
    return x >= CANVAS_PAGE_CLEAR_X && x <= CANVAS_PAGE_CLEAR_X + CANVAS_PAGE_BTN_W &&
           y >= CANVAS_PAGE_BTN_Y && y <= CANVAS_PAGE_BTN_Y + CANVAS_PAGE_BTN_H;
}

bool isCanvasFlipPressed(int x, int y)
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return false;
    return x >= CANVAS_FLIP_X && x <= CANVAS_FLIP_X + CANVAS_PAGE_BTN_W &&
           y >= CANVAS_PAGE_BTN_Y && y <= CANVAS_PAGE_BTN_Y + CANVAS_PAGE_BTN_H;
}

bool isCanvasExitPrivPressed(int x, int y)
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return false;
    if (!isPrivateCanvasActive())
        return false;
    return x >= CANVAS_EXIT_PRIV_X && x <= CANVAS_EXIT_PRIV_X + CANVAS_PAGE_BTN_W &&
           y >= CANVAS_PAGE_BTN_Y && y <= CANVAS_PAGE_BTN_Y + CANVAS_PAGE_BTN_H;
}

void handleCanvasExitPriv()
{
    leavePrivateCanvas();
}

uint8_t getScreenRotation()
{
    return screenRotation;
}

static void applyDisplayRotation(uint8_t rot)
{
    if (rot != SCREEN_ROT_DEFAULT && rot != SCREEN_ROT_FLIPPED)
        rot = SCREEN_ROT_DEFAULT;
    screenRotation = rot;
    tft.setRotation(screenRotation);
    ts.setRotation(screenRotation);
}

void loadScreenRotation()
{
    uint8_t rot = SCREEN_ROT_DEFAULT;
    Preferences p;
    if (p.begin(DEVICE_ID_PREF_NAMESPACE, true)) {
        rot = p.getUChar(SCREEN_ROT_PREF_KEY, SCREEN_ROT_DEFAULT);
        p.end();
    }
    applyDisplayRotation(rot);
}

void handleCanvasFlip()
{
    uint8_t next = (screenRotation == SCREEN_ROT_DEFAULT) ? SCREEN_ROT_FLIPPED : SCREEN_ROT_DEFAULT;
    applyDisplayRotation(next);
    Preferences p;
    if (p.begin(DEVICE_ID_PREF_NAMESPACE, false)) {
        p.putUChar(SCREEN_ROT_PREF_KEY, screenRotation);
        p.end();
    }
    if (currentUIState == UI_STATE_MAIN && !inCustomColorMode)
        paintCurrentCanvasPage();
    else
        redrawMainScreen();
    showStatusToast(screenRotation == SCREEN_ROT_FLIPPED ? "已翻转" : "已正向", 1000);
}

void handleCanvasPagePrev()
{
    if (currentCanvasPage == 0)
        return;
    // 当前页无内容：删除该页，回到上一页
    if (!canvasPageHasContent(currentCanvasPage)) {
        uint8_t del = currentCanvasPage;
        rebuildHistoryDeletePage(del);
        if (canvasPageCount > 1)
            canvasPageCount--;
        if (currentCanvasPage > 0)
            currentCanvasPage--;
        if (currentCanvasPage >= canvasPageCount)
            currentCanvasPage = canvasPageCount ? (uint8_t)(canvasPageCount - 1) : 0;
        broadcastCanvasPage(CANVAS_PAGE_ACT_DELETE, del, canvasPageCount);
        paintCurrentCanvasPage();
        char tip[28];
        snprintf(tip, sizeof(tip), "删空页→%u/%u",
                 (unsigned)(currentCanvasPage + 1), (unsigned)canvasPageCount);
        showStatusToast(tip, 1500);
        return;
    }
    showCanvasPage(currentCanvasPage - 1, true);
}

void handleCanvasPageNext()
{
    if (currentCanvasPage + 1 < canvasPageCount) {
        showCanvasPage(currentCanvasPage + 1, true);
        return;
    }
    // 已在最后一页：新建
    if (canvasPageCount >= CANVAS_MAX_PAGES) {
        showStatusToast("页数已满", 1200);
        return;
    }
    canvasPageCount++;
    currentCanvasPage = (uint8_t)(canvasPageCount - 1);
    broadcastCanvasPage(CANVAS_PAGE_ACT_CREATE, currentCanvasPage, canvasPageCount);
    paintCurrentCanvasPage(); // 新页空白
    char tip[24];
    snprintf(tip, sizeof(tip), "新建第%u页",
             (unsigned)(currentCanvasPage + 1));
    showStatusToast(tip, 1500);
}

void handleCanvasPageClear()
{
    uint8_t page = currentCanvasPage;
    if (!canvasPageHasContent(page)) {
        showStatusToast("本页为空", 1000);
        return;
    }
    rebuildHistoryClearPage(page);
    clearCanvasRedoStack();
    noteLocalDestructiveCanvasEdit();
    // 多发几次清页，降低 ESP-NOW 丢包导致对端仍留旧笔迹的概率
    for (int i = 0; i < 3; i++) {
        broadcastCanvasPage(CANVAS_PAGE_ACT_CLEAR, page, canvasPageCount);
        delay(8);
    }
    // 再强制推送本机历史，确保对端（含未收到 CLEAR 的）与清页后状态一致
    forcePushDrawingHistoryToPeers();
    paintCurrentCanvasPage();
    char tip[24];
    snprintf(tip, sizeof(tip), "已清第%u页", (unsigned)(page + 1));
    showStatusToast(tip, 1200);
}

void applyRemoteCanvasPage(uint8_t action, uint8_t page, uint8_t pageCount, const char *senderId)
{
    if (action == CANVAS_PAGE_ACT_CREATE) {
        if (pageCount > canvasPageCount && pageCount <= CANVAS_MAX_PAGES)
            canvasPageCount = pageCount;
        // 不强制切到新页：各端可停在自己的页
        if (currentUIState == UI_STATE_MAIN)
            drawCanvasPageButtons();
        return;
    }
    if (action == CANVAS_PAGE_ACT_DELETE) {
        if (page < canvasPageCount) {
            rebuildHistoryDeletePage(page);
            if (canvasPageCount > 1)
                canvasPageCount--;
            if (pageCount >= 1 && pageCount <= CANVAS_MAX_PAGES)
                canvasPageCount = pageCount;
            if (currentCanvasPage > page)
                currentCanvasPage--;
            else if (currentCanvasPage == page) {
                if (currentCanvasPage > 0)
                    currentCanvasPage--;
            }
            if (currentCanvasPage >= canvasPageCount)
                currentCanvasPage = canvasPageCount ? (uint8_t)(canvasPageCount - 1) : 0;
            // 若正看着被删页或受影响，重绘
            paintCurrentCanvasPage();
        }
        return;
    }
    if (action == CANVAS_PAGE_ACT_CLEAR) {
        rebuildHistoryClearPage(page);
        clearCanvasRedoStack();
        noteLocalDestructiveCanvasEdit(); // 对端清页也算本地已对齐，防反向旧同步
        if (page == currentCanvasPage)
            paintCurrentCanvasPage();
        else if (currentUIState == UI_STATE_MAIN)
            drawCanvasPageButtons();
        return;
    }
    if (action == CANVAS_PAGE_ACT_UNDO) {
        applyRemoteCanvasUndo(page, senderId);
        if (currentUIState == UI_STATE_MAIN && !inCustomColorMode)
            drawUndoRedoButtons();
        return;
    }
    if (action == CANVAS_PAGE_ACT_INFO) {
        if (pageCount > canvasPageCount && pageCount <= CANVAS_MAX_PAGES)
            canvasPageCount = pageCount;
        if (currentUIState == UI_STATE_MAIN)
            drawCanvasPageButtons();
    }
}

void drawSignalStrengthInfo()
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return;
    if (isProjectInfoPopupVisible || isCoffeePopupVisible || isClearConfirmVisible || privInviteVisible)
        return;

    tft.fillRect(SIGNAL_INFO_X, SIGNAL_INFO_Y, SIGNAL_INFO_W, SIGNAL_INFO_H, TFT_BLACK);

    const bool wifiOn = linkModeWifiEnabled();
    const bool wifiUp = wifiOn && wifiIsConnected();
    int wifiLv = wifiLevelFromLatencyMs(0, wifiUp);

    auto drawEmptyLinkHint = [&]() {
        wifiLv = wifiLevelFromLatencyMs(wifiUp ? 100UL : 0UL, wifiUp);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.setTextDatum(TL_DATUM);
        tft.setTextFont(1);
        tft.setCursor(SIGNAL_INFO_X, SIGNAL_INFO_Y);
        tft.print(linkModeIconLabel(wifiLv));
        drawLinkModeIcon(SIGNAL_INFO_X + 1, SIGNAL_INFO_Y + 8, 0, wifiLv);
    };

    if (peerInfoMap.empty()) {
        drawEmptyLinkHint();
        return;
    }

    // 收集对本机可见的对端
    std::vector<String> peerMacs;
    peerMacs.reserve(peerInfoMap.size());
    for (auto const &kv : peerInfoMap) {
        if (peerVisibleForLocalMode(kv.second))
            peerMacs.push_back(kv.first);
    }
    if (peerMacs.empty()) {
        drawEmptyLinkHint();
        return;
    }

    // 定位当前关注的 MAC
    size_t cur = 0;
    bool found = false;
    for (size_t i = 0; i < peerMacs.size(); i++) {
        if (peerMacs[i] == signalFocusMac) {
            cur = i;
            found = true;
            break;
        }
    }
    if (!found) {
        signalFocusMac = peerMacs[0];
        cur = 0;
        lastSignalPeerRotateTime = millis();
    }

    unsigned long now = millis();
    bool rotated = false;
    if (peerMacs.size() > 1 && (now - lastSignalPeerRotateTime >= SIGNAL_PEER_ROTATE_MS)) {
        cur = (cur + 1) % peerMacs.size();
        signalFocusMac = peerMacs[cur];
        lastSignalPeerRotateTime = now;
        rotated = true;
    }
    signalPeerRotateIndex = cur;

    const auto it = peerInfoMap.find(signalFocusMac);
    if (it == peerInfoMap.end())
        return;
    const PeerInfo_t &pinfo = it->second;
    const char *id = (pinfo.deviceId[0]) ? pinfo.deviceId : "Peer";
    int8_t rssi = pinfo.rssi;
    unsigned long latMs = focusPeerLatencyMs(&pinfo);
    wifiLv = wifiLevelFromLatencyMs(latMs, wifiUp);

    // 轮换到另一台时顶部短暂提示
    if (rotated) {
        char tip[40];
        snprintf(tip, sizeof(tip), "查看 %s 信号", id);
        showStatusToast(tip, 1800);
    }

    // ID 颜色：WiFi 模式看延迟，ESP 模式看 RSSI
    uint16_t color = TFT_GREEN;
    if (getLinkMode() == LINK_MODE_WIFI_ONLY) {
        if (wifiLv < 0)
            color = TFT_DARKGREY;
        else if (wifiLv == 0)
            color = TFT_RED;
        else if (wifiLv == 1)
            color = TFT_YELLOW;
        else
            color = TFT_CYAN;
    } else {
        if (rssi < -80)
            color = TFT_RED;
        else if (rssi < -70)
            color = TFT_YELLOW;
        else if (rssi == 0)
            color = TFT_DARKGREY;
    }

    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(1);
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(SIGNAL_INFO_X, SIGNAL_INFO_Y);
    char idShort[7];
    if (isPrivateCanvasActive()) {
        strncpy(idShort, "PRIV", 6);
        idShort[6] = '\0';
        tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
    } else {
        strncpy(idShort, id, 6);
        idShort[6] = '\0';
    }
    tft.print(idShort);

    drawLinkModeIcon(SIGNAL_INFO_X + 1, SIGNAL_INFO_Y + 8, rssi, wifiLv);

    if (peerMacs.size() > 1 && !isPrivateCanvasActive()) {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.setCursor(SIGNAL_INFO_X, SIGNAL_INFO_Y + 26);
        tft.printf("%u/%u", (unsigned)(cur + 1), (unsigned)peerMacs.size());
    } else if (isPrivateCanvasActive()) {
        tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
        tft.setCursor(SIGNAL_INFO_X, SIGNAL_INFO_Y + 26);
        tft.print("tap");
    } else if (getLinkMode() == LINK_MODE_WIFI_ONLY && wifiUp && latMs > 0) {
        tft.setTextColor(color, TFT_BLACK);
        tft.setCursor(SIGNAL_INFO_X, SIGNAL_INFO_Y + 26);
        if (latMs >= 1000)
            tft.printf("%lus", (unsigned long)(latMs / 1000UL));
        else
            tft.printf("%lu", latMs);
    } else {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.setCursor(SIGNAL_INFO_X, SIGNAL_INFO_Y + 26);
        tft.print(linkModeIconLabel(wifiLv));
    }
}

void updateSignalStrengthDisplay()
{
    drawSignalStrengthInfo();
}

bool saveScreenshotToSD() {
    initSDSPI();
    
    if (!SD.begin(SD_CS, mySpi, SD_SPI_HZ, "/sd", 2)) {
        SD.end();
        initTouchSPI();
        sdCardAvailable = false;
        drawScreenshotButton();
        showScreenshotError("SD Card Not Found");
        return false;
    }
    
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        SD.end();
        initTouchSPI();
        sdCardAvailable = false;
        drawScreenshotButton();
        showScreenshotError("SD Card Not Found");
        return false;
    }
    
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    
    if (cardSize == 0) {
        SD.end();
        initTouchSPI();
        tft.fillRect(SCREENSHOT_BUTTON_X, SCREENSHOT_BUTTON_Y, SCREENSHOT_BUTTON_W, SCREENSHOT_BUTTON_H, TFT_RED);
        showScreenshotError("SD Card Format Error");
        return false;
    }
    
    while (true) {
        char filename[16];
        sprintf(filename, "/%04d.bmp", screenshotCounter);
        
        if (!SD.exists(filename)) {
            tft.fillRect(SCREENSHOT_BUTTON_X, SCREENSHOT_BUTTON_Y, SCREENSHOT_BUTTON_W, SCREENSHOT_BUTTON_H, TFT_YELLOW);
            tft.setTextColor(TFT_BLACK);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("S", SCREENSHOT_BUTTON_X + SCREENSHOT_BUTTON_W / 2, SCREENSHOT_BUTTON_Y + SCREENSHOT_BUTTON_H / 2, 1);
            tft.setTextDatum(TL_DATUM);
            
            File file = SD.open(filename, FILE_WRITE);
            if (!file) {
                SD.end();
                initTouchSPI();
                tft.fillRect(SCREENSHOT_BUTTON_X, SCREENSHOT_BUTTON_Y, SCREENSHOT_BUTTON_W, SCREENSHOT_BUTTON_H, TFT_RED);
                showScreenshotError("File Create Failed");
                return false;
            }
            
            const int BLOCK_HEIGHT = 60;
            const int BLOCK_COUNT = (SCREEN_HEIGHT + BLOCK_HEIGHT - 1) / BLOCK_HEIGHT;
            
            uint16_t* blockBuffer = (uint16_t*)malloc(SCREEN_WIDTH * BLOCK_HEIGHT * 2);
            if (!blockBuffer) {
                file.close();
                SD.end();
                initTouchSPI();
                tft.fillRect(SCREENSHOT_BUTTON_X, SCREENSHOT_BUTTON_Y, SCREENSHOT_BUTTON_W, SCREENSHOT_BUTTON_H, TFT_RED);
                showScreenshotError("Buffer Alloc Failed");
                return false;
            }
            
            uint8_t* bmpRowBuffer = (uint8_t*)malloc(SCREEN_WIDTH * 3);
            if (!bmpRowBuffer) {
                free(blockBuffer);
                file.close();
                SD.end();
                initTouchSPI();
                tft.fillRect(SCREENSHOT_BUTTON_X, SCREENSHOT_BUTTON_Y, SCREENSHOT_BUTTON_W, SCREENSHOT_BUTTON_H, TFT_RED);
                showScreenshotError("Buffer Alloc Failed");
                return false;
            }
            
            int rowSize = ((SCREEN_WIDTH * 24 + 31) / 32) * 4;
            int paddingSize = rowSize - SCREEN_WIDTH * 3;
            uint8_t padding[4] = {0, 0, 0, 0};
            
            uint32_t fileSize = 54 + (uint32_t)SCREEN_WIDTH * (uint32_t)SCREEN_HEIGHT * 3;
            
            uint8_t header[54] = {
                'B', 'M',
                (uint8_t)(fileSize & 0xFF), (uint8_t)((fileSize >> 8) & 0xFF), (uint8_t)((fileSize >> 16) & 0xFF), (uint8_t)((fileSize >> 24) & 0xFF),
                0, 0, 0, 0,
                54, 0, 0, 0,
                40, 0, 0, 0,
                (uint8_t)(SCREEN_WIDTH & 0xFF), (uint8_t)((SCREEN_WIDTH >> 8) & 0xFF), (uint8_t)((SCREEN_WIDTH >> 16) & 0xFF), (uint8_t)((SCREEN_WIDTH >> 24) & 0xFF),
                (uint8_t)(SCREEN_HEIGHT & 0xFF), (uint8_t)((SCREEN_HEIGHT >> 8) & 0xFF), (uint8_t)((SCREEN_HEIGHT >> 16) & 0xFF), (uint8_t)((SCREEN_HEIGHT >> 24) & 0xFF),
                1, 0,
                24, 0,
                0, 0, 0, 0,
                0, 0, 0, 0,
                0, 0, 0, 0,
                0, 0, 0, 0,
                0, 0, 0, 0,
                0, 0, 0, 0
            };
            
            if (file.write(header, 54) != 54) {
                free(blockBuffer);
                free(bmpRowBuffer);
                file.close();
                SD.end();
                initTouchSPI();
                tft.fillRect(SCREENSHOT_BUTTON_X, SCREENSHOT_BUTTON_Y, SCREENSHOT_BUTTON_W, SCREENSHOT_BUTTON_H, TFT_RED);
                showScreenshotError("Write Failed");
                return false;
            }
            
            for (int block = BLOCK_COUNT - 1; block >= 0; block--) {
                int startY = block * BLOCK_HEIGHT;
                int endY = min(startY + BLOCK_HEIGHT, SCREEN_HEIGHT);
                int blockHeight = endY - startY;
                
                tft.readRect(0, startY, SCREEN_WIDTH, blockHeight, blockBuffer);
                
                for (int row = blockHeight - 1; row >= 0; row--) {
                    int bufIdx = 0;
                    for (int col = 0; col < SCREEN_WIDTH; col++) {
                        uint16_t pixel = blockBuffer[row * SCREEN_WIDTH + col];
                        // 提取RGB分量
                        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
                        uint8_t g = ((pixel >> 5) & 0x3F) << 2;
                        uint8_t b = (pixel & 0x1F) << 3;
                        
                        // 调试输出：只输出前10个像素的值
                        if (block == 0 && row == blockHeight - 1 && col < 10) {
                            Serial.printf("Pixel[%d] raw=0x%04X R=%d G=%d B=%d\n", col, pixel, r, g, b);
                        }
                        
                        // GRB顺序：修正循环错位R→B, B→G, G→R
                        bmpRowBuffer[bufIdx++] = g;  // R→G
                        bmpRowBuffer[bufIdx++] = r;  // G→R
                        bmpRowBuffer[bufIdx++] = b;  // B
                    }
                    
                    if (file.write(bmpRowBuffer, SCREEN_WIDTH * 3) != SCREEN_WIDTH * 3) {
                        free(blockBuffer);
                        free(bmpRowBuffer);
                        file.close();
                        SD.end();
                        initTouchSPI();
                        tft.fillRect(SCREENSHOT_BUTTON_X, SCREENSHOT_BUTTON_Y, SCREENSHOT_BUTTON_W, SCREENSHOT_BUTTON_H, TFT_RED);
                        showScreenshotError("Write Failed");
                        return false;
                    }
                    
                    if (paddingSize > 0) {
                        file.write(padding, paddingSize);
                    }
                }
            }
            
            free(blockBuffer);
            free(bmpRowBuffer);
            file.close();
            SD.end();
            initTouchSPI();
            
            screenshotCounter++;
            
            tft.fillRect(SCREENSHOT_BUTTON_X, SCREENSHOT_BUTTON_Y, SCREENSHOT_BUTTON_W, SCREENSHOT_BUTTON_H, TFT_WHITE);
            tft.setTextColor(TFT_BLACK);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("S", SCREENSHOT_BUTTON_X + SCREENSHOT_BUTTON_W / 2, SCREENSHOT_BUTTON_Y + SCREENSHOT_BUTTON_H / 2, 1);
            tft.setTextDatum(TL_DATUM);
            
            return true;
        }
        
        screenshotCounter++;
    }
    
    SD.end();
    initTouchSPI();
    tft.fillRect(SCREENSHOT_BUTTON_X, SCREENSHOT_BUTTON_Y, SCREENSHOT_BUTTON_W, SCREENSHOT_BUTTON_H, TFT_RED);
    showScreenshotError("Unknown Error");
    return false;
}

void showScreenshotError(const char* errorMsg) {
    int popupX = 10;
    int popupY = 10;
    int popupW = SCREEN_WIDTH - 2 * popupX;
    int popupH = 60;
    uint16_t popupBgColor = tft.color565(50, 0, 0);
    uint16_t popupBorderColor = TFT_RED;
    uint16_t textColor = TFT_WHITE;
    
    tft.fillRect(popupX, popupY, popupW, popupH, popupBgColor);
    tft.drawRect(popupX, popupY, popupW, popupH, popupBorderColor);
    
    tft.setTextColor(textColor, popupBgColor);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(errorMsg, popupX + popupW / 2, popupY + popupH / 2, 2);
    tft.setTextDatum(TL_DATUM);
    
    delay(2000);
    
    redrawMainScreen();
}

// ========== 设备标识 / 状态提示 / 橡皮擦 UI 保护 ==========

static Preferences devicePrefs;

static bool circleHitsRect(int cx, int cy, int r, int rx, int ry, int rw, int rh)
{
    int closestX = constrain(cx, rx, rx + rw);
    int closestY = constrain(cy, ry, ry + rh);
    int dx = cx - closestX;
    int dy = cy - closestY;
    return (dx * dx + dy * dy) < (r * r);
}

bool eraserOverlapsUi(int cx, int cy, int r)
{
    // 左侧工具条（重置/颜色/橡皮/对端/进度）
    if (circleHitsRect(cx, cy, r, 0, 0, UI_LEFT_SAFE_X_MAX, UI_LEFT_SAFE_Y_MAX))
        return true;
    // 左侧信号强度区域
    if (circleHitsRect(cx, cy, r, SIGNAL_INFO_X - 1, SIGNAL_INFO_Y - 1, SIGNAL_INFO_W + 2, SIGNAL_INFO_H + 2))
        return true;
    // 左下 Coffee / Chat / Debug
    if (circleHitsRect(cx, cy, r, COFFEE_BUTTON_X - 2, COFFEE_BUTTON_Y - 2,
                       COFFEE_BUTTON_W + 4, (DEBUG_TOGGLE_BUTTON_Y + DEBUG_TOGGLE_BUTTON_H) - (COFFEE_BUTTON_Y - 2) + 4))
        return true;
    // 右上自定义颜色 * + 撤/重 + 笔粗
    if (circleHitsRect(cx, cy, r, UNDO_BUTTON_X - 2, CUSTOM_COLOR_BUTTON_Y - 2,
                       (CUSTOM_COLOR_BUTTON_X + CUSTOM_COLOR_BUTTON_W) - (UNDO_BUTTON_X - 2) + 2,
                       CUSTOM_COLOR_BUTTON_H + BRUSH_BUTTON_H + 8))
        return true;
    if (isBrushSliderVisible) {
        int sliderTop = BRUSH_SLIDER_Y - BRUSH_SLIDER_HEIGHT / 2 - 4;
        if (circleHitsRect(cx, cy, r, BRUSH_SLIDER_X - 4, sliderTop,
                           BRUSH_SLIDER_WIDTH + BRUSH_SLIDER_HANDLE_W + 8, BRUSH_SLIDER_HEIGHT + 20))
            return true;
    }
    // 右下角截屏
    if (sdCardAvailable &&
        circleHitsRect(cx, cy, r, SCREENSHOT_BUTTON_X - 2, SCREENSHOT_BUTTON_Y - 2,
                       SCREENSHOT_BUTTON_W + 4, SCREENSHOT_BUTTON_H + 4))
        return true;
    // 画布翻页 / 清页 / 翻转 / 退出私聊（退 翻 C < >）
    if (circleHitsRect(cx, cy, r, CANVAS_EXIT_PRIV_X - 2, CANVAS_PAGE_BTN_Y - 2,
                       CANVAS_PAGE_BTN_W * 5 + 12, CANVAS_PAGE_BTN_H + 4))
        return true;
    // 橡皮擦滑块 / +/-
    if (isEraserSliderVisible) {
        int sliderTop = ERASER_SLIDER_Y - ERASER_SLIDER_HEIGHT / 2 - ERASER_PM_BTN_H - 8;
        int panelW = ERASER_SLIDER_HANDLE_W + ERASER_PM_BTN_W + 24;
        int panelH = ERASER_SLIDER_HEIGHT + ERASER_PM_BTN_H * 2 + 32;
        if (circleHitsRect(cx, cy, r, ERASER_SLIDER_X - 6, sliderTop, panelW, panelH))
            return true;
    }
    // 顶部状态条
    if (circleHitsRect(cx, cy, r, STATUS_BAR_X, STATUS_BAR_Y, STATUS_BAR_W, STATUS_BAR_H + 2))
        return true;
    // 调试信息框
    if (isDebugInfoVisible) {
        if (circleHitsRect(cx, cy, r, 2, SCREEN_HEIGHT - 42, 120, 42))
            return true;
    }
    return false;
}

static int clampEraserRadius(int r)
{
    if (r < ERASER_MIN_RADIUS)
        r = ERASER_MIN_RADIUS;
    if (r > ERASER_MAX_RADIUS)
        r = ERASER_MAX_RADIUS;
    return r;
}

static int clampBrushRadius(int r)
{
    if (r < BRUSH_MIN_RADIUS)
        r = BRUSH_MIN_RADIUS;
    if (r > BRUSH_MAX_RADIUS)
        r = BRUSH_MAX_RADIUS;
    return r;
}

// 解析同步包中的橡皮半径（0=旧固件，回退到本机当前半径）
int resolveEraserRadius(uint8_t brushR)
{
    if (brushR == 0)
        return clampEraserRadius(eraserRadius);
    return clampEraserRadius((int)brushR);
}

int resolveBrushRadius(uint8_t brushR)
{
    if (brushR == 0)
        return 1; // 旧包：细笔
    return clampBrushRadius((int)brushR);
}

void applyBrushDot(int cx, int cy, uint32_t color, int r)
{
    r = clampBrushRadius(r);
    if (r <= 1)
        tft.drawPixel(cx, cy, color);
    else
        tft.fillCircle(cx, cy, r, color);
}

void applyBrushSegment(int x0, int y0, int x1, int y1, uint32_t color, int r)
{
    r = clampBrushRadius(r);
    if (r <= 1) {
        tft.drawLine(x0, y0, x1, y1, color);
        return;
    }
    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps <= 0) {
        applyBrushDot(x1, y1, color, r);
        return;
    }
    int stride = r / 2;
    if (stride < 1)
        stride = 1;
    for (int i = 0; i <= steps; i += stride) {
        int x = x0 + (int)((long)dx * i / steps);
        int y = y0 + (int)((long)dy * i / steps);
        tft.fillCircle(x, y, r, color);
    }
    tft.fillCircle(x1, y1, r, color);
}

// 始终擦画布；若碰到 UI 区域则返回 true，调用方应 redrawUiChrome
bool applyEraserDot(int cx, int cy, int r)
{
    r = clampEraserRadius(r);
    tft.fillCircle(cx, cy, r, TFT_BLACK);
    return eraserOverlapsUi(cx, cy, r);
}

// 沿轨迹连续擦，避免采样过稀留下笔迹
bool applyEraserSegment(int x0, int y0, int x1, int y1, int r)
{
    r = clampEraserRadius(r);
    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    bool hitUi = false;
    if (steps <= 0) {
        return applyEraserDot(x1, y1, r);
    }
    // 步长约半径一半，保证圆盘重叠覆盖
    int stride = r / 2;
    if (stride < 1)
        stride = 1;
    for (int i = 0; i <= steps; i += stride) {
        int x = x0 + (int)((long)dx * i / steps);
        int y = y0 + (int)((long)dy * i / steps);
        if (applyEraserDot(x, y, r))
            hitUi = true;
    }
    // 保证终点
    if (applyEraserDot(x1, y1, r))
        hitUi = true;
    return hitUi;
}

bool eraseOwnerInkNear(int cx, int cy, int r, uint8_t page, uint16_t ownerHash)
{
    if (ownerHash == 0)
        return false;
    r = clampEraserRadius(r);
    const long r2 = (long)r * (long)r;
    bool any = false;
    std::vector<TouchData_t> kept;
    const size_t n = allDrawingHistory.size();
    kept.reserve(n);
    for (size_t i = 0; i < n; i++) {
        const TouchData_t &d = allDrawingHistory[i];
        // 只删该笔主、非橡皮标记、当前页上的实心点
        if (!d.isReset && d.color != TFT_BLACK && d.page == page && d.ownerHash == ownerHash) {
            long dx = (long)d.x - (long)cx;
            long dy = (long)d.y - (long)cy;
            if (dx * dx + dy * dy <= r2) {
                any = true;
                continue;
            }
        }
        kept.push_back(d);
    }
    if (!any)
        return false;
    allDrawingHistory.clear();
    for (const auto &d : kept)
        allDrawingHistory.push_back(d);
    return true;
}

bool eraseOwnerInkSegment(int x0, int y0, int x1, int y1, int r, uint8_t page, uint16_t ownerHash)
{
    r = clampEraserRadius(r);
    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    bool any = false;
    if (steps <= 0)
        return eraseOwnerInkNear(x1, y1, r, page, ownerHash);
    int stride = r / 2;
    if (stride < 1)
        stride = 1;
    for (int i = 0; i <= steps; i += stride) {
        int x = x0 + (int)((long)dx * i / steps);
        int y = y0 + (int)((long)dy * i / steps);
        if (eraseOwnerInkNear(x, y, r, page, ownerHash))
            any = true;
    }
    if (eraseOwnerInkNear(x1, y1, r, page, ownerHash))
        any = true;
    return any;
}

static unsigned long lastOwnerErasePaintMs = 0;

void paintCanvasAfterOwnerErase()
{
    unsigned long now = millis();
    if (now - lastOwnerErasePaintMs < 45UL)
        return;
    lastOwnerErasePaintMs = now;
    if (currentUIState == UI_STATE_MAIN && !inCustomColorMode)
        paintCurrentCanvasPage();
    else
        pendingCanvasRedrawAfterChat = true;
}

void forcePaintCanvasAfterOwnerErase()
{
    lastOwnerErasePaintMs = 0;
    paintCanvasAfterOwnerErase();
}

bool safeEraserFill(int cx, int cy, int r)
{
    // 兼容旧调用：始终擦除并报告是否碰到 UI
    return !applyEraserDot(cx, cy, r);
}

void redrawUiChrome()
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return;
    drawResetButton();
    drawColorButtons();
    drawEraserButton();
    if (isEraserSliderVisible)
        drawEraserSlider();
    drawPeerInfoButton();
    drawStarButton();
    drawUndoRedoButtons();
    drawBrushButton();
    if (isBrushSliderVisible)
        drawBrushSlider();
    drawScreenshotButton();
    drawCanvasPageButtons();
    drawSignalStrengthInfo();
    drawChatJoinButton();
    drawSettingsButton();
    if (isDebugInfoVisible) {
        drawDebugInfo();
        drawInfoButton();
    }
    if (showSendProgress)
        drawSendProgressIndicator();
    if (showReceiveProgress)
        drawReceiveProgressIndicator();
    updateStatusOverlays();
}

void loadLocalDeviceId()
{
    if (!devicePrefs.begin(DEVICE_ID_PREF_NAMESPACE, true)) {
        strncpy(localDeviceId, "ESP", DEVICE_ID_MAX_LEN);
        localDeviceId[DEVICE_ID_MAX_LEN] = '\0';
        return;
    }
    String stored = devicePrefs.getString(DEVICE_ID_PREF_KEY, "");
    devicePrefs.end();
    if (stored.length() == 0) {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        snprintf(localDeviceId, sizeof(localDeviceId), "E%02X%02X", mac[4], mac[5]);
    } else {
        strncpy(localDeviceId, stored.c_str(), DEVICE_ID_MAX_LEN);
        localDeviceId[DEVICE_ID_MAX_LEN] = '\0';
    }
}

void saveLocalDeviceId(const char *id)
{
    if (!id || id[0] == '\0')
        return;
    // 硬拦截：附近已占用则拒绝写入
    if (isDeviceIdTakenByNearbyPeer(id))
        return;
    strncpy(localDeviceId, id, DEVICE_ID_MAX_LEN);
    localDeviceId[DEVICE_ID_MAX_LEN] = '\0';
    if (devicePrefs.begin(DEVICE_ID_PREF_NAMESPACE, false)) {
        devicePrefs.putString(DEVICE_ID_PREF_KEY, localDeviceId);
        devicePrefs.end();
    }
}

const char *getLocalDeviceId()
{
    return localDeviceId;
}

// 附近对端是否已占用该 ID（忽略大小写）
bool isDeviceIdTakenByNearbyPeer(const char *id)
{
    if (!id || !id[0])
        return false;
    for (auto const &kv : peerInfoMap) {
        const char *peerId = kv.second.deviceId;
        if (!peerId[0])
            continue;
        // 大小写不敏感比较
        const char *a = id;
        const char *b = peerId;
        bool same = true;
        while (*a && *b) {
            char ca = (*a >= 'a' && *a <= 'z') ? (*a - 'a' + 'A') : *a;
            char cb = (*b >= 'a' && *b <= 'z') ? (*b - 'a' + 'A') : *b;
            if (ca != cb) { same = false; break; }
            a++;
            b++;
        }
        if (same && !*a && !*b)
            return true;
    }
    return false;
}

void showStatusToast(const char *msg, unsigned long durationMs)
{
    if (!msg)
        return;
    strncpy(statusToastMsg, msg, sizeof(statusToastMsg) - 1);
    statusToastMsg[sizeof(statusToastMsg) - 1] = '\0';
    statusToastUntil = millis() + durationMs;
    if (isScreenOn && currentUIState == UI_STATE_MAIN && !inCustomColorMode)
        updateStatusOverlays();
}

void setActivityStatus(const char *deviceId, const char *action)
{
    const char *id = (deviceId && deviceId[0]) ? deviceId : "?";
    const char *act = (action && action[0]) ? action : "忙碌";
    snprintf(drawingStatusMsg, sizeof(drawingStatusMsg), "%s %s", id, act);
    drawingStatusUntil = millis() + DRAWING_STATUS_MS;
    if (isScreenOn && currentUIState == UI_STATE_MAIN && !inCustomColorMode)
        updateStatusOverlays();
}

void setDrawingStatus(const char *deviceId)
{
    setActivityStatus(deviceId, "在画");
}

void clearDrawingStatus()
{
    drawingStatusMsg[0] = '\0';
    drawingStatusUntil = 0;
}

void updateStatusOverlays()
{
    if (!isScreenOn || currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return;
    if (isProjectInfoPopupVisible || isCoffeePopupVisible)
        return;

    unsigned long now = millis();
    bool toastActive = (statusToastUntil > now && statusToastMsg[0]);
    bool drawActive = (drawingStatusUntil > now && drawingStatusMsg[0]);
    bool syncing = isReceivingDrawingData || isSendingDrawingData || iamRequestingAllData;

    // 多清 1px，抹掉曾因条高不足留下的黄/青底线
    tft.fillRect(STATUS_BAR_X, STATUS_BAR_Y, STATUS_BAR_W, STATUS_BAR_H + 1, TFT_BLACK);

    char line[64];
    uint16_t color = TFT_DARKGREY;
    if (toastActive) {
        strncpy(line, statusToastMsg, sizeof(line) - 1);
        line[sizeof(line) - 1] = 0;
        color = TFT_YELLOW;
    } else if (syncing) {
        if (isSendingDrawingData)
            strncpy(line, "同步发送中…", sizeof(line));
        else if (isReceivingDrawingData)
            strncpy(line, "同步接收中…", sizeof(line));
        else
            strncpy(line, "同步中…", sizeof(line));
        color = TFT_ORANGE;
    } else if (drawActive) {
        strncpy(line, drawingStatusMsg, sizeof(line) - 1);
        line[sizeof(line) - 1] = 0;
        color = TFT_CYAN;
    } else {
        size_t n = 0;
        for (auto const &kv : peerInfoMap) {
            if (peerVisibleForLocalMode(kv.second))
                n++;
        }
        snprintf(line, sizeof(line), "%s ·%u ·%u/%u ·b%d",
                 localDeviceId,
                 (unsigned)n,
                 (unsigned)(currentCanvasPage + 1),
                 (unsigned)canvasPageCount,
                 brushRadius);
        color = TFT_DARKGREY;
        statusToastMsg[0] = '\0';
        if (drawingStatusUntil && drawingStatusUntil <= now)
            drawingStatusMsg[0] = '\0';
    }

    int tw = cnTextWidth(line);
    int x = STATUS_BAR_X + (STATUS_BAR_W - (tw < STATUS_BAR_W ? tw : STATUS_BAR_W)) / 2;
    if (x < STATUS_BAR_X)
        x = STATUS_BAR_X;
    cnDrawUtf8Ellipsis(tft, x, STATUS_BAR_Y, line, color, STATUS_BAR_W, TFT_BLACK, false);
}

void peerJoinedNotify(const char *idOrMac)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%s 上线", (idOrMac && idOrMac[0]) ? idOrMac : "对端");
    showStatusToast(buf);
}

void peerLeftNotify(const char *idOrMac)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%s 掉线", (idOrMac && idOrMac[0]) ? idOrMac : "对端");
    showStatusToast(buf);
}

void historyRestoredNotify(const char *fromId)
{
    char buf[48];
    if (fromId && fromId[0])
        snprintf(buf, sizeof(buf), "已恢复自 %s", fromId);
    else
        snprintf(buf, sizeof(buf), "笔迹已恢复");
    showStatusToast(buf, 3500);
}

void showNameEditScreen()
{
    currentUIState = UI_STATE_NAME_EDIT;
    strncpy(nameEditBuffer, localDeviceId, DEVICE_ID_MAX_LEN);
    nameEditBuffer[DEVICE_ID_MAX_LEN] = '\0';
    drawNameEditScreen();
}

void hideNameEditScreen()
{
    if (currentUIState != UI_STATE_NAME_EDIT)
        return;
    currentUIState = UI_STATE_MAIN;
    nameEditFingerDown = false;
    redrawMainScreen();
}

void drawNameEditScreen()
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("Set Device ID", SCREEN_WIDTH / 2, 4, 2);
    tft.setTextDatum(TL_DATUM);

    bool dup = isDeviceIdTakenByNearbyPeer(nameEditBuffer);
    // 当前输入框
    tft.drawRect(40, 28, SCREEN_WIDTH - 80, 22, dup ? TFT_RED : TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(dup ? TFT_RED : TFT_CYAN, TFT_BLACK);
    tft.drawString(nameEditBuffer[0] ? nameEditBuffer : "_", SCREEN_WIDTH / 2, 39, 2);
    tft.setTextDatum(TL_DATUM);
    if (dup) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.setTextDatum(TC_DATUM);
        tft.drawString("ID used nearby", SCREEN_WIDTH / 2, 14, 1);
        tft.setTextDatum(TL_DATUM);
    }

    const char *rows[] = {
        "ABCDEFG",
        "HIJKLMN",
        "OPQRSTU",
        "VWXYZ_-",
        "0123456",
        "789 DEL",
        "  OK   "
    };
    const int rowCount = 7;
    const int keyW = 28;
    const int keyH = 18;
    const int startY = 56;

    for (int r = 0; r < rowCount; r++) {
        const char *row = rows[r];
        int len = strlen(row);
        // 特殊行：DEL / OK
        if (r == 5) {
            // 789 + DEL
            for (int i = 0; i < 3; i++) {
                int x = 20 + i * (keyW + 4);
                int y = startY + r * (keyH + 3);
                tft.fillRect(x, y, keyW, keyH, TFT_DARKGREY);
                tft.setTextDatum(MC_DATUM);
                tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
                char c[2] = {row[i], 0};
                tft.drawString(c, x + keyW / 2, y + keyH / 2, 1);
            }
            int x = 20 + 3 * (keyW + 4);
            int y = startY + r * (keyH + 3);
            tft.fillRect(x, y, keyW * 2 + 4, keyH, TFT_ORANGE);
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(TFT_BLACK, TFT_ORANGE);
            tft.drawString("DEL", x + (keyW * 2 + 4) / 2, y + keyH / 2, 1);
        } else if (r == 6) {
            int x = 80;
            int y = startY + r * (keyH + 3);
            uint16_t okBg = dup ? TFT_DARKGREY : TFT_GREEN;
            tft.fillRect(x, y, 160, keyH + 2, okBg);
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(dup ? TFT_RED : TFT_BLACK, okBg);
            tft.drawString(dup ? "ID TAKEN" : "OK / SAVE", x + 80, y + (keyH + 2) / 2, 2);
        } else {
            int totalW = len * (keyW + 2);
            int startX = (SCREEN_WIDTH - totalW) / 2;
            for (int i = 0; i < len; i++) {
                int x = startX + i * (keyW + 2);
                int y = startY + r * (keyH + 3);
                tft.fillRect(x, y, keyW, keyH, TFT_NAVY);
                tft.setTextDatum(MC_DATUM);
                tft.setTextColor(TFT_WHITE, TFT_NAVY);
                char c[2] = {row[i], 0};
                tft.drawString(c, x + keyW / 2, y + keyH / 2, 1);
            }
        }
    }
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Long-press peer btn also opens this", 10, SCREEN_HEIGHT - 12, 1);
}

bool handleNameEditTouch(int x, int y)
{
    if (currentUIState != UI_STATE_NAME_EDIT)
        return false;

    // 按住不松开会连发，仅落笔瞬间响应一次
    bool rising = !nameEditFingerDown;
    nameEditFingerDown = true;
    if (!rising)
        return true;

    const char *rows[] = {
        "ABCDEFG",
        "HIJKLMN",
        "OPQRSTU",
        "VWXYZ_-",
        "0123456",
        "789",
        ""
    };
    const int keyW = 28;
    const int keyH = 18;
    const int startY = 56;

    // OK
    if (y >= startY + 6 * (keyH + 3) && y <= startY + 6 * (keyH + 3) + keyH + 2 &&
        x >= 80 && x <= 240) {
        if (!nameEditBuffer[0]) {
            drawNameEditScreen();
            return true;
        }
        if (isDeviceIdTakenByNearbyPeer(nameEditBuffer)) {
            // 附近已有相同 ID：禁止保存
            drawNameEditScreen();
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.setTextDatum(TC_DATUM);
            tft.drawString("Cannot save: ID taken", SCREEN_WIDTH / 2, 50, 1);
            tft.setTextDatum(TL_DATUM);
            return true;
        }
        saveLocalDeviceId(nameEditBuffer);
        hideNameEditScreen();
        showStatusToast("标识已保存", 1800);
        return true;
    }

    // DEL
    if (y >= startY + 5 * (keyH + 3) && y <= startY + 5 * (keyH + 3) + keyH) {
        int delX = 20 + 3 * (keyW + 4);
        if (x >= delX && x <= delX + keyW * 2 + 4) {
            size_t n = strlen(nameEditBuffer);
            if (n > 0) {
                nameEditBuffer[n - 1] = '\0';
                drawNameEditScreen();
            }
            return true;
        }
        // 789
        for (int i = 0; i < 3; i++) {
            int kx = 20 + i * (keyW + 4);
            if (x >= kx && x <= kx + keyW) {
                if (strlen(nameEditBuffer) < DEVICE_ID_MAX_LEN) {
                    char c = rows[5][i];
                    size_t n = strlen(nameEditBuffer);
                    nameEditBuffer[n] = c;
                    nameEditBuffer[n + 1] = '\0';
                    drawNameEditScreen();
                }
                return true;
            }
        }
    }

    // 字母行 0-4
    for (int r = 0; r < 5; r++) {
        const char *row = rows[r];
        int len = strlen(row);
        int totalW = len * (keyW + 2);
        int startX = (SCREEN_WIDTH - totalW) / 2;
        int ky = startY + r * (keyH + 3);
        if (y < ky || y > ky + keyH)
            continue;
        for (int i = 0; i < len; i++) {
            int kx = startX + i * (keyW + 2);
            if (x >= kx && x <= kx + keyW) {
                if (strlen(nameEditBuffer) < DEVICE_ID_MAX_LEN) {
                    size_t n = strlen(nameEditBuffer);
                    nameEditBuffer[n] = row[i];
                    nameEditBuffer[n + 1] = '\0';
                    drawNameEditScreen();
                }
                return true;
            }
        }
    }
    return true; // 吞掉其它触摸，避免误画
}

// ========== 局域网群聊（中文/私聊/群组/颜色） ==========
#include <Preferences.h>

typedef struct ChatLine_s {
    char senderId[DEVICE_ID_MAX_LEN + 1];
    char targetId[DEVICE_ID_MAX_LEN + 1];
    char text[CHAT_TEXT_MAX + 1];
    uint16_t color;
    uint8_t mode;
    bool isSelf;
} ChatLine_t;

typedef struct ChatGroup_s {
    char id[DEVICE_ID_MAX_LEN + 1];
    char name[CHAT_GROUP_NAME_MAX + 1];
    char ownerId[DEVICE_ID_MAX_LEN + 1]; // 群主；仅群主可删/解散/邀请
} ChatGroup_t;

enum ChatTab_e { CHAT_TAB_PUBLIC = 0, CHAT_TAB_PRIVATE, CHAT_TAB_GROUPS, CHAT_TAB_COLOR, CHAT_TAB_ONLINE };

static ChatLine_t chatHistory[CHAT_HISTORY_MAX];
static size_t chatHistoryCount = 0;
static char chatInputBuffer[CHAT_TEXT_MAX + 1] = {0};
static char pinyinBuffer[32] = {0};
static bool imePinyinMode = true;
static bool engCapsLock = false; // 英文大写锁定

static ChatTab_e chatTab = CHAT_TAB_PUBLIC;
static char privatePeerId[DEVICE_ID_MAX_LEN + 1] = {0};
static char activeGroupId[DEVICE_ID_MAX_LEN + 1] = {0};
static ChatGroup_t chatGroups[CHAT_MAX_GROUPS];
static int chatGroupCount = 0;
static uint16_t chatTextColor = TFT_CYAN;
static Preferences chatPrefs;
static const char *candZh[48];
static int candCount = 0;
static bool candExpanded = false; // 候选匹配页展开
static int candRow1Fit = 0;       // 折叠时一行能放下几个
static int candPage = 0;          // 展开匹配页页码（从 0 起）

// 会话状态：群组/私聊先列表管理，进入后才开键盘
static bool privateChatOpen = false;
static bool groupChatOpen = false;
static int chatScrollOffset = 0;
static int chatDragStartY = -1;
static int chatScrollAtDrag = 0;
static int groupListScroll = 0;
static bool groupInvitePick = false;
static char inviteGroupId[DEVICE_ID_MAX_LEN + 1] = {0};
static bool chatFingerDown = false;
static bool chatPressConsumed = false;

static char chatBannerMsg[40] = {0};
static unsigned long chatBannerUntil = 0;

static const int CHAT_TAB_H = 18;
static const int CHAT_INPUT_H = 24;
static const int CHAT_CAND_H = 14;
static const int CHAT_CAND_EXPAND_ROWS = 4; // 展开后总行数
static const int CHAT_KEY_H = 15;
static const int CHAT_KEY_ROWS = 5; // 4字母行 + 1标点
static const int CAND_FUNC_X = 200;
static const int CAND_ARROW_W = 18;
static const int CAND_PAGE_BTN_W = 16;
static const int CAND_ARROW_X = CAND_FUNC_X - CAND_ARROW_W; // 182
static const int CAND_TEXT_W = CAND_ARROW_X - 4;           // 折叠行候选宽度

static int chatCandRows() { return candExpanded ? CHAT_CAND_EXPAND_ROWS : 1; }
static int chatCandPanelH() { return chatCandRows() * CHAT_CAND_H; }
static int chatInputY() { return SCREEN_HEIGHT - CHAT_INPUT_H - 2; }
static int chatKbdStartY() { return chatInputY() - 2 - CHAT_KEY_ROWS * (CHAT_KEY_H + 1); }
static int chatCandY() { return chatKbdStartY() - chatCandPanelH() - 1; }
static int chatCandFuncY() { return chatCandY() + (chatCandRows() - 1) * CHAT_CAND_H; }
static int chatMsgBottom() { return chatCandY() - 1; }

static int countCandFitWidth(int maxW, int fromIdx, int maxN)
{
    int x = 0, n = 0;
    for (int i = fromIdx; i < candCount && n < maxN; i++) {
        char lab[24];
        snprintf(lab, sizeof(lab), "%d.%s", i + 1, candZh[i]);
        int w = cnTextWidth(lab) + 6;
        if (n > 0 && x + w > maxW)
            break;
        if (w > maxW && n == 0) {
            n = 1;
            break;
        }
        x += w;
        n++;
    }
    return n;
}

static int candExpandLastTextW(bool withPager)
{
    int right = CAND_ARROW_X;
    if (withPager)
        right -= 2 * CAND_PAGE_BTN_W;
    return right - 4;
}

static int countExpandPageSize(int startIdx, bool withPager)
{
    if (startIdx >= candCount)
        return 0;
    int idx = startIdx;
    for (int r = 0; r < CHAT_CAND_EXPAND_ROWS && idx < candCount; r++) {
        bool last = (r == CHAT_CAND_EXPAND_ROWS - 1);
        int maxW = last ? candExpandLastTextW(withPager) : (SCREEN_WIDTH - 8);
        int fit = countCandFitWidth(maxW, idx, candCount - idx);
        if (fit < 1)
            fit = 1;
        idx += fit;
    }
    return idx - startIdx;
}

static int candTotalPages()
{
    if (candCount <= 0)
        return 1;
    int n0 = countExpandPageSize(0, false);
    if (n0 >= candCount)
        return 1;
    int pages = 0, idx = 0;
    while (idx < candCount && pages < 32) {
        int n = countExpandPageSize(idx, true);
        if (n < 1)
            n = 1;
        idx += n;
        pages++;
    }
    return pages > 0 ? pages : 1;
}

static int candPageStartIndex(int page)
{
    int pages = candTotalPages();
    if (page < 0)
        page = 0;
    if (page >= pages)
        page = pages - 1;
    bool multi = pages > 1;
    int idx = 0;
    for (int p = 0; p < page; p++) {
        int n = countExpandPageSize(idx, multi);
        if (n < 1)
            n = 1;
        idx += n;
    }
    return idx;
}

static bool candNeedsExpandArrow()
{
    if (!imePinyinMode || candCount <= 0)
        return false;
    if (candExpanded)
        return true;
    candRow1Fit = countCandFitWidth(CAND_TEXT_W, 0, candCount);
    return candRow1Fit < candCount;
}

static bool inConversationView()
{
    if (chatTab == CHAT_TAB_PUBLIC) return true;
    if (chatTab == CHAT_TAB_PRIVATE) return privateChatOpen && privatePeerId[0];
    if (chatTab == CHAT_TAB_GROUPS) return groupChatOpen && activeGroupId[0];
    return false;
}

static void showChatBanner(const char *msg, unsigned long ms = 2500)
{
    if (!msg) return;
    strncpy(chatBannerMsg, msg, sizeof(chatBannerMsg) - 1);
    chatBannerMsg[sizeof(chatBannerMsg) - 1] = 0;
    chatBannerUntil = millis() + ms;
    if (currentUIState == UI_STATE_CHAT) {
        tft.fillRect(40, CHAT_TAB_H + 2, SCREEN_WIDTH - 80, 16, tft.color565(40, 40, 20));
        tft.drawRect(40, CHAT_TAB_H + 2, SCREEN_WIDTH - 80, 16, TFT_YELLOW);
        cnDrawUtf8(tft, 48, CHAT_TAB_H + 4, chatBannerMsg, TFT_YELLOW);
    } else {
        showStatusToast(msg, ms);
    }
}

void showChatJoinToast(const char *msg)
{
    showChatBanner(msg, 2500);
}

void chatTouchReleased()
{
    chatDragStartY = -1;
    chatFingerDown = false;
    chatPressConsumed = false;
}

void nameEditTouchReleased()
{
    nameEditFingerDown = false;
}

static void paintChatBannerIfNeeded()
{
    if (!chatBannerMsg[0]) return;
    if (millis() > chatBannerUntil) {
        chatBannerMsg[0] = 0;
        return;
    }
    tft.fillRect(40, CHAT_TAB_H + 2, SCREEN_WIDTH - 80, 16, tft.color565(40, 40, 20));
    tft.drawRect(40, CHAT_TAB_H + 2, SCREEN_WIDTH - 80, 16, TFT_YELLOW);
    cnDrawUtf8(tft, 48, CHAT_TAB_H + 4, chatBannerMsg, TFT_YELLOW);
}

static void saveChatGroups(); // forward

static bool isGroupOwner(const ChatGroup_t &g)
{
    return g.ownerId[0] && strcmp(g.ownerId, localDeviceId) == 0;
}

static int findGroupIndexById(const char *gid)
{
    if (!gid || !gid[0])
        return -1;
    for (int i = 0; i < chatGroupCount; i++) {
        if (strcmp(chatGroups[i].id, gid) == 0)
            return i;
    }
    return -1;
}

static bool addChatGroupLocal(const char *gid, const char *gname, const char *owner)
{
    if (!gid || !gid[0] || chatGroupCount >= CHAT_MAX_GROUPS)
        return false;
    if (findGroupIndexById(gid) >= 0)
        return false;
    strncpy(chatGroups[chatGroupCount].id, gid, DEVICE_ID_MAX_LEN);
    chatGroups[chatGroupCount].id[DEVICE_ID_MAX_LEN] = 0;
    strncpy(chatGroups[chatGroupCount].name,
            (gname && gname[0]) ? gname : gid, CHAT_GROUP_NAME_MAX);
    chatGroups[chatGroupCount].name[CHAT_GROUP_NAME_MAX] = 0;
    strncpy(chatGroups[chatGroupCount].ownerId,
            (owner && owner[0]) ? owner : localDeviceId, DEVICE_ID_MAX_LEN);
    chatGroups[chatGroupCount].ownerId[DEVICE_ID_MAX_LEN] = 0;
    chatGroupCount++;
    saveChatGroups();
    return true;
}

static void removeChatGroupLocal(const char *gid)
{
    int idx = findGroupIndexById(gid);
    if (idx < 0)
        return;
    for (int j = idx; j < chatGroupCount - 1; j++)
        chatGroups[j] = chatGroups[j + 1];
    chatGroupCount--;
    if (strcmp(activeGroupId, gid) == 0) {
        activeGroupId[0] = 0;
        groupChatOpen = false;
    }
    if (strcmp(inviteGroupId, gid) == 0) {
        inviteGroupId[0] = 0;
        groupInvitePick = false;
    }
    saveChatGroups();
}

static void loadChatGroups()
{
    chatGroupCount = 0;
    if (!chatPrefs.begin("chatgrp", true))
        return;
    chatGroupCount = constrain((int)chatPrefs.getUChar("n", 0), 0, CHAT_MAX_GROUPS);
    for (int i = 0; i < chatGroupCount; i++) {
        char k1[8], k2[8], k3[8];
        snprintf(k1, sizeof(k1), "id%d", i);
        snprintf(k2, sizeof(k2), "nm%d", i);
        snprintf(k3, sizeof(k3), "ow%d", i);
        String id = chatPrefs.getString(k1, "");
        String nm = chatPrefs.getString(k2, "");
        String ow = chatPrefs.getString(k3, "");
        strncpy(chatGroups[i].id, id.c_str(), DEVICE_ID_MAX_LEN);
        chatGroups[i].id[DEVICE_ID_MAX_LEN] = 0;
        strncpy(chatGroups[i].name, nm.c_str(), CHAT_GROUP_NAME_MAX);
        chatGroups[i].name[CHAT_GROUP_NAME_MAX] = 0;
        strncpy(chatGroups[i].ownerId, ow.c_str(), DEVICE_ID_MAX_LEN);
        chatGroups[i].ownerId[DEVICE_ID_MAX_LEN] = 0;
        // 旧数据无群主：本机视为群主（仅本机已存的群）
        if (!chatGroups[i].ownerId[0]) {
            strncpy(chatGroups[i].ownerId, localDeviceId, DEVICE_ID_MAX_LEN);
            chatGroups[i].ownerId[DEVICE_ID_MAX_LEN] = 0;
        }
    }
    chatPrefs.end();
}

static void saveChatGroups()
{
    if (!chatPrefs.begin("chatgrp", false))
        return;
    chatPrefs.putUChar("n", (uint8_t)chatGroupCount);
    for (int i = 0; i < chatGroupCount; i++) {
        char k1[8], k2[8], k3[8];
        snprintf(k1, sizeof(k1), "id%d", i);
        snprintf(k2, sizeof(k2), "nm%d", i);
        snprintf(k3, sizeof(k3), "ow%d", i);
        chatPrefs.putString(k1, chatGroups[i].id);
        chatPrefs.putString(k2, chatGroups[i].name);
        chatPrefs.putString(k3, chatGroups[i].ownerId);
    }
    chatPrefs.end();
}

static void loadChatHistory()
{
    Preferences hp;
    if (!hp.begin("chathist", true))
        return;
    uint16_t n = hp.getUShort("n", 0);
    if (n > CHAT_HISTORY_MAX)
        n = CHAT_HISTORY_MAX;
    size_t need = (size_t)n * sizeof(ChatLine_t);
    size_t have = hp.getBytesLength("d");
    if (n > 0 && have == need) {
        hp.getBytes("d", chatHistory, need);
        chatHistoryCount = n;
    } else {
        chatHistoryCount = 0;
    }
    hp.end();
}

static void saveChatHistory()
{
    Preferences hp;
    if (!hp.begin("chathist", false))
        return;
    hp.putUShort("n", (uint16_t)chatHistoryCount);
    if (chatHistoryCount > 0)
        hp.putBytes("d", chatHistory, chatHistoryCount * sizeof(ChatLine_t));
    else
        hp.remove("d");
    hp.end();
}

static void clearChatHistoryPersistent()
{
    chatHistoryCount = 0;
    chatScrollOffset = 0;
    Preferences hp;
    if (hp.begin("chathist", false)) {
        hp.clear();
        hp.end();
    }
}


static void refreshCandidates()
{
    candCount = 0;
    if (imePinyinMode && pinyinBuffer[0])
        cnFillCandidates(pinyinBuffer, candZh, 48, candCount);
    candRow1Fit = countCandFitWidth(CAND_TEXT_W, 0, candCount);
    if (candCount == 0 || candRow1Fit >= candCount)
        candExpanded = false;
    int pages = candTotalPages();
    if (candPage >= pages)
        candPage = pages - 1;
    if (candPage < 0)
        candPage = 0;
    if (!candExpanded)
        candPage = 0;
}

static bool chatLineVisible(const ChatLine_t &line)
{
    if (chatTab == CHAT_TAB_PUBLIC)
        return line.mode == CHAT_MODE_PUBLIC;
    if (chatTab == CHAT_TAB_PRIVATE) {
        if (line.mode != CHAT_MODE_PRIVATE || !privatePeerId[0])
            return false;
        bool withPeer =
            (strcmp(line.senderId, privatePeerId) == 0 && strcmp(line.targetId, localDeviceId) == 0) ||
            (strcmp(line.senderId, localDeviceId) == 0 && strcmp(line.targetId, privatePeerId) == 0);
        return withPeer;
    }
    if (chatTab == CHAT_TAB_GROUPS) {
        if (line.mode != CHAT_MODE_GROUP || !activeGroupId[0])
            return false;
        return strcmp(line.targetId, activeGroupId) == 0;
    }
    return false;
}

static void redrawChatMessagesSoft();
static void redrawChatTypingUI();

void drawChatJoinButton()
{
    if (!isScreenOn || inCustomColorMode || currentUIState != UI_STATE_MAIN)
        return;
    tft.fillRect(CHAT_BUTTON_X, CHAT_BUTTON_Y, CHAT_BUTTON_W, CHAT_BUTTON_H, TFT_MAGENTA);
    cnDrawUtf8(tft, CHAT_BUTTON_X + 2, CHAT_BUTTON_Y + 2, "聊", TFT_WHITE);
}

bool isChatJoinButtonPressed(int x, int y)
{
    if (currentUIState != UI_STATE_MAIN)
        return false;
    return x >= CHAT_BUTTON_X && x <= CHAT_BUTTON_X + CHAT_BUTTON_W &&
           y >= CHAT_BUTTON_Y && y <= CHAT_BUTTON_Y + CHAT_BUTTON_H;
}

bool chatHasGroup(const char *gid)
{
    return findGroupIndexById(gid) >= 0;
}

void appendChatMessage(const char *senderId, const char *targetId, const char *text,
                       bool isSelf, uint8_t mode, uint16_t color)
{
    if (!text)
        text = "";
    // 系统协议不进可视聊天流
    bool sysMsg = (mode == CHAT_MODE_GROUP &&
                   (strncmp(text, "CREATE:", 7) == 0 ||
                    strncmp(text, "DISBAND:", 8) == 0 ||
                    strncmp(text, "INVITE:", 7) == 0));

    if (!sysMsg) {
        if (chatHistoryCount >= CHAT_HISTORY_MAX) {
            for (size_t i = 1; i < CHAT_HISTORY_MAX; i++)
                chatHistory[i - 1] = chatHistory[i];
            chatHistoryCount = CHAT_HISTORY_MAX - 1;
        }
        ChatLine_t &line = chatHistory[chatHistoryCount++];
        strncpy(line.senderId, senderId ? senderId : "?", DEVICE_ID_MAX_LEN);
        line.senderId[DEVICE_ID_MAX_LEN] = 0;
        strncpy(line.targetId, targetId ? targetId : "", DEVICE_ID_MAX_LEN);
        line.targetId[DEVICE_ID_MAX_LEN] = 0;
        strncpy(line.text, text, CHAT_TEXT_MAX);
        line.text[CHAT_TEXT_MAX] = 0;
        line.isSelf = isSelf;
        line.mode = mode;
        line.color = color ? color : TFT_WHITE;
        chatScrollOffset = 0;
        saveChatHistory();
    }

    // CREATE：仅本地建群时已写入；远端 CREATE 忽略（未邀请看不到群）
    if (mode == CHAT_MODE_GROUP && strncmp(text, "CREATE:", 7) == 0) {
        if (isSelf) {
            // 已在创建按钮侧写入
        }
        return;
    }

    // INVITE:gid:gname:ownerId —— 仅被邀请方（targetId=本机）加入
    if (mode == CHAT_MODE_GROUP && strncmp(text, "INVITE:", 7) == 0) {
        if (!isSelf && targetId && strcmp(targetId, localDeviceId) == 0) {
            const char *p = text + 7;
            char gid[9] = {0}, gname[13] = {0}, owner[9] = {0};
            const char *c1 = strchr(p, ':');
            if (c1) {
                size_t n = (size_t)(c1 - p);
                if (n > 8) n = 8;
                memcpy(gid, p, n);
                const char *c2 = strchr(c1 + 1, ':');
                if (c2) {
                    size_t n2 = (size_t)(c2 - (c1 + 1));
                    if (n2 > CHAT_GROUP_NAME_MAX) n2 = CHAT_GROUP_NAME_MAX;
                    memcpy(gname, c1 + 1, n2);
                    strncpy(owner, c2 + 1, DEVICE_ID_MAX_LEN);
                } else {
                    strncpy(gname, c1 + 1, CHAT_GROUP_NAME_MAX);
                    strncpy(owner, senderId ? senderId : "", DEVICE_ID_MAX_LEN);
                }
                if (addChatGroupLocal(gid, gname, owner[0] ? owner : senderId)) {
                    showChatBanner("已加入", 2200);
                    if (currentUIState == UI_STATE_CHAT)
                        drawChatRoom();
                }
            }
        }
        return;
    }

    if (mode == CHAT_MODE_GROUP && strncmp(text, "DISBAND:", 8) == 0) {
        const char *gid = text + 8;
        removeChatGroupLocal(gid);
        if (currentUIState == UI_STATE_CHAT)
            drawChatRoom();
        return;
    }

    if (currentUIState == UI_STATE_CHAT && !sysMsg) {
        // 仅会话打字页刷新消息区；颜色/在线/列表页禁止叠画键盘，否则界面花屏
        if (inConversationView())
            redrawChatMessagesSoft();
        // 其它聊天子页只入库，切回大厅/会话时自然可见
    }
}

static void drawChatTabs()
{
    const char *tabs[5] = {"大厅", "私聊", "群组", "颜色", "在线"};
    int tw = SCREEN_WIDTH / 5;
    for (int i = 0; i < 5; i++) {
        uint16_t bg = (chatTab == i) ? tft.color565(20, 60, 120) : tft.color565(40, 40, 48);
        tft.fillRect(i * tw, 0, tw - 1, CHAT_TAB_H, bg);
        cnDrawUtf8(tft, i * tw + 4, 3, tabs[i], TFT_WHITE);
    }
}

static void collectVisible(int *vis, int &vc)
{
    vc = 0;
    for (size_t i = 0; i < chatHistoryCount; i++) {
        if (chatLineVisible(chatHistory[i]))
            vis[vc++] = (int)i;
    }
}

static void drawChatMessagesArea()
{
    const bool hasHeader = (chatTab == CHAT_TAB_PRIVATE && privateChatOpen) ||
                           (chatTab == CHAT_TAB_GROUPS && groupChatOpen);
    const int areaX = 2;
    const int areaY = CHAT_TAB_H + 1 + (hasHeader ? 14 : 0);
    const int areaW = SCREEN_WIDTH - 4;
    const int areaH = chatMsgBottom() - areaY;
    if (areaH < 20) return;
    tft.fillRect(areaX, areaY, areaW, areaH, tft.color565(18, 20, 32));
    // 不用描边方框，避免界面看起来“满屏框”

    int vis[CHAT_HISTORY_MAX];
    int vc = 0;
    collectVisible(vis, vc);

    const int lineH = 14;
    const int maxLines = areaH / lineH;
    int maxScroll = (vc > maxLines) ? (vc - maxLines) : 0;
    if (chatScrollOffset > maxScroll) chatScrollOffset = maxScroll;
    if (chatScrollOffset < 0) chatScrollOffset = 0;

    int start = 0;
    if (vc > maxLines)
        start = vc - maxLines - chatScrollOffset;
    if (start < 0) start = 0;
    int end = start + maxLines;
    if (end > vc) end = vc;

    int y = areaY + 2;
    for (int vi = start; vi < end; vi++) {
        const ChatLine_t &line = chatHistory[vis[vi]];
        char head[20];
        snprintf(head, sizeof(head), "%s:", line.senderId);
        uint16_t hc = line.isSelf ? TFT_GREENYELLOW : TFT_YELLOW;
        int hx = cnDrawUtf8(tft, areaX + 4, y, head, hc);
        cnDrawUtf8(tft, areaX + 4 + hx + 2, y, line.text, line.color);
        y += lineH;
    }

    if (maxScroll > 0) {
        tft.setTextColor(TFT_DARKGREY, tft.color565(18, 20, 32));
        tft.setTextDatum(TR_DATUM);
        tft.drawNumber(chatScrollOffset, areaX + areaW - 4, areaY + 2, 1);
        tft.setTextDatum(TL_DATUM);
    }
}

static void drawChatInputBar()
{
    const int iy = chatInputY();
    const int barW = SCREEN_WIDTH - 100;
    const int barX = (SCREEN_WIDTH - barW - 56) / 2; // 整体水平居中
    tft.fillRect(0, iy - 1, SCREEN_WIDTH, CHAT_INPUT_H + 3, tft.color565(24, 24, 36));
    tft.fillRoundRect(barX, iy, barW, CHAT_INPUT_H, 4, TFT_BLACK);
    tft.drawRoundRect(barX, iy, barW, CHAT_INPUT_H, 4, TFT_LIGHTGREY);
    if (imePinyinMode && pinyinBuffer[0]) {
        char show[80];
        snprintf(show, sizeof(show), "%s|%s", chatInputBuffer, pinyinBuffer);
        cnDrawUtf8(tft, barX + 4, iy + 5, show, chatTextColor);
    } else {
        cnDrawUtf8(tft, barX + 4, iy + 5, chatInputBuffer[0] ? chatInputBuffer : "输入消息...",
                   chatInputBuffer[0] ? chatTextColor : TFT_DARKGREY);
    }
    tft.fillRoundRect(barX + barW + 4, iy, 52, CHAT_INPUT_H, 4, TFT_GREEN);
    cnDrawUtf8(tft, barX + barW + 14, iy + 5, "发送", TFT_BLACK);
}

static void drawCandArrow(int ax, int ay, bool up)
{
    int cx = ax + CAND_ARROW_W / 2;
    int cy = ay + CHAT_CAND_H / 2;
    tft.fillRect(ax, ay, CAND_ARROW_W, CHAT_CAND_H, tft.color565(50, 70, 100));
    if (up) {
        tft.fillTriangle(cx, cy - 4, cx - 5, cy + 3, cx + 5, cy + 3, TFT_YELLOW);
    } else {
        tft.fillTriangle(cx, cy + 4, cx - 5, cy - 3, cx + 5, cy - 3, TFT_YELLOW);
    }
}

static void drawCandPageBtn(int ax, int ay, bool next, bool enabled)
{
    tft.fillRect(ax, ay, CAND_PAGE_BTN_W, CHAT_CAND_H,
                 enabled ? tft.color565(60, 90, 130) : tft.color565(40, 40, 50));
    int cx = ax + CAND_PAGE_BTN_W / 2;
    int cy = ay + CHAT_CAND_H / 2;
    uint16_t col = enabled ? TFT_YELLOW : TFT_DARKGREY;
    if (next)
        tft.fillTriangle(cx + 4, cy, cx - 3, cy - 5, cx - 3, cy + 5, col);
    else
        tft.fillTriangle(cx - 4, cy, cx + 3, cy - 5, cx + 3, cy + 5, col);
}

static void drawChatCandidates()
{
    const int cy0 = chatCandY();
    const int rows = chatCandRows();
    tft.fillRect(0, cy0, SCREEN_WIDTH, chatCandPanelH(), tft.color565(28, 28, 40));

    if (!imePinyinMode) {
        cnDrawUtf8(tft, 6, cy0 + 1, "[英文]", TFT_DARKGREY);
        return;
    }
    if (candCount == 0) {
        if (pinyinBuffer[0])
            cnDrawUtf8(tft, 4, cy0 + 1, pinyinBuffer, TFT_ORANGE);
        return;
    }

    bool showArrow = candNeedsExpandArrow();
    int textW = showArrow ? CAND_TEXT_W : (CAND_FUNC_X - 4);

    if (!candExpanded) {
        int fit = countCandFitWidth(textW, 0, candCount);
        candRow1Fit = fit;
        int x = 4;
        for (int i = 0; i < fit; i++) {
            char lab[24];
            snprintf(lab, sizeof(lab), "%d.%s", i + 1, candZh[i]);
            x += cnDrawUtf8(tft, x, cy0 + 1, lab, TFT_CYAN) + 6;
        }
        if (showArrow)
            drawCandArrow(CAND_ARROW_X, cy0, false);
        return;
    }

    // 展开：按页显示；超过一页时底行显示 ◀ ▶ ▲
    int pages = candTotalPages();
    if (candPage >= pages)
        candPage = pages - 1;
    if (candPage < 0)
        candPage = 0;
    bool multi = pages > 1;
    int idx = candPageStartIndex(candPage);
    int pageEnd = idx + countExpandPageSize(idx, multi);
    if (pageEnd > candCount)
        pageEnd = candCount;

    for (int r = 0; r < rows && idx < pageEnd; r++) {
        int y = cy0 + r * CHAT_CAND_H;
        bool last = (r == rows - 1);
        int maxW = last ? candExpandLastTextW(multi) : (SCREEN_WIDTH - 8);
        int fit = countCandFitWidth(maxW, idx, pageEnd - idx);
        if (fit <= 0)
            fit = 1;
        int x = 4;
        for (int k = 0; k < fit && idx < pageEnd; k++, idx++) {
            char lab[24];
            snprintf(lab, sizeof(lab), "%d.%s", idx + 1, candZh[idx]);
            x += cnDrawUtf8(tft, x, y + 1, lab, TFT_CYAN) + 6;
        }
        if (last) {
            if (multi) {
                int prevX = CAND_ARROW_X - 2 * CAND_PAGE_BTN_W;
                int nextX = CAND_ARROW_X - CAND_PAGE_BTN_W;
                drawCandPageBtn(prevX, y, false, candPage > 0);
                drawCandPageBtn(nextX, y, true, candPage < pages - 1);
            }
            drawCandArrow(CAND_ARROW_X, y, true);
        }
    }
}

static void drawChatKeyboard()
{
    // 英文字母按 caps 显示大小写；拼音模式固定显示大写键面、输入仍小写
    const bool showUpper = !imePinyinMode && engCapsLock;
    const char *rowsUpper[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    const char *rowsLower[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const char **rows = showUpper ? rowsUpper : rowsLower;
    const char *puncts[] = {"，", "。", "！", "？", "、", "；", "：", "“", "”", "（", "）"};
    const int punctN = 11;
    const int keyW = 28, keyH = CHAT_KEY_H, startY = chatKbdStartY();
    const int shiftW = 36;
    tft.fillRect(0, startY - 1, SCREEN_WIDTH, chatInputY() - startY + 1, tft.color565(20, 20, 28));

    for (int r = 0; r < 4; r++) {
        int len = (int)strlen(rows[r]);
        int totalW = len * (keyW + 1);
        int startX = (SCREEN_WIDTH - totalW) / 2;
        // 第 4 行左侧：英文模式显示 ⇧ 大小写键
        if (r == 3 && !imePinyinMode) {
            int sx = startX - shiftW - 2;
            if (sx < 2)
                sx = 2;
            uint16_t bg = engCapsLock ? TFT_CYAN : tft.color565(70, 70, 90);
            tft.fillRoundRect(sx, startY + r * (keyH + 1), shiftW, keyH, 2, bg);
            tft.setTextColor(engCapsLock ? TFT_BLACK : TFT_WHITE, bg);
            tft.setTextDatum(MC_DATUM);
            tft.drawString(engCapsLock ? "ABC" : "abc", sx + shiftW / 2, startY + r * (keyH + 1) + keyH / 2, 1);
        }
        for (int i = 0; i < len; i++) {
            int x = startX + i * (keyW + 1);
            int y = startY + r * (keyH + 1);
            tft.fillRoundRect(x, y, keyW, keyH, 2, TFT_DARKGREY);
            char c[2] = {rows[r][i], 0};
            tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
            tft.setTextDatum(MC_DATUM);
            tft.drawString(c, x + keyW / 2, y + keyH / 2, 1);
        }
    }
    int py = startY + 4 * (keyH + 1);
    int pw = 26;
    int ptotal = punctN * (pw + 1);
    int px0 = (SCREEN_WIDTH - ptotal) / 2;
    for (int i = 0; i < punctN; i++) {
        int x = px0 + i * (pw + 1);
        tft.fillRoundRect(x, py, pw, keyH, 2, tft.color565(55, 55, 85));
        cnDrawUtf8(tft, x + 4, py + 1, puncts[i], TFT_WHITE);
    }
    tft.setTextDatum(TL_DATUM);
}

static void drawConversationHeader();

static void drawChatFuncBar()
{
    // 功能键画在候选面板最底行右侧
    const int cy = chatCandFuncY();
    tft.fillRect(200, cy, 28, CHAT_CAND_H, TFT_PURPLE);
    cnDrawUtf8(tft, 202, cy + 1, imePinyinMode ? "拼" : "英", TFT_WHITE);
    tft.fillRect(230, cy, 28, CHAT_CAND_H, TFT_NAVY);
    cnDrawUtf8(tft, 234, cy + 1, "空", TFT_WHITE);
    tft.fillRect(260, cy, 28, CHAT_CAND_H, TFT_ORANGE);
    cnDrawUtf8(tft, 264, cy + 1, "删", TFT_BLACK);
    tft.fillRect(290, cy, 28, CHAT_CAND_H, TFT_DARKGREY);
    cnDrawUtf8(tft, 294, cy + 1, "返", TFT_WHITE);
}

// 打字时只刷输入区；展开切换会影响消息区高度，需软刷消息
static void redrawChatTypingUI()
{
    drawChatCandidates();
    drawChatFuncBar();
    drawChatInputBar();
}

static void redrawChatMessagesSoft()
{
    // 非会话视图（颜色/在线/好友列表/群管理）禁止绘制消息与键盘
    if (!inConversationView())
        return;
    if ((chatTab == CHAT_TAB_PRIVATE && privateChatOpen) ||
        (chatTab == CHAT_TAB_GROUPS && groupChatOpen))
        drawConversationHeader();
    drawChatMessagesArea();
    redrawChatTypingUI();
    paintChatBannerIfNeeded();
}

static void redrawCandPanelAndMessages()
{
    if (!inConversationView())
        return;
    drawChatMessagesArea();
    redrawChatTypingUI();
    paintChatBannerIfNeeded();
}

static void drawConversationHeader()
{
    // 私聊/群聊顶部返回条
    if (chatTab == CHAT_TAB_PRIVATE && privateChatOpen) {
        tft.fillRect(2, CHAT_TAB_H + 1, SCREEN_WIDTH - 4, 14, tft.color565(30, 40, 60));
        cnDrawUtf8(tft, 6, CHAT_TAB_H + 2, "返回列表", TFT_CYAN);
        char t[24];
        snprintf(t, sizeof(t), "私聊:%s", privatePeerId);
        cnDrawUtf8(tft, 120, CHAT_TAB_H + 2, t, TFT_WHITE);
    } else if (chatTab == CHAT_TAB_GROUPS && groupChatOpen) {
        tft.fillRect(2, CHAT_TAB_H + 1, SCREEN_WIDTH - 4, 14, tft.color565(30, 40, 60));
        cnDrawUtf8(tft, 6, CHAT_TAB_H + 2, "返回群管理", TFT_CYAN);
        const char *gn = activeGroupId;
        for (int i = 0; i < chatGroupCount; i++)
            if (strcmp(chatGroups[i].id, activeGroupId) == 0) gn = chatGroups[i].name;
        char t[28];
        snprintf(t, sizeof(t), "%s", gn);
        cnDrawUtf8(tft, 100, CHAT_TAB_H + 2, t, TFT_WHITE);
    }
}

static void drawPrivatePeerList()
{
    tft.fillRect(2, CHAT_TAB_H + 1, SCREEN_WIDTH - 4, SCREEN_HEIGHT - CHAT_TAB_H - 28, tft.color565(16, 16, 24));
    cnDrawUtf8(tft, 8, CHAT_TAB_H + 4, "选择好友开始私聊", TFT_LIGHTGREY);
    int y = CHAT_TAB_H + 22;
    int i = 0;
    for (auto const &kv : peerInfoMap) {
        if (!peerVisibleForLocalMode(kv.second))
            continue;
        if (i >= 8) break;
        const PeerInfo_t &p = kv.second;
        const char *id = p.deviceId[0] ? p.deviceId : p.macAddress.c_str();
        tft.fillRoundRect(8, y, SCREEN_WIDTH - 16, 18, 3, tft.color565(40, 48, 70));
        cnDrawUtf8(tft, 14, y + 3, id, TFT_WHITE);
        cnDrawUtf8(tft, SCREEN_WIDTH - 50, y + 3, "进入", TFT_CYAN);
        y += 22;
        i++;
    }
    if (i == 0)
        cnDrawUtf8(tft, 14, 80, "暂无在线好友", TFT_DARKGREY);
    // 底部返回
    tft.fillRoundRect(SCREEN_WIDTH / 2 - 40, SCREEN_HEIGHT - 24, 80, 20, 3, TFT_DARKGREY);
    cnDrawUtf8(tft, SCREEN_WIDTH / 2 - 16, SCREEN_HEIGHT - 20, "返回", TFT_WHITE);
}

static void drawOnlinePanel()
{
    tft.fillRect(2, CHAT_TAB_H + 1, SCREEN_WIDTH - 4, SCREEN_HEIGHT - CHAT_TAB_H - 28,
                 tft.color565(16, 16, 24));
    cnDrawUtf8(tft, 8, CHAT_TAB_H + 4, "在线列表", TFT_WHITE);

    const int rowH = 26;
    const int listTop = CHAT_TAB_H + 22;
    int y = listTop;
    int shown = 0;
    unsigned long now = millis();

    for (auto const &kv : peerInfoMap) {
        if (!peerVisibleForLocalMode(kv.second))
            continue;
        if (shown >= 7)
            break;
        const PeerInfo_t &p = kv.second;
        const char *id = p.deviceId[0] ? p.deviceId : p.macAddress.c_str();
        unsigned long lastHb = 0;
        auto hit = peerLastHeartbeat.find(kv.first);
        if (hit != peerLastHeartbeat.end())
            lastHb = hit->second;
        unsigned long latency = (lastHb && now >= lastHb) ? (now - lastHb) : 9999;
        if (latency > 9999)
            latency = 9999;

        uint16_t rowBg = tft.color565(36, 42, 60);
        tft.fillRoundRect(6, y, SCREEN_WIDTH - 12, 22, 3, rowBg);
        cnDrawUtf8(tft, 12, y + 5, id, TFT_WHITE);

        drawRssiBars(118, y + 4, p.rssi);

        char line[20];
        snprintf(line, sizeof(line), "%lums", latency);
        uint16_t latColor = TFT_CYAN;
        if (latency > 4000)
            latColor = TFT_RED;
        else if (latency > 2000)
            latColor = TFT_YELLOW;
        tft.setTextColor(latColor, rowBg);
        tft.setTextDatum(TL_DATUM);
        tft.drawString(line, 160, y + 7, 1);

        y += rowH;
        shown++;
    }
    if (shown == 0)
        cnDrawUtf8(tft, 14, 90, "暂无在线好友", TFT_DARKGREY);

    tft.fillRoundRect(SCREEN_WIDTH / 2 - 40, SCREEN_HEIGHT - 24, 80, 20, 3, TFT_DARKGREY);
    cnDrawUtf8(tft, SCREEN_WIDTH / 2 - 16, SCREEN_HEIGHT - 20, "返回", TFT_WHITE);
}

void updateOnlinePanelLive()
{
    if (currentUIState != UI_STATE_CHAT || chatTab != CHAT_TAB_ONLINE)
        return;
    if (groupInvitePick || privateChatOpen || groupChatOpen)
        return;
    drawOnlinePanel();
}

static void drawGroupInvitePick()
{
    tft.fillRect(2, CHAT_TAB_H + 1, SCREEN_WIDTH - 4, SCREEN_HEIGHT - CHAT_TAB_H - 28,
                 tft.color565(16, 16, 24));
    cnDrawUtf8(tft, 8, CHAT_TAB_H + 4, "选择好友", TFT_YELLOW);
    int y = CHAT_TAB_H + 22;
    int i = 0;
    for (auto const &kv : peerInfoMap) {
        if (!peerVisibleForLocalMode(kv.second))
            continue;
        if (i >= 8)
            break;
        const PeerInfo_t &p = kv.second;
        const char *id = p.deviceId[0] ? p.deviceId : p.macAddress.c_str();
        tft.fillRoundRect(8, y, SCREEN_WIDTH - 16, 18, 3, tft.color565(40, 48, 70));
        cnDrawUtf8(tft, 14, y + 3, id, TFT_WHITE);
        cnDrawUtf8(tft, SCREEN_WIDTH - 60, y + 3, "邀请", TFT_CYAN);
        y += 22;
        i++;
    }
    if (i == 0)
        cnDrawUtf8(tft, 14, 80, "暂无在线好友", TFT_DARKGREY);
    tft.fillRoundRect(SCREEN_WIDTH / 2 - 40, SCREEN_HEIGHT - 24, 80, 20, 3, TFT_DARKGREY);
    cnDrawUtf8(tft, SCREEN_WIDTH / 2 - 16, SCREEN_HEIGHT - 20, "取消", TFT_WHITE);
}

static void drawGroupPanel()
{
    if (groupInvitePick) {
        drawGroupInvitePick();
        return;
    }

    tft.fillRect(2, CHAT_TAB_H + 1, SCREEN_WIDTH - 4, SCREEN_HEIGHT - CHAT_TAB_H - 28,
                 tft.color565(16, 16, 24));
    cnDrawUtf8(tft, 8, CHAT_TAB_H + 4, "群组管理", TFT_WHITE);
    tft.fillRoundRect(220, CHAT_TAB_H + 3, 56, 16, 3, TFT_GREEN);
    cnDrawUtf8(tft, 230, CHAT_TAB_H + 5, "创建", TFT_BLACK);

    const int rowH = 22;
    const int listTop = CHAT_TAB_H + 24;
    const int listBot = SCREEN_HEIGHT - 30;
    const int maxShow = (listBot - listTop) / rowH;
    if (groupListScroll < 0)
        groupListScroll = 0;
    int maxSc = (chatGroupCount > maxShow) ? chatGroupCount - maxShow : 0;
    if (groupListScroll > maxSc)
        groupListScroll = maxSc;

    int y = listTop;
    for (int i = groupListScroll; i < chatGroupCount && (i - groupListScroll) < maxShow; i++) {
        bool owner = isGroupOwner(chatGroups[i]);
        bool sel = (strcmp(activeGroupId, chatGroups[i].id) == 0);
        // 名称 + 进入（所有成员）；删/解散/邀请仅群主
        tft.fillRoundRect(6, y, owner ? 88 : 150, 18, 3,
                          sel ? tft.color565(30, 70, 120) : tft.color565(45, 45, 55));
        cnDrawUtf8(tft, 10, y + 3, chatGroups[i].name, TFT_WHITE);

        if (owner) {
            tft.fillRoundRect(98, y, 40, 18, 3, TFT_PURPLE);
            cnDrawUtf8(tft, 102, y + 3, "邀请", TFT_WHITE);
            tft.fillRoundRect(142, y, 40, 18, 3, TFT_RED);
            cnDrawUtf8(tft, 146, y + 3, "删除", TFT_WHITE);
            tft.fillRoundRect(186, y, 40, 18, 3, TFT_ORANGE);
            cnDrawUtf8(tft, 190, y + 3, "解散", TFT_BLACK);
            tft.fillRoundRect(230, y, 44, 18, 3, TFT_CYAN);
            cnDrawUtf8(tft, 238, y + 3, "进入", TFT_BLACK);
        } else {
            tft.fillRoundRect(160, y, 56, 18, 3, TFT_CYAN);
            cnDrawUtf8(tft, 172, y + 3, "进入", TFT_BLACK);
            cnDrawUtf8(tft, 224, y + 3, "成员", TFT_DARKGREY);
        }
        y += rowH;
    }
    if (chatGroupCount == 0)
        cnDrawUtf8(tft, 14, 90, "暂无群组，点右上角创建", TFT_DARKGREY);

    tft.fillRoundRect(SCREEN_WIDTH / 2 - 40, SCREEN_HEIGHT - 24, 80, 20, 3, TFT_DARKGREY);
    cnDrawUtf8(tft, SCREEN_WIDTH / 2 - 16, SCREEN_HEIGHT - 20, "返回", TFT_WHITE);
}

static void drawColorPanel()
{
    tft.fillRect(2, CHAT_TAB_H + 1, SCREEN_WIDTH - 4, SCREEN_HEIGHT - CHAT_TAB_H - 28, tft.color565(16, 16, 24));
    cnDrawUtf8(tft, 8, CHAT_TAB_H + 6, "聊天文字颜色", TFT_WHITE);
    uint16_t colors[] = {TFT_WHITE, TFT_CYAN, TFT_YELLOW, TFT_GREEN, TFT_MAGENTA, TFT_ORANGE, TFT_RED, TFT_BLUE};
    for (int i = 0; i < 8; i++) {
        int x = 16 + (i % 4) * 72;
        int y = 50 + (i / 4) * 36;
        tft.fillRoundRect(x, y, 64, 28, 4, colors[i]);
        if (chatTextColor == colors[i])
            tft.drawRoundRect(x - 1, y - 1, 66, 30, 4, TFT_WHITE);
    }
    // 清空聊天记录
    tft.fillRoundRect(40, 140, 240, 28, 4, TFT_RED);
    cnDrawUtf8(tft, 100, 146, "清空聊天记录", TFT_WHITE);

    tft.fillRoundRect(SCREEN_WIDTH / 2 - 40, SCREEN_HEIGHT - 24, 80, 20, 3, TFT_DARKGREY);
    cnDrawUtf8(tft, SCREEN_WIDTH / 2 - 16, SCREEN_HEIGHT - 20, "返回", TFT_WHITE);
}

void drawChatRoom()
{
    // 聊天全屏遮罩，杜绝白板笔迹透出
    tft.fillScreen(TFT_BLACK);
    drawChatTabs();

    if (inConversationView()) {
        if ((chatTab == CHAT_TAB_PRIVATE && privateChatOpen) ||
            (chatTab == CHAT_TAB_GROUPS && groupChatOpen))
            drawConversationHeader();
        drawChatMessagesArea();
        drawChatCandidates();
        drawChatFuncBar();
        drawChatKeyboard();
        drawChatInputBar();
    } else if (chatTab == CHAT_TAB_PRIVATE) {
        drawPrivatePeerList();
    } else if (chatTab == CHAT_TAB_GROUPS) {
        drawGroupPanel();
    } else if (chatTab == CHAT_TAB_COLOR) {
        drawColorPanel();
    } else if (chatTab == CHAT_TAB_ONLINE) {
        drawOnlinePanel();
    }
    paintChatBannerIfNeeded();
}

void showChatRoom()
{
    if (!isScreenOn)
        return;
    loadChatGroups();
    loadChatHistory();
    currentUIState = UI_STATE_CHAT;
    chatTab = CHAT_TAB_PUBLIC;
    privateChatOpen = false;
    groupChatOpen = false;
    groupInvitePick = false;
    inviteGroupId[0] = 0;
    chatScrollOffset = 0;
    chatInputBuffer[0] = 0;
    pinyinBuffer[0] = 0;
    candExpanded = false;
    refreshCandidates();
    drawChatRoom();
    // 通知他人：短暂提示，不写聊天记录
    sendChatEx(MSG_TYPE_CHAT_JOIN, CHAT_MODE_PUBLIC, "", localDeviceId, TFT_YELLOW);
}

void hideChatRoom()
{
    if (currentUIState != UI_STATE_CHAT)
        return;
    currentUIState = UI_STATE_MAIN;
    chatBannerMsg[0] = 0;
    privateChatOpen = false;
    groupChatOpen = false;
    chatDragStartY = -1;
    pendingCanvasRedrawAfterChat = false;
    redrawMainScreen();
}

static void appendInputUtf8(const char *utf8)
{
    if (!utf8) return;
    size_t have = strlen(chatInputBuffer);
    size_t add = strlen(utf8);
    if (have + add >= CHAT_TEXT_MAX) return;
    memcpy(chatInputBuffer + have, utf8, add + 1);
}

static void commitChatSend()
{
    if (!chatInputBuffer[0]) return;
    uint8_t mode = CHAT_MODE_PUBLIC;
    const char *target = "";
    if (chatTab == CHAT_TAB_PRIVATE) {
        if (!privatePeerId[0] || !privateChatOpen) return;
        mode = CHAT_MODE_PRIVATE;
        target = privatePeerId;
    } else if (chatTab == CHAT_TAB_GROUPS) {
        if (!activeGroupId[0] || !groupChatOpen) return;
        mode = CHAT_MODE_GROUP;
        target = activeGroupId;
    } else if (chatTab == CHAT_TAB_COLOR) {
        return;
    } else if (chatTab == CHAT_TAB_ONLINE) {
        return;
    }
    sendChatEx(MSG_TYPE_CHAT, mode, target, chatInputBuffer, chatTextColor);
    appendChatMessage(localDeviceId, target, chatInputBuffer, true, mode, chatTextColor);
    chatInputBuffer[0] = 0;
    pinyinBuffer[0] = 0;
    refreshCandidates();
    redrawChatMessagesSoft();
}

static bool handleChatFuncBar(int x, int y)
{
    const int cy0 = chatCandY();
    const int panelH = chatCandPanelH();
    if (y < cy0 || y >= cy0 + panelH)
        return false;

    const int funcY = chatCandFuncY();

    // 展开/收起箭头 + 翻页
    if (candExpanded && y >= funcY && y < funcY + CHAT_CAND_H) {
        int pages = candTotalPages();
        bool multi = pages > 1;
        if (multi) {
            int prevX = CAND_ARROW_X - 2 * CAND_PAGE_BTN_W;
            int nextX = CAND_ARROW_X - CAND_PAGE_BTN_W;
            if (x >= prevX && x < nextX) {
                if (candPage > 0) {
                    candPage--;
                    redrawChatTypingUI();
                }
                return true;
            }
            if (x >= nextX && x < CAND_ARROW_X) {
                if (candPage < pages - 1) {
                    candPage++;
                    redrawChatTypingUI();
                }
                return true;
            }
        }
    }
    if (candNeedsExpandArrow() &&
        x >= CAND_ARROW_X && x < CAND_FUNC_X &&
        y >= funcY && y < funcY + CHAT_CAND_H) {
        candExpanded = !candExpanded;
        if (candExpanded)
            candPage = 0;
        else
            candPage = 0;
        redrawCandPanelAndMessages();
        return true;
    }

    // 功能键（底行右侧）
    if (y >= funcY && y < funcY + CHAT_CAND_H) {
        if (x >= 200 && x < 228) {
            imePinyinMode = !imePinyinMode;
            pinyinBuffer[0] = 0;
            if (imePinyinMode)
                engCapsLock = false;
            candExpanded = false;
            candPage = 0;
            refreshCandidates();
            redrawCandPanelAndMessages();
            drawChatKeyboard();
            return true;
        }
        if (x >= 230 && x < 258) {
            if (imePinyinMode && pinyinBuffer[0]) {
                appendInputUtf8(pinyinBuffer);
                pinyinBuffer[0] = 0;
            } else {
                appendInputUtf8(" ");
            }
            candExpanded = false;
            candPage = 0;
            refreshCandidates();
            redrawCandPanelAndMessages();
            return true;
        }
        if (x >= 260 && x < 288) {
            if (imePinyinMode && pinyinBuffer[0]) {
                pinyinBuffer[strlen(pinyinBuffer) - 1] = 0;
            } else if (chatInputBuffer[0]) {
                size_t n = strlen(chatInputBuffer);
                int i = (int)n - 1;
                while (i > 0 && ((uint8_t)chatInputBuffer[i] & 0xC0) == 0x80) i--;
                chatInputBuffer[i] = 0;
            }
            if (!pinyinBuffer[0]) {
                candExpanded = false;
                candPage = 0;
            }
            refreshCandidates();
            redrawCandPanelAndMessages();
            return true;
        }
        if (x >= 290) {
            candExpanded = false;
            candPage = 0;
            if (chatTab == CHAT_TAB_PRIVATE && privateChatOpen) {
                privateChatOpen = false;
                drawChatRoom();
                return true;
            }
            if (chatTab == CHAT_TAB_GROUPS && groupChatOpen) {
                groupChatOpen = false;
                drawChatRoom();
                return true;
            }
            hideChatRoom();
            return true;
        }
    }

    // 点选候选（折叠一行 / 展开多行分页）
    if (candCount > 0 && imePinyinMode) {
        auto pickAt = [&](int idx) {
            if (idx < 0 || idx >= candCount)
                return false;
            appendInputUtf8(candZh[idx]);
            pinyinBuffer[0] = 0;
            candExpanded = false;
            candPage = 0;
            refreshCandidates();
            redrawCandPanelAndMessages();
            return true;
        };

        if (!candExpanded) {
            if (y >= cy0 && y < cy0 + CHAT_CAND_H && x < CAND_ARROW_X) {
                int fit = countCandFitWidth(CAND_TEXT_W, 0, candCount);
                int cx = 4;
                for (int i = 0; i < fit; i++) {
                    char lab[24];
                    snprintf(lab, sizeof(lab), "%d.%s", i + 1, candZh[i]);
                    int w = cnTextWidth(lab) + 6;
                    if (x >= cx && x < cx + w)
                        return pickAt(i);
                    cx += w;
                }
            }
        } else {
            int pages = candTotalPages();
            bool multi = pages > 1;
            int idx = candPageStartIndex(candPage);
            int pageEnd = idx + countExpandPageSize(idx, multi);
            if (pageEnd > candCount)
                pageEnd = candCount;
            int rows = chatCandRows();
            for (int r = 0; r < rows && idx < pageEnd; r++) {
                int y0 = cy0 + r * CHAT_CAND_H;
                bool last = (r == rows - 1);
                int maxW = last ? candExpandLastTextW(multi) : (SCREEN_WIDTH - 8);
                int fit = countCandFitWidth(maxW, idx, pageEnd - idx);
                if (fit <= 0)
                    fit = 1;
                if (y >= y0 && y < y0 + CHAT_CAND_H) {
                    int maxX = last ? (multi ? (CAND_ARROW_X - 2 * CAND_PAGE_BTN_W) : CAND_ARROW_X)
                                    : SCREEN_WIDTH;
                    if (x < maxX) {
                        int cx = 4;
                        for (int k = 0; k < fit && idx + k < pageEnd; k++) {
                            char lab[24];
                            snprintf(lab, sizeof(lab), "%d.%s", idx + k + 1, candZh[idx + k]);
                            int w = cnTextWidth(lab) + 6;
                            if (x >= cx && x < cx + w)
                                return pickAt(idx + k);
                            cx += w;
                        }
                    }
                }
                idx += fit;
            }
        }
    }
    return true;
}

bool handleChatTouch(int x, int y)
{
    if (currentUIState != UI_STATE_CHAT)
        return false;

    // 横幅超时清掉
    if (chatBannerMsg[0] && millis() > chatBannerUntil) {
        chatBannerMsg[0] = 0;
        drawChatRoom();
    }

    // 按下边沿检测：按住不松开会连发，只在落笔瞬间响应一次
    bool risingEdge = !chatFingerDown;
    chatFingerDown = true;

    // 消息区滑动允许连续拖动（不受边沿限制）
    if (inConversationView()) {
        const int msgTop = CHAT_TAB_H + ((chatTab == CHAT_TAB_PRIVATE || chatTab == CHAT_TAB_GROUPS) ? 16 : 1);
        const int msgBot = chatMsgBottom();
        if (y >= msgTop && y < msgBot) {
            if (chatDragStartY < 0) {
                chatDragStartY = y;
                chatScrollAtDrag = chatScrollOffset;
            } else {
                int dy = chatDragStartY - y;
                int step = dy / 14;
                int newOff = chatScrollAtDrag + step;
                if (newOff < 0) newOff = 0;
                if (newOff != chatScrollOffset) {
                    chatScrollOffset = newOff;
                    drawChatMessagesArea();
                    paintChatBannerIfNeeded();
                }
            }
            return true;
        }
        chatDragStartY = -1;
    }

    // 非滑动区域：按住期间忽略重复触发
    if (!risingEdge)
        return true;

    // Tabs
    if (y < CHAT_TAB_H) {
        int tab = x / (SCREEN_WIDTH / 5);
        if (tab >= 0 && tab < 5) {
            chatTab = (ChatTab_e)tab;
            chatScrollOffset = 0;
            chatDragStartY = -1;
            groupInvitePick = false;
            if (chatTab == CHAT_TAB_PRIVATE) privateChatOpen = false;
            if (chatTab == CHAT_TAB_GROUPS) groupChatOpen = false;
            drawChatRoom();
        }
        return true;
    }

    // 会话头：返回列表
    if (inConversationView() && y >= CHAT_TAB_H + 1 && y < CHAT_TAB_H + 15) {
        if (chatTab == CHAT_TAB_PRIVATE && privateChatOpen && x < 110) {
            privateChatOpen = false;
            drawChatRoom();
            return true;
        }
        if (chatTab == CHAT_TAB_GROUPS && groupChatOpen && x < 100) {
            groupChatOpen = false;
            drawChatRoom();
            return true;
        }
    }

    // 颜色面板
    if (chatTab == CHAT_TAB_COLOR) {
        uint16_t colors[] = {TFT_WHITE, TFT_CYAN, TFT_YELLOW, TFT_GREEN, TFT_MAGENTA, TFT_ORANGE, TFT_RED, TFT_BLUE};
        for (int i = 0; i < 8; i++) {
            int cx = 16 + (i % 4) * 72;
            int cy = 50 + (i / 4) * 36;
            if (x >= cx && x <= cx + 64 && y >= cy && y <= cy + 28) {
                chatTextColor = colors[i];
                drawChatRoom();
                return true;
            }
        }
        // 清空聊天记录
        if (x >= 40 && x <= 280 && y >= 140 && y <= 168) {
            clearChatHistoryPersistent();
            showChatBanner("已清空记录", 1800);
            drawChatRoom();
            return true;
        }
        if (y >= SCREEN_HEIGHT - 26) {
            hideChatRoom();
            return true;
        }
        return true;
    }

    // 在线面板
    if (chatTab == CHAT_TAB_ONLINE) {
        if (y >= SCREEN_HEIGHT - 26) {
            hideChatRoom();
            return true;
        }
        return true;
    }

    // 私聊列表
    if (chatTab == CHAT_TAB_PRIVATE && !privateChatOpen) {
        if (y >= SCREEN_HEIGHT - 26) {
            hideChatRoom();
            return true;
        }
        int idx = (y - (CHAT_TAB_H + 22)) / 22;
        if (idx >= 0) {
            int i = 0;
            for (auto const &kv : peerInfoMap) {
                if (!peerVisibleForLocalMode(kv.second))
                    continue;
                if (i == idx) {
                    strncpy(privatePeerId, kv.second.deviceId[0] ? kv.second.deviceId : kv.first.c_str(), DEVICE_ID_MAX_LEN);
                    privatePeerId[DEVICE_ID_MAX_LEN] = 0;
                    privateChatOpen = true;
                    chatScrollOffset = 0;
                    drawChatRoom();
                    return true;
                }
                i++;
                if (i >= 8) break;
            }
        }
        return true;
    }

    // 群组管理面板
    if (chatTab == CHAT_TAB_GROUPS && !groupChatOpen) {
        if (y >= SCREEN_HEIGHT - 26) {
            if (groupInvitePick) {
                groupInvitePick = false;
                inviteGroupId[0] = 0;
                drawChatRoom();
                return true;
            }
            hideChatRoom();
            return true;
        }

        // 邀请选人
        if (groupInvitePick) {
            int idx = (y - (CHAT_TAB_H + 22)) / 22;
            if (idx >= 0) {
                int i = 0;
                for (auto const &kv : peerInfoMap) {
                    if (!peerVisibleForLocalMode(kv.second))
                        continue;
                    if (i == idx) {
                        int gi = findGroupIndexById(inviteGroupId);
                        if (gi >= 0 && isGroupOwner(chatGroups[gi])) {
                            const char *peerId = kv.second.deviceId[0] ? kv.second.deviceId : kv.first.c_str();
                            char msg[CHAT_TEXT_MAX + 1];
                            snprintf(msg, sizeof(msg), "INVITE:%s:%s:%s",
                                     chatGroups[gi].id, chatGroups[gi].name, chatGroups[gi].ownerId);
                            sendChatEx(MSG_TYPE_CHAT_GROUP, CHAT_MODE_GROUP, peerId, msg, TFT_ORANGE);
                            showChatBanner("已邀请", 1800);
                        }
                        groupInvitePick = false;
                        inviteGroupId[0] = 0;
                        drawChatRoom();
                        return true;
                    }
                    i++;
                    if (i >= 8) break;
                }
            }
            return true;
        }

        // 创建：仅本机可见，不广播（邀请后对端才看到）
        if (x >= 220 && y >= CHAT_TAB_H + 3 && y <= CHAT_TAB_H + 20) {
            if (chatGroupCount < CHAT_MAX_GROUPS) {
                char gid[9];
                snprintf(gid, sizeof(gid), "G%02X%02X",
                         (unsigned)(millis() & 0xFF), (unsigned)(localDeviceId[0] & 0xFF));
                char gname[13];
                snprintf(gname, sizeof(gname), "群%d", chatGroupCount + 1);
                addChatGroupLocal(gid, gname, localDeviceId);
                showChatBanner("已创建群组", 2000);
                drawChatRoom();
            }
            return true;
        }
        const int rowH = 22;
        const int listTop = CHAT_TAB_H + 24;
        int idx = (y - listTop) / rowH + groupListScroll;
        if (idx >= 0 && idx < chatGroupCount && y >= listTop && y < SCREEN_HEIGHT - 30) {
            int gy = listTop + (idx - groupListScroll) * rowH;
            if (y >= gy && y < gy + 18) {
                bool owner = isGroupOwner(chatGroups[idx]);
                if (owner) {
                    if (x >= 98 && x < 138) {
                        // 邀请
                        strncpy(inviteGroupId, chatGroups[idx].id, DEVICE_ID_MAX_LEN);
                        inviteGroupId[DEVICE_ID_MAX_LEN] = 0;
                        groupInvitePick = true;
                        drawChatRoom();
                        return true;
                    }
                    if (x >= 142 && x < 182) {
                        // 删除本地（仅群主）
                        removeChatGroupLocal(chatGroups[idx].id);
                        drawChatRoom();
                        return true;
                    }
                    if (x >= 186 && x < 226) {
                        // 解散广播
                        char gid[DEVICE_ID_MAX_LEN + 1];
                        strncpy(gid, chatGroups[idx].id, DEVICE_ID_MAX_LEN);
                        gid[DEVICE_ID_MAX_LEN] = 0;
                        char msg[32];
                        snprintf(msg, sizeof(msg), "DISBAND:%s", gid);
                        sendChatEx(MSG_TYPE_CHAT_GROUP, CHAT_MODE_GROUP, gid, msg, TFT_ORANGE);
                        removeChatGroupLocal(gid);
                        showChatBanner("已解散群组", 2000);
                        drawChatRoom();
                        return true;
                    }
                    if ((x >= 230 && x < 280) || x < 88) {
                        strncpy(activeGroupId, chatGroups[idx].id, DEVICE_ID_MAX_LEN);
                        activeGroupId[DEVICE_ID_MAX_LEN] = 0;
                        groupChatOpen = true;
                        chatScrollOffset = 0;
                        drawChatRoom();
                        return true;
                    }
                } else {
                    if ((x >= 160 && x < 216) || x < 156) {
                        strncpy(activeGroupId, chatGroups[idx].id, DEVICE_ID_MAX_LEN);
                        activeGroupId[DEVICE_ID_MAX_LEN] = 0;
                        groupChatOpen = true;
                        chatScrollOffset = 0;
                        drawChatRoom();
                        return true;
                    }
                }
            }
        }
        return true;
    }

    // —— 会话视图：输入 / 键盘（已在上方处理滑动）——
    if (!inConversationView())
        return true;

    const int iy = chatInputY();
    const int barW = SCREEN_WIDTH - 100;
    const int barX = (SCREEN_WIDTH - barW - 56) / 2;

    // 发送
    if (y >= iy && y <= iy + CHAT_INPUT_H && x >= barX + barW + 4 && x <= barX + barW + 56) {
        commitChatSend();
        return true;
    }

    // 功能栏 + 候选
    if (handleChatFuncBar(x, y))
        return true;

    const int keyW = 28, keyH = CHAT_KEY_H, startY = chatKbdStartY();
    const int shiftW = 36;
    const char *puncts[] = {"，", "。", "！", "？", "、", "；", "：", "“", "”", "（", "）"};
    const int punctN = 11;
    int pw = 26;
    int py = startY + 4 * (keyH + 1);

    if (y >= py && y <= py + keyH) {
        int ptotal = punctN * (pw + 1);
        int px0 = (SCREEN_WIDTH - ptotal) / 2;
        for (int i = 0; i < punctN; i++) {
            int x0 = px0 + i * (pw + 1);
            if (x >= x0 && x < x0 + pw) {
                appendInputUtf8(puncts[i]);
                redrawChatTypingUI();
                return true;
            }
        }
    }

    // 英文大小写切换（Z 行左侧）
    if (!imePinyinMode) {
        const char *zrow = "ZXCVBNM";
        int zlen = (int)strlen(zrow);
        int ztotal = zlen * (keyW + 1);
        int zstart = (SCREEN_WIDTH - ztotal) / 2;
        int sx = zstart - shiftW - 2;
        if (sx < 2)
            sx = 2;
        int zy = startY + 3 * (keyH + 1);
        if (y >= zy && y <= zy + keyH && x >= sx && x <= sx + shiftW) {
            engCapsLock = !engCapsLock;
            drawChatKeyboard();
            return true;
        }
    }

    const char *rows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (int r = 0; r < 4; r++) {
        int len = (int)strlen(rows[r]);
        int totalW = len * (keyW + 1);
        int startX = (SCREEN_WIDTH - totalW) / 2;
        int ky = startY + r * (keyH + 1);
        if (y < ky || y > ky + keyH)
            continue;
        for (int i = 0; i < len; i++) {
            int kx = startX + i * (keyW + 1);
            if (x >= kx && x <= kx + keyW) {
                char ch = rows[r][i];
                if (ch >= 'A' && ch <= 'Z') {
                    if (imePinyinMode || !engCapsLock)
                        ch = (char)(ch - 'A' + 'a');
                    // engCapsLock：保持大写
                }
                if (imePinyinMode && ch >= 'a' && ch <= 'z') {
                    size_t n = strlen(pinyinBuffer);
                    if (n + 1 < sizeof(pinyinBuffer)) {
                        pinyinBuffer[n] = ch;
                        pinyinBuffer[n + 1] = 0;
                    }
                } else {
                    char tmp[2] = {ch, 0};
                    appendInputUtf8(tmp);
                }
                bool prevExp = candExpanded;
                refreshCandidates();
                if (prevExp != candExpanded)
                    redrawCandPanelAndMessages();
                else
                    redrawChatTypingUI();
                return true;
            }
        }
    }
    return true;
}

// 如果 readBatteryVoltagePercentage 是 ui_manager 的一部分，则在此定义
// 然而，它更像是一个系统工具或电源管理功能。
// 目前，它是 extern 声明的，假设它在 Project-ESPNow.ino 或 power_manager 中。
// float readBatteryVoltagePercentage() { /* ... 实现 ... */ }

// ========== 画板在线列表 / 私聊邀请 ==========

bool isSignalInfoPressed(int x, int y)
{
    if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
        return false;
    return x >= SIGNAL_INFO_X - 2 && x <= SIGNAL_INFO_X + SIGNAL_INFO_W + 4 &&
           y >= SIGNAL_INFO_Y - 2 && y <= SIGNAL_INFO_Y + SIGNAL_INFO_H + 4;
}

static void formatOnlineDuration(unsigned long firstSeenMs, char *out, size_t outLen)
{
    if (!out || outLen < 4) return;
    if (firstSeenMs == 0) {
        snprintf(out, outLen, "--");
        return;
    }
    unsigned long sec = (millis() - firstSeenMs) / 1000UL;
    if (sec < 60)
        snprintf(out, outLen, "%lus", sec);
    else if (sec < 3600)
        snprintf(out, outLen, "%lum", sec / 60);
    else
        snprintf(out, outLen, "%luh", sec / 3600);
}

static int onlineListContentHeight()
{
    int n = 0;
    for (auto const &kv : peerInfoMap) {
        if (peerVisibleForLocalMode(kv.second))
            n++;
    }
    if (n < 1) n = 1;
    return n * ONLINE_LIST_ROW_H;
}

static void clampOnlineListScroll()
{
    int viewH = ONLINE_LIST_BOTTOM - ONLINE_LIST_TOP - ONLINE_LIST_HEADER_H;
    int maxScroll = onlineListContentHeight() - viewH;
    if (maxScroll < 0) maxScroll = 0;
    if (onlineListScrollY < 0) onlineListScrollY = 0;
    if (onlineListScrollY > maxScroll) onlineListScrollY = maxScroll;
}

void drawOnlineListScreen()
{
    tft.fillScreen(tft.color565(12, 14, 22));
    tft.fillRect(0, 0, SCREEN_WIDTH, 26, tft.color565(28, 36, 55));
    cnDrawUtf8(tft, 8, 7, "在线设备", TFT_WHITE);
    tft.fillRoundRect(SCREEN_WIDTH - 56, 4, 50, 18, 3, tft.color565(60, 70, 90));
    cnDrawUtf8(tft, SCREEN_WIDTH - 44, 7, "返回", TFT_CYAN);

    // 表头
    int hy = ONLINE_LIST_TOP;
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(1);
    cnDrawUtf8(tft, 6, hy + 3, "ID", TFT_LIGHTGREY);
    cnDrawUtf8(tft, 48, hy + 3, "信号", TFT_LIGHTGREY);
    cnDrawUtf8(tft, 94, hy + 3, "延迟", TFT_LIGHTGREY);
    cnDrawUtf8(tft, 140, hy + 3, "电量", TFT_LIGHTGREY);
    cnDrawUtf8(tft, 184, hy + 3, "上线", TFT_LIGHTGREY);
    cnDrawUtf8(tft, 248, hy + 3, "操作", TFT_LIGHTGREY);
    tft.drawFastHLine(4, hy + ONLINE_LIST_HEADER_H - 2, SCREEN_WIDTH - 8, TFT_DARKGREY);

    clampOnlineListScroll();
    int listTop = ONLINE_LIST_TOP + ONLINE_LIST_HEADER_H;
    int viewH = ONLINE_LIST_BOTTOM - listTop;
    tft.fillRect(0, listTop, SCREEN_WIDTH, viewH, tft.color565(12, 14, 22));

    int visibleN = 0;
    for (auto const &kv : peerInfoMap) {
        if (peerVisibleForLocalMode(kv.second))
            visibleN++;
    }
    if (visibleN == 0) {
        cnDrawUtf8(tft, 24, listTop + 40, "暂无在线设备", TFT_DARKGREY);
        if (privInviteVisible)
            drawPrivInviteDialog();
        return;
    }

    unsigned long now = millis();
    int idx = 0;
    for (auto const &kv : peerInfoMap) {
        const PeerInfo_t &p = kv.second;
        if (!peerVisibleForLocalMode(p))
            continue;
        int rowY = listTop + idx * ONLINE_LIST_ROW_H - onlineListScrollY;
        idx++;
        if (rowY + ONLINE_LIST_ROW_H < listTop)
            continue;
        if (rowY > ONLINE_LIST_BOTTOM)
            break;

        const char *id = p.deviceId[0] ? p.deviceId : "Peer";
        uint16_t rowBg = (idx & 1) ? tft.color565(22, 28, 42) : tft.color565(18, 22, 34);
        int drawY = rowY;
        int clipH = ONLINE_LIST_ROW_H - 2;
        if (drawY < listTop) {
            clipH -= (listTop - drawY);
            drawY = listTop;
        }
        if (drawY + clipH > ONLINE_LIST_BOTTOM)
            clipH = ONLINE_LIST_BOTTOM - drawY;
        if (clipH <= 0)
            continue;

        tft.fillRoundRect(4, drawY, SCREEN_WIDTH - 8, clipH, 2, rowBg);

        // 仅当整行大致可见时画文字
        if (rowY >= listTop - 4 && rowY + 16 <= ONLINE_LIST_BOTTOM) {
            char idShort[8];
            strncpy(idShort, id, 7);
            idShort[7] = '\0';
            tft.setTextColor(TFT_WHITE, rowBg);
            tft.drawString(idShort, 6, rowY + 6, 1);

            drawRssiBars(54, rowY + 8, p.rssi);

            unsigned long lat = p.latencyMs;
            if (p.lastSeenMs > 0 && now >= p.lastSeenMs) {
                unsigned long age = now - p.lastSeenMs;
                if (age > lat) lat = age;
            }
            if (lat > 9999) lat = 9999;
            char latBuf[12];
            snprintf(latBuf, sizeof(latBuf), "%lu", lat);
            uint16_t latColor = TFT_CYAN;
            if (lat > 4000) latColor = TFT_RED;
            else if (lat > 2000) latColor = TFT_YELLOW;
            tft.setTextColor(latColor, rowBg);
            tft.drawString(latBuf, 100, rowY + 6, 1);

            char batBuf[8];
            if (p.batteryPercent > 0)
                snprintf(batBuf, sizeof(batBuf), "%u%%", (unsigned)p.batteryPercent);
            else
                snprintf(batBuf, sizeof(batBuf), "--");
            tft.setTextColor(TFT_GREENYELLOW, rowBg);
            tft.drawString(batBuf, 148, rowY + 6, 1);

            char upBuf[10];
            formatOnlineDuration(p.firstSeenMs, upBuf, sizeof(upBuf));
            tft.setTextColor(TFT_LIGHTGREY, rowBg);
            tft.drawString(upBuf, 190, rowY + 6, 1);

            bool busy = isPrivateCanvasActive() || isPrivateCanvasInvitePending();
            uint16_t btnC = busy ? tft.color565(70, 70, 80) : tft.color565(40, 110, 90);
            tft.fillRoundRect(SCREEN_WIDTH - ONLINE_LIST_BTN_W - 8, rowY + 8,
                              ONLINE_LIST_BTN_W, ONLINE_LIST_BTN_H, 3, btnC);
            cnDrawUtf8(tft, SCREEN_WIDTH - ONLINE_LIST_BTN_W - 2, rowY + 11,
                       "私聊", busy ? TFT_DARKGREY : TFT_WHITE);
        }
    }
    if (privInviteVisible)
        drawPrivInviteDialog();
}

void updateOnlineListScreen()
{
    if (currentUIState != UI_STATE_ONLINE_LIST)
        return;
    drawOnlineListScreen();
}

void showOnlineListScreen()
{
    if (!isScreenOn || inCustomColorMode)
        return;
    onlineListScrollY = 0;
    onlineListDragging = false;
    onlineListDragLastY = -1;
    onlineListFingerDown = false;
    currentUIState = UI_STATE_ONLINE_LIST;
    drawOnlineListScreen();
}

void hideOnlineListScreen()
{
    if (currentUIState != UI_STATE_ONLINE_LIST)
        return;
    currentUIState = UI_STATE_MAIN;
    redrawMainScreen();
}

void onlineListTouchReleased()
{
    if (onlineListFingerDown && !onlineListDragging) {
        int x = onlineListPressX;
        int y = onlineListPressY;
        if (y <= 26 && x >= SCREEN_WIDTH - 60) {
            hideOnlineListScreen();
        } else {
            int listTop = ONLINE_LIST_TOP + ONLINE_LIST_HEADER_H;
            int idx = 0;
            for (auto const &kv : peerInfoMap) {
                if (!peerVisibleForLocalMode(kv.second))
                    continue;
                int rowY = listTop + idx * ONLINE_LIST_ROW_H - onlineListScrollY;
                idx++;
                int btnX = SCREEN_WIDTH - ONLINE_LIST_BTN_W - 8;
                int btnY = rowY + 8;
                if (x >= btnX && x <= btnX + ONLINE_LIST_BTN_W &&
                    y >= btnY && y <= btnY + ONLINE_LIST_BTN_H &&
                    rowY >= listTop - 4 && rowY + ONLINE_LIST_ROW_H <= ONLINE_LIST_BOTTOM + 4) {
                    if (isPrivateCanvasActive() || isPrivateCanvasInvitePending()) {
                        showStatusToast("已在私聊流程中", 1500);
                        break;
                    }
                    const PeerInfo_t &p = kv.second;
                    const char *id = p.deviceId[0] ? p.deviceId : "Peer";
                    invitePrivateCanvas(id, p.macAddress);
                    hideOnlineListScreen();
                    break;
                }
            }
        }
    }
    onlineListFingerDown = false;
    onlineListDragging = false;
    onlineListDragLastY = -1;
}

bool handleOnlineListTouch(int x, int y)
{
    if (currentUIState != UI_STATE_ONLINE_LIST)
        return false;

    if (!onlineListFingerDown) {
        onlineListFingerDown = true;
        onlineListDragging = false;
        onlineListPressX = x;
        onlineListPressY = y;
        onlineListDragLastY = y;
        return true;
    }

    if (abs(y - onlineListPressY) > 6 || abs(x - onlineListPressX) > 6)
        onlineListDragging = true;

    int listTop = ONLINE_LIST_TOP + ONLINE_LIST_HEADER_H;
    if (onlineListDragging && onlineListDragLastY >= 0) {
        int dy = onlineListDragLastY - y;
        if (abs(dy) > 1) {
            onlineListScrollY += dy;
            clampOnlineListScroll();
            drawOnlineListScreen();
        }
    }
    onlineListDragLastY = y;
    return true;
}

void showPrivInviteDialog(const char *fromId, unsigned long deadlineMs)
{
    strncpy(privInviteFromId, fromId ? fromId : "Peer", DEVICE_ID_MAX_LEN);
    privInviteFromId[DEVICE_ID_MAX_LEN] = '\0';
    privInviteDeadlineMs = deadlineMs;
    privInviteVisible = true;
    privInviteFingerDown = false;
    privInviteLastDrawnSec = 0;
    if (!isScreenOn)
        setScreenPower(true);
    // 非主界面/在线列表时回主界面，保证弹窗可见
    if (currentUIState != UI_STATE_MAIN && currentUIState != UI_STATE_ONLINE_LIST) {
        currentUIState = UI_STATE_MAIN;
        redrawMainScreen();
        return; // redrawMainScreen 末尾会再画邀请框
    }
    drawPrivInviteDialog();
}

void hidePrivInviteDialog()
{
    if (!privInviteVisible)
        return;
    privInviteVisible = false;
    privInviteFingerDown = false;
    if (currentUIState == UI_STATE_MAIN)
        redrawMainScreen();
    else if (currentUIState == UI_STATE_ONLINE_LIST)
        drawOnlineListScreen();
}

bool isPrivInviteDialogVisible()
{
    return privInviteVisible;
}

void drawPrivInviteDialog()
{
    if (!privInviteVisible || !isScreenOn)
        return;
    long remain = (long)(privInviteDeadlineMs - millis());
    if (remain < 0) remain = 0;
    unsigned long sec = (unsigned long)(remain / 1000UL);

    const uint16_t panelBg = tft.color565(30, 36, 55);
    const uint16_t rejectBg = tft.color565(90, 50, 50);
    const uint16_t acceptBg = tft.color565(40, 120, 80);

    tft.fillRoundRect(PRIV_INVITE_POPUP_X, PRIV_INVITE_POPUP_Y,
                      PRIV_INVITE_POPUP_W, PRIV_INVITE_POPUP_H, 6, panelBg);
    tft.drawRoundRect(PRIV_INVITE_POPUP_X, PRIV_INVITE_POPUP_Y,
                      PRIV_INVITE_POPUP_W, PRIV_INVITE_POPUP_H, 6, TFT_CYAN);
    tft.drawRoundRect(PRIV_INVITE_POPUP_X + 1, PRIV_INVITE_POPUP_Y + 1,
                      PRIV_INVITE_POPUP_W - 2, PRIV_INVITE_POPUP_H - 2, 5,
                      tft.color565(80, 160, 200));

    char title[40];
    snprintf(title, sizeof(title), "%s 邀请私聊画板", privInviteFromId);
    int tw = cnTextWidth(title);
    cnDrawUtf8(tft, PRIV_INVITE_POPUP_X + (PRIV_INVITE_POPUP_W - tw) / 2,
               PRIV_INVITE_POPUP_Y + 12, title, TFT_WHITE, panelBg, false);

    char tip[28];
    snprintf(tip, sizeof(tip), "%lus 请选择", sec);
    int tipW = cnTextWidth(tip);
    cnDrawUtf8(tft, PRIV_INVITE_POPUP_X + (PRIV_INVITE_POPUP_W - tipW) / 2,
               PRIV_INVITE_POPUP_Y + 32, tip, TFT_YELLOW, panelBg, false);

    int btnY = PRIV_INVITE_POPUP_Y + PRIV_INVITE_POPUP_H - 34;
    int rejectX = PRIV_INVITE_POPUP_X + 16;
    int acceptX = PRIV_INVITE_POPUP_X + PRIV_INVITE_POPUP_W - CONFIRM_BTN_W - 16;
    tft.fillRoundRect(rejectX, btnY, CONFIRM_BTN_W, CONFIRM_BTN_H, 4, rejectBg);
    tft.fillRoundRect(acceptX, btnY, CONFIRM_BTN_W, CONFIRM_BTN_H, 4, acceptBg);
    cnDrawUtf8(tft, rejectX + (CONFIRM_BTN_W - cnTextWidth("拒绝")) / 2, btnY + 8,
               "拒绝", TFT_WHITE, rejectBg, false);
    cnDrawUtf8(tft, acceptX + (CONFIRM_BTN_W - cnTextWidth("同意")) / 2, btnY + 8,
               "同意", TFT_WHITE, acceptBg, false);
    privInviteLastDrawnSec = sec;
}

void updatePrivInviteDialog()
{
    if (!privInviteVisible)
        return;
    long remain = (long)(privInviteDeadlineMs - millis());
    if (remain < 0) remain = 0;
    unsigned long sec = (unsigned long)(remain / 1000UL);
    if (sec != privInviteLastDrawnSec)
        drawPrivInviteDialog();
}

bool handlePrivInviteTouch(int x, int y)
{
    if (!privInviteVisible)
        return false;
    if (privInviteFingerDown)
        return true;
    int btnY = PRIV_INVITE_POPUP_Y + PRIV_INVITE_POPUP_H - 34;
    int rejectX = PRIV_INVITE_POPUP_X + 16;
    int acceptX = PRIV_INVITE_POPUP_X + PRIV_INVITE_POPUP_W - CONFIRM_BTN_W - 16;
    if (y >= btnY && y <= btnY + CONFIRM_BTN_H) {
        if (x >= rejectX && x <= rejectX + CONFIRM_BTN_W) {
            privInviteFingerDown = true;
            rejectPrivateCanvasInvite();
            return true;
        }
        if (x >= acceptX && x <= acceptX + CONFIRM_BTN_W) {
            privInviteFingerDown = true;
            acceptPrivateCanvasInvite();
            return true;
        }
    }
    // 点在弹窗外忽略（必须点按钮）
    return true;
}

void onPrivateCanvasSessionChanged()
{
    if (currentUIState == UI_STATE_MAIN && !inCustomColorMode)
        drawCanvasPageButtons();
    else if (currentUIState == UI_STATE_ONLINE_LIST)
        drawOnlineListScreen();
    // 在线列表会 fillScreen，必须把邀请弹窗叠回去
    if (privInviteVisible)
        drawPrivInviteDialog();
}

// ========== 设置：WiFi / 传输 ==========

enum SettingsPage_e {
    SETTINGS_PAGE_HOME = 0,
    SETTINGS_PAGE_WIFI_LIST,
    SETTINGS_PAGE_WIFI_PASS
};

static int settingsPage = SETTINGS_PAGE_HOME;
static int settingsWifiScroll = 0;
static int settingsSelectedAp = -1;
static char settingsPassBuf[WIFI_PASS_MAX + 1] = {0};
static bool settingsFingerDown = false;
static bool settingsPassCaps = false;
static int settingsDragLastY = -1;
static bool settingsDragging = false;
static int settingsPressX = 0, settingsPressY = 0;
static bool settingsNeedRedraw = true;
static bool settingsWasConnecting = false;

void drawSettingsButton()
{
    if (!isScreenOn || inCustomColorMode || currentUIState != UI_STATE_MAIN)
        return;
    tft.fillRect(SETTINGS_BUTTON_X, SETTINGS_BUTTON_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H,
                 tft.color565(40, 90, 120));
    cnDrawUtf8(tft, SETTINGS_BUTTON_X + 2, SETTINGS_BUTTON_Y + 2, "设", TFT_WHITE);
}

bool isSettingsButtonPressed(int x, int y)
{
    if (currentUIState != UI_STATE_MAIN)
        return false;
    return x >= SETTINGS_BUTTON_X && x <= SETTINGS_BUTTON_X + SETTINGS_BUTTON_W &&
           y >= SETTINGS_BUTTON_Y && y <= SETTINGS_BUTTON_Y + SETTINGS_BUTTON_H;
}

static void drawSettingsPassKeyboard()
{
    const int keyH = 22;
    const int keyW = 28;
    const int startY = 118;
    const char *rows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (int r = 0; r < 4; r++) {
        int len = (int)strlen(rows[r]);
        int totalW = len * (keyW + 1);
        int startX = (SCREEN_WIDTH - totalW) / 2;
        int ky = startY + r * (keyH + 1);
        for (int i = 0; i < len; i++) {
            int kx = startX + i * (keyW + 1);
            tft.fillRoundRect(kx, ky, keyW, keyH, 2, tft.color565(50, 55, 70));
            char ch = rows[r][i];
            if (ch >= 'A' && ch <= 'Z' && !settingsPassCaps)
                ch = (char)(ch - 'A' + 'a');
            char s[2] = {ch, 0};
            tft.setTextColor(TFT_WHITE, tft.color565(50, 55, 70));
            tft.setTextDatum(MC_DATUM);
            tft.drawString(s, kx + keyW / 2, ky + keyH / 2, 1);
        }
    }
    // caps / del / space / ok
    tft.fillRoundRect(8, SCREEN_HEIGHT - 26, 40, 22, 2, tft.color565(70, 80, 100));
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE);
    tft.drawString(settingsPassCaps ? "ABC" : "abc", 28, SCREEN_HEIGHT - 15, 1);
    tft.fillRoundRect(52, SCREEN_HEIGHT - 26, 50, 22, 2, tft.color565(90, 50, 50));
    tft.drawString("DEL", 77, SCREEN_HEIGHT - 15, 1);
    tft.fillRoundRect(106, SCREEN_HEIGHT - 26, 120, 22, 2, tft.color565(45, 50, 65));
    tft.drawString("SPACE", 166, SCREEN_HEIGHT - 15, 1);
    tft.fillRoundRect(230, SCREEN_HEIGHT - 26, 82, 22, 2, tft.color565(40, 120, 80));
    cnDrawUtf8(tft, 250, SCREEN_HEIGHT - 22, "连接", TFT_WHITE);
    tft.setTextDatum(TL_DATUM);
}

void drawSettingsScreen()
{
    tft.fillScreen(tft.color565(14, 16, 24));
    tft.fillRect(0, 0, SCREEN_WIDTH, 24, tft.color565(28, 36, 55));
    cnDrawUtf8(tft, 8, 6, "设置", TFT_WHITE);
    tft.fillRoundRect(SCREEN_WIDTH - 56, 3, 50, 18, 3, tft.color565(60, 70, 90));
    cnDrawUtf8(tft, SCREEN_WIDTH - 44, 6, "返回", TFT_CYAN);

    if (settingsPage == SETTINGS_PAGE_HOME) {
        cnDrawUtf8(tft, 8, 28, "传输模式", TFT_LIGHTGREY);
        char line[48];
        snprintf(line, sizeof(line), "%s", transportStatusLine());
        tft.setTextColor(TFT_GREENYELLOW, tft.color565(14, 16, 24));
        tft.setTextDatum(TL_DATUM);
        tft.drawString(line, 8, 44, 1);

        uint8_t mode = getLinkMode();
        // 行1：仅 ESP-NOW | 启用 WiFi
        tft.fillRoundRect(6, 62, 150, 26, 4,
                          mode == LINK_MODE_ESPNOW_ONLY ? tft.color565(40, 130, 80)
                                                        : tft.color565(40, 45, 60));
        cnDrawUtf8(tft, 28, 70, "仅ESPNOW", TFT_WHITE);
        tft.fillRoundRect(164, 62, 150, 26, 4,
                          mode == LINK_MODE_WIFI_ON ? tft.color565(40, 100, 150)
                                                    : tft.color565(40, 45, 60));
        cnDrawUtf8(tft, 190, 70, "启用WiFi", TFT_WHITE);
        // 行2：双并发 | 仅 WiFi
        tft.fillRoundRect(6, 92, 150, 26, 4,
                          mode == LINK_MODE_DUAL ? tft.color565(140, 90, 40)
                                                 : tft.color565(40, 45, 60));
        cnDrawUtf8(tft, 42, 100, "双并发", TFT_WHITE);
        tft.fillRoundRect(164, 92, 150, 26, 4,
                          mode == LINK_MODE_WIFI_ONLY ? tft.color565(50, 90, 160)
                                                       : tft.color565(40, 45, 60));
        cnDrawUtf8(tft, 200, 100, "仅WiFi", TFT_WHITE);

        if (mode != LINK_MODE_ESPNOW_ONLY) {
            if (wifiIsConnected()) {
                char ip[40];
                snprintf(ip, sizeof(ip), "IP %s", wifiLocalIp().toString().c_str());
                tft.setTextColor(TFT_CYAN, tft.color565(14, 16, 24));
                tft.drawString(ip, 8, 126, 1);
            } else if (wifiConnectInProgress()) {
                cnDrawUtf8(tft, 8, 126, "WiFi 连接中…", TFT_YELLOW);
            } else if (wifiHasSavedCreds()) {
                cnDrawUtf8(tft, 8, 126, "WiFi 未连接(有保存)", TFT_ORANGE);
            } else {
                cnDrawUtf8(tft, 8, 126, "请扫描并连接 WiFi", TFT_DARKGREY);
            }

            tft.fillRoundRect(8, 148, 140, 28, 4, tft.color565(40, 100, 140));
            cnDrawUtf8(tft, 28, 156, "扫描 WiFi", TFT_WHITE);
            tft.fillRoundRect(160, 148, 140, 28, 4, tft.color565(90, 50, 50));
            cnDrawUtf8(tft, 178, 156, "断开/忘记", TFT_WHITE);

            if (mode == LINK_MODE_WIFI_ONLY)
                cnDrawUtf8(tft, 8, 186, "对端需开WiFi(无仅ESPNOW)", TFT_LIGHTGREY);
            else if (mode == LINK_MODE_DUAL)
                cnDrawUtf8(tft, 8, 186, "已连WiFi时 ESP+WiFi 同时发", TFT_LIGHTGREY);
            else
                cnDrawUtf8(tft, 8, 186, "信号差或有WiFi对端时用WiFi", TFT_LIGHTGREY);
        } else {
            cnDrawUtf8(tft, 8, 130, "仅使用 ESP-NOW", TFT_CYAN);
            cnDrawUtf8(tft, 8, 150, "左侧显示 Signal 信号格", TFT_DARKGREY);
            cnDrawUtf8(tft, 8, 170, "仅WiFi端不可见本机", TFT_DARKGREY);
        }
    } else if (settingsPage == SETTINGS_PAGE_WIFI_LIST) {
        cnDrawUtf8(tft, 8, 30, "选择 WiFi", TFT_WHITE);
        if (!wifiScanDone()) {
            cnDrawUtf8(tft, 8, 100, "扫描中…", TFT_YELLOW);
        } else {
            int n = wifiScanCount();
            if (n == 0)
                cnDrawUtf8(tft, 8, 100, "未找到网络", TFT_DARKGREY);
            int listTop = 48;
            int rowH = 22;
            int maxShow = (SCREEN_HEIGHT - listTop - 8) / rowH;
            if (settingsWifiScroll < 0) settingsWifiScroll = 0;
            if (n > maxShow && settingsWifiScroll > n - maxShow)
                settingsWifiScroll = n - maxShow;
            for (int i = 0; i < maxShow; i++) {
                int idx = settingsWifiScroll + i;
                if (idx >= n) break;
                WifiScanItem_t it;
                if (!wifiGetScanItem(idx, &it)) continue;
                int y = listTop + i * rowH;
                uint16_t bg = (idx == settingsSelectedAp) ? tft.color565(40, 70, 100)
                                                          : tft.color565(24, 28, 40);
                tft.fillRoundRect(6, y, SCREEN_WIDTH - 12, rowH - 2, 2, bg);
                char line[40];
                snprintf(line, sizeof(line), "%s %s %ddB",
                         it.ssid, it.open ? "[open]" : "", (int)it.rssi);
                tft.setTextColor(TFT_WHITE, bg);
                tft.setTextDatum(TL_DATUM);
                tft.drawString(line, 12, y + 5, 1);
            }
        }
    } else if (settingsPage == SETTINGS_PAGE_WIFI_PASS) {
        char title[40];
        WifiScanItem_t it;
        const char *ssid = "?";
        if (settingsSelectedAp >= 0 && wifiGetScanItem(settingsSelectedAp, &it))
            ssid = it.ssid;
        snprintf(title, sizeof(title), "密码: %s", ssid);
        tft.setTextColor(TFT_WHITE, tft.color565(14, 16, 24));
        tft.setTextDatum(TL_DATUM);
        tft.drawString(title, 8, 30, 1);

        tft.fillRoundRect(8, 50, SCREEN_WIDTH - 16, 28, 3, tft.color565(30, 34, 48));
        tft.setTextColor(TFT_CYAN, tft.color565(30, 34, 48));
        tft.drawString(settingsPassBuf[0] ? settingsPassBuf : "(空=开放网络)", 14, 58, 1);
        drawSettingsPassKeyboard();
    }
    settingsNeedRedraw = false;
}

void updateSettingsScreen()
{
    if (currentUIState != UI_STATE_SETTINGS)
        return;
    bool connecting = wifiConnectInProgress();
    if (connecting != settingsWasConnecting || wifiIsConnected() || settingsNeedRedraw) {
        settingsWasConnecting = connecting;
        if (settingsPage == SETTINGS_PAGE_HOME || settingsNeedRedraw)
            drawSettingsScreen();
    }
    if (settingsPage == SETTINGS_PAGE_WIFI_LIST && wifiScanDone())
        drawSettingsScreen();
}

void showSettingsScreen()
{
    if (!isScreenOn || inCustomColorMode)
        return;
    settingsPage = SETTINGS_PAGE_HOME;
    settingsFingerDown = false;
    settingsDragging = false;
    settingsNeedRedraw = true;
    currentUIState = UI_STATE_SETTINGS;
    drawSettingsScreen();
}

void hideSettingsScreen()
{
    if (currentUIState != UI_STATE_SETTINGS)
        return;
    currentUIState = UI_STATE_MAIN;
    redrawMainScreen();
}

static bool settingsPassKeyTouch(int x, int y)
{
    const int keyH = 22;
    const int keyW = 28;
    const int startY = 118;
    const char *rows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (int r = 0; r < 4; r++) {
        int len = (int)strlen(rows[r]);
        int totalW = len * (keyW + 1);
        int startX = (SCREEN_WIDTH - totalW) / 2;
        int ky = startY + r * (keyH + 1);
        if (y < ky || y > ky + keyH) continue;
        for (int i = 0; i < len; i++) {
            int kx = startX + i * (keyW + 1);
            if (x >= kx && x <= kx + keyW) {
                char ch = rows[r][i];
                if (ch >= 'A' && ch <= 'Z' && !settingsPassCaps)
                    ch = (char)(ch - 'A' + 'a');
                size_t n = strlen(settingsPassBuf);
                if (n < WIFI_PASS_MAX) {
                    settingsPassBuf[n] = ch;
                    settingsPassBuf[n + 1] = '\0';
                    drawSettingsScreen();
                }
                return true;
            }
        }
    }
    if (y >= SCREEN_HEIGHT - 26) {
        if (x < 48) {
            settingsPassCaps = !settingsPassCaps;
            drawSettingsScreen();
            return true;
        }
        if (x < 104) {
            size_t n = strlen(settingsPassBuf);
            if (n > 0) settingsPassBuf[n - 1] = '\0';
            drawSettingsScreen();
            return true;
        }
        if (x < 228) {
            size_t n = strlen(settingsPassBuf);
            if (n < WIFI_PASS_MAX) {
                settingsPassBuf[n] = ' ';
                settingsPassBuf[n + 1] = '\0';
                drawSettingsScreen();
            }
            return true;
        }
        // 连接
        WifiScanItem_t it;
        if (settingsSelectedAp >= 0 && wifiGetScanItem(settingsSelectedAp, &it)) {
            showStatusToast("正在连接…", 2000);
            wifiConnect(it.ssid, settingsPassBuf);
            settingsPage = SETTINGS_PAGE_HOME;
            drawSettingsScreen();
        }
        return true;
    }
    return false;
}

bool handleSettingsTouch(int x, int y)
{
    if (currentUIState != UI_STATE_SETTINGS)
        return false;

    if (!settingsFingerDown) {
        settingsFingerDown = true;
        settingsDragging = false;
        settingsPressX = x;
        settingsPressY = y;
        settingsDragLastY = y;

        // 返回
        if (y <= 24 && x >= SCREEN_WIDTH - 60) {
            if (settingsPage == SETTINGS_PAGE_HOME)
                hideSettingsScreen();
            else {
                settingsPage = SETTINGS_PAGE_HOME;
                drawSettingsScreen();
            }
            return true;
        }

        if (settingsPage == SETTINGS_PAGE_HOME) {
            // 四选一：两行
            if (y >= 62 && y <= 88) {
                if (x < 160) {
                    setLinkMode(LINK_MODE_ESPNOW_ONLY);
                    showStatusToast("仅 ESP-NOW", 1200);
                } else {
                    setLinkMode(LINK_MODE_WIFI_ON);
                    showStatusToast("启用 WiFi", 1200);
                }
                drawSettingsScreen();
                return true;
            }
            if (y >= 92 && y <= 118) {
                if (x < 160) {
                    setLinkMode(LINK_MODE_DUAL);
                    showStatusToast("双并发", 1200);
                } else {
                    setLinkMode(LINK_MODE_WIFI_ONLY);
                    showStatusToast("仅 WiFi", 1200);
                }
                drawSettingsScreen();
                return true;
            }
            if (getLinkMode() != LINK_MODE_ESPNOW_ONLY && y >= 148 && y <= 176) {
                if (x < 155) {
                    settingsPage = SETTINGS_PAGE_WIFI_LIST;
                    settingsWifiScroll = 0;
                    settingsSelectedAp = -1;
                    wifiStartScan();
                    drawSettingsScreen();
                } else {
                    wifiDisconnect(true);
                    showStatusToast("已断开并忘记 WiFi", 1800);
                    drawSettingsScreen();
                }
                return true;
            }
        } else if (settingsPage == SETTINGS_PAGE_WIFI_LIST) {
            // 点击在 release 处理，支持滑动
        } else if (settingsPage == SETTINGS_PAGE_WIFI_PASS) {
            settingsPassKeyTouch(x, y);
        }
        return true;
    }

    // 拖动滚动 WiFi 列表
    if (settingsPage == SETTINGS_PAGE_WIFI_LIST) {
        if (abs(y - settingsPressY) > 6)
            settingsDragging = true;
        if (settingsDragging && settingsDragLastY >= 0) {
            int dy = settingsDragLastY - y;
            if (abs(dy) > 2) {
                settingsWifiScroll += (dy > 0) ? 1 : -1;
                if (settingsWifiScroll < 0) settingsWifiScroll = 0;
                drawSettingsScreen();
            }
        }
        settingsDragLastY = y;
    } else if (settingsPage == SETTINGS_PAGE_WIFI_PASS) {
        // 按住重复输入忽略
    }
    return true;
}

// 抬手时选 WiFi
static void settingsSelectWifiOnRelease(int x, int y)
{
    if (settingsDragging || settingsPage != SETTINGS_PAGE_WIFI_LIST)
        return;
    if (!wifiScanDone())
        return;
    int listTop = 48;
    int rowH = 22;
    int n = wifiScanCount();
    int maxShow = (SCREEN_HEIGHT - listTop - 8) / rowH;
    for (int i = 0; i < maxShow; i++) {
        int idx = settingsWifiScroll + i;
        if (idx >= n) break;
        int ry = listTop + i * rowH;
        if (y >= ry && y < ry + rowH) {
            settingsSelectedAp = idx;
            WifiScanItem_t it;
            if (wifiGetScanItem(idx, &it)) {
                if (it.open) {
                    settingsPassBuf[0] = '\0';
                    wifiConnect(it.ssid, "");
                    settingsPage = SETTINGS_PAGE_HOME;
                    showStatusToast("正在连接开放网络…", 2000);
                } else {
                    settingsPassBuf[0] = '\0';
                    settingsPage = SETTINGS_PAGE_WIFI_PASS;
                }
                drawSettingsScreen();
            }
            return;
        }
    }
}

void settingsTouchReleased()
{
    if (settingsFingerDown && !settingsDragging)
        settingsSelectWifiOnRelease(settingsPressX, settingsPressY);
    settingsFingerDown = false;
    settingsDragging = false;
    settingsDragLastY = -1;
}
