#include "referee_client.hpp"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_timer.h>

#include "config.hpp"
#include "hit.hpp"
#include "provisioning.hpp"
#include "settings.hpp"
#include "system_state.hpp"

static WiFiUDP udp;
static const IPAddress server_ip(
    Config::REFEREE_SERVER_IP[0],
    Config::REFEREE_SERVER_IP[1],
    Config::REFEREE_SERVER_IP[2],
    Config::REFEREE_SERVER_IP[3]);
static constexpr size_t RX_BUFFER_SIZE = 17 + 512;
static uint8_t rx_buffer[RX_BUFFER_SIZE];

bool RefereeClient::udp_started = false;
bool RefereeClient::login_acknowledged = false;
bool RefereeClient::match_received = false;
bool RefereeClient::power_chassis = true;
bool RefereeClient::power_gimbal = true;
bool RefereeClient::power_shooter = true;
uint32_t RefereeClient::last_login_ms = 0;
uint32_t RefereeClient::session_restart_ms = 0;
uint32_t RefereeClient::last_login_ack_ms = 0;
uint32_t RefereeClient::last_status_ms = 0;
uint32_t RefereeClient::last_match_ms = 0;
uint32_t RefereeClient::last_print_ms = 0;
uint32_t RefereeClient::last_network_debug_ms = 0;
uint32_t RefereeClient::last_hit_report_ms = 0;
uint32_t RefereeClient::reported_hit_count = 0;
static uint32_t pending_remote_hit_reports = 0;
uint32_t RefereeClient::current_hp_value = 200;
uint32_t RefereeClient::max_hp_value = 200;
uint16_t RefereeClient::sequence = 0;
uint32_t RefereeClient::robot_identifier = 0;
uint32_t RefereeClient::rx_packet_count = 0;
uint32_t RefereeClient::rx_frame_count = 0;
uint32_t RefereeClient::rx_invalid_count = 0;
uint16_t RefereeClient::last_rx_command = 0;
uint32_t RefereeClient::last_rx_packet_ms = 0;
static uint32_t last_rx_command_log_ms = 0;
static uint32_t tx_ok_count = 0;
static uint32_t tx_fail_count = 0;
static uint32_t last_hp_debug_ms = 0;
static uint32_t last_hp_debug_current = 0xFFFFFFFF;
static uint32_t last_hp_debug_max = 0xFFFFFFFF;
static IPAddress last_rx_ip;
static uint16_t last_rx_port = 0;

static void writeU16(uint8_t *dst, uint16_t value) {
    memcpy(dst, &value, sizeof(value));
}

static void writeU32(uint8_t *dst, uint32_t value) {
    memcpy(dst, &value, sizeof(value));
}

static void writeI16(uint8_t *dst, int16_t value) {
    memcpy(dst, &value, sizeof(value));
}

static void writeFloat(uint8_t *dst, float value) {
    memcpy(dst, &value, sizeof(value));
}

static void writeU64(uint8_t *dst, uint64_t value) {
    memcpy(dst, &value, sizeof(value));
}

static uint16_t readU16(const uint8_t *src) {
    uint16_t value = 0;
    memcpy(&value, src, sizeof(value));
    return value;
}

static uint32_t readU32(const uint8_t *src) {
    uint32_t value = 0;
    memcpy(&value, src, sizeof(value));
    return value;
}

void RefereeClient::begin() {
    robot_identifier = static_cast<uint32_t>(ESP.getEfuseMac());
    reported_hit_count = Hit::hit_count;
    last_hit_report_ms = 0;
    pending_remote_hit_reports = 0;
    current_hp_value = 200;
    max_hp_value = 200;
    sequence = 0;
    rx_packet_count = 0;
    rx_frame_count = 0;
    rx_invalid_count = 0;
    last_rx_command = 0;
    last_rx_packet_ms = 0;
    last_rx_command_log_ms = 0;
    last_hp_debug_ms = 0;
    last_hp_debug_current = 0xFFFFFFFF;
    last_hp_debug_max = 0xFFFFFFFF;
    tx_ok_count = 0;
    tx_fail_count = 0;
    last_rx_ip = IPAddress();
    last_rx_port = 0;
    stop();

    if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
        "REFEREE configured: server=%s:%u local_port=%u robot_id=%lu identifier=0x%08lX\n",
        server_ip.toString().c_str(),
        Config::REFEREE_SERVER_PORT,
        Config::REFEREE_LOCAL_PORT,
        static_cast<unsigned long>(Settings::getRobotId(3)),
        static_cast<unsigned long>(robot_identifier));
}

