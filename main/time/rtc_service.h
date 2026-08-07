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
    int ReadInterruptLevel() const;

private:
    esp_err_t ReadSnapshot(RtcSnapshot* snapshot) const;
};

RtcService& GetRtcService();
esp_err_t InitializeRtcService();

}  // namespace photopainter::product
