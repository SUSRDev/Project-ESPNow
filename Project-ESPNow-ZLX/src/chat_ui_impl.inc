// ========== 局域网群聊（中文/私聊/群组/颜色） ==========
#include "cn_text.h"
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
} ChatGroup_t;

enum ChatTab_e { CHAT_TAB_PUBLIC = 0, CHAT_TAB_PRIVATE, CHAT_TAB_GROUPS, CHAT_TAB_COLOR };

static ChatLine_t chatHistory[CHAT_HISTORY_MAX];
static size_t chatHistoryCount = 0;
static char chatInputBuffer[CHAT_TEXT_MAX + 1] = {0};
static char pinyinBuffer[24] = {0};
static bool imePinyinMode = true;
static ChatTab_e chatTab = CHAT_TAB_PUBLIC;
static char privatePeerId[DEVICE_ID_MAX_LEN + 1] = {0};
static char activeGroupId[DEVICE_ID_MAX_LEN + 1] = {0};
static ChatGroup_t chatGroups[CHAT_MAX_GROUPS];
static int chatGroupCount = 0;
static uint16_t chatTextColor = TFT_CYAN;
static Preferences chatPrefs;
static const char *candZh[6];
static int candCount = 0;

static void loadChatGroups()
{
    chatGroupCount = 0;
    if (!chatPrefs.begin("chatgrp", true))
        return;
    chatGroupCount = constrain((int)chatPrefs.getUChar("n", 0), 0, CHAT_MAX_GROUPS);
    for (int i = 0; i < chatGroupCount; i++) {
        char k1[8], k2[8];
        snprintf(k1, sizeof(k1), "id%d", i);
        snprintf(k2, sizeof(k2), "nm%d", i);
        String id = chatPrefs.getString(k1, "");
        String nm = chatPrefs.getString(k2, "");
        strncpy(chatGroups[i].id, id.c_str(), DEVICE_ID_MAX_LEN);
        chatGroups[i].id[DEVICE_ID_MAX_LEN] = 0;
        strncpy(chatGroups[i].name, nm.c_str(), CHAT_GROUP_NAME_MAX);
        chatGroups[i].name[CHAT_GROUP_NAME_MAX] = 0;
    }
    chatPrefs.end();
}

static void saveChatGroups()
{
    if (!chatPrefs.begin("chatgrp", false))
        return;
    chatPrefs.putUChar("n", (uint8_t)chatGroupCount);
    for (int i = 0; i < chatGroupCount; i++) {
        char k1[8], k2[8];
        snprintf(k1, sizeof(k1), "id%d", i);
        snprintf(k2, sizeof(k2), "nm%d", i);
        chatPrefs.putString(k1, chatGroups[i].id);
        chatPrefs.putString(k2, chatGroups[i].name);
    }
    chatPrefs.end();
}

static void refreshCandidates()
{
    candCount = 0;
    if (imePinyinMode && pinyinBuffer[0])
        cnFillCandidates(pinyinBuffer, candZh, 6, candCount);
}

static bool chatLineVisible(const ChatLine_t &line)
{
    if (chatTab == CHAT_TAB_PUBLIC)
        return line.mode == CHAT_MODE_PUBLIC;
    if (chatTab == CHAT_TAB_PRIVATE) {
        if (line.mode != CHAT_MODE_PRIVATE || !privatePeerId[0])
            return false;
        return (strcmp(line.senderId, privatePeerId) == 0 || strcmp(line.targetId, privatePeerId) == 0 ||
                strcmp(line.senderId, localDeviceId) == 0);
    }
    if (chatTab == CHAT_TAB_GROUPS) {
        if (line.mode != CHAT_MODE_GROUP || !activeGroupId[0])
            return false;
        return strcmp(line.targetId, activeGroupId) == 0;
    }
    return false;
}

