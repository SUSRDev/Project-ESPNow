#include "transport_manager.h"
#include "esp_now_handler.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <esp_now.h>
#include <cstring>
#include <vector>
#include <map>
#include <cstdlib>

extern char localDeviceId[DEVICE_ID_MAX_LEN + 1];
extern void ingestIncomingPacket(const uint8_t srcMac[6], const uint8_t *data, int len, int8_t rssi);

static Preferences netPrefs;
static WiFiUDP udp;
static bool udpBound = false;
static TransportKind_t lastUsed = TRANSPORT_NONE;
static char statusBuf[48] = "ESP-NOW";

static char savedSsid[33] = {0};
static char savedPass[65] = {0};
static bool autoConnect = true;
static uint8_t linkMode = LINK_MODE_ESPNOW_ONLY;

// Cross-network MQTT relay
static bool crossNetOn = false;
static char roomIdBuf[MQTT_ROOM_ID_MAX + 1] = "ROOM1";
static WiFiClient mqttClient;
static bool mqttReady = false;
static unsigned long lastMqttConnectMs = 0;
static unsigned long lastMqttPingMs = 0;
static unsigned long lastMqttDiscoverMs = 0;
static char mqttStatus[28] = "MQTT off";
static uint8_t mqttRxBuf[640];
static size_t mqttRxLen = 0;

static int scanCountCached = -1;
static bool scanRunning = false;

struct PeerNetInfo_t {
    IPAddress ip;
    unsigned long lastSeenMs;
    char id[DEVICE_ID_MAX_LEN + 1];
};
static std::map<String, PeerNetInfo_t> peerNetMap;

static unsigned long lastWifiDiscoverMs = 0;
static unsigned long lastWifiReconnectMs = 0;
static bool wifiConnectPending = false;
static unsigned long wifiConnectStartedMs = 0;

static void updateStatus();
static bool bindUdp();
static void mqttDisconnectSock();
static bool mqttDoConnect();

// Dedup dual-path packets (ESP-NOW + WiFi)
static uint32_t recentPktHash[24];
static unsigned long recentPktMs[24];
static uint8_t recentPktIdx = 0;

#pragma pack(push, 1)
struct TransportHdr_t {
    uint32_t magic;
    uint8_t srcMac[6];
    uint16_t payloadLen;
};
#pragma pack(pop)

static String macToKey(const uint8_t mac[6])
{
    char s[18];
    snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(s);
}

static void getMyMac(uint8_t mac[6])
{
    esp_wifi_get_mac(WIFI_IF_STA, mac);
}

