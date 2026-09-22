#pragma once

#include <Arduino.h>

class WifiManager {
  public:
    static void beginNormal();
    static void stop();
    static bool isConnected();
    static bool prepareSta();
    static bool prepareAp();
};