// 读掉并丢弃 socket 缓冲区里已排队的报文。udp.stop() 不会清掉这些包，
void RefereeClient::drainRx() {
    uint32_t drained = 0;
    while (udp.parsePacket() > 0) {
        const int read_size = udp.read(rx_buffer, sizeof(rx_buffer));
        if (read_size <= 0) {
            break;
        }
        drained++;
    }
    if (drained > 0 && !Config::SERIAL_SCAN_ONLY) {
        Serial.printf("REF UDP drained %lu stale packet(s) at session boundary",
            static_cast<unsigned long>(drained));
    }
}
void RefereeClient::stop() {
    if (udp_started) {
        // 关 socket 前先读干净，否则旧包会被下一次会话当成登录结果。
        drainRx();
        udp.stop();
    }

    udp_started = false;
    login_acknowledged = false;
    match_received = false;
    last_login_ms = 0;
    last_login_ack_ms = 0;
    last_status_ms = 0;
    last_match_ms = 0;
    last_print_ms = 0;
    last_network_debug_ms = 0;
    if (Provisioning::isLocalHost()) {
        SystemState::setRefereeState(SystemState::Referee::Disabled);
    }
}

// 主机下线/退出配网时调用：关闭 UDP 并丢弃残留的旧裁判报文。
void RefereeClient::endSessionGeneration() {
    stop();
}

void RefereeClient::restartLogin() {
    if (!Provisioning::isLocalHost()) {
        return;
    }

    // 保留击打累计和待上报队列，只重置裁判登录/比赛状态。
    stop();
    // 每次重新登录都从满血开始，不能沿用上一次离线/比赛会话的残血值。
    current_hp_value = 200;
    max_hp_value = 200;
    last_login_ack_ms = 0;
    // 会话起点：之后 REFEREE_SESSION_SETTLE_MS 内的 0x21/0x24 按旧会话残留丢弃。
    session_restart_ms = millis();
    // 阵营切换后立即进入登录中状态，避免下一次 loop 先显示新阵营常亮。
    SystemState::setRefereeState(SystemState::Referee::LoginPending);
    if (!Config::SERIAL_SCAN_ONLY) {
        Serial.println("REF login restart requested");
    }
}

void RefereeClient::ensureUdp() {
    if (udp_started || WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (!udp.begin(Config::REFEREE_LOCAL_PORT)) {
        if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
            "REF UDP begin failed: local_port=%u\n",
            Config::REFEREE_LOCAL_PORT);
        return;
    }

    udp_started = true;
    // 新 socket 建立后立刻清空缓冲区，这里读到的报文都属于上一个会话。
    drainRx();
    session_restart_ms = millis();
    login_acknowledged = false;
    match_received = false;
    // 新的 Wi-Fi/裁判会话必须重新显示满血，等待裁判系统下发最新同步值。
    current_hp_value = 200;
    max_hp_value = 200;
    last_login_ms = 0;
    last_login_ack_ms = 0;
    last_status_ms = 0;
    last_match_ms = 0;
    last_network_debug_ms = 0;
    SystemState::setRefereeState(SystemState::Referee::LoginPending);

    const IPAddress local = WiFi.localIP();
    const IPAddress mask = WiFi.subnetMask();
    const IPAddress gateway = WiFi.gatewayIP();
    const bool same_subnet =
        (static_cast<uint32_t>(local) & static_cast<uint32_t>(mask)) ==
        (static_cast<uint32_t>(server_ip) & static_cast<uint32_t>(mask));

    if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
        "REF UDP ready: local=%s:%u server=%s:%u gw=%s same_subnet=%u\n",
        local.toString().c_str(),
        Config::REFEREE_LOCAL_PORT,
        server_ip.toString().c_str(),
        Config::REFEREE_SERVER_PORT,
        gateway.toString().c_str(),
        same_subnet ? 1 : 0);
    if (!same_subnet) {
        if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
            "REF WARN server %s is not in STA subnet %s/%s. "
            "If this is a Windows hotspot, the PC address is usually %s.\n",
            server_ip.toString().c_str(),
            local.toString().c_str(),
            mask.toString().c_str(),
            gateway.toString().c_str());
    }
}

