# -*- coding: utf-8 -*-
"""Simplified CN glyphs + syllable IME + 2/3/4-char phrase matching (no hand phrase enum)."""
from PIL import Image, ImageFont, ImageDraw
from pypinyin import pinyin, Style, lazy_pinyin
from pypinyin.phrases_dict import phrases_dict
from zhconv import convert
from collections import defaultdict
import os
import re

PUNCT = list("，。！？、；：""''（）【】《》…—·￥～＿＠＃＊／＼｜＋－＝＜＞")
UI = list(
    "私聊群组管理创建加入退出发送返回颜色设置成员删除保存取消公共大厅选择好友"
    "已加入未命名的聊天室在线离线系统通知拼音英文空格暂无点文字好友列表"
    "画笔橡皮同步接收发送进度确定关闭打开帮助复制粘贴清空重命名输入消息"
    "解散进入删除选择附近相同占用无法设置拼英空删返群管理列表开始私聊"
    "聊天文字颜色点右上角创建暂无在线好友邀请群主成员离开延迟信号强度"
    "本机在线画板页笔在画在擦新建清页翻页记录中人发送接收同步中"
    "已恢复来自上线掉线冲突笔迹历史字迹"
    "设备拒绝同意请超时对方流程无效电量操作上线列表"
    "设置扫描连接断开忘记开关键优先网络蓝牙密码"
    "汪振乐郑恩宁葛天逸娱乐大厅剪刀石头布打地鼠雷达预计距离游戏"
    "比反应恐龙搭桥长按木板掉落分数重新开始过早"
    "我的战绩排行榜胜负场次查看对手胜利失败附近总详情点击"
    "狼人杀预言家女巫猎人白痴平民守卫夜晚白天投票出局屠边警长"
    "发言遗言解药毒药查验刀人准备房间座位开局天黑请闭眼天亮了"
    "空气曲棍球节奏点拍连击完美错过挡板好人大人狼刀毒救验枪"
    "复活死亡存活等待确认跳过弃票平票自爆带走号玩家法官"
    "海龟汤面汤底是不是无关出题喝汤揭晓提问线索餐厅电梯下雨"
    "真相故事倒计时讨论同伴倒牌平安夜队友自由选确认开局"
    "上题下题声称猜中未猜人规则回答最近口头点答盲复明"
    "冰雕溶化酒吧打嗝吓退抢劫空枪威慑推理看底藏底"
    "一七万丈三上下不与丐且世业东丢两严丧个中丹为主久么之乘九乞也习乡书买乱了争事二于些亡交产亭亲人什仇今介从他付代令以们件价任份会伞伤伪伴但位住体何余作你侏供依侣侦保信修俱倒候借倾假做"
    "停偶偷傻像僵儒儿元充先光克免入全八公六关其典养兽内册再写军冥冰冲况冻准减凑几凤凶出击刀分刑列则刚判利别到制刷刺前剧剩副割功加动助劫包化匪医十升午半单博卫即却厅厕原厨去参又友双反发取"
    "受变叙叟叠口句另只叫召可号司叼吃合吊同名后向吓否吧含听启吹呈告员周味命和咒咚咬哒哗哥哭唐啥啦善喜喝喷嗝器嚏四回因团困围图土在地场均坏坐块坠型埋堵塔墓墙声处复外多夜够大天太夫失头夹奠"
    "女奶她好如妈妹妻始姐威娃娘婆婚婴嫌子字孤学孩守安完定宝实客室害家容寄密寝察对寻导寿将小少尔就尸尽局层居屋屑山岁岛岸崖崩工差己已币布师带常幕平年并幸幻幼广庆床库应底店度建开异弄引弟张"
    "强当录形影彻彼往很得心忘忠快忽怎怒思性总恋恐恒悔患悬悲情惊惧惨惯想惶意感愧愿慑戏成我或战截戴户房所扇手才扎打扔执找技把抑抓投抛护报抬抹抽拍拐拒拖拟拨括拮拼拾拿持挂指按挑挖挽捅捉捕捡"
    "换据掉掌掏排探接推掩提握揭援搏搬搭摔撞撤播攀收改攻放故敌救敢散数敲整斗断新方施旁无日旧早时旷明昏是显晓晕晚晨暴更最月有朋服望木未本机杀杂李材村杖条来杯杰松板极果枪染柜柠柴标栋树校样"
    "根格案档桶梦梯械棺楚楼模橘檬次欢款歌正此步死残母每毒比毙毛民气氧水永求汤沙没河油治法注洗活流测浪浴海涂消淇淋淡深清渔渡温游湿溃溶滑滞满滩漉漏演漠激火灭灯炸点烛烤烧热焦然照熬燃爆爬爱"
    "父爸片版牙牛物牲牵特牺犬状狗狞独狰猜猫献王玩环现玻班球理璃瓶甚生用田由电男画界留略疗疯疾病症痛癖白百的皮盐盒盗盘目盯盲直相看真眼着睛睡睹瞒瞬知短矮砍砒破砷砸硬确碎碗碰碳示礼社祈神票"
    "祭祸离种秒秘租称程稻稿空穿突窗窥站童端笑符第等答筛签简管类粉精糊糕系索紧红约纸纹线组细终绍经绑结给绝统继续绿编缝缺网置美羞群翻老考者而耳耽职联肉肚肢肥胆胎胖能脂脏脚脸腿自至致舌舞航"
    "般舱船色芭花苍若苹茨草荒荧药落葬蒙藏虾蚊蛆蛇蛋蜂蜜蜡蝇融血行街衣补表被裂装西要见观规视觉角解言警计认让讯记许设访证诅识诉词试诚话诡询该误诱说读调谈谎谜谢象贝责败货质贮贴费资走赴起超"
    "越足跑跟路跳踪身躯躲躺车转轮轻较辑辨达过运近还这进远连迪迫述迷退送逃选递通逝造逻逼遇遍道遗那邻郁部都酒酿醒里重野量钞钮钱铁链锁键锯镜长门问间闻阅阿限院险陪隐障难集雕雨雪雷需震霜露青"
    "静非靠面鞋音页顶项顺预颅题风飞食餐饥饭饰饱饿馆香马驶驾验骗骨高魂鱼鲜黄黑默鼓鼠齿龄龙龟"

)

font = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", 12, index=0)
W = 12
MAX_GLYPHS = 6200
MAX_PER_SYL = 22
MAX_PHRASES = 2000  # 2/3/4 字词条
MAX_WORDS_PER_KEY = 6

def is_bmp(ch: str) -> bool:
    """Only BMP codepoints fit uint16_t glyph tables."""
    if not ch or len(ch) != 1:
        return False
    o = ord(ch)
    return o <= 0xFFFF and not (0xD800 <= o <= 0xDFFF)

def to_simp_ch(ch: str) -> str:
    if len(ch) != 1 or not is_bmp(ch):
        return ""
    s = convert(ch, "zh-cn")
    if len(s) == 1 and is_bmp(s):
        return s
    return ch  # zhconv 若出非 BMP，保留原 BMP 字

def to_simp_word(w: str) -> str:
    return "".join(to_simp_ch(c) or c for c in w)

def py_key_of_word(word: str) -> str:
    parts = lazy_pinyin(word, style=Style.NORMAL)
    return "".join(p for p in parts if p and p.isalpha())

def jianpin_of_word(word: str) -> str:
    """简拼：每字声母，zh/ch/sh 保留两位。如 你好→nh"""
    parts = lazy_pinyin(word, style=Style.NORMAL)
    out = []
    for p in parts:
        if not p or not p.isalpha():
            continue
        if len(p) >= 2 and p[:2] in ("zh", "ch", "sh"):
            out.append(p[:2])
        else:
            out.append(p[0])
    return "".join(out)

# --- phrases first (drive glyph demand) ---
phrase_pairs = []  # (pykey, word)
seen_phrase = set()
chars_needed = set(PUNCT + UI)

