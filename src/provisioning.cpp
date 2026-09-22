#include "provisioning.hpp"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "config.hpp"
#include "can.hpp"
#include "hit.hpp"
#include "referee_client.hpp"
#include "settings.hpp"
#include "web.hpp"
#include "system_state.hpp"
#include "wifi_manager.hpp"

static WebServer server(80);
static DNSServer dns_server;
// 连接失败后快速重试；每次重试前只在失败状态下做一次扫描诊断。
static constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 15000;
static constexpr uint32_t WIFI_WAIT_LOG_INTERVAL_MS = 2000;
static constexpr uint32_t WIFI_CONNECTED_LOG_INTERVAL_MS = 5000;

static uint32_t last_wifi_retry_ms = 0;
static uint32_t last_wifi_wait_log_ms = 0;
static uint32_t last_wifi_connected_log_ms = 0;
static uint32_t wifi_retry_count = 0;
static uint8_t last_ap_client_count = 0;
static uint32_t last_ap_client_log_ms = 0;
static bool wifi_event_registered = false;
static int32_t scanned_channel = 0;
static uint8_t scanned_bssid[6] = {0};
static bool scanned_bssid_valid = false;
static bool sta_include_11b = false;

static const char *wifiStatusName(int status) {
    switch (status) {
      case WL_CONNECTED:
        return "WL_CONNECTED";
      case WL_NO_SSID_AVAIL:
        return "WL_NO_SSID_AVAIL";
      case WL_CONNECT_FAILED:
        return "WL_CONNECT_FAILED";
      case WL_CONNECTION_LOST:
        return "WL_CONNECTION_LOST";
      case WL_DISCONNECTED:
        return "WL_DISCONNECTED";
      case WL_IDLE_STATUS:
        return "WL_IDLE_STATUS";
      default:
        return "WL_UNKNOWN";
    }
}

static const char *wifiDisconnectReasonName(uint8_t reason) {
    switch (reason) {
      case 2:
        return "AUTH_EXPIRE";
      case 3:
        return "AUTH_LEAVE";
      case 4:
        return "ASSOC_EXPIRE";
      case 5:
        return "ASSOC_TOOMANY";
      case 8:
        return "ASSOC_LEAVE";
      case 15:
        return "4WAY_HANDSHAKE_TIMEOUT";
      case 200:
        return "BEACON_TIMEOUT";
      case 201:
        return "NO_AP_FOUND";
      case 202:
        return "AUTH_FAIL";
      case 203:
        return "ASSOC_FAIL";
      case 204:
        return "HANDSHAKE_TIMEOUT";
      case 205:
        return "CONNECTION_FAIL";
      default:
        return "UNKNOWN";
    }
}

static const char *wifiAuthName(unsigned auth) {
    switch (auth) {
      case 0:
        return "OPEN";
      case 1:
        return "WEP";
      case 2:
        return "WPA_PSK";
      case 3:
        return "WPA2_PSK";
      case 4:
        return "WPA_WPA2_PSK";
      case 5:
        return "ENTERPRISE";
      case 6:
        return "WPA3_PSK";
      case 7:
        return "WPA2_WPA3_PSK";
      default:
        return "OTHER";
    }
}

static void printSsidDebug(const char *tag, const String &ssid) {
    if (Config::SERIAL_SCAN_ONLY || Config::SERIAL_CAN_ONLY) {
        return;
    }
    Serial.printf(
        "WIFI %s ssid=\"%s\" len=%u bytes:",
        tag,
        ssid.c_str(),
        static_cast<unsigned>(ssid.length()));
    for (size_t i = 0; i < ssid.length(); i++) {
        Serial.printf(" %02X", static_cast<unsigned>(static_cast<uint8_t>(ssid[i])));
    }
    Serial.println();
}

