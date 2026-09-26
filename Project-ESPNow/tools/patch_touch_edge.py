# -*- coding: utf-8 -*-
from pathlib import Path

p = Path(r"d:\esp32now\Project-ESPNow\src\touch_handler.cpp")
t = p.read_text(encoding="utf-8")

start = t.find("                case UI_STATE_MAIN: //")
end = t.find("                    break; // End of UI_STATE_MAIN case", start)
if start < 0 or end < 0:
    raise SystemExit(f"markers not found start={start} end={end}")
end = end + len("                    break; // End of UI_STATE_MAIN case")

new = r'''                case UI_STATE_MAIN: // 主绘图界面
                    if (inCustomColorMode) {
                        handleCustomColorTouch(mapX, mapY);
                    } else {
                        bool mainRising = !mainUiFingerDown;
                        mainUiFingerDown = true;

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

                            allDrawingHistory.clear();
                            clearScreenAndCache();

                            relativeBootTimeOffset = 0;
                            iamEffectivelyMoreUptimeDevice = false;
                            iamRequestingAllData = false;
                            initialSyncLogicProcessed = false;

                            SyncMessage_t resetMsg;
                            resetMsg.type = MSG_TYPE_RESET_CANVAS;
                            resetMsg.senderUptime = currentRawUptime;
                            resetMsg.senderOffset = relativeBootTimeOffset;
                            resetMsg.touch_data.isReset = true;
                            resetMsg.touch_data.timestamp = currentRawUptime;
                            resetMsg.touch_data.x = 0;
                            resetMsg.touch_data.y = 0;
                            resetMsg.touch_data.color = currentColor;
                            sendSyncMessage(&resetMsg);
                            return;
                        }

                        uint32_t selectedColorHolder;
                        if (isColorButtonPressed(mapX, mapY, selectedColorHolder)) {
                            if (mainRising && !mainUiPressConsumed) {
                                mainUiPressConsumed = true;
                                updateCurrentColor(selectedColorHolder);
                                redrawStarButton();
                                isEraserMode = false;
                                redrawEraserButton();
                            }
                            return;
                        }

                        if (isEraserButtonPressed(mapX, mapY)) {
                            if (mainRising && !mainUiPressConsumed &&
                                currentRawUptime - lastEraserButtonTime >= ERASER_BUTTON_DEBOUNCE_TIME) {
                                mainUiPressConsumed = true;
                                isEraserMode = !isEraserMode;
                                isEraserSliderVisible = isEraserMode;
                                lastEraserButtonTime = currentRawUptime;
                                if (!isEraserSliderVisible) {
                                    redrawMainScreen();
                                } else {
                                    redrawEraserButton();
                                    drawEraserSlider();
                                }
                            }
                            return;
                        }

                        if (isEraserSliderPressed(mapX, mapY)) {
                            handleEraserSliderTouch(mapX, mapY);
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

                        if (isEraserSliderVisible) {
                            isEraserSliderVisible = false;
                            redrawMainScreen();
                        }

                        if (currentUIState != UI_STATE_MAIN || inCustomColorMode)
                            return;

                        uint32_t drawColor = isEraserMode ? TFT_BLACK : currentColor;
                        if (isEraserMode) {
                            if (!safeEraserFill(mapX, mapY, eraserRadius)) {
                                redrawUiChrome();
                                lastLocalPoint = {mapX, mapY, 1};
                                lastLocalTouchTime = currentRawUptime;
                                return;
                            }
                        } else {
                            if (currentRawUptime - lastLocalTouchTime > TOUCH_STROKE_INTERVAL || lastLocalPoint.z == 0) {
                                tft.drawPixel(mapX, mapY, drawColor);
                            } else {
                                tft.drawLine(lastLocalPoint.x, lastLocalPoint.y, mapX, mapY, drawColor);
                            }
                            setDrawingStatus(getLocalDeviceId());
                        }

                        lastLocalPoint = {mapX, mapY, 1};
                        lastLocalTouchTime = currentRawUptime;

                        TouchData_t currentDrawPoint;
                        currentDrawPoint.x = mapX;
                        currentDrawPoint.y = mapY;
                        currentDrawPoint.timestamp = currentRawUptime;
                        currentDrawPoint.isReset = false;
                        currentDrawPoint.color = drawColor;

                        allDrawingHistory.push_back(currentDrawPoint);

                        SyncMessage_t drawMsg;
                        drawMsg.type = MSG_TYPE_DRAW_POINT;
                        drawMsg.senderUptime = currentRawUptime;
                        drawMsg.senderOffset = relativeBootTimeOffset;
                        drawMsg.touch_data = currentDrawPoint;
                        sendSyncMessage(&drawMsg);
                    }
                    break; // End of UI_STATE_MAIN case'''

t = t[:start] + new + t[end:]

marker = "    } else { // 当前未检测到触摸\n"
idx = t.find(marker)
if idx < 0:
    raise SystemExit("release marker not found")
insert = marker + "        mainUiFingerDown = false;\n        mainUiPressConsumed = false;\n"
if "mainUiFingerDown = false" not in t[idx:idx + 180]:
    t = t[:idx] + insert + t[idx + len(marker):]

p.write_text(t, encoding="utf-8")
print("touch_handler patched")
