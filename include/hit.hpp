#pragma once

#include <Arduino.h>

class Hit {
  public:
    enum LightMode : uint8_t {
        LIGHT_OFFLINE = 0,
        LIGHT_PROVISIONING = 1,
        LIGHT_NETWORK_CONNECTING = 2,
        LIGHT_REFEREE_LOGIN = 3,
        LIGHT_ONLINE = 4,
        LIGHT_DEAD = 5,
    };

    static uint32_t RED;
    static uint32_t BLUE;
    static uint32_t YELLOW;
    static uint32_t PURPLE;

    static uint32_t color;
    static uint32_t hit_count;
    static uint32_t last_hit_ms;
    static uint32_t adc_value;

    static void begin();
    static void onLoop();
    static void toggleColor();
    static void signalOfflineExit();
    static uint32_t adcValue();
    static void setProvisioningMode(bool enabled);
    static void setNetworkConnectingMode(bool enabled);
    static void showNetworkConnecting();
    static void showProvisioningNow(bool blink_on);
    static void signalWifiConnected();
    static void setSharedState(bool valid, LightMode mode, uint32_t shared_color);
    static void setSynchronizedColor(uint32_t shared_color);
    // 配网保存后按机器人编号统一阵营，同步更新 Hit::color、Settings 和 SystemState。
    static void applyConfiguredTeam(uint32_t robot_id);
    static void setSynchronizedBlinkPhase(bool on);
    static void beginTeamSwitchLightHold();
    static bool isTeamSwitchLightHeld();
    // 主机保存后从机保持黄灯常亮，直到联网成功。
    static void beginProvisioningSaveHold();
    static void endProvisioningSaveHold();
    static bool isProvisioningSaveHeld();
    static bool isButtonLocked();
    static bool isSharedStateActive();
    static bool isLoginSuccessfulState();
    static bool isProvisioningState();

  private:
    static constexpr uint32_t HIT_BLINK_MS = 50;
    static bool provisioning_mode;
    static bool network_connecting_mode;
    static bool shared_state_valid;
    static LightMode shared_mode;
    static uint32_t shared_color;
    static bool shared_blink_on;
    static uint32_t wifi_signal_start_ms;
    static uint32_t wifi_signal_until_ms;
    static bool provisioning_save_hold;

    static void updatePixels();
    static bool loginPending();
    static bool hitDetectionBlocked();
};