static void printWifiCredentials(
    const char *tag,
    const String &ssid,
    const String &password) {
    if (Config::SERIAL_SCAN_ONLY || Config::SERIAL_CAN_ONLY) {
        return;
    }
    printSsidDebug(tag, ssid);
    if (!Config::SERIAL_CAN_ONLY) Serial.printf(
        "WIFI %s password=\"%s\" password_len=%u\n",
        tag,
        password.c_str(),
        static_cast<unsigned>(password.length()));
}

static bool sameSubnet(const IPAddress &ip, const IPAddress &other, const IPAddress &mask) {
    return (static_cast<uint32_t>(ip) & static_cast<uint32_t>(mask)) ==
           (static_cast<uint32_t>(other) & static_cast<uint32_t>(mask));
}

static const char *hotspotKind(const IPAddress &gateway) {
    if (gateway[0] == 192 && gateway[1] == 168 && gateway[2] == 137) {
        return "Windows-hotspot";
    }
    if (gateway[0] == 172 && gateway[1] == 20 && gateway[2] == 10) {
        return "iPhone-hotspot";
    }
    if (gateway[0] == 192 && gateway[1] == 168 && gateway[2] == 43) {
        return "Android-hotspot";
    }
    return "router-or-other";
}

static void configureWifiRadio(
    wifi_interface_t iface,
    const char *tag,
    bool include_11b) {
    wifi_country_t country = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    const uint8_t protocol = include_11b
        ? (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)
        : (WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    const esp_err_t country_err = esp_wifi_set_country(&country);
    const esp_err_t protocol_err = esp_wifi_set_protocol(iface, protocol);
    const esp_err_t bandwidth_err =
        esp_wifi_set_bandwidth(iface, WIFI_BW_HT20);
    const esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    if (!Config::SERIAL_SCAN_ONLY) {
        Serial.printf(
            "WIFI %s radio cfg country=0x%X protocol=0x%X mask=0x%X bw=0x%X ps=0x%X tx=19.5dBm 11b=%u\n",
            tag,
            static_cast<unsigned>(country_err),
            static_cast<unsigned>(protocol_err),
            static_cast<unsigned>(protocol),
            static_cast<unsigned>(bandwidth_err),
            static_cast<unsigned>(ps_err),
            include_11b ? 1 : 0);
    }
}

static void printConnectedNetwork(const char *reason) {
    const IPAddress ip = WiFi.localIP();
    const IPAddress gateway = WiFi.gatewayIP();
    const IPAddress mask = WiFi.subnetMask();
    const IPAddress dns = WiFi.dnsIP();
    const IPAddress referee(
        Config::REFEREE_SERVER_IP[0],
        Config::REFEREE_SERVER_IP[1],
        Config::REFEREE_SERVER_IP[2],
        Config::REFEREE_SERVER_IP[3]);
    const bool referee_in_subnet = sameSubnet(ip, referee, mask);

    if (Config::SERIAL_CAN_ONLY) {
        return;
    }

    Serial.printf(
        "WIFI NET %s ssid=\"%s\" ip=%s gw=%s mask=%s dns=%s rssi=%d dBm "
        "channel=%d bssid=%s sta_mac=%s kind=%s\n",
        reason,
        WiFi.SSID().c_str(),
        ip.toString().c_str(),
        gateway.toString().c_str(),
        mask.toString().c_str(),
        dns.toString().c_str(),
        WiFi.RSSI(),
        WiFi.channel(),
        WiFi.BSSIDstr().c_str(),
        WiFi.macAddress().c_str(),
        hotspotKind(gateway));

    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        if (!Config::SERIAL_CAN_ONLY) Serial.printf(
            "WIFI PHY auth=%u(%s) pairwise=%u 11b=%u 11g=%u 11n=%u primary=%u rssi=%d\n",
            static_cast<unsigned>(ap.authmode),
            wifiAuthName(static_cast<unsigned>(ap.authmode)),
            static_cast<unsigned>(ap.pairwise_cipher),
            ap.phy_11b ? 1 : 0,
            ap.phy_11g ? 1 : 0,
            ap.phy_11n ? 1 : 0,
            ap.primary,
            ap.rssi);
    }

    if (!Config::SERIAL_CAN_ONLY) Serial.printf(
        "WIFI REFEREE target=%s:%u local_port=%u same_subnet=%u\n",
        referee.toString().c_str(),
        Config::REFEREE_SERVER_PORT,
        Config::REFEREE_LOCAL_PORT,
        referee_in_subnet ? 1 : 0);

    if (!referee_in_subnet) {
        if (!Config::SERIAL_CAN_ONLY) Serial.println(
            "WIFI WARN referee IP is outside this hotspot subnet; UDP login may fail.");
        if (gateway[0] == 192 && gateway[1] == 168 && gateway[2] == 137) {
            if (!Config::SERIAL_CAN_ONLY) Serial.printf(
                "WIFI HINT Windows hotspot host is %s. If the referee runs on this PC, set REFEREE_SERVER_IP to that gateway.\n",
                gateway.toString().c_str());
        }
        if (!Config::SERIAL_CAN_ONLY) Serial.println(
            "WIFI HINT ESP32-C3 is 2.4GHz only; the computer hotspot must enable 2.4GHz.");
    }
}

