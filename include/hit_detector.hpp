#pragma once

#include <Arduino.h>

class HitDetector {
  public:
    void begin();
    bool accept(uint32_t sample, uint32_t now_ms);
    bool armed() const;

  private:
    bool armed_ = true;
    uint32_t last_hit_ms_ = 0;
};
