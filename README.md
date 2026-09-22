# Outpost 装甲板控制器

RoboMaster 装甲板控制器固件。三块板烧录同一份固件，通过手机配网决定主机，主机连接 Wi-Fi 并登录裁判系统，另外两块板通过 CAN 跟随主机同步状态。

## 硬件平台

- MCU：ESP32-C3
- 框架：Arduino（PlatformIO）
- 单线程 `loop()` 轮询，不使用 FreeRTOS

### 引脚分配

| 功能 | 引脚 |
| --- | --- |
| 击打传感器 | GPIO0（ADC1 CH0）|
| CAN RX | GPIO6 |
| CAN TX | GPIO7 |
| 按键 | GPIO9（内部上拉，按下为低）|
| WS2812 | GPIO10，22 颗 |

CAN 速率 500 kbit/s，串口为 ESP32-C3 原生 USB CDC，115200。

## 编译与烧录

```bash
# 编译
pio run

# 烧录
pio run --target upload

# 串口监视
pio device monitor
```

如果 `pio` 不在 PATH，可以直接调用 PlatformIO 的 venv 可执行文件：

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run
```

## 使用流程

1. **启动**：上电后进入离线状态，显示本机配置的阵营颜色（红或蓝）。
2. **进入配网**：长按按键 1 秒，本板成为配网主机，三块板同步黄灯闪烁。
3. **手机配网**：连接热点 `RM_<MAC地址>`（密码 `12345678`），浏览器打开 `http://192.168.4.1/`。
4. **保存配置**：填写 Wi-Fi 名称、密码和机器人编号，点击保存。三块板转为黄灯常亮。
5. **联网登录**：主机连接 Wi-Fi，登录裁判系统。成功后三块板显示阵营颜色。
6. **切换阵营**：短按按键，红蓝阵营互换（Robot ID 加减 100，例如 `3 <-> 103`）。
7. **退出**：长按按键退出配网或回到离线。

Wi-Fi 名称、密码、Robot ID 和阵营只保存在 RAM，**断电后清空**，需要重新配网。

## 目录结构

```text
outpost/
├─ include/          公共头文件
├─ src/              C++ 实现
├─ platformio.ini    PlatformIO 工程配置
└─ 程序逻辑说明.md   完整设计文档
```

主要模块：

| 模块 | 作用 |
| --- | --- |
| `main` | 初始化模块并按固定顺序运行主循环 |
| `system_state` | 系统角色、网络、裁判和主机状态快照 |
| `button_controller` | 按键边沿、短按和长按判断 |
| `hit_detector` | ADC 击打阈值、重新武装和冷却判断 |
| `hit` | ADC DMA、击打计数、阵营业务和显示状态组合 |
| `led_controller` | WS2812 实际显示 |
| `can` | CAN 收发、主机仲裁、状态同步和帧分发 |
| `wifi_manager` | Wi-Fi AP/STA 底层模式切换 |
| `provisioning` | 配网网页、扫描、连接重试和保存流程 |
| `referee_client` | 裁判系统 UDP 登录、状态和击打上报 |
| `settings` | 本次上电期间的运行时配置（RAM）|
| `config` | 用户可调参数、引脚、阈值和时间参数 |

## 三块板主从逻辑

完成配网并保存 Wi-Fi 的板为主机，另外两块为从机。

- 主机连接 Wi-Fi 和裁判系统，是唯一的状态广播者。
- 从机不联网，只通过 CAN 跟随主机。
- 主机每 50ms 广播一次公共状态（配网/联网/登录/在线/阵亡、阵营颜色、闪烁相位）。
- 在线状态下任意一块板长按，可以抢占成为配网主机，原主机让位。
- 从机发送按键请求和击打事件，由主机统一处理和上报。

## CAN 协议

所有内部报文统一使用标准 ID `0x000`，固定 DLC=8，`data[0]=0xA0`。

```text
byte 0：固定 0xA0
byte 1：模式  0离线 1配网 2连接Wi-Fi 3裁判登录中 4在线 5阵亡
byte 2：标志  bit0配网 bit1黄灯亮相 bit2发送者为主机 bit3状态变化 bit4主机接管
byte 3：阵营  0红 1蓝
byte 4：LED 亮度
byte 5：事件  0状态 1切换阵营 2主机接管 3退出 4击打 5确认
byte 6：状态版本号 uint8_t
byte 7：事件参数
```

主机周期广播状态；从机只接收并应用较新的版本。按键请求和击打事件由从机设置 pending 标志重试发送，主机处理后以新状态或确认事件清除。

## 灯光状态

| 状态 | 显示 |
| --- | --- |
| 离线 / 在线 | 红或蓝阵营色常亮 |
| 配网 | 黄灯闪烁 |
| 保存后联网 | 黄灯常亮 |
| 裁判登录中 | 紫灯常亮 |
| 阵亡 | 灯灭 |
| 被击打 | 灭 50ms 后恢复 |

## 常用参数

需要调整的参数集中在 `include/config.hpp`：

```text
HIT_THRESHOLD                 击打阈值（默认 2000）
HIT_COOLDOWN_MS               击打冷却（默认 500ms）
LED_BRIGHTNESS                WS2812 亮度 0~255
LONG_PRESS_MS                 长按判定时间（默认 1000ms）
PROVISIONING_WIFI_PREFIX      配网热点名称前缀（默认 RM_）
PROVISIONING_WIFI_PASSWORD    配网热点密码
PROVISIONING_BLINK_MS         配网黄灯闪烁周期
TEAM_SWITCH_PURPLE_MIN_MS     切换阵营后紫灯最短保持时间
REFEREE_SERVER_IP             裁判系统服务器地址
KEY_PIN / WS2812_PIN / CAN_RX / CAN_TX    引脚定义
```

调试日志开关：`CAN_DEBUG`、`HIT_DEBUG`、`REFEREE_HP_DEBUG`，正式使用前建议关闭。

## 已知限制

- 配置不持久化，每次上电都需要重新配网。
- 三块板固件完全相同且不携带身份信息，两块板同时长按发起主机接管时无法在协议层可靠区分，正常操作要求同一时刻只有一块板长按。
- 目标热点必须是 2.4GHz（ESP32-C3 不支持 5GHz）。

## 文档

完整的设计说明、状态机流程和协议细节见 [`程序逻辑说明.md`](程序逻辑说明.md)。
