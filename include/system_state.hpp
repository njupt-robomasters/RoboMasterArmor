#pragma once

#include <Arduino.h>

class SystemState {
  public:
    enum class Role : uint8_t {
        Standalone,
        Host,
        Node,
    };

    enum class Network : uint8_t {
        Offline,
        Provisioning,
        Connecting,
        Connected,
    };

    enum class Referee : uint8_t {
        Disabled,
        LoginPending,
        LoggedIn,
        Dead,
    };

    struct Snapshot {
        Role role = Role::Standalone;
        Network network = Network::Offline;
        Referee referee = Referee::Disabled;
        uint32_t team_color = 0;
        uint32_t robot_id = 0;
        uint32_t active_host_id = 0;
        uint32_t last_host_rx_ms = 0;
        bool host_state_valid = false;
        bool blink_on = false;
        uint8_t remote_light_mode = 0;
    };

    static void begin();
    static const Snapshot &get();

    static void setRole(Role role);
    static void setNetwork(Network network);
    static void setReferee(Referee referee);
    static void setTeam(uint32_t color, uint32_t robot_id);
    static void setHost(uint32_t host_id, uint32_t received_at_ms, bool valid);
    static void setBlink(bool on);
    static void setRemoteLight(uint8_t mode);

    // 所有跨模块状态迁移的唯一入口。底层模块可以读取快照，但不再自行拼装迁移流程。
    static void enterProvisioning();
    static void beginConnecting();
    static void markWifiConnected();
    static void enterOffline();
    static void setRefereeState(Referee referee);
    static void acceptHost(uint32_t host_id, uint32_t received_at_ms,
                           uint8_t remote_light_mode, bool blink_on,
                           uint32_t team_color, Network network,
                           Referee referee);
    static void clearHost();
};
