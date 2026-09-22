#include "can.hpp"

#include <WiFi.h>
#include <driver/twai.h>

#include "config.hpp"
#include "hit.hpp"
#include "led_controller.hpp"
#include "provisioning.hpp"
#include "referee_client.hpp"
#include "system_state.hpp"

uint32_t Can::rx_count=0, Can::tx_count=0, Can::tx_error_count=0, Can::last_rx_ms=0;
uint32_t Can::last_state_tx_ms=0, Can::last_request_tx_ms=0, Can::last_host_rx_ms=0;
uint8_t Can::state_version=0, Can::last_host_version=0, Can::pending_event=EVENT_STATE;
uint8_t Can::last_broadcast_mode=0xFF;
uint8_t Can::pending_event_arg=0, Can::pending_event_version=0, Can::next_event_sequence=0;
uint8_t Can::pending_team_color=0, Can::pending_event_attempts=0;
uint32_t Can::last_local_hit_count=0;
bool Can::host_state_valid=false, Can::host_changed=false;
uint8_t Can::host_event=EVENT_STATE, Can::host_event_arg=0;
uint32_t Can::host_claim_until_ms=0;
uint8_t Can::last_team_request_sequence=0, Can::last_hit_event_sequence=0;
bool Can::team_request_seen=false, Can::hit_event_seen=false;

static uint32_t tr_rx=0, tr_tx=0, tr_fail=0, tr_last_rx=0;

bool Can::transportBegin() {
    twai_general_config_t g=TWAI_GENERAL_CONFIG_DEFAULT(Config::CAN_TX,Config::CAN_RX,TWAI_MODE_NORMAL);
    g.tx_queue_len=8; g.rx_queue_len=20;
    twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if(twai_driver_install(&g,&timing,&filter)!=ESP_OK) return false;
    return twai_start()==ESP_OK;
}
bool Can::transportReceive(twai_message_t &f) { if(twai_receive(&f,0)!=ESP_OK)return false; tr_rx++;tr_last_rx=millis();return true; }
bool Can::transportSend(twai_message_t &f) { if(twai_transmit(&f,0)==ESP_OK){tr_tx++;return true;}tr_fail++;return false; }
bool Can::isNewerVersion(uint8_t v,uint8_t p){uint8_t d=static_cast<uint8_t>(v-p);return d!=0&&d<128;}
void Can::markStateChanged(){state_version++;host_changed=true;}

// 从机判主机消失的超时。配网/联网阶段主机主循环会阻塞近 1 秒，必须放宽，
// 否则从机会误判主机掉线，掉回离线红/蓝。
uint32_t Can::hostTimeoutMs(){
    if(Hit::isProvisioningState()||Hit::isProvisioningSaveHeld()){
        return HOST_TIMEOUT_DURING_WIFI_MS;
    }
    const SystemState::Snapshot &s=SystemState::get();
    if(s.host_state_valid){
        const uint8_t m=s.remote_light_mode;
        if(m==MODE_PROVISIONING||m==MODE_CONNECTING){
            return HOST_TIMEOUT_DURING_WIFI_MS;
        }
    }
    return HOST_TIMEOUT_MS;
}

uint8_t Can::localMode(){
    const auto &s=SystemState::get();
    if(s.network==SystemState::Network::Provisioning)return MODE_PROVISIONING;
    if(s.network==SystemState::Network::Connecting||WiFi.status()!=WL_CONNECTED)return MODE_CONNECTING;
    if(Hit::isTeamSwitchLightHeld())return MODE_REFEREE_LOGIN;
    if(s.referee==SystemState::Referee::Dead)return MODE_DEAD;
    if(s.referee!=SystemState::Referee::LoggedIn)return MODE_REFEREE_LOGIN;
    return MODE_ONLINE;
}
uint8_t Can::localFlags(uint8_t mode){
    uint8_t f=FLAG_HOST;
    if(mode==MODE_PROVISIONING)f|=FLAG_PROVISIONING;
    if(mode==MODE_PROVISIONING&&(millis()/Config::PROVISIONING_BLINK_MS)%2==0)f|=FLAG_BLINK_ON;
    if(host_changed)f|=FLAG_CHANGED;
    // 接管窗口内持续置位，保证从机至少收到一次主机切换信号。
    if(host_event==EVENT_HOST_CLAIM ||
       (host_claim_until_ms!=0 && static_cast<int32_t>(host_claim_until_ms-millis())>0)){
        f|=FLAG_HOST_CLAIM;
    }
    return f;
}

