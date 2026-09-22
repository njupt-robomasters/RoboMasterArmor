#include "hit.hpp"

#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <driver/adc.h>
#include <soc/soc_caps.h>

#include "config.hpp"
#include "can.hpp"
#include "hit_detector.hpp"
#include "led_controller.hpp"
#include "provisioning.hpp"
#include "referee_client.hpp"
#include "settings.hpp"
#include "system_state.hpp"

// ADC采样频率。使用ADC数字控制器将采样数据写入DMA缓冲区，避免analogRead轮询速度不足。
// 降低DMA数据产生速率，避免主循环来不及读取导致ESP_ERR_INVALID_STATE(0x103)。
static constexpr uint32_t SAMPLE_FREQ_HZ = 10000;
static constexpr uint32_t SAMPLE_WINDOW_MS = 10;
// 10kHz采样时约2.5ms即可得到一批数据，避免ADC读取长时间阻塞流水灯。
static constexpr uint32_t ADC_READ_TIMEOUT_MS = 5;
static constexpr uint32_t ADC_RESULT_BYTES = SOC_ADC_DIGI_RESULT_BYTES;
static constexpr uint32_t SAMPLE_BYTES =
    SAMPLE_FREQ_HZ / 1000 * SAMPLE_WINDOW_MS * ADC_RESULT_BYTES;

static uint8_t adc_buffer[SAMPLE_BYTES] = {0};
static bool adc_running = false;

static void startAdcDma() {
    if (adc_running) {
        return;
    }
    if (adc_digi_start() == ESP_OK) {
        adc_running = true;
    }
}

static void stopAdcDma() {
    if (!adc_running) {
        return;
    }
    adc_digi_stop();
    adc_running = false;
}

uint32_t Hit::RED = Adafruit_NeoPixel::Color(255, 0, 0);
uint32_t Hit::BLUE = Adafruit_NeoPixel::Color(0, 0, 255);
uint32_t Hit::YELLOW = Adafruit_NeoPixel::Color(255, 180, 0);
uint32_t Hit::PURPLE = Adafruit_NeoPixel::Color(160, 0, 255);

uint32_t Hit::color = 0;
uint32_t Hit::hit_count = 0;
uint32_t Hit::last_hit_ms = 0;
uint32_t Hit::adc_value = 0;
bool Hit::provisioning_mode = false;
bool Hit::network_connecting_mode = false;
bool Hit::shared_state_valid = false;
Hit::LightMode Hit::shared_mode = Hit::LIGHT_OFFLINE;
uint32_t Hit::shared_color = 0;
bool Hit::shared_blink_on = false;
uint32_t Hit::wifi_signal_start_ms = 0;
uint32_t Hit::wifi_signal_until_ms = 0;
bool Hit::provisioning_save_hold = false;

static uint32_t last_adc_debug_ms = 0;
static uint32_t last_adc_output_bytes = 0;
static int last_adc_read_result = ESP_OK;
static uint32_t adc_read_error_count = 0;
static uint32_t adc_recover_count = 0;
static uint32_t last_adc_recover_ms = 0;
static uint32_t last_pixels_update_ms = 0;
static uint32_t last_hit_diag_ms = 0;
static uint32_t offline_exit_signal_until_ms = 0;
// 首次进入配网先统一黄灯常亮，避免从机在主机启动 SoftAP 期间先黑一下。
static uint32_t provisioning_entry_hold_until_ms = 0;
static uint32_t team_switch_purple_until_ms = 0;
static HitDetector hit_detector;

static void continuous_adc_init() {
    adc_digi_init_config_t adc_dma_config = {
        .max_store_buf_size = SAMPLE_BYTES * 2,
        .conv_num_each_intr = SAMPLE_BYTES / ADC_RESULT_BYTES,
        .adc1_chan_mask = BIT(ADC_CHANNEL_0),
        .adc2_chan_mask = 0,
    };
    ESP_ERROR_CHECK(adc_digi_initialize(&adc_dma_config));

    adc_digi_configuration_t dig_cfg = {
        .conv_limit_en = false,
        .conv_limit_num = 255,
        .pattern_num = 1,
        .sample_freq_hz = SAMPLE_FREQ_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };

    adc_digi_pattern_config_t adc_pattern[SOC_ADC_PATT_LEN_MAX] = {0};
    adc_pattern[0].atten = ADC_ATTEN_DB_0;
    adc_pattern[0].channel = ADC_CHANNEL_0;
    adc_pattern[0].unit = 0;
    adc_pattern[0].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
    dig_cfg.adc_pattern = adc_pattern;

    ESP_ERROR_CHECK(adc_digi_controller_configure(&dig_cfg));
}