void RefereeClient::onLoop() {
    if (!Provisioning::isLocalHost()) {
        stop();
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        stop();
        return;
    }

    ensureUdp();
    if (!udp_started) {
        return;
    }

    receivePackets();
    printNetworkDebug();

    const uint32_t now = millis();
    if (!match_received) {
        // 登录结果已经成功时，等待比赛同步，不再重复发送登录包。
        // 只有尚未收到0x21时，才按500ms周期重发0x20。
        if (!login_acknowledged && now - last_login_ms >= LOGIN_INTERVAL_MS) {
            last_login_ms = now;
            sendLogin();
        }
        // 还没有比赛同步时不发送状态保活和击打上报，但不能像以前那样直接
        // return 整个循环：一旦同步标志被清掉，保活和击打上报会跟着停掉
        // 一个周期。这里只跳过依赖比赛状态的后续逻辑。
        if (!match_received) {
            return;
        }
    }

    if (now - last_match_ms > OFFLINE_TIMEOUT_MS) {
        if (!Config::SERIAL_SCAN_ONLY) Serial.println("REF offline: no 0x24 for 3 seconds, restart login");
        match_received = false;
        login_acknowledged = false;
        last_login_ms = 0;
        return;
    }

    if (now - last_status_ms >= STATUS_INTERVAL_MS) {
        last_status_ms = now;
        sendStatus();
    }

    // 裁判系统通信也沿用击打冷却，避免本地或 CAN 队列中的击打报文连续发送。
    const bool hit_report_cooldown_elapsed =
        last_hit_report_ms == 0 ||
        now - last_hit_report_ms > Config::HIT_COOLDOWN_MS;
    if (hit_report_cooldown_elapsed) {
        if (Hit::hit_count != reported_hit_count) {
            if (sendHitReport(Hit::adcValue())) {
                reported_hit_count++;
                last_hit_report_ms = now;
            }
        } else if (pending_remote_hit_reports > 0 &&
                   sendHitReport(0)) {
            pending_remote_hit_reports--;
            last_hit_report_ms = now;
        }
    }

    if (now - last_print_ms >= 1000) {
        last_print_ms = now;
        if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
            "REF STATUS online=1 hp=%lu/%lu hit_count=%lu\n",
            static_cast<unsigned long>(current_hp_value),
            static_cast<unsigned long>(max_hp_value),
            static_cast<unsigned long>(Hit::hit_count));
    }
}

void RefereeClient::receivePackets() {
    while (true) {
        const int packet_size = udp.parsePacket();
        if (packet_size <= 0) {
            return;
        }

        const int read_size = udp.read(rx_buffer, sizeof(rx_buffer));
        if (read_size > 0) {
            rx_packet_count++;
            last_rx_packet_ms = millis();
            last_rx_ip = udp.remoteIP();
            last_rx_port = udp.remotePort();
            handleFrame(rx_buffer, static_cast<size_t>(read_size));
        }
    }
}

