#include "wifi_manager.hpp"

#include <WiFi.h>

void WifiManager::beginNormal() {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_OFF);
}

void WifiManager::stop() {
    WiFi.disconnect(false, false);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

bool WifiManager::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

bool WifiManager::prepareSta() {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    delay(200);
    return WiFi.mode(WIFI_STA);
}

bool WifiManager::prepareAp() {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    delay(50);
    WiFi.mode(WIFI_OFF);
    delay(300);
    return WiFi.mode(WIFI_AP);
}
