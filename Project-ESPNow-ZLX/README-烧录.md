# Project-ESPNow-ZLX（新设备专用）

适配板卡：[ZLX-ESP32-1 / ESP32-2432S0248R-PLUS](https://www.zlxchina.xyz/zh/product/zlx-esp32-1)

相对旧工程的关键差异：

| 项 | 旧板 (Project-ESPNow) | 新板 (本目录) |
|---|---|---|
| LCD 驱动 | `ILI9341_2_DRIVER` | **`ST7789_DRIVER`** |
| 颜色顺序 | BGR | BGR（官网要求） |
| 反相 | 关 | **`TFT_INVERSION_OFF`**（ST7789 默认 INVON，必须关掉才不发白） |
| 分辨率 | 240×320 横屏逻辑 320×240 | 同左 |
| 触摸 | XPT2046（GPIO 与官网一致） | 同左 |

本机检测到串口：**USB-SERIAL CH340 (COM5)** —— 一般就是这块新板。

---

## 怎么烧录（Arduino IDE）

> TFT_eSPI **不会**自动读工程里的 `User_Setup.h`，必须改库目录里的那份。

### 0. 一键切换（推荐）

仓库根目录双击 `switch-device.bat`，选新板；或：

```bat
D:\esp32now\switch-device.bat new
```

说明见 `DEVICE-SWITCH.md`。功能改动请旧板/新板两边一起改。

### 1. 切换显示配置（新板）

已帮你把库配置切到 ZLX 版。若被改乱，手动覆盖：

```
源：  D:\esp32now\Project-ESPNow-ZLX\User_Setup.h
目标：C:\Users\30424\Documents\Arduino\libraries\TFT_eSPI\User_Setup.h
```

备份文件（可随时换回旧板）：

- `User_Setup_ZLX_ESP32_1.h` — 新板
- `User_Setup_OLD_ILI9341.h` — 旧板
- `User_Setup.h.bak-old-board` — 切换前自动备份

换回旧板时：把 `User_Setup_OLD_ILI9341.h` 复制覆盖库里的 `User_Setup.h`，再打开 `Project-ESPNow` 工程烧录。

### 2. 打开工程

Arduino IDE → **文件 → 打开** →

`D:\esp32now\Project-ESPNow-ZLX\Project-ESPNow-ZLX.ino`

### 3. 开发板选项（重要）

- 开发板：`ESP32 Dev Module`（或 ESP32-WROOM-32）
- 端口：`COM5`
- **Partition Scheme：Huge APP (3MB No OTA)**（中文字库很大，不选会 Sketch too big）
- Upload Speed：`921600`（失败可改 `460800`）
- Flash Size：`4MB`

### 4. 上传

点 **上传**。第一次建议勾选擦除，或工具里 **Erase Flash: All Flash Contents** 一次，清掉旧旋转 NVS（翻转错乱时很有用）。

### 5. 烧完仍不对时逐项试

1. **还是发白 / 黑白反了**：确认库 `User_Setup.h` 是 `TFT_INVERSION_OFF`（不是 ON）。再试把 `TFT_BGR` 改成 `TFT_RGB`。  
2. **红蓝反了**：`TFT_BGR` ↔ `TFT_RGB` 对调。  
3. **翻转后按钮点不准**：本版触摸固定 rotation=1，翻转用软件镜像；请擦除 Flash 清 NVS 后再烧。  
4. **大厅滑不动**：列表区**按住上下拖**；轻点松手才进入游戏。

### 串口自检（COM5 / 115200）

复位后应看到类似：

```
[DISP] ST7789 + BGR + invert OFF (fix whiteboard white)
[DISP] after rot: 320x240 rot=1
[DISP] rotation=1 tft=320x240 touch=fixed(1)+mirror=OFF
```

点「翻」后应出现 `rot=3` 且 `mirror=ON`。

---

## 目录说明

```
D:\esp32now\
  Project-ESPNow\          ← 旧设备（ILI9341）
  Project-ESPNow-ZLX\      ← 新设备 ZLX-ESP32-1（ST7789）
```

两套代码可并行维护；烧哪套就打开哪个 `.ino`，并确认库里的 `User_Setup.h` 与板型匹配。