void RefereeClient::handleFrame(const uint8_t *data, size_t length) {
    if (length < FRAME_OVERHEAD || data[0] != 0x5A) {
        rx_invalid_count++;
        return;
    }

    const uint16_t payload_length =
        static_cast<uint16_t>(data[1]) |
        static_cast<uint16_t>((data[2] & 0x0F) << 8);
    const size_t total_length = FRAME_OVERHEAD + payload_length;

    if (payload_length > MAX_PAYLOAD_SIZE || length < total_length) {
        rx_invalid_count++;
        return;
    }

    if (crc8(data, 3) != data[3]) {
        rx_invalid_count++;
        return;
    }

    const uint16_t packet_crc =
        static_cast<uint16_t>(data[total_length - 2]) |
        static_cast<uint16_t>(data[total_length - 1] << 8);
    if (crc16(data, total_length - 2) != packet_crc) {
        rx_invalid_count++;
        return;
    }

    const uint16_t command =
        static_cast<uint16_t>(data[13]) |
        static_cast<uint16_t>(data[14] << 8);
    const uint8_t *payload = &data[15];
    rx_frame_count++;
    last_rx_command = command;

    const uint32_t now = millis();
    const bool important_command = command == 0x21 || command == 0x24;
    const bool log_due = now - last_rx_command_log_ms >= 1000;
    if (important_command || log_due) {
        if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
            "REF RX cmd=0x%04X payload=%u bytes=%u\n",
            command,
            payload_length,
            static_cast<unsigned>(length));
        last_rx_command_log_ms = now;
    }

    // 会话沉降期：重开 socket 后最初几百毫秒读到的包可能仍是旧会话残留。
    // 放行会让板子用旧包的 ACK/同步误判登录成功。这里只续心跳，不刷新登录状态。
    const bool session_settling =
        session_restart_ms != 0 &&
        now - session_restart_ms < Config::REFEREE_SESSION_SETTLE_MS;
    if (session_settling && (command == 0x21 || command == 0x24)) {
        if (command == 0x24 && last_match_ms != 0) {
            last_match_ms = now;
        }
        if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
            "REF settle: drop stale cmd=0x%04X (t=%lu ms after session start)\n",
            command,
            static_cast<unsigned long>(now - session_restart_ms));
        return;
    }

    if (command == 0x21 && payload_length >= 4) {
        const uint32_t result = readU32(payload);
        login_acknowledged = result == 0;
        if (login_acknowledged) {
            // 已在线时周期 ACK 只刷新时间，不得撤销在线状态，否则会跳回紫灯。
            if (!match_received) {
                last_login_ack_ms = millis();
            } else {
                last_login_ack_ms = millis();
                last_match_ms = millis();
            }
        } else {
            match_received = false;
            last_login_ack_ms = 0;
        }
        SystemState::setRefereeState(
            (login_acknowledged && match_received)
                ? SystemState::Referee::LoggedIn
                : SystemState::Referee::LoginPending);
        if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
            "REF LOGIN ACK result=%lu\n",
            static_cast<unsigned long>(result));
    } else if (command == 0x24) {
        handleMatchSync(payload, payload_length);
    } else if (command == 0x02) {
        handleControlCommand(payload, payload_length);
    }
}

void RefereeClient::handleMatchSync(
    const uint8_t *payload,
    size_t length) {
    // ACK 之前收到的0x24可能是旧会话残留，不能用来结束本次紫灯登录。
    if (!login_acknowledged || last_login_ack_ms == 0) {
        return;
    }

    // 当前12.0.0.34 Engine实际下发的robots_data_sync载荷为68字节。
    // 学长资料中的旧版本结构为74字节，但当前HP/maxHP字段偏移不变。
    if (length < 68) {
        if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
            "REF MATCH SYNC ignored: payload=%u, need>=68\n",
            static_cast<unsigned>(length));
        return;
    }

    const uint8_t progress = payload[0];
    const uint32_t current_hp = readU32(&payload[7]);
    const uint32_t maximum_hp = readU32(&payload[11]);

    current_hp_value = current_hp;
    max_hp_value = maximum_hp;
    last_match_ms = millis();

    const uint32_t now = millis();
    if (Config::REFEREE_HP_DEBUG && maximum_hp > 0 &&
        (current_hp != last_hp_debug_current ||
         maximum_hp != last_hp_debug_max ||
         now - last_hp_debug_ms >= 1000)) {
        last_hp_debug_current = current_hp;
        last_hp_debug_max = maximum_hp;
        last_hp_debug_ms = now;
        if (!Config::SERIAL_CAN_ONLY) Serial.printf(
            "REF HP current=%lu max=%lu state=%s\n",
            static_cast<unsigned long>(current_hp),
            static_cast<unsigned long>(maximum_hp),
            current_hp == 0 ? "DEAD" : "ALIVE");
    }

    if (!match_received) {
        if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
            "REF MATCH SYNC accepted payload=%u progress=%u hp=%lu/%lu\n",
            static_cast<unsigned>(length),
            progress,
            static_cast<unsigned long>(current_hp_value),
            static_cast<unsigned long>(max_hp_value));
    }

    match_received = true;
    SystemState::setRefereeState(
        current_hp_value == 0
            ? SystemState::Referee::Dead
            : SystemState::Referee::LoggedIn);
}