static void handleWifiEvent(
    arduino_event_id_t event,
    arduino_event_info_t info) {
    if (Config::SERIAL_CAN_ONLY) {
        return;
    }

    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_START:
        Serial.println("WIFI EVENT STA_START");
        break;
      case ARDUINO_EVENT_WIFI_STA_CONNECTED: {
        char ssid[33] = {0};
        const uint8_t ssid_len = info.wifi_sta_connected.ssid_len > 32
            ? 32
            : info.wifi_sta_connected.ssid_len;
        memcpy(ssid, info.wifi_sta_connected.ssid, ssid_len);
        Serial.printf(
            "WIFI EVENT STA_CONNECTED ssid=\"%s\" channel=%u auth=%u(%s) "
            "bssid=%02X:%02X:%02X:%02X:%02X:%02X\n",
            ssid,
            static_cast<unsigned>(info.wifi_sta_connected.channel),
            static_cast<unsigned>(info.wifi_sta_connected.authmode),
            wifiAuthName(static_cast<unsigned>(info.wifi_sta_connected.authmode)),
            info.wifi_sta_connected.bssid[0],
            info.wifi_sta_connected.bssid[1],
            info.wifi_sta_connected.bssid[2],
            info.wifi_sta_connected.bssid[3],
            info.wifi_sta_connected.bssid[4],
            info.wifi_sta_connected.bssid[5]);
        break;
      }
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
        char ssid[33] = {0};
        const uint8_t ssid_len = info.wifi_sta_disconnected.ssid_len > 32
            ? 32
            : info.wifi_sta_disconnected.ssid_len;
        memcpy(ssid, info.wifi_sta_disconnected.ssid, ssid_len);
        Serial.printf(
            "WIFI EVENT STA_DISCONNECTED reason=%u (%s) rssi=%d ssid=\"%s\"\n",
            static_cast<unsigned>(info.wifi_sta_disconnected.reason),
            wifiDisconnectReasonName(info.wifi_sta_disconnected.reason),
            info.wifi_sta_disconnected.rssi,
            ssid);
        break;
      }
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.printf(
            "WIFI EVENT GOT_IP ip=%s mask=%s gw=%s\n",
            IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str(),
            IPAddress(info.got_ip.ip_info.netmask.addr).toString().c_str(),
            IPAddress(info.got_ip.ip_info.gw.addr).toString().c_str());
        break;
      case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        Serial.println("WIFI EVENT LOST_IP");
        break;
      default:
        break;
    }
}

static bool is24ghzChannel(int channel) {
    return channel >= 1 && channel <= 14;
}

