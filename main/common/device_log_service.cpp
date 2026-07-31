#include "device_log_service.h"

#include <algorithm>

#include "esp_timer.h"

namespace photopainter::product {

void DeviceLogService::Add(DeviceLogSeverity severity, const char* component, const char* code, const char* message) {
    DeviceLogEntry entry;
    entry.uptime_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
    entry.severity = severity;
    entry.component = component == nullptr ? "system" : component;
    entry.code = code == nullptr ? "event" : code;
    entry.message = message == nullptr ? "" : message;
    taskENTER_CRITICAL(&lock_);
    entries_[next_] = std::move(entry);
    next_ = (next_ + 1U) % kCapacity;
    count_ = std::min(count_ + 1U, kCapacity);
    taskEXIT_CRITICAL(&lock_);
}

std::vector<DeviceLogEntry> DeviceLogService::Recent(std::size_t limit) const {
    std::vector<DeviceLogEntry> result;
    taskENTER_CRITICAL(&lock_);
    const std::size_t take = std::min(limit, count_);
    result.reserve(take);
    for (std::size_t offset = 0; offset < take; ++offset) {
        const std::size_t index = (next_ + kCapacity - 1U - offset) % kCapacity;
        result.push_back(entries_[index]);
    }
    taskEXIT_CRITICAL(&lock_);
    return result;
}

DeviceLogService& GetDeviceLogService() {
    static DeviceLogService service;
    return service;
}

const char* DeviceLogSeverityName(DeviceLogSeverity severity) {
    switch (severity) {
        case DeviceLogSeverity::kInfo: return "info";
        case DeviceLogSeverity::kWarning: return "warning";
        case DeviceLogSeverity::kError: return "error";
    }
    return "info";
}

}  // namespace photopainter::product
