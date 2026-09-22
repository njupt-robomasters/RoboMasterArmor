#include "serial_logger.hpp"

#include <Arduino.h>

#include "config.hpp"

void SerialLogger::begin() {
    Serial.begin(Config::SERIAL_BAUDRATE);
}

void SerialLogger::printStartupDiagnostics() {
    Serial.printf("FW=%s compile=%s %s\n", Config::FIRMWARE_TAG, __DATE__, __TIME__);
    const uint64_t mac = ESP.getEfuseMac();
    Serial.printf("CHIP MAC=%04X%08X\n",
                  static_cast<unsigned>(mac >> 32),
                  static_cast<unsigned>(mac & 0xFFFFFFFFULL));
    Serial.printf("SERIAL FLAGS scan_only=%u can_only=%u can_debug=%u referee_hp=%u\n",
                  Config::SERIAL_SCAN_ONLY ? 1U : 0U,
                  Config::SERIAL_CAN_ONLY ? 1U : 0U,
                  Config::CAN_DEBUG ? 1U : 0U,
                  Config::REFEREE_HP_DEBUG ? 1U : 0U);
    Serial.printf("CAN PINS rx=%d tx=%d bitrate=%lu common_id=0x000\n",
                  static_cast<int>(Config::CAN_RX),
                  static_cast<int>(Config::CAN_TX),
                  static_cast<unsigned long>(Config::CAN_BITRATE));
    Serial.println("SERIAL DIAGNOSTICS ENABLED: WIFI + REFEREE + CAN + HP");
    Serial.flush();
}
