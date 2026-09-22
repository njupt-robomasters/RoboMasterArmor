#include "system_state.hpp"

static SystemState::Snapshot state;

void SystemState::begin() {
    state = Snapshot{};
}

const SystemState::Snapshot &SystemState::get() {
    return state;
}

void SystemState::setRole(Role role) {
    state.role = role;
}

void SystemState::setNetwork(Network network) {
    state.network = network;
}

void SystemState::setReferee(Referee referee) {
    state.referee = referee;
}

void SystemState::setTeam(uint32_t color, uint32_t robot_id) {
    state.team_color = color;
    state.robot_id = robot_id;
}

void SystemState::setHost(uint32_t host_id, uint32_t received_at_ms, bool valid) {
    state.active_host_id = host_id;
    state.last_host_rx_ms = received_at_ms;
    state.host_state_valid = valid;
}

void SystemState::setBlink(bool on) {
    state.blink_on = on;
}

void SystemState::setRemoteLight(uint8_t mode) {
    state.remote_light_mode = mode;
}

void SystemState::enterProvisioning() {
    state.role = Role::Host;
    state.network = Network::Provisioning;
    state.referee = Referee::Disabled;
    state.host_state_valid = false;
    state.active_host_id = 0;
}

void SystemState::beginConnecting() {
    state.role = Role::Host;
    state.network = Network::Connecting;
    state.referee = Referee::Disabled;
    state.host_state_valid = false;
    state.active_host_id = 0;
}

void SystemState::markWifiConnected() {
    state.role = Role::Host;
    state.network = Network::Connected;
    state.referee = Referee::LoginPending;
    state.host_state_valid = false;
    state.active_host_id = 0;
}

void SystemState::enterOffline() {
    state.role = Role::Standalone;
    state.network = Network::Offline;
    state.referee = Referee::Disabled;
    state.active_host_id = 0;
    state.last_host_rx_ms = 0;
    state.host_state_valid = false;
    state.remote_light_mode = 0;
    state.blink_on = false;
}

void SystemState::setRefereeState(Referee referee) {
    state.referee = referee;
}

void SystemState::acceptHost(uint32_t host_id, uint32_t received_at_ms,
                             uint8_t remote_light_mode, bool blink_on,
                             uint32_t team_color, Network network,
                             Referee referee) {
    state.role = Role::Node;
    state.network = network;
    state.referee = referee;
    state.active_host_id = host_id;
    state.last_host_rx_ms = received_at_ms;
    state.host_state_valid = true;
    state.remote_light_mode = remote_light_mode;
    state.blink_on = blink_on;
    state.team_color = team_color;
}

void SystemState::clearHost() {
    state.active_host_id = 0;
    state.last_host_rx_ms = 0;
    state.host_state_valid = false;
    state.role = Role::Standalone;
    state.network = Network::Offline;
    state.referee = Referee::Disabled;
    state.remote_light_mode = 0;
    state.blink_on = false;
}