void Hit::begin() {
    color = Settings::getColor(RED);
    // 启动时按 robot_id 修正阵营，避免旧配置造成颜色不一致。
    const uint32_t robot_id = Settings::getRobotId(3);
    if (robot_id >= 101 && robot_id <= 111) {
        color = BLUE;
        Settings::setColor(color);
    } else if (robot_id >= 1 && robot_id <= 11) {
        color = RED;
        Settings::setColor(color);
    }
    SystemState::setTeam(color, robot_id);

    LedController::begin(color);
    wifi_signal_start_ms = 0;
    wifi_signal_until_ms = 0;
    network_connecting_mode = false;
    provisioning_save_hold = false;
    hit_detector.begin();
    last_adc_debug_ms = millis();
    last_pixels_update_ms = 0;

    // GPIO0对应ADC_CHANNEL_0。
    continuous_adc_init();
    ESP_ERROR_CHECK(adc_digi_start());
    adc_running = true;
}

void Hit::onLoop() {
    if (Config::HIT_DEBUG && millis() - last_hit_diag_ms >= 1000) {
        last_hit_diag_ms = millis();
        Serial.printf(
            "HIT MONITOR role=%s adc_running=%u adc=%lu threshold=%lu "
            "blocked=%u local_host=%u shared=%u hit_count=%lu\n",
            Provisioning::isLocalHost() ? "HOST" : "NODE",
            adc_running ? 1U : 0U,
            static_cast<unsigned long>(adc_value),
            static_cast<unsigned long>(Config::HIT_THRESHOLD),
            hitDetectionBlocked() ? 1U : 0U,
            Provisioning::isLocalHost() ? 1U : 0U,
            isSharedStateActive() ? 1U : 0U,
            static_cast<unsigned long>(hit_count));
    }

    // 配网和 STA 扫描/关联期间停 ADC，避免和 Wi-Fi 抢 GDMA。
    if (provisioning_mode || network_connecting_mode || !adc_running) {
        updatePixels();
        return;
    }

    uint32_t out_length = 0;
    uint32_t max_adc_value = 0;
    const esp_err_t adc_read_result = adc_digi_read_bytes(
            adc_buffer,
            sizeof(adc_buffer),
            &out_length,
            ADC_READ_TIMEOUT_MS);
    last_adc_read_result = adc_read_result;
    last_adc_output_bytes = out_length;

    if (adc_read_result == ESP_OK) {
        for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= out_length;
             i += SOC_ADC_DIGI_RESULT_BYTES) {
            auto *result = reinterpret_cast<adc_digi_output_data_t *>
                (&adc_buffer[i]);
            const uint32_t data = result->type2.data;
            max_adc_value = max(max_adc_value, data);

            // 配网、连 Wi-Fi、裁判登录期间不检测击打。
            if (!hitDetectionBlocked() &&
                hit_detector.accept(data, millis())) {
                hit_count++;
                last_hit_ms = millis();
                if (Config::HIT_DEBUG) {
                    Serial.printf(
                        "HIT DETECTED role=%s adc=%lu hit_count=%lu\n",
                        Provisioning::isLocalHost() ? "HOST" : "NODE",
                        static_cast<unsigned long>(data),
                        static_cast<unsigned long>(hit_count));
                }
            }
        }
    } else {
        adc_read_error_count++;
        // 读取状态异常时尝试重新启动DMA，避免一直停留在0x103。
        if (millis() - last_adc_recover_ms >= 1000) {
            last_adc_recover_ms = millis();
            stopAdcDma();
            startAdcDma();
            if (adc_running) {
                adc_recover_count++;
            }
        }
    }

    adc_value = max_adc_value;

    if (!Config::SERIAL_SCAN_ONLY &&
        millis() - last_adc_debug_ms >= Config::ADC_DEBUG_INTERVAL_MS) {
        last_adc_debug_ms = millis();
        Serial.printf(
            "ADC GPIO%d raw=%lu threshold=%lu bytes=%lu read=0x%X hit_count=%lu\n",
            Config::HIT_ADC_GPIO,
            static_cast<unsigned long>(adc_value),
            static_cast<unsigned long>(Config::HIT_THRESHOLD),
            static_cast<unsigned long>(last_adc_output_bytes),
            static_cast<unsigned>(last_adc_read_result),
            static_cast<unsigned long>(hit_count));
        Serial.printf(
            "ADC DMA errors=%lu recoveries=%lu\n",
            static_cast<unsigned long>(adc_read_error_count),
            static_cast<unsigned long>(adc_recover_count));
    }

    updatePixels();
}