void drawChatJoinButton()
{
    if (!isScreenOn || inCustomColorMode || !showDebugToggleButton || currentUIState != UI_STATE_MAIN)
        return;
    tft.fillRect(CHAT_BUTTON_X, CHAT_BUTTON_Y, CHAT_BUTTON_W, CHAT_BUTTON_H, TFT_MAGENTA);
    cnDrawUtf8(tft, CHAT_BUTTON_X + 2, CHAT_BUTTON_Y + 4, "聊", TFT_WHITE);
}

bool isChatJoinButtonPressed(int x, int y)
{
    if (!showDebugToggleButton || currentUIState != UI_STATE_MAIN)
        return false;
    return x >= CHAT_BUTTON_X && x <= CHAT_BUTTON_X + CHAT_BUTTON_W &&
           y >= CHAT_BUTTON_Y && y <= CHAT_BUTTON_Y + CHAT_BUTTON_H;
}

void appendChatMessage(const char *senderId, const char *targetId, const char *text,
                       bool isSelf, uint8_t mode, uint16_t color)
{
    if (!text)
        text = "";
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

    // 远端创建群：CREATE:id:name
    if (mode == CHAT_MODE_GROUP && strncmp(text, "CREATE:", 7) == 0 && !isSelf) {
        const char *p = text + 7;
        char gid[9] = {0}, gname[13] = {0};
        const char *colon = strchr(p, ':');
        if (colon) {
            size_t n = (size_t)(colon - p);
            if (n > 8) n = 8;
            memcpy(gid, p, n);
            strncpy(gname, colon + 1, CHAT_GROUP_NAME_MAX);
            bool exists = false;
            for (int i = 0; i < chatGroupCount; i++)
                if (strcmp(chatGroups[i].id, gid) == 0) exists = true;
            if (!exists && chatGroupCount < CHAT_MAX_GROUPS && gid[0]) {
                strncpy(chatGroups[chatGroupCount].id, gid, DEVICE_ID_MAX_LEN);
                strncpy(chatGroups[chatGroupCount].name, gname[0] ? gname : gid, CHAT_GROUP_NAME_MAX);
                chatGroupCount++;
                saveChatGroups();
            }
        }
    }

    if (currentUIState == UI_STATE_CHAT)
        drawChatRoom();
}

static void drawChatTabs()
{
    const char *tabs[4] = {"大厅", "私聊", "群组", "颜色"};
    int tw = SCREEN_WIDTH / 4;
    for (int i = 0; i < 4; i++) {
        uint16_t bg = (chatTab == i) ? TFT_NAVY : TFT_DARKGREY;
        tft.fillRect(i * tw, 0, tw - 1, 18, bg);
        cnDrawUtf8(tft, i * tw + 8, 3, tabs[i], TFT_WHITE);
    }
}

static void drawChatMessagesArea()
{
    const int areaX = 2, areaY = 20, areaW = SCREEN_WIDTH - 4, areaH = 78;
    tft.fillRect(areaX, areaY, areaW, areaH, tft.color565(16, 16, 28));
    tft.drawRect(areaX, areaY, areaW, areaH, TFT_DARKGREY);

    // 收集可见消息
    int vis[CHAT_HISTORY_MAX];
    int vc = 0;
    for (size_t i = 0; i < chatHistoryCount; i++) {
        if (chatLineVisible(chatHistory[i]))
            vis[vc++] = (int)i;
    }
    const int lineH = 13;
    const int maxLines = areaH / lineH;
    int start = 0;
    if (vc > maxLines)
        start = vc - maxLines;
    int y = areaY + 2;
    for (int vi = start; vi < vc; vi++) {
        const ChatLine_t &line = chatHistory[vis[vi]];
        char head[24];
        snprintf(head, sizeof(head), "%s:", line.senderId);
        int hx = cnDrawUtf8(tft, areaX + 3, y, head, line.isSelf ? TFT_GREENYELLOW : TFT_YELLOW);
        cnDrawUtf8(tft, areaX + 3 + hx + 2, y, line.text, line.color);
        y += lineH;
    }
}