void RefereeClient::handleControlCommand(
    const uint8_t *payload,
    size_t length) {
    if (length < 8) {
        return;
    }

    const uint16_t operation = readU16(payload);
    const uint16_t parameter = readU16(&payload[2]);
    const uint32_t identifier = readU32(&payload[4]);

    if (identifier != robot_identifier) {
        return;
    }

    switch (operation) {
      case 2:
        power_chassis = false;
        break;
      case 3:
        power_gimbal = false;
        break;
      case 4:
        power_shooter = false;
        break;
      case 5:
        power_chassis = true;
        power_gimbal = true;
        power_shooter = true;
        break;
      case 6:
        match_received = false;
        login_acknowledged = false;
        SystemState::setRefereeState(SystemState::Referee::LoginPending);
        break;
      case 7:
        // 本装甲板没有底盘功率控制器，暂时只记录命令。
        break;
      default:
        return;
    }

    if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
        "REF CONTROL op=%u parameter=%u\n",
        operation,
        parameter);
}

void RefereeClient::sendLogin() {
    uint8_t payload[8] = {0};
    writeU32(&payload[0], robot_identifier);
    writeU32(&payload[4], 1);

    const bool sent = sendFrame(0x20, payload, sizeof(payload));
    if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
        "REF TX 0x20 LOGIN result=%s seq=%u dest=%s:%u identifier=0x%08lX\n",
        sent ? "OK" : "FAIL",
        static_cast<unsigned>(sequence - 1),
        server_ip.toString().c_str(),
        Config::REFEREE_SERVER_PORT,
        static_cast<unsigned long>(robot_identifier));
}

void RefereeClient::printNetworkDebug() {
    const uint32_t now = millis();
    if (now - last_network_debug_ms < 1000) {
        return;
    }

    last_network_debug_ms = now;
    const uint32_t last_rx_age =
        last_rx_packet_ms == 0 ? 0xFFFFFFFF : now - last_rx_packet_ms;

    if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
        "REF NET udp=1 login_ack=%u match=%u rx_packets=%lu rx_frames=%lu "
        "rx_invalid=%lu last_cmd=0x%04X last_rx_age_ms=%lu rssi=%d dBm "
        "local=%s gw=%s server=%s:%u last_rx_from=%s:%u tx_ok=%lu tx_fail=%lu\n",
        login_acknowledged ? 1 : 0,
        match_received ? 1 : 0,
        static_cast<unsigned long>(rx_packet_count),
        static_cast<unsigned long>(rx_frame_count),
        static_cast<unsigned long>(rx_invalid_count),
        last_rx_command,
        static_cast<unsigned long>(last_rx_age),
        WiFi.RSSI(),
        WiFi.localIP().toString().c_str(),
        WiFi.gatewayIP().toString().c_str(),
        server_ip.toString().c_str(),
        Config::REFEREE_SERVER_PORT,
        last_rx_ip.toString().c_str(),
        last_rx_port,
        static_cast<unsigned long>(tx_ok_count),
        static_cast<unsigned long>(tx_fail_count));
}

void RefereeClient::sendStatus() {
    uint8_t payload[117] = {0};
    const uint32_t hp = min(current_hp_value, static_cast<uint32_t>(0xFFFF));
    writeU16(&payload[0], static_cast<uint16_t>(hp));

    writeFloat(&payload[2], 0.0f);
    writeFloat(&payload[6], 0.0f);
    writeFloat(&payload[10], 0.0f);
    writeFloat(&payload[14], 0.0f);
    writeU32(&payload[18], 0);
    writeFloat(&payload[22], 0.0f);
    writeFloat(&payload[26], 0.0f);
    writeFloat(&payload[30], 0.0f);
    writeFloat(&payload[34], 0.0f);
    payload[38] = static_cast<uint8_t>(constrain(-WiFi.RSSI(), 0, 127));
    writeU32(&payload[39], 0);
    writeI16(&payload[43], 0);
    writeI16(&payload[45], 0);
    writeI16(&payload[47], 0);

    // system_status_t，最小实现只置 power=1。
    payload[49] = 1;

    // 电源状态：底盘、云台、发射机构。
    payload[106] = power_chassis ? 1 : 0;
    payload[107] = power_gimbal ? 1 : 0;
    payload[108] = power_shooter ? 1 : 0;

    writeU64(&payload[109], static_cast<uint64_t>(esp_timer_get_time()));
    sendFrame(0x01, payload, sizeof(payload));
}