void Can::begin(){
    rx_count=tx_count=tx_error_count=last_rx_ms=0;last_state_tx_ms=last_request_tx_ms=last_host_rx_ms=0;
    state_version=last_host_version=0;last_broadcast_mode=0xFF;pending_event=EVENT_STATE;pending_event_arg=pending_event_version=0;
    next_event_sequence=0;last_local_hit_count=Hit::hit_count;host_state_valid=false;host_changed=false;
    pending_team_color=Hit::color==Hit::BLUE?1:0;pending_event_attempts=0;
    host_event=EVENT_STATE;host_event_arg=0;team_request_seen=hit_event_seen=false;
    host_claim_until_ms=0;
    SystemState::enterOffline();Hit::setSharedState(false,Hit::LIGHT_OFFLINE,Hit::color);transportBegin();
    if(Config::CAN_DEBUG)Serial.println("CAN unified protocol id=0x000 dlc=8 magic=0xA0");
}

void Can::onLoop(){
    receiveFrames(); const uint32_t now=millis();
    if(!Provisioning::isLocalHost()&&Hit::hit_count!=last_local_hit_count){
        last_local_hit_count=Hit::hit_count;
        if(host_state_valid&&pending_event==EVENT_STATE){pending_event=EVENT_HIT;pending_event_arg=++next_event_sequence;pending_event_version=last_host_version;}
    } else if(Hit::hit_count!=last_local_hit_count) last_local_hit_count=Hit::hit_count;
    if(Provisioning::isLocalHost()){
        if(now-last_state_tx_ms>=STATE_INTERVAL_MS){
            if(Config::CAN_DEBUG){
                static uint32_t last_tx_gap_diag_ms=0;
                const uint32_t gap=now-last_state_tx_ms;
                if(gap>300&&now-last_tx_gap_diag_ms>=300){
                    last_tx_gap_diag_ms=now;
                    Serial.printf("CAN TX GAP %lu ms (loop blocked?)\n",(unsigned long)gap);
                }
            }
            last_state_tx_ms=now;sendHostState();
        }
    }else if(pending_event!=EVENT_STATE &&
             (host_state_valid || pending_event==EVENT_TEAM_TOGGLE) &&
             now-last_request_tx_ms>=REQUEST_RETRY_MS &&
             (host_state_valid ? now-last_host_rx_ms>=3 : true)){
        last_request_tx_ms=now;sendRequest(pending_event,pending_event_arg);
        if (!host_state_valid && pending_event == EVENT_TEAM_TOGGLE &&
            ++pending_event_attempts >= 3) {
            pending_event=EVENT_STATE;
            pending_event_attempts=0;
        }
    }
    if(!Provisioning::isLocalHost()&&host_state_valid&&now-last_host_rx_ms>hostTimeoutMs()){

        if(Config::CAN_DEBUG)Serial.printf("CAN HOST TIMEOUT age=%lu ms limit=%lu ms, clear host (light falls back)\n",(unsigned long)(now-last_host_rx_ms),(unsigned long)hostTimeoutMs());

        host_state_valid=false;SystemState::clearHost();Hit::endProvisioningSaveHold();Hit::setSharedState(false,Hit::LIGHT_OFFLINE,Hit::color);}
}

bool Can::transmit(twai_message_t &f,const char *label){
    f.identifier=COMMON_CAN_ID;f.data_length_code=8;
    if(transportSend(f)){tx_count=tr_tx;return true;}tx_error_count=tr_fail;
    if(Config::CAN_DEBUG)Serial.printf("CAN TX FAILED %s\n",label?label:"?");return false;
}