static int runWifiScan(const String &target_ssid) {
    int count = WIFI_SCAN_FAILED;
    for (int attempt = 1; attempt <= 3; attempt++) {
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        delay(150);
        WiFi.scanDelete();
        if (!Config::SERIAL_CAN_ONLY) Serial.printf(
            "WIFI SCAN start target=\"%s\" attempt=%d\n",
            target_ssid.c_str(),
            attempt);
        count = WiFi.scanNetworks(false, true, false, 500);
        if (count >= 0) {
            return count;
        }
        if (!Config::SERIAL_CAN_ONLY) Serial.printf(
            "WIFI SCAN failed result=%d attempt=%d\n",
            count,
            attempt);
        delay(250);
    }
    return count;
}

static bool printWifiScanDiagnostics(const String &target_ssid) {
    scanned_bssid_valid = false;
    scanned_channel = 0;
    printSsidDebug("scan-target", target_ssid);

    const int count = runWifiScan(target_ssid);
    if (count < 0) {
        if (!Config::SERIAL_CAN_ONLY) Serial.printf("WIFI SCAN failed result=%d after retries\n", count);
        return false;
    }

    bool target_found = false;
    int target_rssi = -127;
    int target_channel = 0;
    unsigned target_auth = 0;
    String target_bssid = "-";

    if (!Config::SERIAL_CAN_ONLY) Serial.printf("WIFI SCAN count=%d target=\"%s\"\n", count, target_ssid.c_str());
    for (int index = 0; index < count; index++) {
        const String ssid = WiFi.SSID(index);
        const int rssi = WiFi.RSSI(index);
        const int channel = WiFi.channel(index);
        const unsigned auth =
            static_cast<unsigned>(WiFi.encryptionType(index));
        const String bssid = WiFi.BSSIDstr(index);
        const bool is_24g = is24ghzChannel(channel);
        const bool is_target = (ssid == target_ssid);
        const char *name = ssid.length() > 0 ? ssid.c_str() : "(hidden)";

        if (!Config::SERIAL_CAN_ONLY) Serial.printf(
            "  %s  rssi=%d  ch=%d%s\n",
            name,
            rssi,
            channel,
            is_target ? "  *target" : "");

        if (is_target) {
            const bool have_24g = is24ghzChannel(target_channel);
            bool take = false;
            if (!target_found) {
                take = true;
            } else if (is_24g && !have_24g) {
                take = true;
            } else if (is_24g == have_24g && rssi > target_rssi) {
                take = true;
            }

            if (take) {
                target_found = true;
                target_rssi = rssi;
                target_channel = channel;
                target_auth = auth;
                target_bssid = bssid;
                scanned_channel = channel;
                uint8_t *bssid_bytes = WiFi.BSSID(index);
                if (bssid_bytes != nullptr) {
                    memcpy(scanned_bssid, bssid_bytes, 6);
                    scanned_bssid_valid = true;
                }
            }
        }
    }

    if (target_found) {
        if (!Config::SERIAL_CAN_ONLY) Serial.printf(
            "WIFI SCAN target found rssi=%d ch=%d\n",
            target_rssi,
            target_channel);
    } else {
        if (!Config::SERIAL_CAN_ONLY) Serial.printf(
            "WIFI SCAN target missing: \"%s\"\n",
            target_ssid.c_str());
    }
    WiFi.scanDelete();
    delay(50);
    return target_found;
}

static void beginStaConnect(const String &ssid, const String &password, const char *why) {
    printWifiCredentials(why, ssid, password);
    if (scanned_bssid_valid) {
        WiFi.begin(
            ssid.c_str(),
            password.c_str(),
            scanned_channel,
            scanned_bssid);
        if (!Config::SERIAL_CAN_ONLY) Serial.printf(
            "WIFI %s targeted SSID=%s channel=%ld bssid=%02X:%02X:%02X:%02X:%02X:%02X password_len=%u\n",
            why,
            ssid.c_str(),
            static_cast<long>(scanned_channel),
            scanned_bssid[0],
            scanned_bssid[1],
            scanned_bssid[2],
            scanned_bssid[3],
            scanned_bssid[4],
            scanned_bssid[5],
            static_cast<unsigned>(password.length()));
        return;
    }

    WiFi.begin(ssid.c_str(), password.c_str());
    if (!Config::SERIAL_CAN_ONLY) Serial.printf(
        "WIFI %s SSID=%s password_len=%u (all-channel, no BSSID yet)\n",
        why,
        ssid.c_str(),
        static_cast<unsigned>(password.length()));
}

