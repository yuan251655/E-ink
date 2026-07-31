#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"

namespace photopainter::product {

enum class DeviceLogSeverity : std::uint8_t { kInfo, kWarning, kError };

struct DeviceLogEntry {
    std::uint64_t uptime_ms = 0;
    DeviceLogSeverity severity = DeviceLogSeverity::kInfo;
    std::string component;
    std::string code;
    std::string message;
};

/** Bounded RAM buffer; recording an event never performs TF I/O. */
class DeviceLogService {
public:
    void Add(DeviceLogSeverity severity, const char* component, const char* code, const char* message);
    std::vector<DeviceLogEntry> Recent(std::size_t limit) const;

private:
    static constexpr std::size_t kCapacity = 200;
    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    DeviceLogEntry entries_[kCapacity];
    std::size_t count_ = 0;
    std::size_t next_ = 0;
};

DeviceLogService& GetDeviceLogService();
const char* DeviceLogSeverityName(DeviceLogSeverity severity);

}  // namespace photopainter::product