static void drawChatInputBar()
{
    tft.fillRect(2, 100, SCREEN_WIDTH - 64, 16, TFT_BLACK);
    tft.drawRect(2, 100, SCREEN_WIDTH - 64, 16, TFT_WHITE);
    if (imePinyinMode && pinyinBuffer[0]) {
        char show[80];
        snprintf(show, sizeof(show), "%s|%s", chatInputBuffer, pinyinBuffer);
        cnDrawUtf8(tft, 4, 102, show, chatTextColor);
    } else {
        cnDrawUtf8(tft, 4, 102, chatInputBuffer[0] ? chatInputBuffer : "_", chatTextColor);
    }
    tft.fillRect(SCREEN_WIDTH - 60, 100, 58, 16, TFT_GREEN);
    cnDrawUtf8(tft, SCREEN_WIDTH - 48, 102, "发送", TFT_BLACK);
}

static void drawChatCandidates()
{
    tft.fillRect(2, 118, SCREEN_WIDTH - 4, 14, tft.color565(30, 30, 40));
    if (!imePinyinMode) {
        cnDrawUtf8(tft, 4, 119, "[英文]", TFT_DARKGREY);
        return;
    }
    int x = 4;
    for (int i = 0; i < candCount; i++) {
        char lab[20];
        snprintf(lab, sizeof(lab), "%d.%s", i + 1, candZh[i]);
        x += cnDrawUtf8(tft, x, 119, lab, TFT_CYAN) + 8;
    }
    if (candCount == 0 && pinyinBuffer[0])
        cnDrawUtf8(tft, 4, 119, pinyinBuffer, TFT_ORANGE);
}

static void drawChatKeyboard()
{
    const char *rows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM", ""};
    const int keyW = 28, keyH = 15, startY = 134;
    for (int r = 0; r < 4; r++) {
        int len = (int)strlen(rows[r]);
        int totalW = len * (keyW + 1);
        int startX = (SCREEN_WIDTH - totalW) / 2;
        for (int i = 0; i < len; i++) {
            int x = startX + i * (keyW + 1);
            int y = startY + r * (keyH + 2);
            tft.fillRect(x, y, keyW, keyH, TFT_DARKGREY);
            char c[2] = {rows[r][i], 0};
            tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
            tft.setTextDatum(MC_DATUM);
            tft.drawString(c, x + keyW / 2, y + keyH / 2, 1);
        }
    }
    tft.setTextDatum(TL_DATUM);
    // 底栏：拼音/英文 空格 DEL 返回
    int y = startY + 4 * (keyH + 2);
    tft.fillRect(4, y, 50, keyH, TFT_PURPLE);
    cnDrawUtf8(tft, 10, y + 1, imePinyinMode ? "拼音" : "英文", TFT_WHITE);
    tft.fillRect(58, y, 100, keyH, TFT_NAVY);
    cnDrawUtf8(tft, 90, y + 1, "空格", TFT_WHITE);
    tft.fillRect(162, y, 50, keyH, TFT_ORANGE);
    cnDrawUtf8(tft, 172, y + 1, "删除", TFT_BLACK);
    tft.fillRect(216, y, 50, keyH, TFT_DARKGREY);
    cnDrawUtf8(tft, 226, y + 1, "返回", TFT_WHITE);
    tft.setTextDatum(TL_DATUM);
}

static void drawPrivatePeerList()
{
    tft.fillRect(2, 20, SCREEN_WIDTH - 4, 78, TFT_BLACK);
    cnDrawUtf8(tft, 6, 22, "选择好友:", TFT_WHITE);
    int y = 38;
    int i = 0;
    for (auto const &kv : peerInfoMap) {
        if (i >= 4) break;
        const PeerInfo_t &p = kv.second;
        const char *id = p.deviceId[0] ? p.deviceId : p.macAddress.c_str();
        bool sel = (strcmp(privatePeerId, p.deviceId) == 0);
        tft.fillRect(6, y - 1, SCREEN_WIDTH - 12, 14, sel ? TFT_NAVY : TFT_DARKGREY);
        cnDrawUtf8(tft, 10, y, id, TFT_WHITE);
        y += 16;
        i++;
    }
    if (peerInfoMap.empty())
        cnDrawUtf8(tft, 10, 50, "暂无在线好友", TFT_DARKGREY);
}