static uint32_t hashPacket(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u;
    size_t n = len < 64 ? len : 64;
    for (size_t i = 0; i < n; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    h ^= (uint32_t)len;
    return h;
}

bool transportIsDuplicatePacket(const uint8_t *data, int len)
{
    if (!data || len <= 0)
        return false;
    uint32_t h = hashPacket(data, (size_t)len);
    unsigned long now = millis();
    for (int i = 0; i < 24; i++) {
        if (recentPktHash[i] == h && (now - recentPktMs[i]) < 120UL)
            return true;
    }
    recentPktHash[recentPktIdx] = h;
    recentPktMs[recentPktIdx] = now;
    recentPktIdx = (uint8_t)((recentPktIdx + 1) % 24);
    return false;
}

// Worst (lowest) recent peer ESP-NOW RSSI; 0 if unknown
static int8_t worstPeerEspNowRssi()
{
    int8_t worst = 0;
    bool any = false;
    unsigned long now = millis();
    for (auto const &kv : peerInfoMap) {
        const PeerInfo_t &p = kv.second;
        if (p.lastSeenMs == 0 || (now - p.lastSeenMs) > 15000UL)
            continue;
        if (p.rssi == 0)
            continue;
        if (!any || p.rssi < worst) {
            worst = p.rssi;
            any = true;
        }
    }
    return any ? worst : (int8_t)0;
}

static int8_t peerEspNowRssi(const uint8_t mac[6])
{
    if (!mac)
        return worstPeerEspNowRssi();
    String key = macToKey(mac);
    auto it = peerInfoMap.find(key);
    if (it == peerInfoMap.end())
        return worstPeerEspNowRssi();
    if (it->second.rssi == 0)
        return worstPeerEspNowRssi();
    return it->second.rssi;
}

// WiFi path allowed by mode + connected
static bool shouldSendWifi(const uint8_t *destMacOrNull)
{
    if (linkMode == LINK_MODE_ESPNOW_ONLY)
        return false;
    if (!wifiIsConnected())
        return false;
    if (linkMode == LINK_MODE_DUAL || linkMode == LINK_MODE_WIFI_ONLY)
        return true;
    // WIFI_ON: 有局域网 WiFi 对端时始终发，保证「仅 WiFi」设备能互通；否则差信号才辅助
    if (!peerNetMap.empty())
        return true;
    int8_t rssi = peerEspNowRssi(destMacOrNull);
    if (rssi == 0)
        return false;
    return rssi <= SIGNAL_WIFI_ASSIST_RSSI_DBM;
}

bool wifiAssistActive()
{
    return shouldSendWifi(nullptr);
}

bool linkModeIsEspNowOnly() { return linkMode == LINK_MODE_ESPNOW_ONLY; }
bool linkModeWifiEnabled()
{
    return linkMode == LINK_MODE_WIFI_ON || linkMode == LINK_MODE_DUAL ||
           linkMode == LINK_MODE_WIFI_ONLY;
}
bool linkModeIsDual() { return linkMode == LINK_MODE_DUAL; }
bool linkModeIsWifiOnly() { return linkMode == LINK_MODE_WIFI_ONLY; }
uint8_t getLinkMode() { return linkMode; }

void setLinkMode(uint8_t mode)
{
    if (mode > LINK_MODE_WIFI_ONLY)
        mode = LINK_MODE_ESPNOW_ONLY;
    linkMode = mode;
    netPrefs.begin(NET_PREF_NAMESPACE, false);
    netPrefs.putUChar(NET_PREF_LINK_MODE, linkMode);
    netPrefs.end();

    if (linkMode == LINK_MODE_ESPNOW_ONLY) {
        udp.stop();
        udpBound = false;
        mqttDisconnectSock();
    } else {
        if (autoConnect && savedSsid[0] && !wifiIsConnected())
            wifiLoadAndAutoConnect();
        if (wifiIsConnected())
            bindUdp();
    }
    if (linkMode == LINK_MODE_WIFI_ONLY)
        purgePeersNotVisibleForWifiOnly();
    updateStatus();
}

bool transportUsesWifiIcon()
{
    if (linkMode == LINK_MODE_WIFI_ONLY)
        return wifiIsConnected();
    return lastUsed == TRANSPORT_WIFI || lastUsed == TRANSPORT_BOTH ||
           (linkMode == LINK_MODE_DUAL && wifiIsConnected());
}

static void updateStatus()
{
    if (linkMode == LINK_MODE_ESPNOW_ONLY) {
        snprintf(statusBuf, sizeof(statusBuf), "仅 ESP-NOW");
        return;
    }
    if (linkMode == LINK_MODE_WIFI_ONLY) {
        if (wifiIsConnected()) {
            if (crossNetOn)
                snprintf(statusBuf, sizeof(statusBuf), "仅WiFi %s %s",
                         mqttReady ? "云" : "…", roomIdBuf);
            else
                snprintf(statusBuf, sizeof(statusBuf), "仅WiFi %s", wifiConnectedSsid());
        } else {
            snprintf(statusBuf, sizeof(statusBuf), "仅WiFi(未连接)");
        }
        return;
    }
    if (linkMode == LINK_MODE_DUAL) {
        if (wifiIsConnected()) {
            if (crossNetOn)
                snprintf(statusBuf, sizeof(statusBuf), "双并发+云 %s", roomIdBuf);
            else
                snprintf(statusBuf, sizeof(statusBuf), "双并发 %s", wifiConnectedSsid());
        } else {
            snprintf(statusBuf, sizeof(statusBuf), "双并发(待连WiFi)");
        }
        return;
    }
    // WIFI_ON assist
    if (wifiIsConnected()) {
        if (crossNetOn)
            snprintf(statusBuf, sizeof(statusBuf), "WiFi+云 %s", roomIdBuf);
        else if (shouldSendWifi(nullptr))
            snprintf(statusBuf, sizeof(statusBuf), "WiFi备份 %s", wifiConnectedSsid());
        else
            snprintf(statusBuf, sizeof(statusBuf), "ESP-NOW(WiFi待命)");
    } else {
        snprintf(statusBuf, sizeof(statusBuf), "启用WiFi(未连接)");
    }
}

static bool bindUdp()
{
    if (udpBound)
        return true;
    if (udp.begin(TRANSPORT_UDP_PORT)) {
        udpBound = true;
        return true;
    }
    return false;
}

static void sendWifiDiscover()
{
    if (linkMode == LINK_MODE_ESPNOW_ONLY)
        return;
    if (!wifiIsConnected() || !bindUdp())
        return;
    uint8_t mac[6];
    getMyMac(mac);
    char body[40];
    snprintf(body, sizeof(body), "ID:%s|M:%u",
             localDeviceId[0] ? localDeviceId : "Peer", (unsigned)linkMode);
    TransportHdr_t hdr;
    hdr.magic = TRANSPORT_UDP_MAGIC;
    memcpy(hdr.srcMac, mac, 6);
    hdr.payloadLen = (uint16_t)strlen(body);

    IPAddress bcast = WiFi.broadcastIP();
    if (udp.beginPacket(bcast, TRANSPORT_UDP_PORT) == 1) {
        udp.write((uint8_t *)&hdr, sizeof(hdr));
        udp.write((uint8_t *)body, hdr.payloadLen);
        udp.endPacket();
    }
    for (auto const &kv : peerNetMap) {
        if (udp.beginPacket(kv.second.ip, TRANSPORT_UDP_PORT) == 1) {
            udp.write((uint8_t *)&hdr, sizeof(hdr));
            udp.write((uint8_t *)body, hdr.payloadLen);
            udp.endPacket();
        }
    }
}

// Fast UDP: unicast every known peer IP, then one broadcast fallback
static bool udpSendRaw(const uint8_t *data, size_t len, const uint8_t *destMacOrNull)
{
    if (!wifiIsConnected() || !bindUdp() || !data || len == 0 || len > 1400)
        return false;

    uint8_t mac[6];
    getMyMac(mac);
    TransportHdr_t hdr;
    hdr.magic = TRANSPORT_UDP_MAGIC;
    memcpy(hdr.srcMac, mac, 6);
    hdr.payloadLen = (uint16_t)len;

    auto sendTo = [&](IPAddress ip) -> bool {
        if (!udp.beginPacket(ip, TRANSPORT_UDP_PORT))
            return false;
        if (udp.write((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr))
            return false;
        if (udp.write(data, len) != len)
            return false;
        return udp.endPacket() == 1;
    };

    bool ok = false;
    if (destMacOrNull) {
        String key = macToKey(destMacOrNull);
        auto it = peerNetMap.find(key);
        if (it != peerNetMap.end())
            ok = sendTo(it->second.ip);
    } else {
        // Fan-out unicast to all known LAN peers (much faster than broadcast alone)
        for (auto const &kv : peerNetMap) {
            if (sendTo(kv.second.ip))
                ok = true;
        }
    }
    // Broadcast once for peers not yet discovered
    if (sendTo(WiFi.broadcastIP()))
        ok = true;
    return ok;
}

static bool espNowSendRaw(const uint8_t *data, size_t len, const uint8_t *destMacOrNull)
{
    if (!data || len == 0)
        return false;
    const uint8_t *dest = destMacOrNull ? destMacOrNull : broadcastAddress;
    if (destMacOrNull)
        ensureUnicastPeer(destMacOrNull);
    return esp_now_send(dest, (uint8_t *)data, len) == ESP_OK;
}

// ---- Minimal MQTT 3.1.1 (QoS0) for cross-WiFi relay ----

static void mqttTopic(char *out, size_t outLen)
{
    snprintf(out, outLen, "%s%s/d", MQTT_TOPIC_PREFIX, roomIdBuf[0] ? roomIdBuf : "ROOM1");
}

static void mqttWriteRemainingLength(uint8_t *buf, size_t *idx, size_t rem)
{
    do {
        uint8_t b = rem % 128;
        rem /= 128;
        if (rem)
            b |= 0x80;
        buf[(*idx)++] = b;
    } while (rem);
}

static bool mqttWriteFrame(uint8_t typeFlags, const uint8_t *vh, size_t vhLen,
                           const uint8_t *pl, size_t plLen)
{
    if (!mqttClient.connected())
        return false;
    size_t rem = vhLen + plLen;
    uint8_t hdr[5];
    size_t hi = 0;
    hdr[hi++] = typeFlags;
    mqttWriteRemainingLength(hdr, &hi, rem);
    if (mqttClient.write(hdr, hi) != hi)
        return false;
    if (vhLen && mqttClient.write(vh, vhLen) != vhLen)
        return false;
    if (plLen && mqttClient.write(pl, plLen) != plLen)
        return false;
    return true;
}

static void mqttDisconnectSock()
{
    mqttReady = false;
    if (mqttClient.connected())
        mqttClient.stop();
    snprintf(mqttStatus, sizeof(mqttStatus), "MQTT down");
}

static bool mqttDoConnect()
{
    if (!wifiIsConnected() || !crossNetOn || linkMode == LINK_MODE_ESPNOW_ONLY)
        return false;
    if (mqttClient.connected() && mqttReady)
        return true;
    mqttDisconnectSock();
    snprintf(mqttStatus, sizeof(mqttStatus), "MQTT…");
    if (!mqttClient.connect(MQTT_BROKER_HOST, MQTT_BROKER_PORT, 4000)) {
        snprintf(mqttStatus, sizeof(mqttStatus), "MQTT fail");
        return false;
    }
    mqttClient.setNoDelay(true);

    char clientId[28];
    uint8_t mac[6];
    getMyMac(mac);
    snprintf(clientId, sizeof(clientId), "espn%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);

    // CONNECT
    uint8_t vh[12];
    size_t vi = 0;
    vh[vi++] = 0;
    vh[vi++] = 4;
    vh[vi++] = 'M';
    vh[vi++] = 'Q';
    vh[vi++] = 'T';
    vh[vi++] = 'T';
    vh[vi++] = 4;  // protocol level
    vh[vi++] = 0x02; // clean session
    vh[vi++] = (MQTT_KEEPALIVE_SEC >> 8) & 0xFF;
    vh[vi++] = MQTT_KEEPALIVE_SEC & 0xFF;
    size_t idLen = strlen(clientId);
    uint8_t pl[40];
    size_t pi = 0;
    pl[pi++] = (idLen >> 8) & 0xFF;
    pl[pi++] = idLen & 0xFF;
    memcpy(pl + pi, clientId, idLen);
    pi += idLen;
    if (!mqttWriteFrame(0x10, vh, vi, pl, pi)) {
        mqttDisconnectSock();
        return false;
    }

    // wait CONNACK
    unsigned long t0 = millis();
    while (millis() - t0 < 3000UL) {
        if (mqttClient.available() >= 4) {
            uint8_t ack[4];
            if (mqttClient.readBytes(ack, 4) == 4 && ack[0] == 0x20 && ack[3] == 0) {
                // SUBSCRIBE topic
                char topic[48];
                mqttTopic(topic, sizeof(topic));
                size_t tlen = strlen(topic);
                uint8_t svh[2];
                svh[0] = 0;
                svh[1] = 1; // packet id
                uint8_t spl[64];
                size_t si = 0;
                spl[si++] = (tlen >> 8) & 0xFF;
                spl[si++] = tlen & 0xFF;
                memcpy(spl + si, topic, tlen);
                si += tlen;
                spl[si++] = 0; // QoS0
                if (!mqttWriteFrame(0x82, svh, 2, spl, si)) {
                    mqttDisconnectSock();
                    return false;
                }
                mqttReady = true;
                mqttRxLen = 0;
                lastMqttPingMs = millis();
                snprintf(mqttStatus, sizeof(mqttStatus), "MQTT ok");
                return true;
            }
            mqttDisconnectSock();
            return false;
        }
        delay(10);
    }
    mqttDisconnectSock();
    return false;
}

static bool mqttPublishRaw(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > 500)
        return false;
    if (!mqttReady && !mqttDoConnect())
        return false;

    uint8_t mac[6];
    getMyMac(mac);
    TransportHdr_t hdr;
    hdr.magic = TRANSPORT_UDP_MAGIC;
    memcpy(hdr.srcMac, mac, 6);
    hdr.payloadLen = (uint16_t)len;

    char topic[48];
    mqttTopic(topic, sizeof(topic));
    size_t tlen = strlen(topic);
    size_t bodyLen = sizeof(hdr) + len;
    if (bodyLen > 520)
        return false;

    uint8_t vh[64];
    size_t vi = 0;
    vh[vi++] = (tlen >> 8) & 0xFF;
    vh[vi++] = tlen & 0xFF;
    memcpy(vh + vi, topic, tlen);
    vi += tlen;

    uint8_t body[520];
    memcpy(body, &hdr, sizeof(hdr));
    memcpy(body + sizeof(hdr), data, len);

    if (!mqttWriteFrame(0x30, vh, vi, body, bodyLen)) {
        mqttDisconnectSock();
        return false;
    }
    return true;
}

static void mqttHandlePublish(const uint8_t *topic, size_t topicLen,
                              const uint8_t *payload, size_t payloadLen)
{
    (void)topic;
    (void)topicLen;
    if (payloadLen < sizeof(TransportHdr_t))
        return;
    const TransportHdr_t *hdr = (const TransportHdr_t *)payload;
    if (hdr->magic != TRANSPORT_UDP_MAGIC)
        return;
    if (sizeof(TransportHdr_t) + hdr->payloadLen > payloadLen)
        return;
    uint8_t myMac[6];
    getMyMac(myMac);
    if (memcmp(hdr->srcMac, myMac, 6) == 0)
        return;

    const uint8_t *pl = payload + sizeof(TransportHdr_t);
    int plen = hdr->payloadLen;

    if (plen >= 3 && pl[0] == 'I' && pl[1] == 'D' && pl[2] == ':') {
        char idBuf[DEVICE_ID_MAX_LEN + 1] = {0};
        uint8_t peerMode = 0xFF;
        const char *idStart = (const char *)pl + 3;
        const char *modeSep = strstr(idStart, "|M:");
        size_t idLen = modeSep ? (size_t)(modeSep - idStart) : strlen(idStart);
        if (idLen > DEVICE_ID_MAX_LEN)
            idLen = DEVICE_ID_MAX_LEN;
        memcpy(idBuf, idStart, idLen);
        idBuf[idLen] = '\0';
        if (modeSep)
            peerMode = (uint8_t)atoi(modeSep + 3);
        if (peerMode != LINK_MODE_ESPNOW_ONLY)
            notePeerWifiPresence(hdr->srcMac, idBuf, peerMode);
        return;
    }

    if (transportIsDuplicatePacket(pl, plen))
        return;
    ingestIncomingPacketEx(hdr->srcMac, pl, plen, -60, true);
}

static void mqttPoll()
{
    if (!crossNetOn || linkMode == LINK_MODE_ESPNOW_ONLY)
        return;
    if (!wifiIsConnected()) {
        if (mqttReady)
            mqttDisconnectSock();
        return;
    }
    if (!mqttReady) {
        if (millis() - lastMqttConnectMs > 5000UL) {
            lastMqttConnectMs = millis();
            mqttDoConnect();
        }
        return;
    }
    if (!mqttClient.connected()) {
        mqttDisconnectSock();
        return;
    }

    // keepalive ping
    if (millis() - lastMqttPingMs > (MQTT_KEEPALIVE_SEC * 700UL)) {
        uint8_t ping[2] = {0xC0, 0x00};
        if (mqttClient.write(ping, 2) != 2)
            mqttDisconnectSock();
        lastMqttPingMs = millis();
    }

    while (mqttClient.available()) {
        int b = mqttClient.read();
        if (b < 0)
            break;
        if (mqttRxLen < sizeof(mqttRxBuf))
            mqttRxBuf[mqttRxLen++] = (uint8_t)b;
        else {
            mqttRxLen = 0;
            break;
        }

        if (mqttRxLen < 2)
            continue;
        // decode remaining length
        size_t mul = 1, rem = 0, rli = 1;
        bool remDone = false;
        while (rli < mqttRxLen && rli < 5) {
            uint8_t enc = mqttRxBuf[rli];
            rem += (size_t)(enc & 0x7F) * mul;
            mul *= 128;
            rli++;
            if (!(enc & 0x80)) {
                remDone = true;
                break;
            }
        }
        if (!remDone)
            continue;
        size_t frameLen = rli + rem;
        if (mqttRxLen < frameLen)
            continue;

        uint8_t type = mqttRxBuf[0] & 0xF0;
        if (type == 0x30) { // PUBLISH QoS0
            if (rem >= 2) {
                size_t tlen = ((size_t)mqttRxBuf[rli] << 8) | mqttRxBuf[rli + 1];
                size_t topicStart = rli + 2;
                if (topicStart + tlen <= frameLen) {
                    size_t payloadStart = topicStart + tlen;
                    size_t payloadLen = frameLen - payloadStart;
                    mqttHandlePublish(mqttRxBuf + topicStart, tlen,
                                      mqttRxBuf + payloadStart, payloadLen);
                }
            }
        } else if (type == 0xD0) {
            // PINGRESP
        } else if (type == 0x90) {
            // SUBACK
        }

        // shift buffer
        size_t left = mqttRxLen - frameLen;
        if (left)
            memmove(mqttRxBuf, mqttRxBuf + frameLen, left);
        mqttRxLen = left;
    }

    // presence over MQTT
    if (mqttReady && millis() - lastMqttDiscoverMs > 5000UL) {
        lastMqttDiscoverMs = millis();
        char body[40];
        snprintf(body, sizeof(body), "ID:%s|M:%u",
                 localDeviceId[0] ? localDeviceId : "Peer", (unsigned)linkMode);
        mqttPublishRaw((const uint8_t *)body, strlen(body));
    }
}

static bool shouldSendMqtt()
{
    return crossNetOn && linkMode != LINK_MODE_ESPNOW_ONLY && wifiIsConnected();
}

bool transportSend(const uint8_t *data, size_t len, const uint8_t *destMacOrNull)
{
    bool espOk = false;
    if (linkMode != LINK_MODE_WIFI_ONLY)
        espOk = espNowSendRaw(data, len, destMacOrNull);

    bool wifiOk = false;
    if (shouldSendWifi(destMacOrNull))
        wifiOk = udpSendRaw(data, len, destMacOrNull);

    bool mqttOk = false;
    if (shouldSendMqtt())
        mqttOk = mqttPublishRaw(data, len);

    if ((espOk && wifiOk) || (espOk && mqttOk) || (wifiOk && mqttOk))
        lastUsed = TRANSPORT_BOTH;
    else if (espOk)
        lastUsed = TRANSPORT_ESPNOW;
    else if (wifiOk || mqttOk)
        lastUsed = TRANSPORT_WIFI;
    else
        lastUsed = TRANSPORT_NONE;

    updateStatus();
    return espOk || wifiOk || mqttOk;
}

bool wifiIsConnected()
{
    return WiFi.status() == WL_CONNECTED;
}

int32_t wifiApRssi()
{
    if (!wifiIsConnected())
        return 0;
    return WiFi.RSSI();
}

const char *wifiConnectedSsid()
{
    static char ssidBuf[33];
    if (!wifiIsConnected()) {
        ssidBuf[0] = '\0';
        return ssidBuf;
    }
    strncpy(ssidBuf, WiFi.SSID().c_str(), 32);
    ssidBuf[32] = '\0';
    return ssidBuf;
}

IPAddress wifiLocalIp()
{
    return WiFi.localIP();
}

void wifiStartScan()
{
    scanRunning = true;
    scanCountCached = -1;
    WiFi.scanDelete();
    WiFi.scanNetworks(true, true);
}

bool wifiScanDone()
{
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING)
        return false;
    (void)scanRunning;
    scanRunning = false;
    scanCountCached = (n < 0) ? 0 : n;
    return true;
}

int wifiScanCount()
{
    if (scanCountCached >= 0)
        return scanCountCached;
    int n = WiFi.scanComplete();
    if (n < 0)
        return 0;
    scanCountCached = n;
    return n;
}

bool wifiGetScanItem(int index, WifiScanItem_t *out)
{
    if (!out)
        return false;
    int n = wifiScanCount();
    if (index < 0 || index >= n)
        return false;
    String ssid = WiFi.SSID(index);
    strncpy(out->ssid, ssid.c_str(), 32);
    out->ssid[32] = '\0';
    out->rssi = WiFi.RSSI(index);
    out->open = (WiFi.encryptionType(index) == WIFI_AUTH_OPEN);
    return true;
}

bool wifiConnectInProgress()
{
    return wifiConnectPending && WiFi.status() != WL_CONNECTED &&
           (millis() - wifiConnectStartedMs < 20000UL);
}

bool wifiConnectSucceeded()
{
    return wifiIsConnected();
}

bool wifiConnect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0])
        return false;
    strncpy(savedSsid, ssid, 32);
    savedSsid[32] = '\0';
    if (password) {
        strncpy(savedPass, password, 64);
        savedPass[64] = '\0';
    } else {
        savedPass[0] = '\0';
    }
    netPrefs.begin(NET_PREF_NAMESPACE, false);
    netPrefs.putString(NET_PREF_SSID, savedSsid);
    netPrefs.putString(NET_PREF_PASS, savedPass);
    netPrefs.putBool(NET_PREF_AUTO, true);
    netPrefs.end();
    autoConnect = true;

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // lower latency for UDP assist
    WiFi.disconnect(false, false);
    delay(20);
    WiFi.begin(savedSsid, savedPass);
    wifiConnectPending = true;
    wifiConnectStartedMs = millis();
    updateStatus();
    return true;
}

