#pragma once

#include <Arduino.h>

class RefereeClient {
  public:
    static void begin();
    static void onLoop();
    static void stop();

    static bool isOnline();
    static bool isLoggedIn();
    static bool isDead();
    static uint32_t currentHp();
    static uint32_t maxHp();
    static void queueRemoteHitReports(uint32_t count);
    static void restartLogin();
    // 主机退出配网/离线时调用：关闭 UDP 并作废缓冲区里残留的旧裁判报文。
    static void endSessionGeneration();

  private:
    static constexpr uint16_t MAX_PAYLOAD_SIZE = 512;
    static constexpr uint8_t FRAME_HEADER_SIZE = 15;
    static constexpr uint8_t FRAME_OVERHEAD = 17;
    static constexpr uint32_t LOGIN_INTERVAL_MS = 500;
    static constexpr uint32_t STATUS_INTERVAL_MS = 100;
    static constexpr uint32_t OFFLINE_TIMEOUT_MS = 3000;

    static bool udp_started;
    static bool login_acknowledged;
    static bool match_received;
    static bool power_chassis;
    static bool power_gimbal;
    static bool power_shooter;
    static uint32_t last_login_ms;
    // 本次登录会话的起点，用于判断 0x21/0x24 是否属于上一个会话的残留报文。
    static uint32_t session_restart_ms;
    static uint32_t last_login_ack_ms;
    static uint32_t last_status_ms;
    static uint32_t last_match_ms;
    static uint32_t last_print_ms;
    static uint32_t last_network_debug_ms;
    static uint32_t last_hit_report_ms;
    static uint32_t reported_hit_count;
    static uint32_t current_hp_value;
    static uint32_t max_hp_value;
    static uint16_t sequence;
    static uint32_t robot_identifier;
    static uint32_t rx_packet_count;
    static uint32_t rx_frame_count;
    static uint32_t rx_invalid_count;
    static uint16_t last_rx_command;
    static uint32_t last_rx_packet_ms;

    static void drainRx();
    static void ensureUdp();
    static void receivePackets();
    static void printNetworkDebug();
    static void handleFrame(const uint8_t *data, size_t length);
    static void handleMatchSync(const uint8_t *payload, size_t length);
    static void handleControlCommand(const uint8_t *payload, size_t length);
    static void sendLogin();
    static void sendStatus();
    static bool sendHitReport(uint32_t adc_value);
    static bool sendFrame(uint16_t command, const uint8_t *payload, uint16_t length);

    static uint8_t robotType(uint8_t robot_id);
    static uint8_t crc8(const uint8_t *data, size_t length);
    static uint16_t crc16(const uint8_t *data, size_t length);
};
