#pragma once

#include <Arduino.h>

#include "hit.hpp"

class LedController {
  public:
    static void begin(uint32_t initial_color);
    static void setBrightness(uint8_t brightness);
    static void showSolid(uint32_t color);
    static void render(
        Hit::LightMode mode,
        uint32_t display_color,
        bool local_host,
        bool blink_on,
        bool hit_active,
        bool offline_signal,
        uint32_t now);

  private:
    static void showColor(uint32_t color);
};