void Hit::toggleColor() {
    // 已是从机时必须把切阵营交给主机。不能只看 shared_state_valid，
    // 否则丢一帧时会误走离线切色分支，跳过紫灯登录阶段。
    if (!Provisioning::isLocalHost() &&
        (shared_state_valid ||
         SystemState::get().role == SystemState::Role::Node)) {
        // 从机发起切换时先本地进入登录中画面，避免等待主机广播期间
        // 仍显示旧阵营，或因短暂丢帧直接跳到目标阵营颜色。
        shared_state_valid = true;
        shared_mode = LIGHT_REFEREE_LOGIN;
        SystemState::setRemoteLight(LIGHT_REFEREE_LOGIN);
        SystemState::setRefereeState(SystemState::Referee::LoginPending);
        beginTeamSwitchLightHold();
        last_pixels_update_ms = 0;
        Can::requestTeamToggle();
        return;
    }

    // 先开紫灯窗口再改阵营数据。
    // 登录包要到下一次 RefereeClient::onLoop() 才发出，
    // 而帧头里的机器人编号取自 Settings::getRobotId()，
    // 必须先更新编号，否则会带旧编号登录，裁判端认不出该机器人。
    if (Provisioning::isLocalHost()) {
        beginTeamSwitchLightHold();
    }

    const uint32_t previous_robot_id = Settings::getRobotId(3);

    if (color == RED) {
        color = BLUE;
    } else {
        color = RED;
    }
    Settings::setColor(color);
    uint32_t next_robot_id = previous_robot_id;
    if (previous_robot_id >= 1 && previous_robot_id <= 11) {
        next_robot_id = previous_robot_id + 100;
    } else if (previous_robot_id >= 101 && previous_robot_id <= 111) {
        next_robot_id = previous_robot_id - 100;
    } else {
        next_robot_id = color == RED ? 3 : 103;
    }
    Settings::setRobotId(next_robot_id);
    SystemState::setTeam(color, next_robot_id);

    if (Provisioning::isLocalHost()) {
        // 编号已更新，此时重启登录，0x20 会带新编号。
        RefereeClient::restartLogin();
        SystemState::setRefereeState(SystemState::Referee::LoginPending);
        // 主机不写从机共享缓存，否则 shared_mode 停在 LOGIN，按键会被永久锁住。
        setSharedState(false, LIGHT_OFFLINE, color);
        Can::notifyLocalStateChanged();
        // 立即把新阵营与登录中广播给两个从机，先显示紫灯再等登录成功。
        Can::broadcastHostStateNow();
    } else if (!shared_state_valid) {
        // 离线时没有主机，发送本板切换后的目标颜色给其它装甲板。
        Can::notifyOfflineColorChanged();
    }

    if (!Config::SERIAL_SCAN_ONLY) {
        Serial.printf(
            "TEAM switched to %s robot_id=%lu\n",
            color == RED ? "RED" : "BLUE",
            static_cast<unsigned long>(next_robot_id));
    }
    last_pixels_update_ms = 0;
}

uint32_t Hit::adcValue() {
    return adc_value;
}

void Hit::setProvisioningMode(bool enabled) {
    provisioning_mode = enabled;
    if (enabled) {
        network_connecting_mode = false;
        wifi_signal_start_ms = 0;
        wifi_signal_until_ms = 0;
        stopAdcDma();
    } else if (!network_connecting_mode) {
        startAdcDma();
    }
    last_pixels_update_ms = 0;
}

void Hit::setNetworkConnectingMode(bool enabled) {
    if (provisioning_mode) {
        return;
    }

    const bool changed = (network_connecting_mode != enabled);
    network_connecting_mode = enabled;
    if (enabled) {
        wifi_signal_start_ms = 0;
        wifi_signal_until_ms = 0;
        stopAdcDma();
        if (changed) {
            if (!Config::SERIAL_SCAN_ONLY) Serial.println("HIT ADC stopped for STA connect");
        }
    } else {
        startAdcDma();
        if (changed) {
            if (!Config::SERIAL_SCAN_ONLY) Serial.println("HIT ADC resumed after STA connect");
        }
    }
    last_pixels_update_ms = 0;
}

bool Hit::loginPending() {
    return WiFi.status() == WL_CONNECTED && !RefereeClient::isLoggedIn();
}

