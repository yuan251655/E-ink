#include "product_network.h"

#include <algorithm>
#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "nvs.h"

namespace photopainter::product {
namespace {
constexpr char kDefaultApSsid[] = "esp_network";
constexpr char kDefaultApPassword[] = "1234567890";
constexpr char kNetworkNvsNamespace[] = "product_net";
constexpr char kApSsidKey[] = "ap_ssid";
constexpr char kApPasswordKey[] = "ap_pass";
constexpr char kApIp[] = "192.168.4.1";
constexpr std::uint8_t kApChannel = 1;
constexpr std::uint8_t kMaxStaReconnectAttempts = 5;
constexpr std::uint32_t kReconnectBaseDelayMs = 1000;
constexpr std::uint32_t kReconnectMaxDelayMs = 16000;

NetworkSnapshot snapshot;
bool initialized = false;
bool configuration_in_progress = false;
bool sta_test_pending = false;
wifi_config_t previous_sta_config{};
wifi_config_t pending_sta_config{};
EventGroupHandle_t connection_events = nullptr;
constexpr EventBits_t kStaConnected = BIT0;
TimerHandle_t reconnect_timer = nullptr;
std::uint8_t reconnect_attempts = 0;

bool IsValidSsid(const std::string& ssid) { return !ssid.empty() && ssid.size() <= 32; }
bool IsValidPassword(const std::string& password) { return password.size() >= 8 && password.size() <= 63; }

void CopyWifiString(std::uint8_t* destination, std::size_t destination_size, const std::string& value) {
    std::memset(destination, 0, destination_size);
    std::memcpy(destination, value.data(), std::min(destination_size - 1, value.size()));
}

wifi_config_t BuildApConfig(const std::string& ssid, const std::string& password) {
    wifi_config_t ap{};
    CopyWifiString(ap.ap.ssid, sizeof(ap.ap.ssid), ssid);
    CopyWifiString(ap.ap.password, sizeof(ap.ap.password), password);
    ap.ap.ssid_len = ssid.size();
    ap.ap.channel = kApChannel;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    return ap;
}

esp_err_t LoadApConfig(std::string* ssid, std::string* password) {
    *ssid = kDefaultApSsid;
    *password = kDefaultApPassword;
    nvs_handle_t handle;
    const esp_err_t open_result = nvs_open(kNetworkNvsNamespace, NVS_READONLY, &handle);
    if (open_result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (open_result != ESP_OK) return open_result;
    char stored_ssid[33]{};
    char stored_password[64]{};
    std::size_t ssid_size = sizeof(stored_ssid);
    std::size_t password_size = sizeof(stored_password);
    const esp_err_t ssid_result = nvs_get_str(handle, kApSsidKey, stored_ssid, &ssid_size);
    const esp_err_t password_result = nvs_get_str(handle, kApPasswordKey, stored_password, &password_size);
    nvs_close(handle);
    if (ssid_result == ESP_OK && password_result == ESP_OK && IsValidSsid(stored_ssid) && IsValidPassword(stored_password)) {
        *ssid = stored_ssid;
        *password = stored_password;
    }
    return ESP_OK;
}

esp_err_t StoreApConfig(const std::string& ssid, const std::string& password) {
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(kNetworkNvsNamespace, NVS_READWRITE, &handle), "product_network", "open network nvs");
    const esp_err_t ssid_result = nvs_set_str(handle, kApSsidKey, ssid.c_str());
    const esp_err_t password_result = ssid_result == ESP_OK ? nvs_set_str(handle, kApPasswordKey, password.c_str()) : ssid_result;
    const esp_err_t commit_result = password_result == ESP_OK ? nvs_commit(handle) : password_result;
    nvs_close(handle);
    return commit_result;
}

void RefreshApRuntimeState() {
    wifi_sta_list_t list{};
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) snapshot.ap_connected_clients = list.num;
    snapshot.ap_enabled = true;
    snapshot.ap_ip = kApIp;
    snapshot.ap_channel = kApChannel;
}

void RefreshStaRuntimeState() {
    auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) return;
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        char ip[16]{}, gateway[16]{};
        esp_ip4addr_ntoa(&info.ip, ip, sizeof(ip));
        esp_ip4addr_ntoa(&info.gw, gateway, sizeof(gateway));
        snapshot.sta_ip = ip;
        snapshot.sta_gateway = gateway;
    }
    wifi_ap_record_t record{};
    if (esp_wifi_sta_get_ap_info(&record) == ESP_OK) snapshot.sta_rssi_dbm = record.rssi;
}

