#pragma once

#include <Arduino.h>

namespace Config {


// 手机连接的装甲板配网热点名称：前缀 + 设备 MAC 地址。
// 例如：RM_A1B2C3D4E5F6
static constexpr const char *PROVISIONING_WIFI_PREFIX = "RM_";

// 手机连接装甲板配网热点时使用的密码。
static constexpr const char *PROVISIONING_WIFI_PASSWORD = "12345678";

// WS2812 亮度，范围 0~255。255 为最亮，建议根据现场需要调整。
static constexpr uint8_t LED_BRIGHTNESS = 255;

// 配网黄灯闪烁周期，亮灭各约为周期的一半。
static constexpr uint32_t PROVISIONING_BLINK_MS = 500;

// 长按进入配网后，三块板先统一黄灯常亮的时间；结束后才进入黄灯闪烁。
// 这段时间也覆盖主机启动 SoftAP 时的同步阻塞，避免从机先熄灯。
static constexpr uint32_t PROVISIONING_ENTRY_SOLID_MS = 1000;

// ADC 击打阈值：采样值大于该值才可能判定为击打。
static constexpr uint32_t HIT_THRESHOLD = 2000;

// 两次有效击打的时间间隔必须严格大于该值。
static constexpr uint32_t HIT_COOLDOWN_MS = 50;

// 阵营切换后紫灯至少保持这段时间，避免裁判 ACK 很快到达时看不到登录过程。
static constexpr uint32_t TEAM_SWITCH_PURPLE_MIN_MS = 800;

// 退出配网/联网后的状态同步窗口，期间忽略按键，避免立刻切色造成三板不同步。
static constexpr uint32_t POST_PROVISIONING_KEY_GUARD_MS = 800;

// 切换阵营后的按键不应期，期间短按忽略。
static constexpr uint32_t TEAM_SWITCH_KEY_GUARD_MS = 500;

// 重新登录后的会话沉降时间。重开 socket 后最初这几百毫秒读到的 0x21/0x24
// 可能是旧会话残留，按丢弃处理，否则会误判登录成功。
// 必须小于 TEAM_SWITCH_PURPLE_MIN_MS。
static constexpr uint32_t REFEREE_SESSION_SETTLE_MS = 400;

// 配网页面默认填写的目标 Wi-Fi，仅用于快速填写，不是配网热点参数。
static constexpr const char *DEFAULT_WIFI_SSID = "Potential-Robot";
static constexpr const char *DEFAULT_WIFI_PASSWORD = "12345678";

// 烧录确认标识。烧录后串口首屏必须出现该字符串。
static constexpr const char *FIRMWARE_TAG = "OUTPOST-20260914-DIAG";

// GPIO配置
static constexpr gpio_num_t CAN_RX = GPIO_NUM_6;
static constexpr gpio_num_t CAN_TX = GPIO_NUM_7;
static constexpr uint8_t KEY_PIN = 9;
//static constexpr uint8_t KEY_PIN = 2;//新装甲板引脚配置 ·
static constexpr uint8_t WS2812_PIN = 10;
static constexpr uint8_t HIT_ADC_GPIO = 0;
static constexpr uint8_t WS2812_COUNT = 22;

// SoftAP 配网固定参数。
static constexpr uint8_t AP_CHANNEL = 6;

static constexpr uint32_t LONG_PRESS_MS = 1000;

// CAN配置。内部所有报文统一使用标准 ID 0x00，报文类型放在数据区。
static constexpr uint32_t CAN_BITRATE = 1000000;

// 裁判系统 UDP 配置
static constexpr uint8_t REFEREE_SERVER_IP[4] = {192, 168, 1, 2};
static constexpr uint16_t REFEREE_SERVER_PORT = 62101;
static constexpr uint16_t REFEREE_LOCAL_PORT = 60000;

// 击打检测内部重新武装阈值
static constexpr uint32_t HIT_REARM_THRESHOLD = 1000;
static constexpr uint32_t ADC_DEBUG_INTERVAL_MS = 500;

// USB CDC配置
static constexpr uint32_t SERIAL_BAUDRATE = 115200;
// 打开后关掉 ADC/CAN/裁判日志，只留 Wi-Fi 扫描名单、强度、连接状态和报错。
static constexpr bool SERIAL_SCAN_ONLY = false;
// CAN调试独立打开，便于排查主从同步，不影响上面的网络日志开关。
static constexpr bool CAN_DEBUG = true;
// 击打链路诊断：输出 ADC 门控、检测结果和从机计数变化，不输出 CAN 原始帧。
static constexpr bool HIT_DEBUG = true;
// 事件日志总开关：按键、切阵营、灯光模式跳变等一次性事件的时间戳日志。
// 与周期日志（ADC/CAN 状态帧）无关，专门用于排查时序问题。
// 正式版可以关掉，排查时打开。
static constexpr bool EVENT_LOG = true;
// 收到主机状态帧时是否打印每一帧。默认关闭：主机 20Hz 广播，三块板各打一行
// 会把真正有用的按键/登录日志完全淹没。打开后 CAN STATE 逐帧输出。
static constexpr bool CAN_STATE_VERBOSE = false;

// CAN排查时只保留 CAN 诊断输出，避免 Wi-Fi/裁判系统周期日志刷屏。
static constexpr bool SERIAL_CAN_ONLY = false;
// 只输出裁判系统同步得到的当前血量，不打开其它裁判协议日志。
static constexpr bool REFEREE_HP_DEBUG = true;

} // namespace Config
