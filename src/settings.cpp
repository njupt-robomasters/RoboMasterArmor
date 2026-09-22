#include "settings.hpp"

#include <Preferences.h>

namespace {
constexpr const char *NVS_NAMESPACE = "outpost";
constexpr const char *KEY_SSID = "ssid";
constexpr const char *KEY_PASS = "pass";
constexpr const char *KEY_COLOR = "color";
constexpr const char *KEY_ROBOT_ID = "robot_id";
constexpr const char *KEY_SAVED = "saved";

Preferences prefs;
bool opened = false;
}

static void openPrefs() {
    if (!opened) {
        opened = prefs.begin(NVS_NAMESPACE, false);
    }
}

void Settings::begin() {
    openPrefs();
    if (!opened) {
        Serial.println("SETTINGS NVS open failed, running with defaults");
    }
}

bool Settings::hasSavedConfig() {
    openPrefs();
    return opened && prefs.getBool(KEY_SAVED, false);
}

void Settings::clear() {
    openPrefs();
    if (!opened) {
        return;
    }
    prefs.clear();
}

uint32_t Settings::getColor(uint32_t default_color) {
    openPrefs();
    if (!opened) {
        return default_color;
    }
    const uint32_t stored = prefs.getUInt(KEY_COLOR, 0);
    return stored == 0 ? default_color : stored;
}

void Settings::setColor(uint32_t color) {
    openPrefs();
    if (opened) {
        prefs.putUInt(KEY_COLOR, color);
    }
}

bool Settings::hasWiFiCredentials() {
    openPrefs();
    return opened && prefs.isKey(KEY_SSID) && prefs.getString(KEY_SSID, "").length() > 0;
}

String Settings::getWiFiSsid() {
    openPrefs();
    return opened ? prefs.getString(KEY_SSID, "") : String();
}

String Settings::getWiFiPassword() {
    openPrefs();
    return opened ? prefs.getString(KEY_PASS, "") : String();
}

void Settings::setWiFiCredentials(
    const String &ssid,
    const String &password) {
    openPrefs();
    if (!opened) {
        return;
    }
    prefs.putString(KEY_SSID, ssid);
    prefs.putString(KEY_PASS, password);
    // 配网保存过才标记为已配置，决定下次上电是否自动联网。
    prefs.putBool(KEY_SAVED, true);
}

uint32_t Settings::getRobotId(uint32_t default_id) {
    openPrefs();
    if (!opened) {
        return default_id;
    }
    const uint32_t stored = prefs.getUInt(KEY_ROBOT_ID, 0);
    return stored == 0 ? default_id : stored;
}

void Settings::setRobotId(uint32_t robot_id) {
    openPrefs();
    if (opened) {
        prefs.putUInt(KEY_ROBOT_ID, robot_id);
    }
}