for word in phrases_dict.keys():
    if not word:
        continue
    w = to_simp_word(word)
    if not (2 <= len(w) <= 4):
        continue
    if any(ord(c) < 0x4E00 or ord(c) > 0x9FFF for c in w):
        continue
    if any(not is_bmp(c) for c in w):
        continue
    key = py_key_of_word(w)
    if not key or len(key) < 2 or not key.isalpha():
        continue
    if (key, w) in seen_phrase:
        continue
    seen_phrase.add((key, w))
    phrase_pairs.append((key, w))
    jp = jianpin_of_word(w)
    if jp and len(jp) >= 2 and jp != key and (jp, w) not in seen_phrase:
        seen_phrase.add((jp, w))
        phrase_pairs.append((jp, w))
    for c in w:
        chars_needed.add(c)
    if len(phrase_pairs) >= MAX_PHRASES * 4:
        break

# Prefer shorter then alpha key
phrase_pairs.sort(key=lambda x: (len(x[1]), x[0], x[1]))
phrase_pairs = phrase_pairs[: MAX_PHRASES * 2]  # keep room for jp+full before glyph filter

syl_map = defaultdict(list)
seen_in_syl = defaultdict(set)

for code in range(0x4E00, 0x9FA6):
    ch = to_simp_ch(chr(code))
    if not ch:
        continue
    try:
        pys = pinyin(ch, style=Style.NORMAL, heteronym=False)
        if not pys or not pys[0]:
            continue
        py = pys[0][0]
        if not py or not py.isalpha():
            continue
        if ch in seen_in_syl[py]:
            continue
        if len(syl_map[py]) >= MAX_PER_SYL:
            continue
        syl_map[py].append(ch)
        seen_in_syl[py].add(ch)
    except Exception:
        pass

ordered = []
seen = set()
for ch in PUNCT + UI:
    ch = to_simp_ch(ch) if (len(ch) == 1 and ord(ch) >= 0x4E00) else ch
    if ch and is_bmp(ch) and ch not in seen:
        ordered.append(ch)
        seen.add(ch)
for ch in sorted(chars_needed, key=ord):
    if ch and is_bmp(ch) and ch not in seen:
        ordered.append(ch)
        seen.add(ch)

syl_keys = sorted(syl_map.keys())
idx = {k: 0 for k in syl_keys}
while len(ordered) < MAX_GLYPHS:
    progressed = False
    for k in syl_keys:
        i = idx[k]
        lst = syl_map[k]
        if i < len(lst):
            ch = lst[i]
            idx[k] = i + 1
            if ch not in seen and is_bmp(ch):
                ordered.append(ch)
                seen.add(ch)
                progressed = True
                if len(ordered) >= MAX_GLYPHS:
                    break
    if not progressed:
        break

sel = set(ordered)
# drop phrases whose glyphs missing
phrase_pairs = [(k, w) for k, w in phrase_pairs if all(c in sel for c in w)]
# re-trim but keep both full + jianpin keys
phrase_pairs = phrase_pairs[: MAX_PHRASES * 2]

syl_map2 = defaultdict(list)
for k, lst in syl_map.items():
    for ch in lst:
        if ch in sel:
            syl_map2[k].append(ch)
syl_map = syl_map2
chars = sorted(ordered, key=lambda c: ord(c))
print("glyphs", len(chars), "syllables", len(syl_map), "phrases", len(phrase_pairs),
      "est_kb", round(len(chars) * 24 / 1024, 1))

out_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "src"))