bool Provisioning::config_mode = false;
bool Provisioning::routes_registered = false;
bool Provisioning::wifi_session_active = false;

static String provisioningApSsid() {
    const uint64_t mac = ESP.getEfuseMac();
    char mac_text[20];
    snprintf(
        mac_text,
        sizeof(mac_text),
        "%04X%08X",
        static_cast<unsigned>(mac >> 32),
        static_cast<unsigned>(mac & 0xFFFFFFFFULL));
    return String(Config::PROVISIONING_WIFI_PREFIX) + mac_text;
}

void Provisioning::startStaSession(const String &ssid, const String &password) {
    wifi_session_active = true;
    SystemState::beginConnecting();
    // 配网转联网必须产生新版本，防止从机重新应用队列里的旧配网帧。
    Can::notifyLocalStateChanged();
    Hit::setNetworkConnectingMode(true);
    RefereeClient::stop();
    // 先广播连接中状态，再执行可能阻塞的 STA 扫描，避免从机继续显示配网闪烁。
    Can::broadcastHostStateNow();

    if (!Config::SERIAL_CAN_ONLY) Serial.println("WIFI connect indicator: solid yellow");
    printWifiCredentials("runtime", ssid, password);
    last_wifi_retry_ms = millis();
    last_wifi_wait_log_ms = 0;
    last_wifi_connected_log_ms = 0;
    wifi_retry_count = 0;
    scanned_bssid_valid = false;
    sta_include_11b = false;

    WifiManager::prepareSta();
    delay(200);
    if (!wifi_event_registered) {
        WiFi.onEvent(handleWifiEvent);
        wifi_event_registered = true;
    }
    WiFi.setHostname("outpost");
    WiFi.setSleep(false);
    WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    configureWifiRadio(WIFI_IF_STA, "STA", sta_include_11b);

    if (!Config::SERIAL_CAN_ONLY) Serial.println("WIFI SCAN before first STA connect");
    printWifiScanDiagnostics(ssid);
    beginStaConnect(ssid, password, "first-connect");
}

void Provisioning::beginNormal() {
    // 有已保存配置时自动恢复联网，否则停在离线状态等待长按配网。
    Hit::setProvisioningMode(false);
    Hit::setNetworkConnectingMode(false);
    wifi_session_active = false;
    SystemState::enterOffline();
    WifiManager::beginNormal();
    if (!Config::SERIAL_SCAN_ONLY) {
        Serial.printf(
            "ROBOT ID=%lu\n",
            static_cast<unsigned long>(Settings::getRobotId(3)));
    }
    if (!Config::SERIAL_CAN_ONLY) Serial.println("WIFI not configured; hold KEY for 2 seconds");

    // 已配网过的板子上电直接恢复联网，避免每次断电都要重新配网。
    // 未配网过的板子仍停在离线状态，需要长按进入配网。
    if (Settings::hasSavedConfig() && Settings::hasWiFiCredentials()) {
        if (!Config::SERIAL_CAN_ONLY) Serial.printf(
            "WIFI saved config found, restoring SSID=%s\n",
            Settings::getWiFiSsid().c_str());
        startStaSession(Settings::getWiFiSsid(), Settings::getWiFiPassword());
        return;
    }
}

static void configureSoftApRadio() {
    configureWifiRadio(WIFI_IF_AP, "AP", true);
}