void ScheduleStaReconnect();

void OnReconnectTimer(TimerHandle_t) {
    if (!initialized || sta_test_pending || !snapshot.sta_configured || snapshot.sta_connected) return;
    if (esp_wifi_connect() != ESP_OK) {
        snapshot.last_error_code = "sta_reconnect_failed";
        ++snapshot.revision;
        ScheduleStaReconnect();
    }
}

void ScheduleStaReconnect() {
    if (configuration_in_progress || sta_test_pending || reconnect_timer == nullptr || !snapshot.sta_configured || snapshot.sta_connected) return;
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
        snapshot.sta_gateway.clear();
        xEventGroupClearBits(connection_events, kStaConnected);
        ++snapshot.revision;
        ScheduleStaReconnect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        RefreshStaRuntimeState();
        snapshot.sta_connected = true;
        snapshot.last_error_code.clear();
        reconnect_attempts = 0;
        if (reconnect_timer != nullptr) (void)xTimerStop(reconnect_timer, 0);
        ++snapshot.revision;
        xEventGroupSetBits(connection_events, kStaConnected);
    }
}

esp_err_t RestorePreviousStaAfterFailedTest() {
    (void)esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), "product_network", "restore RAM storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &previous_sta_config), "product_network", "restore STA config");
    snapshot.sta_configured = previous_sta_config.sta.ssid[0] != '\0';
    snapshot.sta_ssid = snapshot.sta_configured ? reinterpret_cast<const char*>(previous_sta_config.sta.ssid) : "";
    snapshot.sta_connected = false;
    snapshot.sta_ip.clear();
    snapshot.sta_gateway.clear();
    snapshot.last_error_code = "sta_connect_failed";
    sta_test_pending = false;
    ++snapshot.revision;
    if (snapshot.sta_configured) (void)esp_wifi_connect();
    return ESP_OK;
}
}  // namespace

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
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), "product_network", "set flash storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &OnWifiEvent, nullptr), "product_network", "wifi event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &OnWifiEvent, nullptr), "product_network", "ip event");
    std::string ap_ssid, ap_password;
    ESP_RETURN_ON_ERROR(LoadApConfig(&ap_ssid, &ap_password), "product_network", "load AP config");
    wifi_config_t ap = BuildApConfig(ap_ssid, ap_password);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), "product_network", "set APSTA");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), "product_network", "set AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), "product_network", "start wifi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), "product_network", "disable wifi power save");
    snapshot.ap_ssid = ap_ssid;
    RefreshApRuntimeState();
    wifi_config_t persisted_sta{};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &persisted_sta), "product_network", "read STA config");
    // Mark the runtime ready before the first connect attempt. A fast initial
    // disconnect can otherwise arrive while `initialized` is still false;
    // its reconnect timer is then skipped and STA remains offline until a
    // user manually configures Wi-Fi again.
    initialized = true;
    if (persisted_sta.sta.ssid[0] != '\0') {
        snapshot.sta_configured = true;
        snapshot.sta_ssid = reinterpret_cast<const char*>(persisted_sta.sta.ssid);
        (void)esp_wifi_connect();
    }
    ++snapshot.revision;
    return ESP_OK;
}

esp_err_t ConfigureProductSta(const std::string& ssid, const std::string& password) {
    if (!initialized || !IsValidSsid(ssid) || !IsValidPassword(password) || sta_test_pending) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &previous_sta_config), "product_network", "read previous STA");
    pending_sta_config = {};
    CopyWifiString(pending_sta_config.sta.ssid, sizeof(pending_sta_config.sta.ssid), ssid);
    CopyWifiString(pending_sta_config.sta.password, sizeof(pending_sta_config.sta.password), password);
    configuration_in_progress = true;
    reconnect_attempts = 0;
    if (reconnect_timer != nullptr) (void)xTimerStop(reconnect_timer, 0);
    xEventGroupClearBits(connection_events, kStaConnected);
    (void)esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), "product_network", "set RAM storage");
    const esp_err_t config_result = esp_wifi_set_config(WIFI_IF_STA, &pending_sta_config);
    configuration_in_progress = false;
    if (config_result != ESP_OK) return config_result;
    sta_test_pending = true;
    snapshot.sta_connected = false;
    snapshot.sta_ssid = ssid;
    snapshot.sta_ip.clear();
    snapshot.sta_gateway.clear();
    snapshot.last_error_code = "sta_testing";
    ++snapshot.revision;
    return esp_wifi_connect();
}