lines = [
    "#pragma once",
    "#include <stdint.h>",
    "#include <pgmspace.h>",
    f"#define CN_FONT_W {W}",
    f"#define CN_FONT_H {W}",
    "#define CN_FONT_BYTES_PER_GLYPH 24",
    f"#define CN_FONT_COUNT {len(chars)}",
    "typedef struct { uint16_t unicode; uint8_t bits[24]; } CnGlyph;",
    "static const CnGlyph CN_GLYPHS[] PROGMEM = {",
]
for ch in chars:
    img = Image.new("L", (W, W), 0)
    d = ImageDraw.Draw(img)
    bbox = d.textbbox((0, 0), ch, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = max(0, (W - tw) // 2 - bbox[0])
    y = max(-2, (W - th) // 2 - bbox[1] - 1)
    d.text((x, y), ch, font=font, fill=255)
    bits = []
    for row in range(W):
        b0 = b1 = 0
        for col in range(W):
            on = img.getpixel((col, row)) > 100
            if col < 8:
                if on:
                    b0 |= 0x80 >> col
            else:
                if on:
                    b1 |= 0x80 >> (col - 8)
        bits.extend([b0, b1])
    bstr = ",".join(f"0x{b:02X}" for b in bits)
    lines.append(f"  {{0x{ord(ch):04X}, {{{bstr}}}}},")
lines.append("};")
font_path = os.path.join(out_dir, "cn_font_data.h")
open(font_path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print("font", os.path.getsize(font_path))

all_codes = []
syl_entries = []
for py in sorted(syl_map.keys()):
    cs = syl_map[py]
    if not cs:
        continue
    start = len(all_codes)
    for ch in cs:
        all_codes.append(ord(ch))
    syl_entries.append((py, start, len(cs)))

# group phrases by py key for compact lookup
from collections import OrderedDict
py_to_words = OrderedDict()
for key, w in sorted(phrase_pairs, key=lambda x: x[0]):
    py_to_words.setdefault(key, [])
    if w not in py_to_words[key] and len(py_to_words[key]) < MAX_WORDS_PER_KEY:
        py_to_words[key].append(w)

phrase_flat = []  # utf8 strings stored as C string literals
phrase_index = []  # (py, start, count)
for key, words in py_to_words.items():
    start = len(phrase_flat)
    for w in words:
        phrase_flat.append(w)
    phrase_index.append((key, start, len(words)))

dlines = [
    "#pragma once",
    "#include <stdint.h>",
    "#include <pgmspace.h>",
    f"#define PY_SYL_COUNT {len(syl_entries)}",
    f"#define PY_CODE_COUNT {len(all_codes)}",
    f"#define PY_PHRASE_KEY_COUNT {len(phrase_index)}",
    f"#define PY_PHRASE_COUNT {len(phrase_flat)}",
    "typedef struct { const char *py; uint16_t start; uint8_t count; } PySyl;",
    "typedef struct { const char *py; uint16_t start; uint8_t count; } PyPhraseKey;",
    "static const uint16_t PY_CODES[] PROGMEM = {",
]
chunk = []
for code in all_codes:
    chunk.append(f"0x{code:04X}")
    if len(chunk) == 16:
        dlines.append("  " + ", ".join(chunk) + ",")
        chunk = []
if chunk:
    dlines.append("  " + ", ".join(chunk) + ",")
dlines.append("};")
dlines.append("static const PySyl PY_SYLS[] = {")
for py, start, count in syl_entries:
    dlines.append(f'  {{"{py}", {start}, {count}}},')
dlines.append("};")
dlines.append("static const char *const PY_PHRASE_WORDS[] = {")
for w in phrase_flat:
    esc = w.replace("\\", "\\\\").replace('"', '\\"')
    dlines.append(f'  "{esc}",')
dlines.append("};")
dlines.append("static const PyPhraseKey PY_PHRASE_KEYS[] = {")
for py, start, count in phrase_index:
    dlines.append(f'  {{"{py}", {start}, {count}}},')
dlines.append("};")
dict_path = os.path.join(out_dir, "cn_pinyin_data.h")
open(dict_path, "w", encoding="utf-8").write("\n".join(dlines) + "\n")
print("pinyin", os.path.getsize(dict_path), "codes", len(all_codes), "syl", len(syl_entries),
      "phraseKeys", len(phrase_index), "phrases", len(phrase_flat))