static bool startProvisioningAp() {
    // ESP32-C3 上 disconnect(wifioff=true) 再立刻开 AP，beacon 经常发不出来。
    if (!WifiManager::prepareAp()) {
        if (!Config::SERIAL_CAN_ONLY) Serial.println("WIFI AP mode failed");
        return false;
    }
    delay(300);
    WiFi.setSleep(false);
    configureSoftApRadio();

    WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),
        IPAddress(192, 168, 4, 1),
        IPAddress(255, 255, 255, 0));

    const String ap_ssid = provisioningApSsid();
    const bool started = WiFi.softAP(
        ap_ssid.c_str(),
        Config::PROVISIONING_WIFI_PASSWORD,
        Config::AP_CHANNEL,
        0,
        4);
    if (!started) {
        if (!Config::SERIAL_CAN_ONLY) Serial.println("WIFI AP start failed");
        return false;
    }

    delay(200);
    configureSoftApRadio();
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    wifi_config_t conf{};
    if (esp_wifi_get_config(WIFI_IF_AP, &conf) == ESP_OK) {
        if (!Config::SERIAL_CAN_ONLY) Serial.printf(
            "WIFI AP ready ssid=%s channel=%u hidden=%u auth=%u mac=%s ip=%s\n",
            ap_ssid.c_str(),
            conf.ap.channel,
            conf.ap.ssid_hidden,
            conf.ap.authmode,
            WiFi.softAPmacAddress().c_str(),
            WiFi.softAPIP().toString().c_str());
    }
    return true;
}

void Provisioning::enterConfigMode() {
    if (config_mode) {
        return;
    }

    config_mode = true;
    SystemState::enterProvisioning();
    Can::requestHostClaim();
    wifi_session_active = false;
    Can::stopOfflineColorNormalization();
    RefereeClient::stop();
    // 先停 ADC DMA，避免和 SoftAP beacon 抢 GDMA。
    Hit::setProvisioningMode(true);
    // 立即进入黄灯闪烁相位，避免启动 AP 的阻塞期间继续显示原阵营色。
    Hit::showProvisioningNow((millis() / Config::PROVISIONING_BLINK_MS) % 2 == 0);
    // 先广播配网状态，再启动 AP，避免其他板等到 AP 初始化完成后才切灯。
    Can::broadcastHostStateNow();
    registerRoutes();

    const bool started = startProvisioningAp();
    if (started) {
        server.begin();
        dns_server.start(53, "*", WiFi.softAPIP());
        last_ap_client_count = 0;
        last_ap_client_log_ms = millis();
        if (!Config::SERIAL_CAN_ONLY) {
            Serial.println("WIFI provisioning mode");
            Serial.printf("AP SSID: %s\n", provisioningApSsid().c_str());
            Serial.printf("AP password: %s\n", Config::PROVISIONING_WIFI_PASSWORD);
            Serial.printf("AP channel: %u (HT20 2.4GHz)\n", Config::AP_CHANNEL);
            Serial.printf(
                "AP IP: %s\n",
                WiFi.softAPIP().toString().c_str());
            Serial.println("Open http://192.168.4.1/");
        }
    }
}

