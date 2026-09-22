#pragma once

#include <Arduino.h>
#include <driver/twai.h>

// Unified CAN protocol: standard ID 0x000, DLC=8, data[0]=0xA0.
class Can {
  public:
    static void begin();
    static void onLoop();
    static void requestTeamToggle();
    static void notifyLocalStateChanged();
    static void requestHostClaim();
    static void notifyOfflineColorChanged();
    static void requestOffline();
    static void broadcastHostStateNow();
    static void clearHostState();
    static void stopOfflineColorNormalization();
    static uint32_t rxCount();
    static uint32_t txCount();
    static uint32_t txErrorCount();
    static uint32_t lastRxMs();

    static constexpr uint32_t COMMON_CAN_ID = 0x000;
    static constexpr uint8_t PROTOCOL_MAGIC = 0xA0;
    enum Mode : uint8_t { MODE_OFFLINE, MODE_PROVISIONING, MODE_CONNECTING,
                          MODE_REFEREE_LOGIN, MODE_ONLINE, MODE_DEAD };
    enum Event : uint8_t { EVENT_STATE, EVENT_TEAM_TOGGLE, EVENT_HOST_CLAIM,
                           EVENT_OFFLINE, EVENT_HIT, EVENT_ACK };
    static constexpr uint8_t FLAG_PROVISIONING = 1u << 0;
    static constexpr uint8_t FLAG_BLINK_ON = 1u << 1;
    static constexpr uint8_t FLAG_HOST = 1u << 2;
    static constexpr uint8_t FLAG_CHANGED = 1u << 3;
    static constexpr uint8_t FLAG_HOST_CLAIM = 1u << 4;

  private:
    // 主机状态广播间隔。决定从机相位滞后的上限，越小越齐。
    static constexpr uint32_t STATE_INTERVAL_MS = 50;
    static constexpr uint32_t HOST_TIMEOUT_MS = 1500;
    // 配网/联网阶段主机主循环会阻塞近 1 秒，看门狗必须放宽，否则会掉回离线。
    static constexpr uint32_t HOST_TIMEOUT_DURING_WIFI_MS = 6000;
    static constexpr uint32_t REQUEST_RETRY_MS = 120;
    // 主机接管后持续广播 HOST_CLAIM 的时间，覆盖从机漏帧和重传。
    static constexpr uint32_t HOST_CLAIM_ADVERTISE_MS = 1000;
    static uint32_t rx_count, tx_count, tx_error_count, last_rx_ms;
    static uint32_t last_state_tx_ms, last_request_tx_ms, last_host_rx_ms;
    static uint8_t state_version, last_host_version, pending_event;
    static uint8_t last_broadcast_mode;
    static uint8_t pending_event_arg, pending_event_version, next_event_sequence;
    static uint8_t pending_team_color, pending_event_attempts;
    static uint32_t last_local_hit_count;
    static bool host_state_valid, host_changed;
    // 接管后持续携带 HOST_CLAIM，避免从机漏掉一帧就看不到主机切换。
    static uint32_t host_claim_until_ms;
    static uint8_t host_event, host_event_arg;
    static uint8_t last_team_request_sequence, last_hit_event_sequence;
    static bool team_request_seen, hit_event_seen;
    static bool transportBegin();
    static bool transportReceive(twai_message_t &frame);
    static bool transportSend(twai_message_t &frame);
    static bool transmit(twai_message_t &frame, const char *label);
    static void receiveFrames();
    static void handleFrame(const twai_message_t &frame);
    static void applyHostState(const twai_message_t &frame);
    static void handleRequest(const twai_message_t &frame);
    static void sendHostState();
    static void sendOfflineState();
    static void sendRequest(uint8_t event, uint8_t argument);
    static uint8_t localMode();
    static uint8_t localFlags(uint8_t mode);
    static bool isNewerVersion(uint8_t version, uint8_t previous);
    static uint32_t hostTimeoutMs();
    static void markStateChanged();
    static void printStatus();
};
