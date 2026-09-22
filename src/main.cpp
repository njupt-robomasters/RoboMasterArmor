#include <Arduino.h>

#include "can.hpp"
#include "button_controller.hpp"
#include "config.hpp"
#include "hit.hpp"
#include "provisioning.hpp"
#include "referee_client.hpp"
#include "settings.hpp"
#include "serial_logger.hpp"
#include "system_state.hpp"

static uint32_t startup_diag_last_ms = 0;

void setup() {
    SystemState::begin();
    Settings::begin();

    // platformio.ini已开启原生USB CDC，Serial用于电脑串口监视器。
    SerialLogger::begin();
    delay(500);
    if (!Config::SERIAL_SCAN_ONLY) {
        Serial.println();
        Serial.println("Outpost armor controller");
        SerialLogger::printStartupDiagnostics();
        startup_diag_last_ms = millis();
    }

    ButtonController::begin();

    Hit::begin();
    Can::begin();
    Provisioning::beginNormal();
    RefereeClient::begin();
}

void loop() {

    if (!Config::SERIAL_SCAN_ONLY && millis() < 30000 &&
        millis() - startup_diag_last_ms >= 5000) {
        SerialLogger::printStartupDiagnostics();
        startup_diag_last_ms = millis();
    }
    ButtonController::update();
    Hit::onLoop();
    Can::onLoop();
    Provisioning::onLoop();
    RefereeClient::onLoop();
}
