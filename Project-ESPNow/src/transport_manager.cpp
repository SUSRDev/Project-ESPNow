#include "transport_manager.h"
#include "esp_now_handler.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <esp_now.h>
#include <cstring>
#include <vector>
#include <map>

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
        if (wifiIsConnected())
            snprintf(statusBuf, sizeof(statusBuf), "仅WiFi %s", wifiConnectedSsid());
        else
            snprintf(statusBuf, sizeof(statusBuf), "仅WiFi(未连接)");
        return;
    }
    if (linkMode == LINK_MODE_DUAL) {
        if (wifiIsConnected())
            snprintf(statusBuf, sizeof(statusBuf), "双并发 %s", wifiConnectedSsid());
        else
            snprintf(statusBuf, sizeof(statusBuf), "双并发(待连WiFi)");
        return;
    }
    // WIFI_ON assist
    if (wifiIsConnected()) {
        if (shouldSendWifi(nullptr))
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

bool transportSend(const uint8_t *data, size_t len, const uint8_t *destMacOrNull)
{
    bool espOk = false;
    if (linkMode != LINK_MODE_WIFI_ONLY)
        espOk = espNowSendRaw(data, len, destMacOrNull);

    bool wifiOk = false;
    if (shouldSendWifi(destMacOrNull))
        wifiOk = udpSendRaw(data, len, destMacOrNull);

    if (espOk && wifiOk)
        lastUsed = TRANSPORT_BOTH;
    else if (espOk)
        lastUsed = TRANSPORT_ESPNOW;
    else if (wifiOk)
        lastUsed = TRANSPORT_WIFI;
    else
        lastUsed = TRANSPORT_NONE;

    updateStatus();
    return espOk || wifiOk;
}

bool wifiIsConnected()
{
    return WiFi.status() == WL_CONNECTED;
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
    netPrefs.end();

    strncpy(savedSsid, ssid.c_str(), 32);
    savedSsid[32] = '\0';
    strncpy(savedPass, pass.c_str(), 64);
    savedPass[64] = '\0';

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    if (linkMode != LINK_MODE_ESPNOW_ONLY && autoConnect && savedSsid[0])
        WiFi.begin(savedSsid, savedPass);

    updateStatus();
}