bool Hit::hitDetectionBlocked() {
    if (provisioning_mode || network_connecting_mode) {
        return true;
    }

    if (Provisioning::isLocalHost()) {
        return loginPending() || RefereeClient::isDead();
    }

    // 从机在主机联网/登录期间跟随主机，不提前产生裁判击打事件。
    if (shared_state_valid) {
        return shared_mode == LIGHT_PROVISIONING ||
               shared_mode == LIGHT_NETWORK_CONNECTING ||
               shared_mode == LIGHT_REFEREE_LOGIN ||
               shared_mode == LIGHT_DEAD;
    }

    // 没有主机时保持原来的离线模式：本地仍可检测击打。
    return false;
}

void Hit::signalOfflineExit() {
    // 长按退出离线后给出一次短闪，提示用户退出动作已经生效。
    offline_exit_signal_until_ms = millis() + 1000;
    last_pixels_update_ms = 0;
}

void Hit::showNetworkConnecting() {
    // 保存请求可能落在配网闪烁的灭相位，立即刷新避免复位前残留黑帧。
    LedController::showSolid(YELLOW);
    last_pixels_update_ms = millis();
}

void Hit::showProvisioningNow(bool blink_on) {
    // 立即亮黄灯，不等下一次 loop；灭相位也先亮，避免看到黑灯。
    (void)blink_on;
    provisioning_entry_hold_until_ms =
        millis() + Config::PROVISIONING_ENTRY_SOLID_MS;
    LedController::showSolid(YELLOW);
    last_pixels_update_ms = millis();
}

void Hit::signalWifiConnected() {
    if (provisioning_mode) {
        return;
    }

    network_connecting_mode = false;
    wifi_signal_start_ms = 0;
    wifi_signal_until_ms = 0;
    startAdcDma();
    last_pixels_update_ms = 0;
}

void Hit::setSharedState(
    bool valid,
    LightMode mode,
    uint32_t color_value) {
    shared_state_valid = valid;
    shared_mode = mode;
    shared_color = color_value;
    if (valid && color != color_value) {
        setSynchronizedColor(color_value);
    }
    last_pixels_update_ms = 0;
}


void Hit::setSynchronizedBlinkPhase(bool on) {
    shared_blink_on = on;
    last_pixels_update_ms = 0;
}

void Hit::beginTeamSwitchLightHold() {
    team_switch_purple_until_ms = millis() + Config::TEAM_SWITCH_PURPLE_MIN_MS;
    last_pixels_update_ms = 0;
}

bool Hit::isTeamSwitchLightHeld() {
    return team_switch_purple_until_ms != 0 &&
           static_cast<int32_t>(team_switch_purple_until_ms - millis()) > 0;
}

// 保存后从机保持黄灯常亮，屏蔽配网闪烁相位，直到联网成功。
void Hit::beginProvisioningSaveHold() {
    provisioning_save_hold = true;
    LedController::showSolid(YELLOW);
    last_pixels_update_ms = millis();
}

void Hit::endProvisioningSaveHold() {
    provisioning_save_hold = false;
    last_pixels_update_ms = 0;
}

bool Hit::isProvisioningSaveHeld() {
    return provisioning_save_hold;
}

bool Hit::isSharedStateActive() {
    return shared_state_valid;
}

bool Hit::isLoginSuccessfulState() {
    return RefereeClient::isLoggedIn() ||
           (shared_state_valid &&
            (shared_mode == LIGHT_ONLINE || shared_mode == LIGHT_DEAD));
}

bool Hit::isProvisioningState() {
    return provisioning_mode ||
           (shared_state_valid && shared_mode == LIGHT_PROVISIONING);
}

bool Hit::isButtonLocked() {
    if (provisioning_mode || network_connecting_mode) {
        return true;
    }
    return shared_state_valid &&
           (shared_mode == LIGHT_PROVISIONING ||
            shared_mode == LIGHT_NETWORK_CONNECTING ||
            shared_mode == LIGHT_REFEREE_LOGIN);
}


// 配网保存后按机器人编号决定阵营，同时更新 Hit::color、Settings 和 SystemState。
void Hit::applyConfiguredTeam(uint32_t robot_id) {
    if (robot_id >= 101 && robot_id <= 111) {
        color = BLUE;
    } else if (robot_id >= 1 && robot_id <= 11) {
        color = RED;
    } else {
        // 编号非法时沿用当前颜色，避免把灯变成未知状态。
        Settings::setRobotId(robot_id);
        SystemState::setTeam(color, robot_id);
        last_pixels_update_ms = 0;
        return;
    }

    Settings::setColor(color);
    Settings::setRobotId(robot_id);
    SystemState::setTeam(color, robot_id);
    last_pixels_update_ms = 0;
}

