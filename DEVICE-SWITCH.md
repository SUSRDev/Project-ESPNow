# ESPNow 双设备切换 / 同步

## 一键切换旧板 / 新板

双击仓库根目录：

```
D:\esp32now\switch-device.bat
```

或命令行：

```bat
switch-device.bat new
switch-device.bat old
switch-device.bat status
```

作用：把对应工程的 `User_Setup.h` 覆盖到

`%USERPROFILE%\Documents\Arduino\libraries\TFT_eSPI\User_Setup.h`

并写入 `ACTIVE_DEVICE.txt`，然后定位对应 `.ino`。

| 参数 | 设备 | 工程 |
|---|---|---|
| `new` / `zlx` | 新板 ST7789 | `Project-ESPNow-ZLX` |
| `old` / `ili` | 旧板 ILI9341 | `Project-ESPNow` |

**注意：** 换板后必须重新编译上传；分区选 **Huge APP (3MB No OTA)**。

## 源码同步（两边功能对齐）

```powershell
# 以新板为源，同步可共享文件到旧板
.\tools\sync-boards.ps1 -From new

# 以旧板为源，同步到新板
.\tools\sync-boards.ps1 -From old

# 强制覆盖 game_arcade/touch_handler/ui_manager（覆盖后核对板级差异）
.\tools\sync-boards.ps1 -From new -ForceLogic
```

永不自动同步：`User_Setup.h`、`src/config.h`、各自 `.ino`。

## Cursor / AI

仓库规则 `.cursor/rules/dual-board-sync.mdc`：以后功能改动必须同时更新旧板和新板。
