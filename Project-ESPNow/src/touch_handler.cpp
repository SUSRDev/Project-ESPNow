#include "touch_handler.h"
#include "config.h"
#include <Arduino.h> // 用于 millis, delay, Serial, map 等

// 包含依赖模块的头文件
#include "ui_manager.h"       // 用于UI函数和状态 (inCustomColorMode, currentColor, currentUIState 等)
#include "esp_now_handler.h"  // 用于ESP-NOW函数 (sendSyncMessage) 和数据 (allDrawingHistory 等)
#include "drawing_history.h" // 包含自定义绘图历史头文件

// --- 静态 (文件局部) 全局变量，用于触摸处理状态 ---
static TS_Point lastLocalPoint = {0, 0, 0};  // 本地最后一次触摸点坐标
static unsigned long lastLocalTouchTime = 0; // 本地最后一次触摸事件的时间戳

// 来自 ui_manager 的外部变量
extern bool isEraserMode; // 橡皮擦模式状态
extern int eraserRadius; // 当前橡皮擦半径
extern int brushRadius;
extern bool isEraserSliderVisible; // 橡皮擦滑块是否可见
extern bool isBrushSliderVisible;

// 来自 Project-ESPNow.ino 的外部变量
extern SPIClass mySpi; // SPI对象

// 彩蛋相关变量，现为本模块局部变量
static unsigned long lastResetTime = 0;
static int resetPressCount = 0;

// 橡皮擦按钮防抖
static unsigned long lastEraserButtonTime = 0;
#define ERASER_BUTTON_DEBOUNCE_TIME 300 // 橡皮擦按钮防抖时间（毫秒）

// 对端信息按钮长按 -> 编辑设备标识
static unsigned long peerInfoPressStart = 0;
static bool peerInfoLongPressHandled = false;

// 主界面操作按钮：仅落笔瞬间触发一次
static bool mainUiFingerDown = false;
static bool mainUiPressConsumed = false;
static bool clearConfirmFingerDown = false;
static bool screenOffTouchDown = false;

// --- 函数实现 ---

// SPI总线切换函数 - 初始化SPI为触摸屏配置
void initTouchSPI() {
    mySpi.end();
    mySpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    mySpi.setFrequency(2500000);
    mySpi.setDataMode(SPI_MODE0);
    mySpi.setBitOrder(MSBFIRST);
    ts.begin(mySpi);
    ts.setRotation(1);
}

// SPI总线切换函数 - 初始化SPI为SD卡配置
void initSDSPI() {
    mySpi.end();
    mySpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    mySpi.setFrequency(SD_SPI_HZ);
    mySpi.setDataMode(SPI_MODE0);
    mySpi.setBitOrder(MSBFIRST);
}

void touchHandlerInit() {
    // 如果将来需要任何触摸相关的特定初始化，则为占位符
    // 例如：ts.setThreshold(某个值);
}