void Hit::setSynchronizedColor(uint32_t shared_color) {
    if (shared_color != RED && shared_color != BLUE) {
        return;
    }

    color = shared_color;
    Settings::setColor(color);

    const uint32_t current_robot_id = Settings::getRobotId(3);
    uint32_t next_robot_id = current_robot_id;
    if (current_robot_id >= 1 && current_robot_id <= 11) {
        next_robot_id = color == BLUE ? current_robot_id + 100 : current_robot_id;
    } else if (current_robot_id >= 101 && current_robot_id <= 111) {
        next_robot_id = color == RED ? current_robot_id - 100 : current_robot_id;
    } else {
        next_robot_id = color == RED ? 3 : 103;
    }
    Settings::setRobotId(next_robot_id);
    SystemState::setTeam(color, next_robot_id);
    last_pixels_update_ms = 0;
}

void Hit::updatePixels() {
    const uint32_t now = millis();
    // WS2812每颗约24bit，避免在主循环中连续重复发送同一帧。
    if (last_pixels_update_ms != 0 &&
        now - last_pixels_update_ms < 1) {
        return;
    }
    last_pixels_update_ms = now;

    const SystemState::Snapshot &state = SystemState::get();
    LightMode display_mode = LIGHT_OFFLINE;
    uint32_t display_color = color;
    bool local_host = state.role == SystemState::Role::Host;

    if (state.network == SystemState::Network::Provisioning) {
        display_mode = LIGHT_PROVISIONING;
        // 保存后锁定的常亮阶段：不参与配网闪烁，也不让底色露出。
        if (provisioning_save_hold) {
            display_mode = LIGHT_NETWORK_CONNECTING;
        }
    } else if (local_host) {
        if (isTeamSwitchLightHeld()) {
            display_mode = LIGHT_REFEREE_LOGIN;
        } else if (state.network == SystemState::Network::Connecting) {
            display_mode = LIGHT_NETWORK_CONNECTING;
        } else if (state.referee == SystemState::Referee::Dead) {
            display_mode = LIGHT_DEAD;
        } else if (state.referee != SystemState::Referee::LoggedIn) {
            display_mode = LIGHT_REFEREE_LOGIN;
        } else {
            display_mode = LIGHT_ONLINE;
        }
    } else if (state.host_state_valid) {
        display_mode = static_cast<LightMode>(state.remote_light_mode);
        display_color = state.team_color == 0 ? color : state.team_color;
    }

    const bool local_hit_active =
        last_hit_ms != 0 &&
        now - last_hit_ms < HIT_BLINK_MS &&
        !hitDetectionBlocked();
    const bool offline_signal =
        offline_exit_signal_until_ms != 0 &&
        static_cast<int32_t>(offline_exit_signal_until_ms - now) > 0;
    const bool provisioning_entry_hold =
        provisioning_entry_hold_until_ms != 0 &&
        static_cast<int32_t>(provisioning_entry_hold_until_ms - now) > 0;
    const bool provisioning_blink = provisioning_entry_hold ||
        provisioning_save_hold ||
        (local_host
            ? ((now / Config::PROVISIONING_BLINK_MS) % 2 == 0)
            : state.blink_on);
    if (Config::CAN_DEBUG) {
        static uint32_t last_light_diag_ms = 0;
        static uint8_t last_light_diag_mode = 0xFF;
        static bool last_light_diag_blink = false;
        const bool light_blink = display_mode == LIGHT_PROVISIONING ? provisioning_blink : true;
        if (now - last_light_diag_ms >= 500 ||
            display_mode != last_light_diag_mode ||
            light_blink != last_light_diag_blink) {
            last_light_diag_ms = now;
            last_light_diag_mode = static_cast<uint8_t>(display_mode);
            last_light_diag_blink = light_blink;
            Serial.printf(
                "LIGHT role=%s net=%u ref=%u remote=%u shared_v=%u shared_m=%u " 
                "entry_hold=%u save_hold=%u blink=%u prov_mode=%u conn_mode=%u mode=%u\n",
                local_host ? "HOST" : "NODE",
                static_cast<unsigned>(state.network),
                static_cast<unsigned>(state.referee),
                static_cast<unsigned>(state.remote_light_mode),
                shared_state_valid ? 1U : 0U,
                static_cast<unsigned>(shared_mode),
                provisioning_entry_hold ? 1U : 0U,
                provisioning_save_hold ? 1U : 0U,
                light_blink ? 1U : 0U,
                provisioning_mode ? 1U : 0U,
                network_connecting_mode ? 1U : 0U,
                static_cast<unsigned>(display_mode));
        }
    }

    LedController::render(
        display_mode,
        display_color,
        local_host,
        display_mode == LIGHT_PROVISIONING ? provisioning_blink : true,
        local_hit_active,
        offline_signal,
        now);
}
