#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace photopainter::product {

struct NetworkSnapshot {
    bool ap_enabled = false;
    bool sta_configured = false;
    bool sta_connected = false;
    std::string sta_ssid;
    std::string sta_ip;
    std::string last_error_code;
    std::uint64_t revision = 0;
};

esp_err_t InitializeProductNetwork();
esp_err_t ConfigureProductSta(const std::string& ssid, const std::string& password);
bool WaitForProductStaConnection(std::uint32_t timeout_ms);
NetworkSnapshot GetProductNetworkSnapshot();

}  // namespace photopainter::product