// 触摸点平均值计算
XY_TouchPoint_t averageXY() { // 返回类型已更改为 XY_TouchPoint_t
    // TS_Point p = ts.getPoint(); // 初始的 getPoint 由于循环似乎是多余的
    bool fly = false;
    int cnt = 0;
    int i, j, k, min_idx, temp_val; // 将 min 重命名为 min_idx，temp 重命名为 temp_val，以避免与潜在函数冲突
    int tmp[2][10]; // 10个样本的缓冲区

    for (cnt = 0; cnt < 10; cnt++) { // 收集10个样本
        TS_Point p_sample = ts.getPoint();
        if (p_sample.z > 200) { // 检查压力阈值
            tmp[0][cnt] = p_sample.x;
            tmp[1][cnt] = p_sample.y;
            delay(2); // 样本之间的小延迟
        } else {
            fly = true; // 无效触摸 (太轻或无触摸)
            break;
        }
    }

    XY_TouchPoint_t resultXY; // 已更改类型
    if (fly || cnt < 4) { // 如果标记为飞点或有效样本不足 (例如，平均中间4个点至少需要4个)
        resultXY.fly = true;
        return resultXY;
    }

    // 对样本进行排序以移除异常值 (对于小N使用简单冒泡排序)
    for (k = 0; k < 2; k++) { // 对于 X (0) 和 Y (1)
        for (i = 0; i < cnt - 1; i++) {
            min_idx = i;
            for (j = i + 1; j < cnt; j++) {
                if (tmp[k][min_idx] > tmp[k][j]) {
                    min_idx = j;
                }
            }
            // 交换
            temp_val = tmp[k][i];
            tmp[k][i] = tmp[k][min_idx];
            tmp[k][min_idx] = temp_val;
        }
    }

    // 平均中间样本
    // 对于不同的cnt值，选择合适的中间样本
    if (cnt >= 10) {
        // 如果有10个样本，平均中间4个 (索引3,4,5,6)
        resultXY.x = (tmp[0][3] + tmp[0][4] + tmp[0][5] + tmp[0][6]) / 4.0f;
        resultXY.y = (tmp[1][3] + tmp[1][4] + tmp[1][5] + tmp[1][6]) / 4.0f;
        resultXY.fly = false;
    } else if (cnt >= 7) {
        // 如果有7-9个样本，平均中间3个
        int mid = cnt / 2;
        resultXY.x = (tmp[0][mid-1] + tmp[0][mid] + tmp[0][mid+1]) / 3.0f;
        resultXY.y = (tmp[1][mid-1] + tmp[1][mid] + tmp[1][mid+1]) / 3.0f;
        resultXY.fly = false;
    } else if (cnt >= 4) {
        // 如果有4-6个样本，平均所有样本
        int sumX = 0, sumY = 0;
        for (int i = 0; i < cnt; i++) {
            sumX += tmp[0][i];
            sumY += tmp[1][i];
        }
        resultXY.x = sumX / (float)cnt;
        resultXY.y = sumY / (float)cnt;
        resultXY.fly = false;
    } else { // 点数不足
        resultXY.fly = true;
    }
    return resultXY;
}