void wifiDisconnect(bool forgetCreds)
{
    WiFi.disconnect(true, false);
    udp.stop();
    udpBound = false;
    if (forgetCreds) {
        savedSsid[0] = 0;
        savedPass[0] = 0;
        autoConnect = false;
        netPrefs.begin(NET_PREF_NAMESPACE, false);
        netPrefs.remove(NET_PREF_SSID);
        netPrefs.remove(NET_PREF_PASS);
        netPrefs.putBool(NET_PREF_AUTO, false);
        netPrefs.end();
    }
    updateStatus();
}

bool wifiHasSavedCreds()
{
    return savedSsid[0] != 0;
}

void wifiLoadAndAutoConnect()
{
    if (!autoConnect || !savedSsid[0])
        return;
    if (wifiIsConnected())
        return;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(savedSsid, savedPass);
}

TransportKind_t transportLastUsed() { return lastUsed; }

const char *transportStatusLine()
{
    updateStatus();
    return statusBuf;
}

static void pollUdp()
{
    if (linkMode == LINK_MODE_ESPNOW_ONLY)
        return;
    if (!wifiIsConnected() || !bindUdp())
        return;

    // Drain multiple packets per loop for throughput
    for (int drain = 0; drain < 8; drain++) {
        int packetSize = udp.parsePacket();
        if (packetSize < (int)sizeof(TransportHdr_t))
            break;
        if (packetSize > 1500)
            continue;

        uint8_t buf[1500];
        int n = udp.read(buf, sizeof(buf));
        if (n < (int)sizeof(TransportHdr_t))
            continue;
        TransportHdr_t *hdr = (TransportHdr_t *)buf;
        if (hdr->magic != TRANSPORT_UDP_MAGIC)
            continue;
        if (sizeof(TransportHdr_t) + hdr->payloadLen > (size_t)n)
            continue;

        uint8_t myMac[6];
        getMyMac(myMac);
        if (memcmp(hdr->srcMac, myMac, 6) == 0)
            continue;

        const uint8_t *payload = buf + sizeof(TransportHdr_t);
        int plen = hdr->payloadLen;

        if (plen >= 3 && payload[0] == 'I' && payload[1] == 'D' && payload[2] == ':') {
            PeerNetInfo_t info;
            info.ip = udp.remoteIP();
            info.lastSeenMs = millis();
            char idBuf[DEVICE_ID_MAX_LEN + 1] = {0};
            uint8_t peerMode = 0xFF;
            const char *idStart = (const char *)payload + 3;
            const char *modeSep = strstr(idStart, "|M:");
            size_t idLen = modeSep ? (size_t)(modeSep - idStart) : strlen(idStart);
            if (idLen > DEVICE_ID_MAX_LEN)
                idLen = DEVICE_ID_MAX_LEN;
            memcpy(idBuf, idStart, idLen);
            idBuf[idLen] = '\0';
            if (modeSep)
                peerMode = (uint8_t)atoi(modeSep + 3);
            strncpy(info.id, idBuf, DEVICE_ID_MAX_LEN);
            info.id[DEVICE_ID_MAX_LEN] = '\0';
            peerNetMap[macToKey(hdr->srcMac)] = info;
            // 登记 WiFi 在线：仅 WiFi 模式靠此看到「启用WiFi/双并发/仅WiFi」设备
            if (peerMode != LINK_MODE_ESPNOW_ONLY)
                notePeerWifiPresence(hdr->srcMac, idBuf, peerMode);
            continue;
        }

        if (transportIsDuplicatePacket(payload, plen))
            continue;
        ingestIncomingPacketEx(hdr->srcMac, payload, plen, -55, true);
    }
}