static void drawGroupPanel()
{
    tft.fillRect(2, 20, SCREEN_WIDTH - 4, 78, TFT_BLACK);
    cnDrawUtf8(tft, 6, 22, "群组管理", TFT_WHITE);
    // 创建
    tft.fillRect(200, 22, 56, 14, TFT_GREEN);
    cnDrawUtf8(tft, 208, 23, "创建", TFT_BLACK);
    int y = 40;
    for (int i = 0; i < chatGroupCount && i < 3; i++) {
        bool sel = (strcmp(activeGroupId, chatGroups[i].id) == 0);
        tft.fillRect(6, y, 160, 14, sel ? TFT_NAVY : TFT_DARKGREY);
        cnDrawUtf8(tft, 10, y + 1, chatGroups[i].name, TFT_WHITE);
        tft.fillRect(170, y, 40, 14, TFT_RED);
        cnDrawUtf8(tft, 176, y + 1, "删除", TFT_WHITE);
        tft.fillRect(214, y, 40, 14, TFT_CYAN);
        cnDrawUtf8(tft, 220, y + 1, "进入", TFT_BLACK);
        y += 16;
    }
    if (chatGroupCount == 0)
        cnDrawUtf8(tft, 10, 55, "暂无群组,点创建", TFT_DARKGREY);
}

static void drawColorPanel()
{
    tft.fillRect(2, 20, SCREEN_WIDTH - 4, 78, TFT_BLACK);
    cnDrawUtf8(tft, 6, 22, "设置聊天文字颜色", TFT_WHITE);
    uint16_t colors[] = {TFT_WHITE, TFT_CYAN, TFT_YELLOW, TFT_GREEN, TFT_MAGENTA, TFT_ORANGE, TFT_RED, TFT_BLUE};
    for (int i = 0; i < 8; i++) {
        int x = 12 + (i % 4) * 70;
        int y = 42 + (i / 4) * 26;
        tft.fillRect(x, y, 60, 22, colors[i]);
        if (chatTextColor == colors[i])
            tft.drawRect(x, y, 60, 22, TFT_WHITE);
    }
}

void drawChatRoom()
{
    tft.fillScreen(TFT_BLACK);
    drawChatTabs();
    if (chatTab == CHAT_TAB_PUBLIC || (chatTab == CHAT_TAB_PRIVATE && privatePeerId[0]) ||
        (chatTab == CHAT_TAB_GROUPS && activeGroupId[0])) {
        drawChatMessagesArea();
        drawChatInputBar();
        drawChatCandidates();
        drawChatKeyboard();
    } else if (chatTab == CHAT_TAB_PRIVATE) {
        drawPrivatePeerList();
        drawChatKeyboard();
    } else if (chatTab == CHAT_TAB_GROUPS) {
        drawGroupPanel();
    } else if (chatTab == CHAT_TAB_COLOR) {
        drawColorPanel();
    }
}

void showChatRoom()
{
    if (!isScreenOn)
        return;
    loadChatGroups();
    currentUIState = UI_STATE_CHAT;
    chatTab = CHAT_TAB_PUBLIC;
    chatInputBuffer[0] = 0;
    pinyinBuffer[0] = 0;
    refreshCandidates();
    drawChatRoom();
    // 静默进入：不广播 JOIN（避免大厅刷屏）
    appendChatMessage(localDeviceId, "", "[加入大厅]", true, CHAT_MODE_PUBLIC, TFT_YELLOW);
    showStatusToast("Joined chat");
}

