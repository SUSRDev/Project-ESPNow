#ifndef TRANSPORT_MANAGER_H
#define TRANSPORT_MANAGER_H

#include <Arduino.h>
#include <stdint.h>
#include "config.h"

enum TransportKind_e {
    TRANSPORT_NONE = 0,
    TRANSPORT_WIFI = 1,
    TRANSPORT_ESPNOW = 2,
    TRANSPORT_BOTH = 3
};
typedef enum TransportKind_e TransportKind_t;

struct WifiScanItem_t {
    char ssid[33];
    int32_t rssi;
    bool open;
};

void transportInit();
void transportLoop();

bool transportSend(const uint8_t *data, size_t len, const uint8_t *destMacOrNull);

bool wifiIsConnected();
const char *wifiConnectedSsid();
IPAddress wifiLocalIp();
void wifiStartScan();
bool wifiScanDone();
int wifiScanCount();
bool wifiGetScanItem(int index, WifiScanItem_t *out);
bool wifiConnect(const char *ssid, const char *password);
bool wifiConnectInProgress();
bool wifiConnectSucceeded();
void wifiDisconnect(bool forgetCreds);
bool wifiHasSavedCreds();
void wifiLoadAndAutoConnect();

// Link mode: ESPNOW_ONLY / WIFI_ON(assist) / DUAL / WIFI_ONLY
uint8_t getLinkMode();
void setLinkMode(uint8_t mode);
bool linkModeIsEspNowOnly();
bool linkModeWifiEnabled(); // WIFI_ON / DUAL / WIFI_ONLY
bool linkModeIsDual();
bool linkModeIsWifiOnly();
bool wifiAssistActive();
bool transportUsesWifiIcon(); // UI: show WiFi icon vs signal bars
bool transportIsDuplicatePacket(const uint8_t *data, int len);
TransportKind_t transportLastUsed();
const char *transportStatusLine();

#endif