void transportLoop()
{
    if (linkMode == LINK_MODE_ESPNOW_ONLY) {
        updateStatus();
        return;
    }

    if (wifiConnectPending) {
        if (WiFi.status() == WL_CONNECTED) {
            wifiConnectPending = false;
            WiFi.setSleep(false);
            bindUdp();
            sendWifiDiscover();
            updateStatus();
        } else if (millis() - wifiConnectStartedMs > 20000UL) {
            wifiConnectPending = false;
            updateStatus();
        }
    }

    if (autoConnect && savedSsid[0] && !wifiIsConnected() && !wifiConnectPending) {
        if (millis() - lastWifiReconnectMs > 20000UL) {
            lastWifiReconnectMs = millis();
            wifiLoadAndAutoConnect();
        }
    }

    if (wifiIsConnected()) {
        if (!udpBound)
            bindUdp();
        if (millis() - lastWifiDiscoverMs > 4000UL) {
            lastWifiDiscoverMs = millis();
            sendWifiDiscover();
        }
        pollUdp();
        mqttPoll();
    } else if (mqttReady) {
        mqttDisconnectSock();
    }

    updateStatus();
}

void transportInit()
{
    memset(recentPktHash, 0, sizeof(recentPktHash));
    memset(recentPktMs, 0, sizeof(recentPktMs));

    netPrefs.begin(NET_PREF_NAMESPACE, true);
    String ssid = netPrefs.getString(NET_PREF_SSID, "");
    String pass = netPrefs.getString(NET_PREF_PASS, "");
    autoConnect = netPrefs.getBool(NET_PREF_AUTO, true);
    linkMode = netPrefs.getUChar(NET_PREF_LINK_MODE, LINK_MODE_ESPNOW_ONLY);
    if (linkMode > LINK_MODE_WIFI_ONLY)
        linkMode = LINK_MODE_ESPNOW_ONLY;
    crossNetOn = netPrefs.getBool(NET_PREF_CROSS_NET, false);
    String room = netPrefs.getString(NET_PREF_ROOM_ID, "ROOM1");
    netPrefs.end();

    strncpy(savedSsid, ssid.c_str(), 32);
    savedSsid[32] = '\0';
    strncpy(savedPass, pass.c_str(), 64);
    savedPass[64] = '\0';
    strncpy(roomIdBuf, room.c_str(), MQTT_ROOM_ID_MAX);
    roomIdBuf[MQTT_ROOM_ID_MAX] = '\0';
    if (!roomIdBuf[0])
        strncpy(roomIdBuf, "ROOM1", MQTT_ROOM_ID_MAX);

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    if (linkMode != LINK_MODE_ESPNOW_ONLY && autoConnect && savedSsid[0])
        WiFi.begin(savedSsid, savedPass);

    updateStatus();
}

