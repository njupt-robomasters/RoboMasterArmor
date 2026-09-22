#include "settings.hpp"

static String wifi_ssid;
static String wifi_password;
static uint32_t runtime_color = 0;
static uint32_t runtime_robot_id = 0;

void Settings::begin() {
    // 配置只在本次上电期间有效，断电/复位后从空配置开始。
    wifi_ssid = "";
    wifi_password = "";
    runtime_color = 0;
    runtime_robot_id = 0;
}

uint32_t Settings::getColor(uint32_t default_color) {
    return runtime_color == 0 ? default_color : runtime_color;
}

void Settings::setColor(uint32_t color) {
    runtime_color = color;
}

bool Settings::hasWiFiCredentials() {
    return wifi_ssid.length() > 0;
}

String Settings::getWiFiSsid() {
    return wifi_ssid;
}

String Settings::getWiFiPassword() {
    return wifi_password;
}

void Settings::setWiFiCredentials(
    const String &ssid,
    const String &password) {
    wifi_ssid = ssid;
    wifi_password = password;
}

uint32_t Settings::getRobotId(uint32_t default_id) {
    return runtime_robot_id == 0 ? default_id : runtime_robot_id;
}

void Settings::setRobotId(uint32_t robot_id) {
    runtime_robot_id = robot_id;
}