void Can::sendHostState(){
    twai_message_t f{};const uint8_t mode=localMode();
    // 模式变化本身就是一次新状态，即使没有显式事件也必须推进版本号。
    // 这保证 CONNECTING->LOGIN->ONLINE 的状态不会被从机当成旧帧丢弃。
    if (last_broadcast_mode != mode) {
        state_version++;
        host_changed=true;
        last_broadcast_mode=mode;
    }
    f.data[0]=PROTOCOL_MAGIC;f.data[1]=mode;f.data[2]=localFlags(mode);
    f.data[3]=Hit::color==Hit::BLUE?1:0;f.data[4]=Config::LED_BRIGHTNESS;f.data[5]=host_event;f.data[6]=state_version;f.data[7]=host_event_arg;
    if(transmit(f,"state")){host_changed=false;if(host_event==EVENT_ACK||host_event==EVENT_HOST_CLAIM){host_event=EVENT_STATE;host_event_arg=0;}}
}
void Can::sendOfflineState(){
    // 退出配网必须发送真正的 MODE_OFFLINE；不能沿用当前的配网模式，
    // 否则从机会把 EVENT_OFFLINE 当成配网状态继续显示黄灯。
    twai_message_t f{};
    f.data[0]=PROTOCOL_MAGIC;
    f.data[1]=MODE_OFFLINE;
    f.data[2]=FLAG_HOST|FLAG_CHANGED;
    f.data[3]=Hit::color==Hit::BLUE?1:0;
    f.data[4]=Config::LED_BRIGHTNESS;
    f.data[5]=EVENT_OFFLINE;
    f.data[6]=++state_version;
    f.data[7]=0;
    for(uint8_t i=0;i<3;i++) transmit(f,"offline-state");
}
void Can::sendRequest(uint8_t event,uint8_t arg){
    twai_message_t f{};f.data[0]=PROTOCOL_MAGIC;f.data[1]=localMode();f.data[2]=0;
    f.data[3]=event==EVENT_TEAM_TOGGLE
        ? (host_state_valid ? (Hit::color==Hit::BLUE?0:1) : pending_team_color)
        : (Hit::color==Hit::BLUE?1:0);
    f.data[4]=Config::LED_BRIGHTNESS;f.data[5]=event;f.data[6]=state_version;f.data[7]=arg;transmit(f,"request");
}
void Can::broadcastHostStateNow(){if(!Provisioning::isLocalHost())return;for(uint8_t i=0;i<3;i++)sendHostState();last_state_tx_ms=millis();}
void Can::requestHostClaim(){if(!Provisioning::isLocalHost())return;host_event=EVENT_HOST_CLAIM;host_claim_until_ms=millis()+HOST_CLAIM_ADVERTISE_MS;markStateChanged();broadcastHostStateNow();}
void Can::requestTeamToggle(){if(Provisioning::isLocalHost())Hit::toggleColor();else if(pending_event==EVENT_STATE){pending_event=EVENT_TEAM_TOGGLE;pending_event_arg=++next_event_sequence;pending_event_version=last_host_version;}}
void Can::notifyLocalStateChanged(){if(Provisioning::isLocalHost())markStateChanged();}
void Can::requestOffline(){
    if(Provisioning::isLocalHost()) {
        sendOfflineState();
    } else {
        sendRequest(EVENT_OFFLINE,0);
    }
    Provisioning::abortToOffline();
}
void Can::notifyOfflineColorChanged(){
    if (Provisioning::isLocalHost() || host_state_valid) return;
    pending_event=EVENT_TEAM_TOGGLE;
    pending_event_arg=++next_event_sequence;
    pending_team_color=Hit::color==Hit::BLUE?1:0;
    pending_event_attempts=0;
    last_request_tx_ms=0;
}
void Can::clearHostState(){host_state_valid=false;last_host_rx_ms=0;SystemState::clearHost();Hit::endProvisioningSaveHold();Hit::setSharedState(false,Hit::LIGHT_OFFLINE,Hit::color);}
void Can::stopOfflineColorNormalization(){}

