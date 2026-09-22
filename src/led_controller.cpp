#include "led_controller.hpp"

#include <Adafruit_NeoPixel.h>

#include "config.hpp"

static const uint32_t YELLOW = Adafruit_NeoPixel::Color(255, 180, 0);
static const uint32_t PURPLE = Adafruit_NeoPixel::Color(160, 0, 255);

static Adafruit_NeoPixel pixels(
    Config::WS2812_COUNT,
    Config::WS2812_PIN,
    NEO_GRB + NEO_KHZ800);
static uint32_t last_shown_color = 0;
static bool has_last_shown_color = false;

void LedController::begin(uint32_t initial_color) {
    pixels.begin();
    pixels.setBrightness(Config::LED_BRIGHTNESS);
    showColor(initial_color);
}

void LedController::setBrightness(uint8_t brightness) {
    pixels.setBrightness(brightness);
    has_last_shown_color = false;
}

void LedController::showColor(uint32_t color) {
    // 相同画面不重复向 WS2812 发送数据，避免主循环高频刷新造成可见抖动。
    if (has_last_shown_color && last_shown_color == color) {
        return;
    }
    pixels.fill(color, 0, Config::WS2812_COUNT);
    pixels.show();
    last_shown_color = color;
    has_last_shown_color = true;
}

void LedController::showSolid(uint32_t color) {
    showColor(color);
}

void LedController::render(
    Hit::LightMode mode,
    uint32_t display_color,
    bool local_host,
    bool blink_on,
    bool hit_active,
    bool offline_signal,
    uint32_t now) {
    if (offline_signal) {
        if ((now / 160) % 2 == 0) {
            showColor(display_color);
        } else {
            showColor(0);
        }
        return;
    }

    if (hit_active) {
        showColor(0);
        return;
    }

    switch (mode) {
      case Hit::LIGHT_PROVISIONING:
        if (blink_on) {
            showColor(YELLOW);
        } else {
            showColor(0);
        }
        break;
      case Hit::LIGHT_NETWORK_CONNECTING:
        showColor(YELLOW);
        break;
      case Hit::LIGHT_REFEREE_LOGIN:
        showColor(PURPLE);
        break;
      case Hit::LIGHT_DEAD:
        showColor(0);
        break;
      case Hit::LIGHT_OFFLINE:
      case Hit::LIGHT_ONLINE:
      default:
        showColor(display_color);
        break;
    }
}
