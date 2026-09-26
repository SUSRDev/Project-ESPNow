#ifndef CONFIG_H
#define CONFIG_H

// 触摸屏引脚定义
#define XPT2046_IRQ 36     // XPT2046 中断引脚
#define XPT2046_MOSI 32    // XPT2046 SPI MOSI 引脚
#define XPT2046_MISO 39    // XPT2046 SPI MISO 引脚
#define XPT2046_CLK 25     // XPT2046 SPI 时钟引脚
#define XPT2046_CS 33      // XPT2046 SPI 片选引脚

// IO0 按钮引脚定义
#define BUTTON_IO0 0       // GPIO 0, 通常是 Boot 按钮

// LED 及背光引脚定义
#define TFT_BL 21          // GPIO 21 用于控制 TFT 背光
#define GREEN_LED 16       // GPIO 16 用于控制绿色 LED
#define BLUE_LED 17        // GPIO 17 用于控制蓝色 LED
#define RED_LED 22         // GPIO 22 用于控制红色 LED

// 电池检测引脚定义
#define BATTERY_PIN 34     // GPIO 34, 用于读取电池电压

// TF卡引脚定义
#define SD_CS 5           // TF卡片选信号
#define SD_MOSI 23       // TF卡SPI总线写数据信号（MicroSD 卡和SPI外设共用）
#define SD_MISO 19        // TF卡SPI总线读数据信号（MicroSD 卡和SPI外设共用）
#define SD_SCK 18         // TF卡SPI总线时钟信号（MicroSD 卡和SPI外设共用）

// 触摸校准参数
#define TOUCH_MIN_X 200    // 触摸 X 轴最小值
#define TOUCH_MAX_X 3700   // 触摸 X 轴最大值
#define TOUCH_MIN_Y 300    // 触摸 Y 轴最小值
#define TOUCH_MAX_Y 3800   // 触摸 Y 轴最大值

// 重置按钮位置和大小（紧凑，多留给画布）
#define RESET_BUTTON_X 2
#define RESET_BUTTON_Y 2
#define RESET_BUTTON_W 22
#define RESET_BUTTON_H 8

// 颜色按钮位置和大小
#define COLOR_BUTTON_WIDTH 14
#define COLOR_BUTTON_HEIGHT 12
#define COLOR_BUTTON_START_Y (RESET_BUTTON_Y + RESET_BUTTON_H + 2)
#define COLOR_BUTTON_SPACING 2

// 橡皮擦按钮位置和大小 (圆形)
#define ERASER_BUTTON_X (RESET_BUTTON_X + COLOR_BUTTON_WIDTH / 2)
#define ERASER_BUTTON_Y (COLOR_BUTTON_START_Y + (COLOR_BUTTON_HEIGHT + COLOR_BUTTON_SPACING) * 4 + ERASER_BUTTON_RADIUS)
#define ERASER_BUTTON_RADIUS 5

// 对端信息按钮位置和大小
#define PEER_INFO_BUTTON_X RESET_BUTTON_X
#define PEER_INFO_BUTTON_Y (ERASER_BUTTON_Y + ERASER_BUTTON_RADIUS + 2)
#define PEER_INFO_BUTTON_W 8
#define PEER_INFO_BUTTON_H 8

// 截屏按钮位置和大小
#define SCREENSHOT_BUTTON_X (SCREEN_WIDTH - COLOR_BUTTON_WIDTH - 1)
#define SCREENSHOT_BUTTON_Y (SCREEN_HEIGHT - COLOR_BUTTON_HEIGHT - 1)
#define SCREENSHOT_BUTTON_W COLOR_BUTTON_WIDTH
#define SCREENSHOT_BUTTON_H COLOR_BUTTON_HEIGHT

// 画布翻页按钮（截屏左侧）
#define CANVAS_PAGE_BTN_W COLOR_BUTTON_WIDTH
#define CANVAS_PAGE_BTN_H COLOR_BUTTON_HEIGHT
#define CANVAS_PAGE_NEXT_X (SCREENSHOT_BUTTON_X - CANVAS_PAGE_BTN_W - 1)
#define CANVAS_PAGE_PREV_X (CANVAS_PAGE_NEXT_X - CANVAS_PAGE_BTN_W - 1)
#define CANVAS_PAGE_CLEAR_X (CANVAS_PAGE_PREV_X - CANVAS_PAGE_BTN_W - 1)
#define CANVAS_FLIP_X (CANVAS_PAGE_CLEAR_X - CANVAS_PAGE_BTN_W - 1) // 屏幕翻转（C 左侧）
#define CANVAS_PAGE_BTN_Y SCREENSHOT_BUTTON_Y
#define CANVAS_MAX_PAGES 8
#define SCREEN_ROT_PREF_KEY "rot"
#define SCREEN_ROT_DEFAULT 1
#define SCREEN_ROT_FLIPPED 3

