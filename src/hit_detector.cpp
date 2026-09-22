#include "hit_detector.hpp"

#include "config.hpp"

void HitDetector::begin() {
    armed_ = true;
    last_hit_ms_ = 0;
}

bool HitDetector::accept(uint32_t sample, uint32_t now_ms) {
    if (sample < Config::HIT_REARM_THRESHOLD) {
        armed_ = true;
    }

    if (!armed_ || sample <= Config::HIT_THRESHOLD) {
        return false;
    }

    // 严格大于500ms，保证两次有效击打的间隔不会等于冷却边界。
    if (last_hit_ms_ != 0 && now_ms - last_hit_ms_ <= Config::HIT_COOLDOWN_MS) {
        return false;
    }

    armed_ = false;
    last_hit_ms_ = now_ms;
    return true;
}

bool HitDetector::armed() const {
    return armed_;
}