void hideChatRoom()
{
    if (currentUIState != UI_STATE_CHAT)
        return;
    currentUIState = UI_STATE_MAIN;
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
        if (!privatePeerId[0]) return;
        mode = CHAT_MODE_PRIVATE;
        target = privatePeerId;
    } else if (chatTab == CHAT_TAB_GROUPS) {
        if (!activeGroupId[0]) return;
        mode = CHAT_MODE_GROUP;
        target = activeGroupId;
    } else if (chatTab == CHAT_TAB_COLOR) {
        return;
    }
    sendChatEx(MSG_TYPE_CHAT, mode, target, chatInputBuffer, chatTextColor);
    appendChatMessage(localDeviceId, target, chatInputBuffer, true, mode, chatTextColor);
    chatInputBuffer[0] = 0;
    pinyinBuffer[0] = 0;
    refreshCandidates();
    drawChatRoom();
}

bool handleChatTouch(int x, int y)
{
    if (currentUIState != UI_STATE_CHAT)
        return false;

    // Tabs
    if (y < 18) {
        int tab = x / (SCREEN_WIDTH / 4);
        if (tab >= 0 && tab < 4) {
            chatTab = (ChatTab_e)tab;
            drawChatRoom();
        }
        return true;
    }

    // 颜色面板
    if (chatTab == CHAT_TAB_COLOR && y >= 42 && y <= 94) {
        uint16_t colors[] = {TFT_WHITE, TFT_CYAN, TFT_YELLOW, TFT_GREEN, TFT_MAGENTA, TFT_ORANGE, TFT_RED, TFT_BLUE};
        for (int i = 0; i < 8; i++) {
            int cx = 12 + (i % 4) * 70;
            int cy = 42 + (i / 4) * 26;
            if (x >= cx && x <= cx + 60 && y >= cy && y <= cy + 22) {
                chatTextColor = colors[i];
                drawChatRoom();
                return true;
            }
        }
    }

    // 私聊选人
    if (chatTab == CHAT_TAB_PRIVATE && !privatePeerId[0] && y >= 38 && y < 100) {
        int idx = (y - 38) / 16;
        int i = 0;
        for (auto const &kv : peerInfoMap) {
            if (i == idx) {
                strncpy(privatePeerId, kv.second.deviceId[0] ? kv.second.deviceId : kv.first.c_str(), DEVICE_ID_MAX_LEN);
                privatePeerId[DEVICE_ID_MAX_LEN] = 0;
                drawChatRoom();
                return true;
            }
            i++;
            if (i >= 4) break;
        }
    }

    // 群组面板
    if (chatTab == CHAT_TAB_GROUPS) {
        if (x >= 200 && x <= 256 && y >= 22 && y <= 36) {
            // 创建群
            if (chatGroupCount < CHAT_MAX_GROUPS) {
                char gid[9];
                snprintf(gid, sizeof(gid), "G%02X%02X", (unsigned)(millis() & 0xFF), (unsigned)(localDeviceId[0] & 0xFF));
                char gname[13];
                snprintf(gname, sizeof(gname), "群%d", chatGroupCount + 1);
                strncpy(chatGroups[chatGroupCount].id, gid, DEVICE_ID_MAX_LEN);
                strncpy(chatGroups[chatGroupCount].name, gname, CHAT_GROUP_NAME_MAX);
                chatGroupCount++;
                saveChatGroups();
                char msg[48];
                snprintf(msg, sizeof(msg), "CREATE:%s:%s", gid, gname);
                sendChatEx(MSG_TYPE_CHAT_GROUP, CHAT_MODE_GROUP, gid, msg, TFT_ORANGE);
                appendChatMessage(localDeviceId, gid, msg, true, CHAT_MODE_GROUP, TFT_ORANGE);
                strncpy(activeGroupId, gid, DEVICE_ID_MAX_LEN);
                drawChatRoom();
            }
            return true;
        }
        int y0 = 40;
        for (int i = 0; i < chatGroupCount && i < 3; i++) {
            int gy = y0 + i * 16;
            if (y >= gy && y < gy + 14) {
                if (x >= 170 && x < 210) {
                    // 删除
                    for (int j = i; j < chatGroupCount - 1; j++)
                        chatGroups[j] = chatGroups[j + 1];
                    chatGroupCount--;
                    if (strcmp(activeGroupId, chatGroups[i].id) == 0)
                        activeGroupId[0] = 0;
                    saveChatGroups();
                    drawChatRoom();
                    return true;
                }
                if (x >= 214 && x < 254) {
                    strncpy(activeGroupId, chatGroups[i].id, DEVICE_ID_MAX_LEN);
                    activeGroupId[DEVICE_ID_MAX_LEN] = 0;
                    drawChatRoom();
                    return true;
                }
                strncpy(activeGroupId, chatGroups[i].id, DEVICE_ID_MAX_LEN);
                drawChatRoom();
                return true;
            }
        }
    }

    // 发送
    if (x >= SCREEN_WIDTH - 60 && y >= 100 && y <= 116) {
        commitChatSend();
        return true;
    }

    // 候选词
    if (y >= 118 && y < 132 && candCount > 0) {
        int slot = (x - 4) / 52;
        if (slot >= 0 && slot < candCount) {
            appendInputUtf8(candZh[slot]);
            pinyinBuffer[0] = 0;
            refreshCandidates();
            drawChatRoom();
            return true;
        }
    }

    const int keyW = 28, keyH = 15, startY = 134;
    int yBar = startY + 4 * (keyH + 2);

    // 底栏
    if (y >= yBar && y <= yBar + keyH) {
        if (x >= 4 && x < 54) {
            imePinyinMode = !imePinyinMode;
            pinyinBuffer[0] = 0;
            refreshCandidates();
            drawChatRoom();
            return true;
        }
        if (x >= 58 && x < 158) {
            if (imePinyinMode && pinyinBuffer[0]) {
                // 直接上屏拼音字母
                appendInputUtf8(pinyinBuffer);
                pinyinBuffer[0] = 0;
            } else {
                appendInputUtf8(" ");
            }
            refreshCandidates();
            drawChatRoom();
            return true;
        }
        if (x >= 162 && x < 212) {
            if (imePinyinMode && pinyinBuffer[0]) {
                pinyinBuffer[strlen(pinyinBuffer) - 1] = 0;
            } else if (chatInputBuffer[0]) {
                // 删 UTF-8 最后字符
                size_t n = strlen(chatInputBuffer);
                if (n >= 3 && ((uint8_t)chatInputBuffer[n - 1] & 0xC0) == 0x80) {
                    // 三字节汉字
                    int i = (int)n - 1;
                    while (i > 0 && ((uint8_t)chatInputBuffer[i] & 0xC0) == 0x80) i--;
                    chatInputBuffer[i] = 0;
                } else {
                    chatInputBuffer[n - 1] = 0;
                }
            }
            refreshCandidates();
            drawChatRoom();
            return true;
        }
        if (x >= 216) {
            if (chatTab == CHAT_TAB_PRIVATE && privatePeerId[0] && y < 100) {
                // ignore
            }
            hideChatRoom();
            return true;
        }
    }

    // 字母键
    const char *rows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (int r = 0; r < 4; r++) {
        int len = (int)strlen(rows[r]);
        int totalW = len * (keyW + 1);
        int startX = (SCREEN_WIDTH - totalW) / 2;
        int ky = startY + r * (keyH + 2);
        if (y < ky || y > ky + keyH) continue;
        for (int i = 0; i < len; i++) {
            int kx = startX + i * (keyW + 1);
            if (x >= kx && x <= kx + keyW) {
                char ch = rows[r][i];
                if (ch >= 'A' && ch <= 'Z')
                    ch = ch - 'A' + 'a';
                if (imePinyinMode && ch >= 'a' && ch <= 'z') {
                    size_t n = strlen(pinyinBuffer);
                    if (n + 1 < sizeof(pinyinBuffer)) {
                        pinyinBuffer[n] = ch;
                        pinyinBuffer[n + 1] = 0;
                    }
                } else {
                    char tmp[2] = {rows[r][i], 0};
                    if (ch >= 'a' && ch <= 'z')
                        tmp[0] = ch;
                    appendInputUtf8(tmp);
                }
                refreshCandidates();
                drawChatRoom();
                return true;
            }
        }
    }
    return true;
}
