# -*- coding: utf-8 -*-
from pathlib import Path

ui = Path(r"d:\esp32now\Project-ESPNow\src\ui_manager.cpp")
inc = Path(r"d:\esp32now\Project-ESPNow\src\chat_ui_impl.inc.cpp")
text = ui.read_text(encoding="utf-8")
impl = inc.read_text(encoding="utf-8")

old_del = """                if (x >= 170 && x < 210) {
                    // 删除
                    for (int j = i; j < chatGroupCount - 1; j++)
                        chatGroups[j] = chatGroups[j + 1];
                    chatGroupCount--;
                    if (strcmp(activeGroupId, chatGroups[i].id) == 0)
                        activeGroupId[0] = 0;
                    saveChatGroups();
                    drawChatRoom();
                    return true;
                }"""

new_del = """                if (x >= 170 && x < 210) {
                    // 删除
                    char removed[DEVICE_ID_MAX_LEN + 1];
                    strncpy(removed, chatGroups[i].id, DEVICE_ID_MAX_LEN);
                    removed[DEVICE_ID_MAX_LEN] = 0;
                    for (int j = i; j < chatGroupCount - 1; j++)
                        chatGroups[j] = chatGroups[j + 1];
                    chatGroupCount--;
                    if (strcmp(activeGroupId, removed) == 0)
                        activeGroupId[0] = 0;
                    saveChatGroups();
                    drawChatRoom();
                    return true;
                }"""

impl = impl.replace(old_del, new_del)

if "__CHAT_PLACEHOLDER__" not in text:
    raise SystemExit("placeholder missing")

if '#include "cn_text.h"' not in text:
    text = text.replace(
        "#include <Preferences.h>",
        '#include <Preferences.h>\n#include "cn_text.h"',
    )

text = text.replace("__CHAT_PLACEHOLDER__", impl)
ui.write_text(text, encoding="utf-8")
print("ok", ui.stat().st_size)
