#pragma once

#include <cstdint>

#include "esp_err.h"

namespace photopainter::product {

// BOOT long-press requests ESP deep sleep. KEY wakes it. GPIO5 is SYS_OUT
// and must never be used as a button.
esp_err_t InitializeButtonSleepService();

// Test-only: runs ten 10-second RTC deep-sleep cycles. KEY remains a fallback
// wake source and cancels the remaining cycles.
esp_err_t StartRtcWakeVerification();
void ResumeRtcWakeVerification();

struct RtcWakeVerificationSnapshot {
    bool active = false;
    std::uint8_t completed = 0;
    std::uint8_t remaining = 0;
};

RtcWakeVerificationSnapshot GetRtcWakeVerificationSnapshot();

}  // namespace photopainter::product