void Provisioning::onLoop() {
    if (config_mode) {
        dns_server.processNextRequest();
        server.handleClient();

        const uint8_t client_count = WiFi.softAPgetStationNum();
        if (client_count != last_ap_client_count ||
            millis() - last_ap_client_log_ms >= 5000) {
            last_ap_client_count = client_count;
            last_ap_client_log_ms = millis();
            if (!Config::SERIAL_SCAN_ONLY) {
                Serial.printf(
                    "WIFI AP clients=%u ip=%s\n",
                    client_count,
                    WiFi.softAPIP().toString().c_str());
            }
        }

        return;
    }

    if (!wifi_session_active) {
        return;
    }

    static int last_status = -1;
    const uint32_t now = millis();
    const int status = static_cast<int>(WiFi.status());
        if (status == WL_CONNECTED) {
        if (status != last_status) {
            WiFi.setAutoReconnect(true);
            Hit::setProvisioningMode(false);
            Hit::setNetworkConnectingMode(false);
            Hit::endProvisioningSaveHold();
            Hit::signalWifiConnected();
    SystemState::markWifiConnected();
            printConnectedNetwork("connected");
            last_wifi_connected_log_ms = now;
        } else if (now - last_wifi_connected_log_ms >= WIFI_CONNECTED_LOG_INTERVAL_MS) {
            last_wifi_connected_log_ms = now;
            printConnectedNetwork("alive");
        }
        } else {
        // 已进入联网流程后，未连接状态用流水灯表示；普通离线启动不会进入这里。
            Hit::setNetworkConnectingMode(true);
            SystemState::beginConnecting();
        WiFi.setAutoReconnect(false);
        if (status != last_status) {
            if (!Config::SERIAL_CAN_ONLY) Serial.printf(
                "WIFI status=%d (%s), waiting for SSID=%s; LED running light\n",
                status,
                wifiStatusName(status),
                Settings::getWiFiSsid().c_str());
        }

        if (now - last_wifi_wait_log_ms >= WIFI_WAIT_LOG_INTERVAL_MS) {
            last_wifi_wait_log_ms = now;
            if (!Config::SERIAL_CAN_ONLY) Serial.printf(
                "WIFI WAIT status=%d (%s) retry=%lu ssid=\"%s\" rssi=%d\n",
                status,
                wifiStatusName(status),
                static_cast<unsigned long>(wifi_retry_count),
                Settings::getWiFiSsid().c_str(),
                WiFi.RSSI());
        }

        // 先断开再扫，避免关联过程中 scan 返回 -2。
        if (now - last_wifi_retry_ms >= WIFI_RETRY_INTERVAL_MS) {
            last_wifi_retry_ms = now;
            wifi_retry_count++;
            const String ssid = Settings::getWiFiSsid();
            const String password = Settings::getWiFiPassword();

            if (wifi_retry_count == 3 && !sta_include_11b) {
                sta_include_11b = true;
                configureWifiRadio(WIFI_IF_STA, "STA-fallback-11b", true);
            }

            if (!Config::SERIAL_CAN_ONLY) Serial.printf(
                "WIFI retry #%lu: disconnect, scan, then begin SSID=%s\n",
                static_cast<unsigned long>(wifi_retry_count),
                ssid.c_str());
            printWifiScanDiagnostics(ssid);
            beginStaConnect(ssid, password, "retry");
        }
    }

    if (status != last_status) {
        // Wi-Fi 状态变化会改变主机广播模式，也必须推进统一状态版本。
        Can::notifyLocalStateChanged();
        last_status = status;
    }
}

bool Provisioning::isConfigMode() {
    return config_mode;
}

bool Provisioning::isBusy() {
    if (config_mode) {
        return true;
    }
    if (!wifi_session_active) {
        return false;
    }
    return !RefereeClient::isLoggedIn();
}

bool Provisioning::isSessionActive() {
    return config_mode || wifi_session_active;
}

void Provisioning::requestOffline() {
    Can::requestOffline();
}

bool Provisioning::isLocalHost() {
    return SystemState::get().role == SystemState::Role::Host;
}

void Provisioning::surrenderHostToPeer() {
    if (!isLocalHost()) {
        return;
    }

    wifi_session_active = false;
    // 让位前作废残留的裁判报文，否则可能用旧包判定登录成功。
    RefereeClient::endSessionGeneration();

    if (config_mode) {
        dns_server.stop();
        server.stop();
        config_mode = false;
    }

    WifiManager::stop();

    // 收到对端主机状态的 CAN 回调会在本函数返回后立刻写入新的从机状态。
    // 这里不能先清空 SystemState，否则关闭 Wi-Fi/AP 的阻塞窗口会把黄灯
    // 覆盖成离线颜色或黑灯。
    Hit::setProvisioningMode(false);
    Hit::setNetworkConnectingMode(false);
    if (!Config::SERIAL_CAN_ONLY) Serial.println("CAN host arbitration: surrendered to peer host");
}

