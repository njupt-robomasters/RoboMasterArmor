#pragma once

#include <Arduino.h>

class Provisioning {
  public:
    static void beginNormal();
    static void enterConfigMode();
    static void abortToOffline();
    static void requestOffline();
    static void onLoop();
    static bool isConfigMode();
    static bool isBusy();
    static bool isSessionActive();
    static bool isLocalHost();
    static void surrenderHostToPeer();

  private:
    static bool config_mode;
    static bool routes_registered;
    static bool wifi_session_active;

    static void registerRoutes();
    static void startStaSession(const String &ssid, const String &password);
    static void handleIndex();
    static void handleSave();
    static void handleNotFound();
};
