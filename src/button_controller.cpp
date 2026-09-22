#include "button_controller.hpp"

#include <Arduino.h>

#include "can.hpp"
#include "config.hpp"
#include "hit.hpp"
#include "provisioning.hpp"

static bool key_pressed = false;
static bool key_config_triggered = false;
static uint32_t key_pressed_at_ms = 0;
static uint32_t key_guard_until_ms = 0;
// 切阵营不应期的开始时刻。和配网退出不应期分开记，两者互不干扰。
static uint32_t last_team_toggle_ms = 0;

static bool keyGuardActive(uint32_t now) {
    return key_guard_until_ms != 0 &&
           static_cast<int32_t>(key_guard_until_ms - now) > 0;
}

static void startKeyGuard() {
    key_guard_until_ms = millis() + Config::POST_PROVISIONING_KEY_GUARD_MS;
}

static void handleClick() {
    if (Hit::isProvisioningState()) {
        Provisioning::requestOffline();
        startKeyGuard();
        return;
    }
    if (Provisioning::isBusy() || Hit::isButtonLocked()) {
        return;
    }
    // 切阵营不应期：刚登录后裁判仍在连续下发，此时切阵营会让灯光和登录状态脱节。
    if (last_team_toggle_ms != 0 &&
        millis() - last_team_toggle_ms < Config::TEAM_SWITCH_KEY_GUARD_MS) {
        return;
    }
    last_team_toggle_ms = millis();
    Hit::toggleColor();
}

void ButtonController::begin() {
    pinMode(Config::KEY_PIN, INPUT_PULLUP);
}

void ButtonController::update() {
    const bool pressed = digitalRead(Config::KEY_PIN) == LOW;

    if (pressed && !key_pressed) {
        key_pressed = true;
        key_config_triggered = false;
        key_pressed_at_ms = millis();

        if (keyGuardActive(key_pressed_at_ms)) {
            // 整次按键作废，避免按住跨过不应期后又被识别成长按。
            key_config_triggered = true;
            return;
        }

        // 配网短按在按下沿立即退出，同时阻止本次按住继续触发长按。
        if (Hit::isProvisioningState()) {
            handleClick();
            key_config_triggered = true;
        }
    }

    const uint32_t now = millis();
    if (pressed && !key_config_triggered &&
        now - key_pressed_at_ms >= Config::LONG_PRESS_MS) {
        key_config_triggered = true;
        if (Hit::isLoginSuccessfulState()) {
            Provisioning::enterConfigMode();
        } else if (Provisioning::isSessionActive() ||
                   Provisioning::isLocalHost() ||
                   Hit::isSharedStateActive()) {
            Provisioning::requestOffline();
            Hit::signalOfflineExit();
            startKeyGuard();
        } else {
            Provisioning::enterConfigMode();
        }
    }

    if (!pressed) {
        if (key_pressed && !key_config_triggered &&
            now - key_pressed_at_ms >= 50) {
            handleClick();
        }
        key_pressed = false;
        key_config_triggered = false;
    }
}