bool crossNetEnabled() { return crossNetOn; }

void setCrossNetEnabled(bool on)
{
    crossNetOn = on;
    netPrefs.begin(NET_PREF_NAMESPACE, false);
    netPrefs.putBool(NET_PREF_CROSS_NET, crossNetOn);
    netPrefs.end();
    if (!crossNetOn)
        mqttDisconnectSock();
    else if (wifiIsConnected())
        mqttDoConnect();
    updateStatus();
}

const char *crossNetRoomId() { return roomIdBuf; }

void setCrossNetRoomId(const char *room)
{
    if (!room || !room[0])
        return;
    // 只保留字母数字
    size_t j = 0;
    for (size_t i = 0; room[i] && j < MQTT_ROOM_ID_MAX; i++) {
        char c = room[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            roomIdBuf[j++] = c;
    }
    roomIdBuf[j] = '\0';
    if (!roomIdBuf[0])
        strncpy(roomIdBuf, "ROOM1", MQTT_ROOM_ID_MAX);
    netPrefs.begin(NET_PREF_NAMESPACE, false);
    netPrefs.putString(NET_PREF_ROOM_ID, roomIdBuf);
    netPrefs.end();
    // 房间变更需重连订阅新 topic
    mqttDisconnectSock();
    if (crossNetOn && wifiIsConnected())
        mqttDoConnect();
    updateStatus();
}

bool crossNetIsConnected() { return mqttReady; }

const char *crossNetStatusLine()
{
    return mqttStatus;
}