void Provisioning::abortToOffline() {
    wifi_session_active = false;
    SystemState::enterOffline();
    // 退出配网/离线：作废残留裁判报文，避免刚退出就误判登录成功。
    RefereeClient::endSessionGeneration();

    if (config_mode) {
        dns_server.stop();
        server.stop();
        config_mode = false;
    }

    WifiManager::stop();

    // 退出配网必须同时清除本板缓存的主机状态，避免紧接着短按切色时
    // 仍被当作从机处理，或让从机继续显示旧的配网状态直到超时。
    Can::clearHostState();
    Hit::setProvisioningMode(false);
    Hit::setNetworkConnectingMode(false);
    Hit::endProvisioningSaveHold();

    if (!Config::SERIAL_CAN_ONLY) Serial.println("WIFI aborted to offline; short-press ignored, long-press for config");
}

void Provisioning::registerRoutes() {
    if (routes_registered) {
        return;
    }

    server.on("/", HTTP_GET, handleIndex);
    // 手机系统会先访问这些地址判断是否存在可用网络；统一返回配网页面，
    // 否则手机可能显示“无法打开页面”而不弹出配网界面。
    server.on("/generate_204", HTTP_GET, handleIndex);
    server.on("/hotspot-detect.html", HTTP_GET, handleIndex);
    server.on("/connecttest.txt", HTTP_GET, handleIndex);
    server.on("/fwlink", HTTP_GET, handleIndex);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleNotFound);
    routes_registered = true;
}

void Provisioning::handleIndex() {
    ProvisioningWeb::sendIndex(server);
}

void Provisioning::handleSave() {
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    ssid.trim();
    password.trim();
    const String robot_id_text = server.arg("robot_id");
    const long robot_id = robot_id_text.toInt();

    const bool valid_robot_id =
        (robot_id >= 1 && robot_id <= 11) ||
        (robot_id >= 101 && robot_id <= 111);

    if (ssid.length() == 0 || ssid.length() > 32 ||
        password.length() > 63 || !valid_robot_id) {
        server.send(
            400,
            "text/html; charset=utf-8",
            "<h3>输入无效</h3><p>请检查 Wi-Fi 名称和密码长度。</p>");
        return;
    }

    Settings::setWiFiCredentials(ssid, password);
    // 必须走 applyConfiguredTeam 统一更新 Hit::color/Settings/SystemState，
    Hit::applyConfiguredTeam(static_cast<uint32_t>(robot_id));
    if (!Config::SERIAL_CAN_ONLY) Serial.printf(
        "WIFI team applied robot_id=%ld color=%s\n",
        robot_id,
        Hit::color == Hit::BLUE ? "BLUE" : "RED");
    printWifiCredentials("saved-form", ssid, password);
    if (!Config::SERIAL_CAN_ONLY) Serial.printf(
        "WIFI credentials saved robot_id=%ld\n",
        robot_id);
    // 配置写入 NVS，掉电后保留，下次上电自动恢复联网。
    server.send(204);

    dns_server.stop();
    server.stop();
    config_mode = false;
    Hit::setProvisioningMode(false);
    // 保存可能落在灭相位，先锁黄灯常亮再做 Wi-Fi 切换。
    Hit::beginProvisioningSaveHold();
    Hit::showNetworkConnecting();
    // 先广播联网常亮再执行阻塞操作，否则从机会继续按旧相位闪烁，暗相位保存时先灭再亮。
    SystemState::beginConnecting();
    Can::notifyLocalStateChanged();
    Hit::setNetworkConnectingMode(true);
    Can::broadcastHostStateNow();
    WiFi.softAPdisconnect(true);
    startStaSession(ssid, password);
    if (!Config::SERIAL_CAN_ONLY) Serial.println("WIFI credentials saved; connecting without restart");
}

void Provisioning::handleNotFound() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
}