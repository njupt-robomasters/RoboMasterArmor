#include "web.hpp"

#include <WebServer.h>

#include "config.hpp"
#include "settings.hpp"

void ProvisioningWeb::sendIndex(WebServer &server) {
    String html = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>自制装甲板 WiFi 配网</title>
  <style>
    body{font-family:Arial,sans-serif;max-width:480px;margin:40px auto;padding:0 20px}
    input,select,button{box-sizing:border-box;width:100%;padding:12px;margin:8px 0;font-size:16px}
    button{background:#1677ff;color:white;border:0;border-radius:5px}
    .tip{color:#666;font-size:14px}
  </style>
</head>
<body>
  <h2>自制装甲板 WiFi 配网</h2>
  <p class="tip">填写路由器 Wi-Fi 信息，保存后设备立即连接。</p>
  <form method="post" action="/save">
    <label>Wi-Fi 名称</label>
    <input name="ssid" autocomplete="off" value="%SSID_VALUE%">
    <label>Wi-Fi 密码</label>
    <input name="password" type="text" autocomplete="off" value="%PASSWORD_VALUE%">
    <label>机器人 ID</label>
    <select name="robot_id" required>
      %ROBOT_ID_OPTIONS%
    </select>
    <button type="submit">保存并连接</button>
  </form>
</body>
</html>
)HTML";

    struct RobotOption {
        uint16_t id;
        const char *name;
    };

    static const RobotOption robot_options[] = {
        {1, "红方-英雄"}, {2, "红方-工程"}, {3, "红方-步兵"},
        {4, "红方-步兵"}, {5, "红方-步兵"}, {6, "红方-空中"},
        {7, "红方-哨兵"}, {8, "红方-飞镖"}, {9, "红方-雷达"},
        {10, "红方-基地"}, {11, "红方-前哨站"},
        {101, "蓝方-英雄"}, {102, "蓝方-工程"}, {103, "蓝方-步兵"},
        {104, "蓝方-步兵"}, {105, "蓝方-步兵"}, {106, "蓝方-空中"},
        {107, "蓝方-哨兵"}, {108, "蓝方-飞镖"}, {109, "蓝方-雷达"},
        {110, "蓝方-基地"}, {111, "蓝方-前哨站"},
    };

    const uint32_t current_robot_id = Settings::getRobotId(3);
    String robot_id_options;
    for (const RobotOption &option : robot_options) {
        robot_id_options += "<option value=\"";
        robot_id_options += String(option.id);
        robot_id_options += "\"";
        if (current_robot_id == option.id) {
            robot_id_options += " selected";
        }
        robot_id_options += ">";
        robot_id_options += String(option.id);
        robot_id_options += "-";
        robot_id_options += option.name;
        robot_id_options += "</option>";
    }

    html.replace("%ROBOT_ID_OPTIONS%", robot_id_options);
    const String form_ssid = Settings::hasWiFiCredentials()
        ? Settings::getWiFiSsid()
        : Config::DEFAULT_WIFI_SSID;
    const String form_password = Settings::hasWiFiCredentials()
        ? Settings::getWiFiPassword()
        : Config::DEFAULT_WIFI_PASSWORD;
    html.replace("%SSID_VALUE%", form_ssid);
    html.replace("%PASSWORD_VALUE%", form_password);
    server.send(200, "text/html; charset=utf-8", html);
}