// 画布页控制动作（SyncMessage.touch_data.color）
#define CANVAS_PAGE_ACT_CREATE 1
#define CANVAS_PAGE_ACT_DELETE 2
#define CANVAS_PAGE_ACT_INFO   3 // 仅同步页数，不强制切页
#define CANVAS_PAGE_ACT_CLEAR  4 // 仅清空指定页笔迹，不删页
#define CANVAS_PAGE_ACT_UNDO   5 // 撤销当前页最后一笔
#define CANVAS_PAGE_ACT_REDO   6 // 预留（重做通过补发 DRAW_POINT）

// 清空确认弹窗
#define CONFIRM_POPUP_W 220
#define CONFIRM_POPUP_H 110
#define CONFIRM_POPUP_X ((SCREEN_WIDTH - CONFIRM_POPUP_W) / 2)
#define CONFIRM_POPUP_Y ((SCREEN_HEIGHT - CONFIRM_POPUP_H) / 2)
#define CONFIRM_BTN_W 80
#define CONFIRM_BTN_H 28
#define CONFIRM_BTN_Y (CONFIRM_POPUP_Y + CONFIRM_POPUP_H - CONFIRM_BTN_H - 12)
#define CONFIRM_CANCEL_X (CONFIRM_POPUP_X + 18)
#define CONFIRM_OK_X (CONFIRM_POPUP_X + CONFIRM_POPUP_W - CONFIRM_BTN_W - 18)

// 在线列表刷新间隔
#define ONLINE_PANEL_REFRESH_MS 500UL

// 左侧信号强度显示
#define SIGNAL_INFO_X 1
#define SIGNAL_INFO_Y (RECEIVE_PROGRESS_Y + PROGRESS_CIRCLE_RADIUS + 4)
#define SIGNAL_INFO_W 30
#define SIGNAL_INFO_H 28
#define SIGNAL_PEER_ROTATE_MS 5000UL

// 橡皮擦相关常量
#define ERASER_COLOR TFT_WHITE      // 橡皮擦颜色 (白色)
#define ERASER_ACTIVE_COLOR TFT_CYAN // 橡皮擦激活时颜色 (青色)
#define ERASER_MIN_RADIUS 1        // 橡皮擦最小半径
#define ERASER_MAX_RADIUS 20       // 橡皮擦最大半径

// 橡皮擦滑块配置
#define ERASER_SLIDER_X (ERASER_BUTTON_X + ERASER_BUTTON_RADIUS + 6)
#define ERASER_SLIDER_Y ERASER_BUTTON_Y
#define ERASER_SLIDER_WIDTH 14
#define ERASER_SLIDER_HEIGHT 80
#define ERASER_SLIDER_HANDLE_W 20
#define ERASER_SLIDER_HANDLE_H 14
#define ERASER_SLIDER_HIT_PAD 12
// +/- 快捷键（滑条右侧）
#define ERASER_PM_BTN_W 18
#define ERASER_PM_BTN_H 18
#define ERASER_PM_BTN_X (ERASER_SLIDER_X + ERASER_SLIDER_HANDLE_W + 4)

// 对端信息界面相关常量
#define MAX_PEERS_TO_DISPLAY 8 // 对端信息界面最多显示的对端数量
#define PEER_INFO_UPDATE_INTERVAL 500UL // 对端信息界面更新间隔 (毫秒)

// 自定义颜色按钮 ("*") 位置和大小
#define CUSTOM_COLOR_BUTTON_X (SCREEN_WIDTH - COLOR_BUTTON_WIDTH - 2)
#define CUSTOM_COLOR_BUTTON_Y 2
#define CUSTOM_COLOR_BUTTON_W COLOR_BUTTON_WIDTH
#define CUSTOM_COLOR_BUTTON_H COLOR_BUTTON_HEIGHT

// 撤销 / 重做（颜色 * 左侧：撤 | 重 | *）
#define UNDO_REDO_BTN_W COLOR_BUTTON_WIDTH
#define UNDO_REDO_BTN_H COLOR_BUTTON_HEIGHT
#define UNDO_REDO_BTN_Y CUSTOM_COLOR_BUTTON_Y
#define REDO_BUTTON_X (CUSTOM_COLOR_BUTTON_X - UNDO_REDO_BTN_W - 2)
#define UNDO_BUTTON_X (REDO_BUTTON_X - UNDO_REDO_BTN_W - 2)
#define CANVAS_REDO_STACK_MAX 20