void Can::receiveFrames(){twai_message_t f{};while(transportReceive(f)){rx_count=tr_rx;last_rx_ms=tr_last_rx;if(f.extd||f.rtr||f.identifier!=COMMON_CAN_ID||f.data_length_code!=8||f.data[0]!=PROTOCOL_MAGIC)continue;handleFrame(f);}}
void Can::handleFrame(const twai_message_t &f){
    const uint8_t flags=f.data[2];const uint8_t event=f.data[5];
    // 离线没有主机，阵营事件由其它板直接应用；发起板已先完成本地切色。
    if (!Provisioning::isLocalHost() &&
        !(flags&FLAG_HOST) && event==EVENT_TEAM_TOGGLE && !host_state_valid) {
        Hit::setSynchronizedColor(f.data[3]?Hit::BLUE:Hit::RED);
        return;
    }
    // 只看标志位：接管窗口内 event 已回到 EVENT_STATE，不能再依赖 event 字段。
    if(flags&FLAG_HOST_CLAIM){if(Provisioning::isLocalHost())Provisioning::surrenderHostToPeer();applyHostState(f);return;}
    if(flags&FLAG_HOST){if(!Provisioning::isLocalHost())applyHostState(f);return;}
    if(Provisioning::isLocalHost())handleRequest(f);
}

void Can::applyHostState(const twai_message_t &f){
    const uint8_t mode=f.data[1],ver=f.data[6];if(mode>MODE_DEAD)return;
    // 版本号是每块板各自递增的，换主机后不可比，必须重置基线。
    // 只看标志位：接管窗口内 event 已回到 EVENT_STATE。
    const bool host_takeover = (f.data[2] & FLAG_HOST_CLAIM) != 0;
    if (host_takeover) {
        if(Config::CAN_DEBUG)Serial.printf("CAN HOST TAKEOVER ver=%u reset baseline (was %u)\n",(unsigned)ver,(unsigned)last_host_version);
        host_state_valid = false;
        last_host_version = 0;
    }
    const bool newer=!host_state_valid||isNewerVersion(ver,last_host_version);
    // d==0 同版本周期帧放行刷相位但禁止改模式；d>=128 真正更旧的帧整帧丢弃。
    const uint8_t version_delta=static_cast<uint8_t>(ver-last_host_version);
    if (host_state_valid && !newer) {
        last_host_rx_ms=millis();
        if (version_delta != 0) {
            // 真正更旧的版本：不覆盖模式、阵营和灯光，只续心跳。
            return;
        }
        // 同版本周期帧：允许刷新相位，但模式变化要拒绝。
        if (mode != SystemState::get().remote_light_mode) {
            return;
        }
    }
    // 只执行一次常亮初始化；每帧都调用会不断刷新计时，从机就一直不闪烁。
    const bool entering_provisioning =
        mode == MODE_PROVISIONING &&
        (!host_state_valid ||
         SystemState::get().network != SystemState::Network::Provisioning);
    const uint32_t color=f.data[3]?Hit::BLUE:Hit::RED;host_state_valid=true;last_host_rx_ms=millis();last_host_version=ver;
    SystemState::Network n=SystemState::Network::Offline;SystemState::Referee r=SystemState::Referee::Disabled;
    if(mode==MODE_PROVISIONING)n=SystemState::Network::Provisioning;else if(mode==MODE_CONNECTING)n=SystemState::Network::Connecting;
    else if(mode==MODE_REFEREE_LOGIN){n=SystemState::Network::Connected;r=SystemState::Referee::LoginPending;}
    else if(mode==MODE_ONLINE||mode==MODE_DEAD){n=SystemState::Network::Connected;r=mode==MODE_DEAD?SystemState::Referee::Dead:SystemState::Referee::LoggedIn;}
    SystemState::acceptHost(1,last_host_rx_ms,mode,(f.data[2]&FLAG_BLINK_ON)!=0,color,n,r);
    // 先锁联网黄灯再同步阵营，避免颜色同步过程中露出红蓝。
    if (mode == MODE_CONNECTING) {
        Hit::setProvisioningMode(false);
        Hit::setNetworkConnectingMode(true);
        // 从机也锁黄灯常亮，避免灭相位露出红蓝底色。
        Hit::beginProvisioningSaveHold();
        Hit::showNetworkConnecting();
    }
    Hit::setSharedState(true,static_cast<Hit::LightMode>(mode),color);Hit::setSynchronizedBlinkPhase((f.data[2]&FLAG_BLINK_ON)!=0);LedController::setBrightness(f.data[4]);
    if(mode==MODE_PROVISIONING){
        Hit::setProvisioningMode(false);
        Hit::setNetworkConnectingMode(false);
        if (entering_provisioning) {
            Hit::showProvisioningNow((f.data[2]&FLAG_BLINK_ON)!=0);
        }
    }
    else if(mode==MODE_CONNECTING){Hit::setProvisioningMode(false);Hit::setNetworkConnectingMode(true);Hit::showNetworkConnecting();}
    else{
        Hit::setProvisioningMode(false);
        Hit::setNetworkConnectingMode(false);
        // 主机已离开配网/联网阶段，解除常亮锁。
        Hit::endProvisioningSaveHold();
    }
    if(pending_event!=EVENT_STATE&&isNewerVersion(ver,pending_event_version))pending_event=EVENT_STATE;
    if(f.data[5]==EVENT_ACK && pending_event==EVENT_HIT &&
       f.data[7]==pending_event_arg) {
        pending_event=EVENT_STATE;
        pending_event_arg=0;
    }
    if(Config::CAN_DEBUG)Serial.printf("CAN STATE mode=%u version=%u event=%u color=%s\n",mode,ver,f.data[5],color==Hit::BLUE?"BLUE":"RED");
}