// 处理本地触摸输入
void handleLocalTouch() {
    // 来自 ui_manager.h 的依赖项:
    // extern bool inCustomColorMode;
    // extern uint32_t currentColor;
    // extern UIState_t currentUIState; // 新增
    // ... (其他UI相关函数)

    // 来自 esp_now_handler.h 的依赖项:
    extern DrawingHistory allDrawingHistory;
    // extern long relativeBootTimeOffset;
    // ... (其他ESP-NOW相关函数和变量)

    // tft 和 ts 是来自 touch_handler.h 的 extern 变量 (在 .ino 中定义)

    float x1, y1;
    bool touched = ts.tirqTouched() && ts.touched(); // 检查IRQ，然后通过压力确认
    unsigned long currentRawUptime = millis();       // 此触摸事件的本地原始运行时间
    XY_TouchPoint_t xy1;

    if (touched) {
        noteUserActivity();

        // 息屏中：触摸只负责唤醒，本笔不画画/不点按钮
        if (!isScreenOn) {
            if (!screenOffTouchDown) {
                screenOffTouchDown = true;
                setScreenPower(true);
            }
            lastLocalPoint.z = 0;
            return;
        }

        // 首先处理弹窗关闭逻辑 (Coffee 弹窗优先于项目信息弹窗，如果两者都可能存在)
        if (isPrivInviteDialogVisible()) {
            xy1 = averageXY();
            if (!xy1.fly) {
                int mapX = map(xy1.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, SCREEN_WIDTH);
                int mapY = map(xy1.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, SCREEN_HEIGHT);
                handlePrivInviteTouch(mapX, mapY);
                lastLocalPoint.z = 0;
                return;
            }
        } else if (isClearConfirmVisible) {
            xy1 = averageXY();
            if (!xy1.fly) {
                if (!clearConfirmFingerDown) {
                    clearConfirmFingerDown = true;
                    int mapX = map(xy1.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, SCREEN_WIDTH);
                    int mapY = map(xy1.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, SCREEN_HEIGHT);
                    handleClearConfirmTouch(mapX, mapY);
                }
                lastLocalPoint.z = 0;
                return;
            }
        } else if (isCoffeePopupVisible) {
            xy1 = averageXY(); // 确认是有效触摸
            if (!xy1.fly) {
                hideCoffeePopup(); // 关闭 Coffee 弹窗
                lastLocalPoint.z = 0; // 标记为无触摸
                return; // 操作已处理
            }
        } else if (isProjectInfoPopupVisible) { // 然后处理项目信息弹窗的关闭逻辑
            xy1 = averageXY();
            if (!xy1.fly) {
                 hideProjectInfoPopup(); // 关闭弹窗
                 lastLocalPoint.z = 0; // 标记为无触摸, 避免后续处理
                 return; // 操作已处理
            }
        }

        xy1 = averageXY(); // 获取滤波后的触摸点
        x1 = xy1.x;
        y1 = xy1.y;

        if (!xy1.fly) { // 如果触摸点有效
            // 将原始触摸坐标映射到屏幕坐标
            int mapX = map(x1, TOUCH_MIN_X, TOUCH_MAX_X, 0, SCREEN_WIDTH);
            int mapY = map(y1, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, SCREEN_HEIGHT);

            // 根据当前 UI 状态处理触摸事件
            switch (currentUIState) {
                case UI_STATE_NAME_EDIT:
                    handleNameEditTouch(mapX, mapY);
                    break;

                case UI_STATE_CHAT:
                    handleChatTouch(mapX, mapY);
                    break;

                case UI_STATE_ONLINE_LIST:
                    handleOnlineListTouch(mapX, mapY);
                    break;

                case UI_STATE_SETTINGS:
                    handleSettingsTouch(mapX, mapY);
                    break;

                case UI_STATE_MAIN: // 主绘图界面
                    if (inCustomColorMode) {
                        handleCustomColorTouch(mapX, mapY);
                    } else {
                        bool mainRising = !mainUiFingerDown;
                        mainUiFingerDown = true;

                        if (isSettingsButtonPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                showSettingsScreen();
                            }
                            return;
                        }

                        if (isSignalInfoPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                showOnlineListScreen();
                            }
                            return;
                        }

                        if (isChatJoinButtonPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                showChatRoom();
                            }
                            return;
                        }

                        if (isDebugInfoVisible && isInfoButtonPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                showProjectInfoPopup();
                            }
                            return;
                        }

                        if (isDebugInfoVisible) {
                            if (mapX >= 2 && mapX <= 122 && mapY >= (SCREEN_HEIGHT - 42) && mapY <= SCREEN_HEIGHT) {
                                if (!isInfoButtonPressed(mapX, mapY)) {
                                    if (mainRising && !mainUiPressConsumed) {
                                        mainUiPressConsumed = true;
                                        toggleDebugInfo();
                                    }
                                    return;
                                }
                            }
                        }

                        if (isResetButtonPressed(mapX, mapY)) {
                            if (!(mainRising && !mainUiPressConsumed))
                                return;
                            mainUiPressConsumed = true;
                            if (currentRawUptime - lastResetTime < 1000) {
                                resetPressCount++;
                            } else {
                                resetPressCount = 1;
                            }
                            lastResetTime = currentRawUptime;

                            if (resetPressCount >= 10) {
                                Serial.println("Kurio Reiko thanks all the recognition and redistribution,");
                                Serial.println("but if someone commercializes this project without declaring Kurio Reiko's originality, then he is a bitch");
                                resetPressCount = 0;
                            }

                            showClearConfirm(CLEAR_CONFIRM_ALL);
                            // 开弹窗的这次按住还没松开，避免被当成「点外面」立刻关掉
                            clearConfirmFingerDown = true;
                            return;
                        }

                        uint32_t selectedColorHolder;
                        if (isColorButtonPressed(mapX, mapY, selectedColorHolder)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                updateCurrentColor(selectedColorHolder);
                                redrawStarButton();
                                isEraserMode = false;
                                isEraserSliderVisible = false;
                                isBrushSliderVisible = false;
                                redrawEraserButton();
                                redrawBrushButton();
                            }
                            return;
                        }

                        if (isEraserButtonPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed &&
                                currentRawUptime - lastEraserButtonTime >= ERASER_BUTTON_DEBOUNCE_TIME) {
                                mainUiPressConsumed = true;
                                isEraserMode = !isEraserMode;
                                isEraserSliderVisible = isEraserMode;
                                isBrushSliderVisible = false;
                                lastEraserButtonTime = currentRawUptime;
                                if (!isEraserSliderVisible) {
                                    redrawMainScreen();
                                } else {
                                    redrawEraserButton();
                                    drawEraserSlider();
                                    redrawBrushButton();
                                }
                            }
                            return;
                        }

                        if (isEraserSliderPressed(mapX, mapY)) {
                            handleEraserSliderTouch(mapX, mapY);
                            return;
                        }

                        if (isBrushButtonPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                isBrushSliderVisible = !isBrushSliderVisible;
                                if (isBrushSliderVisible) {
                                    isEraserMode = false;
                                    isEraserSliderVisible = false;
                                    redrawEraserButton();
                                    redrawBrushButton();
                                    drawBrushSlider();
                                } else {
                                    redrawMainScreen();
                                }
                            }
                            return;
                        }

                        if (isBrushSliderPressed(mapX, mapY)) {
                            handleBrushSliderTouch(mapX, mapY);
                            return;
                        }

                        if (isPeerInfoButtonPressed(mapX, mapY)) {
                            if (peerInfoPressStart == 0) {
                                peerInfoPressStart = currentRawUptime;
                                peerInfoLongPressHandled = false;
                            } else if (!peerInfoLongPressHandled &&
                                       (currentRawUptime - peerInfoPressStart >= DEVICE_ID_LONG_PRESS_MS)) {
                                peerInfoLongPressHandled = true;
                                showNameEditScreen();
                            }
                            return;
                        } else {
                            peerInfoPressStart = 0;
                            peerInfoLongPressHandled = false;
                        }

                        if (isUndoButtonPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                handleCanvasUndo();
                            }
                            return;
                        }

                        if (isRedoButtonPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                handleCanvasRedo();
                            }
                            return;
                        }

                        if (isCustomColorButtonPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                inCustomColorMode = true;
                                currentUIState = UI_STATE_COLOR_PICKER;
                                saveScreenArea();
                                drawColorSelectors();
                                hideStarButton();
                            }
                            return;
                        }

                        if (isScreenshotButtonPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                saveScreenshotToSD();
                            }
                            return;
                        }

                        if (isCanvasPagePrevPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                handleCanvasPagePrev();
                            }
                            return;
                        }

                        if (isCanvasPageNextPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                handleCanvasPageNext();
                            }
                            return;
                        }

                        if (isCanvasFlipPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                handleCanvasFlip();
                            }
                            return;
                        }

                        if (isCanvasExitPrivPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                handleCanvasExitPriv();
                            }
                            return;
                        }

                        if (isCanvasPageClearPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                if (!canvasPageHasContent(currentCanvasPage)) {
                                    showStatusToast("本页为空", 1000);
                                } else {
                                    showClearConfirm(CLEAR_CONFIRM_PAGE);
                                    clearConfirmFingerDown = true;
                                }
                            }
                            return;
                        }

                        if (isEraserSliderVisible) {
                            isEraserSliderVisible = false;
                            redrawMainScreen();
                        }
                        if (isBrushSliderVisible) {
                            isBrushSliderVisible = false;
                            redrawMainScreen();
                        }

                        if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
                            return;

                        uint32_t drawColor = isEraserMode ? TFT_BLACK : currentColor;
                        if (isEraserMode) {
                            bool hitUi = false;
                            int r = eraserRadius;
                            if (lastLocalPoint.z != 0 &&
                                (currentRawUptime - lastLocalTouchTime <= TOUCH_STROKE_INTERVAL)) {
                                hitUi = applyEraserSegment(lastLocalPoint.x, lastLocalPoint.y,
                                                           mapX, mapY, r);
                            } else {
                                hitUi = applyEraserDot(mapX, mapY, r);
                            }
                            if (hitUi)
                                redrawUiChrome();
                            setActivityStatus(getLocalDeviceId(), "在擦");
                        } else {
                            int r = brushRadius;
                            if (currentRawUptime - lastLocalTouchTime > TOUCH_STROKE_INTERVAL || lastLocalPoint.z == 0) {
                                applyBrushDot(mapX, mapY, drawColor, r);
                            } else {
                                applyBrushSegment(lastLocalPoint.x, lastLocalPoint.y, mapX, mapY, drawColor, r);
                            }
                            setActivityStatus(getLocalDeviceId(), "在画");
                        }

                        lastLocalPoint = {mapX, mapY, 1};
                        lastLocalTouchTime = currentRawUptime;

                        TouchData_t currentDrawPoint;
                        currentDrawPoint.x = mapX;
                        currentDrawPoint.y = mapY;
                        currentDrawPoint.timestamp = currentRawUptime;
                        currentDrawPoint.isReset = false;
                        currentDrawPoint.color = drawColor;
                        currentDrawPoint.brushR = isEraserMode ? (uint8_t)eraserRadius : (uint8_t)brushRadius;
                        currentDrawPoint.page = currentCanvasPage;

                        clearCanvasRedoStack();
                        allDrawingHistory.push_back(currentDrawPoint);

                        SyncMessage_t drawMsg;
                        drawMsg.type = MSG_TYPE_DRAW_POINT;
                        drawMsg.senderUptime = currentRawUptime;
                        drawMsg.senderOffset = relativeBootTimeOffset;
                        drawMsg.touch_data = currentDrawPoint;
                        sendSyncMessage(&drawMsg);
                    }
                    break; // End of UI_STATE_MAIN case

                case UI_STATE_COLOR_PICKER: // 颜色选择器界面
                    handleCustomColorTouch(mapX, mapY); // 触摸处理已在 ui_manager 中实现
                    break; // End of UI_STATE_COLOR_PICKER case

                case UI_STATE_POPUP: // 弹窗界面 (项目信息或 Coffee)
                    // 弹窗触摸处理已在 handleLocalTouch 开头处理，这里无需额外逻辑
                    break; // End of UI_STATE_POPUP case

                case UI_STATE_PEER_INFO: // 对端信息界面
                    // 检查返回按钮
                    if (isPeerInfoScreenBackButtonPressed(mapX, mapY)) { // 更正为 isPeerInfoScreenBackButtonPressed
                        Serial.println("Peer Info Back button pressed. Hiding Peer Info Screen."); // 添加调试输出
                        hidePeerInfoScreen(); // 返回主界面
                        return; // 操作已处理
                    }
                    // SET ID 按钮区域 (左下)
                    if (mapX >= 10 && mapX <= 90 && mapY >= SCREEN_HEIGHT - 28 && mapY <= SCREEN_HEIGHT - 8) {
                        showNameEditScreen();
                        return;
                    }
                    // 如果将来对端信息界面有其他可交互元素，可以在这里添加处理逻辑
                    break; // End of UI_STATE_PEER_INFO case

                default:
                    // 未知状态，可能需要默认行为或错误处理
                    break;
            }
        }
    } else { // 当前未检测到触摸
        mainUiFingerDown = false;
        mainUiPressConsumed = false;
        clearConfirmFingerDown = false;
        screenOffTouchDown = false;
        if (currentUIState == UI_STATE_CHAT)
            chatTouchReleased();
        if (currentUIState == UI_STATE_NAME_EDIT)
            nameEditTouchReleased();
        if (currentUIState == UI_STATE_ONLINE_LIST)
            onlineListTouchReleased();
        if (currentUIState == UI_STATE_SETTINGS)
            settingsTouchReleased();
        // 对端信息按钮抬起：短按打开对端列表
        if (peerInfoPressStart != 0 && !peerInfoLongPressHandled) {
            showPeerInfoScreen();
        }
        peerInfoPressStart = 0;
        peerInfoLongPressHandled = false;
        // 橡皮擦抬起后补绘一次 UI，防止边缘擦花按钮
        if (isEraserMode && lastLocalPoint.z != 0) {
            redrawUiChrome();
        }
        lastLocalPoint.z = 0; // 标记为无触摸 (压力 = 0)
    }
}