// 笔粗细按钮（* 下方）
#define BRUSH_BUTTON_X CUSTOM_COLOR_BUTTON_X
#define BRUSH_BUTTON_Y (CUSTOM_COLOR_BUTTON_Y + CUSTOM_COLOR_BUTTON_H + 2)
#define BRUSH_BUTTON_W CUSTOM_COLOR_BUTTON_W
#define BRUSH_BUTTON_H CUSTOM_COLOR_BUTTON_H
#define BRUSH_MIN_RADIUS 1
#define BRUSH_MAX_RADIUS 12
#define BRUSH_SLIDER_X (BRUSH_BUTTON_X - 12)
#define BRUSH_SLIDER_Y (BRUSH_BUTTON_Y + BRUSH_BUTTON_H / 2)
#define BRUSH_SLIDER_WIDTH 5
#define BRUSH_SLIDER_HEIGHT 56
#define BRUSH_SLIDER_HANDLE_W 8
#define BRUSH_SLIDER_HANDLE_H 7

// 返回按钮 (调色界面中使用) 位置和大小
#define BACK_BUTTON_X (SCREEN_WIDTH - COLOR_BUTTON_WIDTH - 4)     // 返回按钮 X 坐标 (屏幕右侧)
#define BACK_BUTTON_Y (SCREEN_HEIGHT - COLOR_BUTTON_HEIGHT - 4) // 返回按钮 Y 坐标 (屏幕右下角)
#define BACK_BUTTON_W COLOR_BUTTON_WIDTH                          // 返回按钮宽度
#define BACK_BUTTON_H COLOR_BUTTON_HEIGHT                         // 返回按钮高度

// 屏幕宽高定义
#define SCREEN_WIDTH 320  // 屏幕宽度 (像素)
#define SCREEN_HEIGHT 240 // 屏幕高度 (像素)

// 手动定义 TFT_GRAY (灰色)
// 标准 TFT_eSPI 库通常包含此颜色，但为确保可用性在此处定义
// 格式为 RGB565, 计算方式: ( (R & 0xF8) << 8 ) | ( (G & 0xFC) << 3 ) | ( B >> 3 )
// 对于灰色 (128, 128, 128): R=128 (0x80), G=128 (0x80), B=128 (0x80)
// (0x80 & 0xF8) << 8  => 0x8000
// (0x80 & 0xFC) << 3  => 0x0400 (0x80 >> 2 << 5)
// (0x80 >> 3)         => 0x0010
// 0x8000 | 0x0400 | 0x0010 = 0x8410
#define TFT_GRAY 0x8410

// ESP-NOW 通信相关常量
#define BROADCAST_INTERVAL 2000             // MAC 地址发现广播间隔 (毫秒)
#define DEBUG_INFO_UPDATE_INTERVAL 500      // 调试信息更新间隔 (毫秒) — 降低刷屏开销
#define UPTIME_INFO_BROADCAST_INTERVAL 2000 // 加快 reboot 后画板恢复

// 调色界面相关常量 — 加宽滑条，底部留给返回键
#define COLOR_SLIDER_WIDTH 36
#define COLOR_SLIDER_TOUCH_PAD 28   // 触控向左扩展，边缘好点
#define COLOR_SLIDER_HEIGHT ((SCREEN_HEIGHT - BACK_BUTTON_H - 16) / 3)

// LED 调光相关常量 (息屏状态下)
#define BLUE_LED_DIM_DUTY_CYCLE 14 // 蓝色 LED 息屏时亮度 (PWM duty cycle, 0-255), 约 5% (14/255)
#define RED_LED_DIM_DUTY_CYCLE 4   // 红色 LED 息屏时亮度 (PWM duty cycle, 0-255), 约 1.5% (4/255)

// 触摸笔划间隔阈值 (毫秒)
// 小于此间隔的触摸/绘制事件被视为连续笔划的一部分
#define TOUCH_STROKE_INTERVAL 50

// ESP-NOW 同步逻辑相关常量
#define MIN_UPTIME_DIFF_FOR_NEW_SYNC_TARGET 2000UL // 更换同步源的最小 uptime 差异
#define EFFECTIVE_UPTIME_SYNC_THRESHOLD 3000UL     // 有效运行时间差阈值
#define CANVAS_RESYNC_COOLDOWN_MS 15000UL          // 同步成功后短冷却，避免连环 restore

// 信号变差→恢复后强制重同步画面（滞回，避免临界抖动）
#define SIGNAL_BAD_RSSI_DBM (-80)                  // ≤ 此值视为差
#define SIGNAL_GOOD_RSSI_DBM (-68)                 // ≥ 此值视为好
#define SIGNAL_BAD_HOLD_MS 3500UL                  // 持续差多久才记为 BAD
#define SIGNAL_GOOD_HOLD_MS 1800UL                 // 持续好多久才触发恢复同步
#define SIGNAL_RECOVERY_RESYNC_COOLDOWN_MS 25000UL // 两次信号恢复同步最小间隔
#define CANVAS_FORCE_RESYNC_FLAG 1                 // SyncMessage.touch_data.x：强制重同步

