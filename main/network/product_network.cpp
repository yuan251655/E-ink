#include "product_network.h"

#include <algorithm>
#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

namespace photopainter::product {
namespace {
constexpr char kApSsid[] = "esp_network";
constexpr char kApPassword[] = "1234567890";
NetworkSnapshot snapshot;
bool initialized = false;
EventGroupHandle_t connection_events = nullptr;
constexpr EventBits_t kStaConnected = BIT0;
constexpr std::uint8_t kMaxStaReconnectAttempts = 5;
constexpr std::uint32_t kReconnectBaseDelayMs = 1000;
constexpr std::uint32_t kReconnectMaxDelayMs = 16000;
TimerHandle_t reconnect_timer = nullptr;
std::uint8_t reconnect_attempts = 0;
bool configuration_in_progress = false;

void ScheduleStaReconnect();

void OnReconnectTimer(TimerHandle_t) {
    if (!initialized || !snapshot.sta_configured || snapshot.sta_connected) return;
    const esp_err_t error = esp_wifi_connect();
    if (error != ESP_OK) {
        snapshot.last_error_code = "sta_reconnect_failed";
        ++snapshot.revision;
        ScheduleStaReconnect();
    }
}

void ScheduleStaReconnect() {
    if (configuration_in_progress || reconnect_timer == nullptr || !snapshot.sta_configured || snapshot.sta_connected) return;
    if (reconnect_attempts >= kMaxStaReconnectAttempts) {
        snapshot.last_error_code = "sta_reconnect_exhausted";
        ++snapshot.revision;
        return;
    }
    const std::uint32_t delay = std::min(kReconnectBaseDelayMs << reconnect_attempts, kReconnectMaxDelayMs);
    ++reconnect_attempts;
    snapshot.last_error_code = "sta_reconnecting";
    ++snapshot.revision;
    (void)xTimerChangePeriod(reconnect_timer, pdMS_TO_TICKS(delay), 0);
}

void OnWifiEvent(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        snapshot.sta_connected = false;
        snapshot.sta_ip.clear();
        xEventGroupClearBits(connection_events, kStaConnected);
        ++snapshot.revision;
        ScheduleStaReconnect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        esp_netif_ip_info_t info{};
        auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            char ip[16]{};
            esp_ip4addr_ntoa(&info.ip, ip, sizeof(ip));
            snapshot.sta_ip = ip;
        }
        snapshot.sta_connected = true;
        snapshot.last_error_code.clear();
        reconnect_attempts = 0;
        if (reconnect_timer != nullptr) (void)xTimerStop(reconnect_timer, 0);
        ++snapshot.revision;
        xEventGroupSetBits(connection_events, kStaConnected);
    }
}
}

esp_err_t InitializeProductNetwork() {
    if (initialized) return ESP_OK;
    ESP_RETURN_ON_ERROR(esp_netif_init(), "product_network", "netif init");
    connection_events = xEventGroupCreate();
    if (connection_events == nullptr) return ESP_ERR_NO_MEM;
    reconnect_timer = xTimerCreate("sta_reconnect", pdMS_TO_TICKS(kReconnectBaseDelayMs), pdFALSE, nullptr, &OnReconnectTimer);
    if (reconnect_timer == nullptr) return ESP_ERR_NO_MEM;
    if (esp_netif_create_default_wifi_ap() == nullptr || esp_netif_create_default_wifi_sta() == nullptr) return ESP_FAIL;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), "product_network", "wifi init");
    // Keep STA credentials in the ESP-IDF Wi-Fi NVS namespace.  The password
    // never enters NetworkSnapshot or any HTTP response.
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), "product_network", "set flash storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &OnWifiEvent, nullptr), "product_network", "wifi event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &OnWifiEvent, nullptr), "product_network", "ip event");
    wifi_config_t ap{};
    std::strncpy(reinterpret_cast<char*>(ap.ap.ssid), kApSsid, sizeof(ap.ap.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(ap.ap.password), kApPassword, sizeof(ap.ap.password) - 1);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), "product_network", "set APSTA");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), "product_network", "set AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), "product_network", "start wifi");
    // The product API is controlled from phones/PCs over LAN.  Modem sleep can
    // delay or drop inbound TCP requests while the station is otherwise still
    // associated, so keep the AP+STA control path awake in local-album mode.
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), "product_network", "disable wifi power save");
    snapshot.ap_enabled = true;
    wifi_config_t persisted_sta{};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &persisted_sta), "product_network", "read STA config");
    if (persisted_sta.sta.ssid[0] != '\0') {
        snapshot.sta_configured = true;
        snapshot.sta_ssid = reinterpret_cast<const char*>(persisted_sta.sta.ssid);
        // AP remains available even if this background connection attempt
        // fails, so users always retain a recovery/configuration path.
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), "product_network", "connect saved STA");
    }
    ++snapshot.revision;
    initialized = true;
    return ESP_OK;
}

esp_err_t ConfigureProductSta(const std::string& ssid, const std::string& password) {
    if (!initialized || ssid.empty() || ssid.size() > 32 || password.size() > 64) return ESP_ERR_INVALID_ARG;
    wifi_config_t sta{};
    std::strncpy(reinterpret_cast<char*>(sta.sta.ssid), ssid.c_str(), sizeof(sta.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(sta.sta.password), password.c_str(), sizeof(sta.sta.password) - 1);
    // This writes the new credentials only after the user submits them from
    // the local setup page; status endpoints intentionally expose no password.
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), "product_network", "set flash storage");
    configuration_in_progress = true;
    reconnect_attempts = 0;
    if (reconnect_timer != nullptr) (void)xTimerStop(reconnect_timer, 0);
    xEventGroupClearBits(connection_events, kStaConnected);
    const esp_err_t disconnect_error = esp_wifi_disconnect();
    if (disconnect_error != ESP_OK && disconnect_error != ESP_ERR_WIFI_NOT_CONNECT) {
        configuration_in_progress = false;
        return disconnect_error;
    }
    const esp_err_t config_error = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (config_error != ESP_OK) {
        configuration_in_progress = false;
        return config_error;
    }
    snapshot.sta_configured = true;
    snapshot.sta_connected = false;
    snapshot.sta_ssid = ssid;
    snapshot.sta_ip.clear();
    snapshot.last_error_code.clear();
    ++snapshot.revision;
    configuration_in_progress = false;
    const esp_err_t connect_error = esp_wifi_connect();
    if (connect_error != ESP_OK) ScheduleStaReconnect();
    return connect_error;
}

bool WaitForProductStaConnection(std::uint32_t timeout_ms) {
    return (xEventGroupWaitBits(connection_events, kStaConnected, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)) & kStaConnected) != 0;
}

NetworkSnapshot GetProductNetworkSnapshot() { return snapshot; }

}  // namespace photopainter::product
