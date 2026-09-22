#pragma once

#include <Arduino.h>

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

};