// 心跳包相关常量
#define HEARTBEAT_SEND_INTERVAL_MS 3000UL // 心跳包发送间隔 (毫秒)
#define HEARTBEAT_TIMEOUT_MS 12000UL      // 心跳超时

// 调试信息切换按钮位置 (C/D 已移除，仅作聊天按钮锚点)
#define DEBUG_TOGGLE_BUTTON_X 2
#define DEBUG_TOGGLE_BUTTON_W 16
#define DEBUG_TOGGLE_BUTTON_H 16
#define DEBUG_TOGGLE_BUTTON_Y (SCREEN_HEIGHT - DEBUG_TOGGLE_BUTTON_H - 1)

// 加入聊天室按钮 — 放在原 D 左下角位置
#define CHAT_BUTTON_X DEBUG_TOGGLE_BUTTON_X
#define CHAT_BUTTON_W DEBUG_TOGGLE_BUTTON_W
#define CHAT_BUTTON_H DEBUG_TOGGLE_BUTTON_H
#define CHAT_BUTTON_Y DEBUG_TOGGLE_BUTTON_Y

// 圆形进度条相关定义
#define PROGRESS_CIRCLE_RADIUS 6
#define PROGRESS_CIRCLE_THICKNESS 2
#define SEND_PROGRESS_X (RESET_BUTTON_X + PROGRESS_CIRCLE_RADIUS)
#define SEND_PROGRESS_Y (PEER_INFO_BUTTON_Y + PEER_INFO_BUTTON_H + PROGRESS_CIRCLE_RADIUS + 3)
#define RECEIVE_PROGRESS_X SEND_PROGRESS_X
#define RECEIVE_PROGRESS_Y (SEND_PROGRESS_Y + 2 * PROGRESS_CIRCLE_RADIUS + 3)
#define PROGRESS_SEND_COLOR TFT_BLUE
#define PROGRESS_RECEIVE_COLOR TFT_GREEN
#define PROGRESS_BG_COLOR TFT_DARKGREY

// 项目信息按钮 (调试界面上方)
#define INFO_BUTTON_W 15
#define INFO_BUTTON_H 15
#define INFO_BUTTON_X (2 + 120 - INFO_BUTTON_W - 2) // 调试信息框 (startX=2, width=120) 右上角
#define INFO_BUTTON_Y (SCREEN_HEIGHT - 42 - INFO_BUTTON_H - 2) // 调试信息框 (startY=SCREEN_HEIGHT-42) 上方

// "Coffee" 按钮 (聊天按钮上方)
#define COFFEE_BUTTON_X DEBUG_TOGGLE_BUTTON_X      // 与调试按钮 X 坐标相同
#define COFFEE_BUTTON_W DEBUG_TOGGLE_BUTTON_W      // 与调试按钮宽度相同
#define COFFEE_BUTTON_H COFFEE_BUTTON_W      // 与调试按钮高度相同
#define COFFEE_BUTTON_Y (CHAT_BUTTON_Y - COFFEE_BUTTON_H - 2) // 在聊天按钮上方

// 群聊 / 私聊 / 群组
#define CHAT_TEXT_MAX 64
#define CHAT_HISTORY_MAX 40
#define CHAT_ROOM_NAME "大厅"
#define CHAT_MAX_GROUPS 6
#define CHAT_GROUP_NAME_MAX 12
#define CHAT_MODE_PUBLIC  0
#define CHAT_MODE_PRIVATE 1
#define CHAT_MODE_GROUP   2
#define DEVICE_ID_MAX_LEN 8
#define DEVICE_ID_PREF_NAMESPACE "espnow"
#define DEVICE_ID_PREF_KEY "devid"
#define DEVICE_ID_LONG_PRESS_MS 800UL

// 状态条 / Toast（高度须 ≥ 中文字高 CN_FONT_H=12，否则底下一行会留彩线）
#define STATUS_BAR_X 30
#define STATUS_BAR_Y 0
#define STATUS_BAR_W (UNDO_BUTTON_X - STATUS_BAR_X - 4)
#define STATUS_BAR_H 12
#define STATUS_TOAST_MS 2800UL
#define DRAWING_STATUS_MS 1600UL

// 无操作自动息屏（触摸/按键可唤醒；BOOT 短按仍可手动开关）
#define SCREEN_IDLE_OFF_MS (5UL * 60UL * 1000UL)

// 左侧 UI 保护区 (橡皮擦不可覆盖)
#define UI_LEFT_SAFE_X_MAX 28
#define UI_LEFT_SAFE_Y_MAX (RECEIVE_PROGRESS_Y + PROGRESS_CIRCLE_RADIUS + 3)

// SD 检测优化：启动后延迟探测，避免白屏卡死
#define SD_DETECT_DELAY_MS 1200UL
#define SD_SPI_HZ 4000000UL

#endif // CONFIG_H