bool RefereeClient::sendHitReport(uint32_t adc_value) {
    uint8_t payload[30] = {0};
    payload[0] = 1;
    writeI16(&payload[1], 0);
    writeI16(&payload[3], 0);
    payload[5] = 0;
    writeFloat(&payload[6], static_cast<float>(adc_value));
    writeFloat(&payload[10], 0.0f);
    writeFloat(&payload[14], 0.0f);
    writeFloat(&payload[18], 0.0f);
    writeU64(&payload[22], static_cast<uint64_t>(esp_timer_get_time()));

    if (!sendFrame(0x25, payload, sizeof(payload))) {
        return false;
    }

    if (!Config::SERIAL_SCAN_ONLY) Serial.printf(
        "REF TX 0x25 HIT count=%lu adc=%lu\n",
        static_cast<unsigned long>(Hit::hit_count),
        static_cast<unsigned long>(Hit::adcValue()));
    return true;
}

void RefereeClient::queueRemoteHitReports(uint32_t count) {
    const uint32_t available = 0xFFFFFFFFu - pending_remote_hit_reports;
    pending_remote_hit_reports += min(count, available);
    if (Config::HIT_DEBUG) {
        Serial.printf(
            "HIT REF_QUEUE remote_add=%lu pending=%lu\n",
            static_cast<unsigned long>(count),
            static_cast<unsigned long>(pending_remote_hit_reports));
    }
}

bool RefereeClient::sendFrame(
    uint16_t command,
    const uint8_t *payload,
    uint16_t length) {
    if (!udp_started || length > MAX_PAYLOAD_SIZE) {
        return false;
    }

    uint8_t frame[FRAME_HEADER_SIZE + MAX_PAYLOAD_SIZE + 2] = {0};
    frame[0] = 0x5A;
    frame[1] = length & 0xFF;
    frame[2] = 0x10 | ((length >> 8) & 0x0F);
    frame[4] = robotType(static_cast<uint8_t>(Settings::getRobotId(3)));
    frame[5] = static_cast<uint8_t>(Settings::getRobotId(3));
    frame[6] = 0x80;
    frame[7] = 0;
    frame[8] = 1;
    frame[9] = 0;
    frame[10] = sequence & 0xFF;
    frame[11] = sequence >> 8;
    frame[12] = 0;
    frame[13] = command & 0xFF;
    frame[14] = command >> 8;
    frame[3] = crc8(frame, 3);

    memcpy(&frame[15], payload, length);
    const size_t body_length = FRAME_HEADER_SIZE + length;
    const uint16_t checksum = crc16(frame, body_length);
    frame[body_length] = checksum & 0xFF;
    frame[body_length + 1] = checksum >> 8;
    sequence++;

    if (!udp.beginPacket(server_ip, Config::REFEREE_SERVER_PORT)) {
        tx_fail_count++;
        return false;
    }
    udp.write(frame, body_length + 2);
    if (udp.endPacket() != 1) {
        tx_fail_count++;
        return false;
    }
    tx_ok_count++;
    return true;
}

uint8_t RefereeClient::robotType(uint8_t robot_id) {
    switch (robot_id) {
      case 1:
      case 101:
        return 2;
      case 2:
      case 102:
        return 3;
      case 3:
      case 4:
      case 5:
      case 103:
      case 104:
      case 105:
        return 1;
      case 7:
      case 107:
        return 4;
      case 8:
      case 108:
        return 6;
      case 9:
      case 109:
        return 7;
      case 10:
      case 110:
        return 0x80;
      case 11:
      case 111:
        return 8;
      default:
        return 1;
    }
}

uint8_t RefereeClient::crc8(const uint8_t *data, size_t length) {
    uint8_t crc = 0x77;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8C : crc >> 1;
        }
    }
    return crc;
}

uint16_t RefereeClient::crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0x1862;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
        }
    }
    return crc;
}

bool RefereeClient::isOnline() {
    return match_received && millis() - last_match_ms <= OFFLINE_TIMEOUT_MS;
}

bool RefereeClient::isLoggedIn() {
    // 登录 ACK 只代表连接被接受；必须等到比赛/机器人状态同步后，
    // 才算真正登录完成，否则灯光会过早切红蓝、击打状态也不完整。
    return login_acknowledged && match_received;
}

bool RefereeClient::isDead() {
    return isLoggedIn() && max_hp_value > 0 && current_hp_value == 0;
}

uint32_t RefereeClient::currentHp() {
    return current_hp_value;
}

uint32_t RefereeClient::maxHp() {
    return max_hp_value;
}
