#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace photopainter::product {

// BOOT long-press requests ESP deep sleep. KEY wakes it. GPIO5 is SYS_OUT
// and must never be used as a button.
esp_err_t InitializeButtonSleepService();

// Persisted product configuration. This only records the user's choice; the
// automatic scheduler is enabled separately after the playback wake path has
// passed hardware validation.
struct AutomaticSleepConfig {
    bool enabled = false;
    std::uint8_t idle_timeout_minutes = 15;
    bool wake_for_playback = true;
};

AutomaticSleepConfig GetAutomaticSleepConfig();
esp_err_t UpdateAutomaticSleepConfig(const AutomaticSleepConfig& config);
bool IsAllowedAutomaticSleepTimeout(std::uint8_t minutes);

// Global low-power policy. Playback modules expose their own next deadline;
// this service only observes it and never mutates playback mode, order, or
// interval. App/KEY activity resets the idle deadline without changing the
// active feature's playback deadline.
esp_err_t InitializeAutomaticSleepService();
void RecordAutomaticSleepActivity();

struct AutomaticSleepStatus {
    bool rtc_valid = false;
    bool enabled = false;
    bool busy = false;
    std::uint64_t last_activity_epoch_seconds = 0;
    std::uint64_t idle_sleep_at_epoch_seconds = 0;
    std::uint64_t next_play_at_epoch_seconds = 0;
    std::string state;
};

AutomaticSleepStatus GetAutomaticSleepStatus();

// Used only by local-album playback after a completed physical refresh. It
// arms the RTC and sleeps with KEY as a fallback.  A non-RTC wake cancels the
// pending turn rather than treating the wake key as a next-image command.
esp_err_t RequestScheduledLocalPlaybackSleep(std::uint32_t delay_seconds);
bool ConsumeScheduledLocalPlaybackWake();

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
