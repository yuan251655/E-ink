#pragma once

#include <cstdint>

#include "esp_err.h"

namespace photopainter::product {

struct RtcSnapshot {
    bool initialized = false;
    bool present = false;
    bool valid = false;
    std::uint16_t year = 0;
    std::uint8_t month = 0;
    std::uint8_t day = 0;
    std::uint8_t weekday = 0;
    std::uint8_t hour = 0;
    std::uint8_t minute = 0;
    std::uint8_t second = 0;
};

class RtcService {
public:
    esp_err_t Initialize();
    RtcSnapshot GetSnapshot() const;
    esp_err_t SetTime(const RtcSnapshot& snapshot);
    esp_err_t ArmInterruptDiagnostic(std::uint8_t seconds);
    // PCF85063 supports an 8-bit countdown at 1 Hz or 1/60 Hz.  This covers
    // the product playback intervals from 1 second through 255 minutes.
    esp_err_t ArmWakeAfterSeconds(std::uint32_t seconds);
    esp_err_t DisarmWakeTimer();
    // Returns UTC seconds only when the hardware RTC currently has a valid
    // calendar value. Product schedules use this, never boot-local uptime.
    bool GetUnixTimeSeconds(std::uint64_t* output) const;
    int ReadInterruptLevel() const;

private:
    esp_err_t ReadSnapshot(RtcSnapshot* snapshot) const;
};

RtcService& GetRtcService();
esp_err_t InitializeRtcService();

}  // namespace photopainter::product
