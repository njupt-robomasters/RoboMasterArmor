#pragma once

#include <Arduino.h>

// 运行时配置。写入时同步保存到 NVS，掉电后仍保留。
// 只有被显式保存过的项才会持久化：未配网过的板子上电仍是离线状态，
// 配网过的板子上电自动按已保存的 Wi-Fi 和阵营进入联网流程。
class Settings {
  public:
    static void begin();

    static uint32_t getColor(uint32_t default_color);
    static void setColor(uint32_t color);

    static bool hasWiFiCredentials();
    static String getWiFiSsid();
    static String getWiFiPassword();
    static void setWiFiCredentials(const String &ssid, const String &password);
    static uint32_t getRobotId(uint32_t default_id);
    static void setRobotId(uint32_t robot_id);

    // 是否曾经完成过一次配网保存。决定上电后进入离线还是自动联网。
    static bool hasSavedConfig();
    // 清除全部已保存配置，恢复到未配网的离线状态。
    static void clear();
};