void Can::handleRequest(const twai_message_t &f){
    const uint8_t event=f.data[5],seq=f.data[7];
    if(event==EVENT_TEAM_TOGGLE){
        if(team_request_seen&&seq==last_team_request_sequence)return;
        team_request_seen=true;
        last_team_request_sequence=seq;
        RefereeClient::restartLogin();
        Hit::beginTeamSwitchLightHold();
        const uint32_t requested_color=f.data[3]?Hit::BLUE:Hit::RED;
        Hit::setSynchronizedColor(requested_color);
        SystemState::setRefereeState(SystemState::Referee::LoginPending);
        // 主机不应写入从机共享状态缓存，否则本机登录成功后仍会被当作
        // shared LOGIN，按键锁定无法解除。
        Hit::setSharedState(false,Hit::LIGHT_OFFLINE,requested_color);
        markStateChanged();
        broadcastHostStateNow();
    }
    else if(event==EVENT_OFFLINE){sendOfflineState();Provisioning::abortToOffline();}
    else if(event==EVENT_HIT){if(hit_event_seen&&seq==last_hit_event_sequence)return;hit_event_seen=true;last_hit_event_sequence=seq;RefereeClient::queueRemoteHitReports(1);host_event=EVENT_ACK;host_event_arg=seq;markStateChanged();}
}

void Can::printStatus(){if(!Config::CAN_DEBUG)return;Serial.printf("CAN STATUS id=0x000 tx=%lu rx=%lu fail=%lu role=%s version=%u host=%u age=%lu\n",(unsigned long)tx_count,(unsigned long)rx_count,(unsigned long)tx_error_count,Provisioning::isLocalHost()?"HOST":"NODE",state_version,host_state_valid?1:0,last_host_rx_ms?(unsigned long)(millis()-last_host_rx_ms):0UL);}
uint32_t Can::rxCount(){return rx_count;}uint32_t Can::txCount(){return tx_count;}uint32_t Can::txErrorCount(){return tx_error_count;}uint32_t Can::lastRxMs(){return last_rx_ms;}