bool WaitForProductStaConnection(std::uint32_t timeout_ms) {
    const bool connected = (xEventGroupWaitBits(connection_events, kStaConnected, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)) & kStaConnected) != 0;
    if (!sta_test_pending) return connected;
    if (!connected) {
        (void)RestorePreviousStaAfterFailedTest();
        return false;
    }
    const esp_err_t storage_result = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    const esp_err_t persist_result = storage_result == ESP_OK ? esp_wifi_set_config(WIFI_IF_STA, &pending_sta_config) : storage_result;
    if (persist_result != ESP_OK) {
        (void)RestorePreviousStaAfterFailedTest();
        return false;
    }
    sta_test_pending = false;
    snapshot.sta_configured = true;
    snapshot.last_error_code.clear();
    ++snapshot.revision;
    return true;
}

esp_err_t ForgetProductSta() {
    if (!initialized || sta_test_pending) return ESP_ERR_INVALID_STATE;
    configuration_in_progress = true;
    if (reconnect_timer != nullptr) (void)xTimerStop(reconnect_timer, 0);
    (void)esp_wifi_disconnect();
    wifi_config_t empty{};
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), "product_network", "set flash storage");
    const esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &empty);
    configuration_in_progress = false;
    if (result != ESP_OK) return result;
    snapshot.sta_configured = false;
    snapshot.sta_connected = false;
    snapshot.sta_ssid.clear();
    snapshot.sta_ip.clear();
    snapshot.sta_gateway.clear();
    snapshot.last_error_code.clear();
    ++snapshot.revision;
    return ESP_OK;
}

esp_err_t ScanProductWifi24Ghz(std::vector<ScannedWifiNetwork>* networks) {
    if (!initialized || networks == nullptr || configuration_in_progress || sta_test_pending) return ESP_ERR_INVALID_STATE;
    networks->clear();
    wifi_scan_config_t config{};
    config.show_hidden = false;
    const esp_err_t start = esp_wifi_scan_start(&config, true);
    if (start != ESP_OK) return start;
    std::uint16_t count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&count), "product_network", "scan result count");
    count = std::min<std::uint16_t>(count, 20);
    std::vector<wifi_ap_record_t> records(count);
    if (count > 0) ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&count, records.data()), "product_network", "scan results");
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto& record = records[i];
        if (record.ssid[0] == '\0') continue;
        const char* security = record.authmode == WIFI_AUTH_OPEN ? "open" : (record.authmode == WIFI_AUTH_WPA3_PSK ? "wpa3" : "wpa2");
        networks->push_back({reinterpret_cast<const char*>(record.ssid), record.rssi, record.primary, security});
    }
    std::sort(networks->begin(), networks->end(), [](const auto& left, const auto& right) { return left.rssi_dbm > right.rssi_dbm; });
    return ESP_OK;
}

esp_err_t ConfigureProductAp(const std::string& ssid, const std::string& password) {
    if (!initialized || !IsValidSsid(ssid) || !IsValidPassword(password)) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(StoreApConfig(ssid, password), "product_network", "save AP config");
    wifi_config_t ap = BuildApConfig(ssid, password);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), "product_network", "apply AP config");
    snapshot.ap_ssid = ssid;
    RefreshApRuntimeState();
    ++snapshot.revision;
    return ESP_OK;
}

esp_err_t RestoreDefaultProductAp() { return ConfigureProductAp(kDefaultApSsid, kDefaultApPassword); }

NetworkSnapshot GetProductNetworkSnapshot() {
    if (initialized) {
        RefreshApRuntimeState();
        if (snapshot.sta_connected) RefreshStaRuntimeState();
    }
    return snapshot;
}

}  // namespace photopainter::product
