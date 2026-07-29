#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

namespace photopainter::product {

struct NetworkSnapshot {
    bool ap_enabled = false;
    std::string ap_ssid;
    std::string ap_ip;
    std::uint8_t ap_channel = 0;
    std::uint8_t ap_connected_clients = 0;
    bool sta_configured = false;
    bool sta_connected = false;
    std::string sta_ssid;
    std::string sta_ip;
    std::string sta_gateway;
    std::int8_t sta_rssi_dbm = 0;
    std::string last_error_code;
    std::uint64_t revision = 0;
};

struct ScannedWifiNetwork {
    std::string ssid;
    std::int8_t rssi_dbm = 0;
    std::uint8_t channel = 0;
    std::string security;
};

esp_err_t InitializeProductNetwork();
esp_err_t ConfigureProductSta(const std::string& ssid, const std::string& password);
bool WaitForProductStaConnection(std::uint32_t timeout_ms);
esp_err_t ForgetProductSta();
esp_err_t ScanProductWifi24Ghz(std::vector<ScannedWifiNetwork>* networks);
esp_err_t ConfigureProductAp(const std::string& ssid, const std::string& password);
esp_err_t RestoreDefaultProductAp();
NetworkSnapshot GetProductNetworkSnapshot();

}  // namespace photopainter::product
