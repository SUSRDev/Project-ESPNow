# Project-ESPNow · 无线小纸条

基于 **ESP32-CYD** 的 ESP-NOW 无线同步画板 + 局域网中文聊天。多台板子靠近即可互相画画、聊天，无需路由器。

> 原作：[Kur1oR3iko/Project-ESPNow](https://github.com/Kur1oR3iko/Project-ESPNow)（Kurio Reiko）  
> 本仓库为功能增强版（中文输入、多页画布、状态同步等），商业化请保留对原作者的声明。

![板子示意](https://github.com/user-attachments/assets/9870ed31-667e-4ff3-ab37-1c473c22b1a5)

---

## 功能一览

### 画板
- ESP-NOW 实时同步笔迹（多设备）
- 颜色选择 / 自定义 RGB / 橡皮擦 + 半径滑条
- **画笔粗细**可调（同步到对端）
- **多页画布**：右下角翻页 / 新建 / 清当前页（不伤其它页）
- 顶部中文状态栏：谁在画、同步中、页码、在线人数（过长省略号）
- 左侧动态信号格（多对端轮换）
- SD 截屏（有卡才显示 S 键）
- 息屏 / BOOT 键控制（见原版说明）

### 聊天室
- 大厅 / 私聊 / 群组 / 文字颜色 / 在线列表
- **中文显示 + 拼音输入**（单音节 / 简拼双音节，如 `ge`→个，`nh`→你好）
- 扩充中文字库 + 全拼/简拼词组（体积已按 Flash 裁剪）
- 聊天记录 **掉电保存**，「颜色」页可一键清空
- 在线列表：信号格 + 延迟动态刷新
- 群组：创建 / 邀请 / 解散（群主权限）

---

## 硬件

常见 **ESP32-CYD** 2.8" 总成即可，例如：

- [LCDWIKI 2.8inch ESP32-32E Display](http://www.lcdwiki.com/zh/2.8inch_ESP32-32E_Display)

7789 板型可参考群友 King 版：[Project-ESPNow.KingVer](https://github.com/Kur1oR3iko/Project-ESPNow.KingVer)

---

## 快速烧录

材料：电脑、板子、数据线。工具可在原仓库 `tools` 分支或下方镜像获取。

### 重要：分区（Flash 不够时必看）

中文字库较大。本仓库已附带 `Project-ESPNow/partitions.csv`（**3MB APP**）。

若 IDE 仍报 `Sketch too big` / `text section exceeds`：

1. 工具 → **Partition Scheme** → 选 **Huge APP (3MB No OTA)**  
2. 或确认打开的是带 `partitions.csv` 的 `Project-ESPNow` 工程目录  
3. 重新编译上传

### 1. 驱动
安装 **CH340** 驱动（Windows 必做）。

### 2. Arduino IDE 烧录（推荐开发/二创）
1. 安装 [Arduino IDE](https://www.arduino.cc/en/software)
2. 开发板管理器安装 **esp32**；库管理器安装：
   - `TFT_eSPI`
   - `XPT2046_Touchscreen`
   - `SD`（一般随 IDE）
3. 按本仓库 `User_Setup.h` 配置 TFT_eSPI（或复制到库目录）
4. 打开 `Project-ESPNow/Project-ESPNow.ino`
5. 开发板选 `ESP32 Dev Module` / `ESP32 WROOM DA Module`，选对 COM 口
6. 上传

> ESP32 核心包较大，下载失败可参考离线安装教程。

### 3. BIN 烧录（适合小白/量产）
用乐鑫 `flash_download_tool`：

| 文件 | 地址 |
|------|------|
| `*.ino.bootloader.bin` | `0x1000` |
| `*.ino.partitions.bin` | `0x8000` |
| `*.ino.bin` | `0x10000` |

Chip：ESP32；可试 SPI 80MHz / QIO；烧录失败再降速。

---

## 仓库结构

```
Project-ESPNow/
  Project-ESPNow.ino     # 入口
  User_Setup.h           # TFT_eSPI 引脚
  src/                   # UI / 触摸 / ESP-NOW / 中文 / 电源
  tools/                 # 字库与拼音生成脚本
README.md
```

---

## 使用提示

- **两台都要烧同一版本**，ESP-NOW 包结构变更后旧固件无法互通
- 右下角：`C` 清当前页，`<` `>`/`+` 翻页/新建
- 右上 `*` 下圆点按钮：调笔粗
- 聊天「颜色」页：改字色 + **清空聊天记录**
- 拼音：完整单音节优先单字；`nh` 这类简拼走双字词

---

## 鸣谢

- 原作者 **Kurio Reiko** 与所有早期支持者、外壳建模（机械银狼）等  
- 历史贡献：QWQ、xiao_hj909、砂纸鸽（Shapaper223）、King 等群友  
- 吹水群：[391437320](https://jq.qq.com/) · 原作 B 站：[Kurio Reiko](https://space.bilibili.com/341918869)

开源欢迎二创；商业化请注明原创归属。

---

## License

沿用原项目开源精神；再分发请保留本说明与原作者致谢。
