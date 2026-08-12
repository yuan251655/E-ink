#include "dashboard_weather_service.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <utility>

#include "ArduinoJson.h"
#include "dashboard_data_service.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "product_network.h"
#include "storage_runtime.h"
#include "storage_service.h"

namespace photopainter::product {
namespace {
constexpr char kTag[] = "dashboard_weather";
constexpr char kCacheFile[] = "dashboard_weather.json";
constexpr std::size_t kMaximumResponseBytes = 12 * 1024;
constexpr std::uint64_t kUpdateIntervalUs = 60ULL * 60ULL * 1000000ULL;
constexpr std::uint64_t kRetryIntervalUs = 5ULL * 60ULL * 1000000ULL;

struct HttpResponseBuffer {
    std::string body;
    bool overflow = false;
};

esp_err_t OnHttpEvent(esp_http_client_event_t* event) {
    auto* response = static_cast<HttpResponseBuffer*>(event->user_data);
    if (event->event_id != HTTP_EVENT_ON_DATA || response == nullptr || event->data_len <= 0) return ESP_OK;
    if (response->body.size() + static_cast<std::size_t>(event->data_len) > kMaximumResponseBytes) {
        response->overflow = true;
        return ESP_FAIL;
    }
    response->body.append(static_cast<const char*>(event->data), static_cast<std::size_t>(event->data_len));
    return ESP_OK;
}

std::string UrlEncode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.') {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[character >> 4U]);
            encoded.push_back(hex[character & 0x0FU]);
        }
    }
    return encoded;
}

bool SameCoordinates(const DashboardWeatherSnapshot& weather, const DashboardDataSnapshot& dashboard) {
    return weather.has_coordinates == dashboard.has_coordinates &&
        (!weather.has_coordinates || (std::fabs(weather.latitude - dashboard.latitude) < 0.000001 &&
                                      std::fabs(weather.longitude - dashboard.longitude) < 0.000001));
}

const char* WeatherErrorCode(esp_err_t error) {
    if (error == ESP_ERR_INVALID_RESPONSE) return "invalid_payload";
    if (error == ESP_ERR_INVALID_SIZE) return "response_too_large";
    if (error >= ESP_ERR_HTTP_BASE) return "http_status";
    return "request_failed";
}
}  // namespace

DashboardWeatherService& GetDashboardWeatherService() {
    static DashboardWeatherService service;
    return service;
}

esp_err_t InitializeDashboardWeatherService() {
    return GetDashboardWeatherService().Initialize(&GetStorageService());
}

esp_err_t DashboardWeatherService::Initialize(StorageService* storage) {
    if (storage == nullptr || !storage->IsReady()) return ESP_ERR_INVALID_STATE;
    if (task_ != nullptr) return ESP_OK;
    mutex_ = xSemaphoreCreateMutex();
    fetch_mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr || fetch_mutex_ == nullptr) return ESP_ERR_NO_MEM;
    storage_ = storage;
    const esp_err_t load_result = LoadCache();
    if (load_result != ESP_OK && load_result != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "Weather cache unavailable: %s", esp_err_to_name(load_result));
    }
    if (xTaskCreate(&TaskEntry, "dashboard_weather", 8192, this, 3, &task_) != pdPASS) {
        task_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

DashboardWeatherSnapshot DashboardWeatherService::GetSnapshot() const {
    if (mutex_ == nullptr) return snapshot_;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    DashboardWeatherSnapshot copy = snapshot_;
    xSemaphoreGive(mutex_);
    return copy;
}

esp_err_t DashboardWeatherService::RefreshNow() {
    if (fetch_mutex_ == nullptr || xSemaphoreTake(fetch_mutex_, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    const DashboardDataSnapshot dashboard = GetDashboardDataService().GetSnapshot();
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!dashboard.has_coordinates) result = ESP_ERR_INVALID_ARG;
    else if (!GetProductNetworkSnapshot().sta_connected) result = ESP_ERR_INVALID_STATE;
    else {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        snapshot_.refreshing = true;
        snapshot_.state = "updating";
        xSemaphoreGive(mutex_);
        DashboardWeatherSnapshot fetched;
        result = Fetch(dashboard.city_name, dashboard.latitude, dashboard.longitude, dashboard.timezone, &fetched);
        if (result == ESP_OK) {
            const esp_err_t persist_result = PersistCache(fetched);
            if (persist_result != ESP_OK) fetched.last_error_code = "cache_persist_failed";
            xSemaphoreTake(mutex_, portMAX_DELAY);
            fetched.revision = snapshot_.revision + 1;
            snapshot_ = std::move(fetched);
            xSemaphoreGive(mutex_);
        } else {
            xSemaphoreTake(mutex_, portMAX_DELAY);
            snapshot_.refreshing = false;
            snapshot_.state = snapshot_.last_success_at == 0 ? "error" : "stale";
            snapshot_.last_error_code = WeatherErrorCode(result);
            ++snapshot_.revision;
            xSemaphoreGive(mutex_);
        }
    }
    xSemaphoreGive(fetch_mutex_);
    return result;
}

void DashboardWeatherService::TaskEntry(void* context) {
    static_cast<DashboardWeatherService*>(context)->TaskLoop();
}

void DashboardWeatherService::TaskLoop() {
    std::uint64_t observed_dashboard_revision = 0;
    std::uint64_t next_attempt_us = 0;
    while (true) {
        const DashboardDataSnapshot dashboard = GetDashboardDataService().GetSnapshot();
        const std::uint64_t now_us = static_cast<std::uint64_t>(esp_timer_get_time());
        const bool uses_weather = dashboard.layout_id == "weather_date" || dashboard.layout_id == "weather_memo_todo";
        if (dashboard.revision != observed_dashboard_revision) {
            observed_dashboard_revision = dashboard.revision;
            next_attempt_us = 0;
        }
        if (!dashboard.has_coordinates) {
            xSemaphoreTake(mutex_, portMAX_DELAY);
            snapshot_.state = snapshot_.last_success_at == 0 ? "location_unconfigured" : "stale";
            snapshot_.last_error_code = "location_unconfigured";
            snapshot_.refreshing = false;
            xSemaphoreGive(mutex_);
        } else if (uses_weather && now_us >= next_attempt_us) {
            if (!GetProductNetworkSnapshot().sta_connected) {
                xSemaphoreTake(mutex_, portMAX_DELAY);
                snapshot_.state = snapshot_.last_success_at == 0 ? "waiting_for_sta" : "stale";
                snapshot_.last_error_code = "sta_not_connected";
                xSemaphoreGive(mutex_);
                next_attempt_us = now_us + 30ULL * 1000000ULL;
            } else {
                xSemaphoreTake(mutex_, portMAX_DELAY);
                snapshot_.refreshing = true;
                snapshot_.state = "updating";
                xSemaphoreGive(mutex_);
                DashboardWeatherSnapshot fetched;
                const esp_err_t result = Fetch(dashboard.city_name, dashboard.latitude, dashboard.longitude,
                                               dashboard.timezone, &fetched);
                if (result == ESP_OK) {
                    const esp_err_t persist_result = PersistCache(fetched);
                    if (persist_result != ESP_OK) fetched.last_error_code = "cache_persist_failed";
                    xSemaphoreTake(mutex_, portMAX_DELAY);
                    fetched.revision = snapshot_.revision + 1;
                    snapshot_ = std::move(fetched);
                    xSemaphoreGive(mutex_);
                    next_attempt_us = now_us + kUpdateIntervalUs;
                } else {
                    xSemaphoreTake(mutex_, portMAX_DELAY);
                    snapshot_.refreshing = false;
                    snapshot_.state = snapshot_.last_success_at == 0 ? "error" : "stale";
                    snapshot_.last_error_code = WeatherErrorCode(result);
                    ++snapshot_.revision;
                    xSemaphoreGive(mutex_);
                    next_attempt_us = now_us + kRetryIntervalUs;
                    ESP_LOGW(kTag, "Weather update failed: %s", esp_err_to_name(result));
                }
            }
        } else if (!SameCoordinates(GetSnapshot(), dashboard)) {
            xSemaphoreTake(mutex_, portMAX_DELAY);
            snapshot_.state = "stale";
            xSemaphoreGive(mutex_);
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t DashboardWeatherService::Fetch(const std::string& city_name, double latitude, double longitude,
                                         const std::string& timezone, DashboardWeatherSnapshot* output) {
    if (output == nullptr) return ESP_ERR_INVALID_ARG;
    std::string url = "https://api.open-meteo.com/v1/forecast?latitude=" + std::to_string(latitude) +
        "&longitude=" + std::to_string(longitude) +
        "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=" + UrlEncode(timezone) +
        "&forecast_days=3";
    HttpResponseBuffer response;
    response.body.reserve(4096);
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.event_handler = &OnHttpEvent;
    config.user_data = &response;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 15000;
    config.buffer_size = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) return ESP_ERR_NO_MEM;
    const esp_err_t request_result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (request_result != ESP_OK) return request_result;
    if (response.overflow) return ESP_ERR_INVALID_SIZE;
    if (status != 200) return ESP_ERR_HTTP_BASE + status;

    JsonDocument document;
    if (deserializeJson(document, response.body) != DeserializationError::Ok) return ESP_ERR_INVALID_RESPONSE;
    JsonObjectConst daily = document["daily"].as<JsonObjectConst>();
    JsonArrayConst dates = daily["time"].as<JsonArrayConst>();
    JsonArrayConst codes = daily["weather_code"].as<JsonArrayConst>();
    JsonArrayConst maximums = daily["temperature_2m_max"].as<JsonArrayConst>();
    JsonArrayConst minimums = daily["temperature_2m_min"].as<JsonArrayConst>();
    if (dates.size() < 3 || codes.size() < 3 || maximums.size() < 3 || minimums.size() < 3) return ESP_ERR_INVALID_RESPONSE;

    DashboardWeatherSnapshot next;
    next.state = "ready";
    next.city_name = city_name;
    next.has_coordinates = true;
    next.latitude = latitude;
    next.longitude = longitude;
    next.last_success_at = static_cast<std::uint64_t>(std::time(nullptr));
    for (std::size_t index = 0; index < next.days.size(); ++index) {
        const char* date = dates[index] | "";
        const int code = codes[index] | -1;
        const double maximum = maximums[index] | NAN;
        const double minimum = minimums[index] | NAN;
        if (std::strlen(date) != 10 || code < 0 || code > 99 || !std::isfinite(maximum) || !std::isfinite(minimum) ||
            maximum < -100.0 || maximum > 100.0 || minimum < -100.0 || minimum > 100.0) return ESP_ERR_INVALID_RESPONSE;
        next.days[index] = {date, static_cast<std::int16_t>(code),
                            static_cast<std::int16_t>(std::lround(minimum)),
                            static_cast<std::int16_t>(std::lround(maximum))};
    }
    *output = std::move(next);
    return ESP_OK;
}

esp_err_t DashboardWeatherService::LoadCache() {
    std::string raw;
    const esp_err_t read_result = storage_->ReadStateText(kCacheFile, 4096, &raw);
    if (read_result != ESP_OK) return read_result;
    JsonDocument document;
    if (deserializeJson(document, raw) != DeserializationError::Ok) return ESP_ERR_INVALID_RESPONSE;
    DashboardWeatherSnapshot loaded;
    loaded.state = "ready";
    loaded.city_name = document["city_name"] | "";
    loaded.has_coordinates = true;
    loaded.latitude = document["latitude"] | NAN;
    loaded.longitude = document["longitude"] | NAN;
    loaded.last_success_at = document["last_success_at"] | 0ULL;
    JsonArrayConst days = document["days"].as<JsonArrayConst>();
    if (!std::isfinite(loaded.latitude) || !std::isfinite(loaded.longitude) || loaded.last_success_at == 0 || days.size() != 3) return ESP_ERR_INVALID_RESPONSE;
    for (std::size_t index = 0; index < loaded.days.size(); ++index) {
        JsonObjectConst day = days[index].as<JsonObjectConst>();
        loaded.days[index].date = day["date"] | "";
        loaded.days[index].weather_code = day["weather_code"] | -1;
        loaded.days[index].temperature_min_c = day["temperature_min_c"] | 0;
        loaded.days[index].temperature_max_c = day["temperature_max_c"] | 0;
        if (loaded.days[index].date.size() != 10 || loaded.days[index].weather_code < 0 || loaded.days[index].weather_code > 99) return ESP_ERR_INVALID_RESPONSE;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    snapshot_ = std::move(loaded);
    xSemaphoreGive(mutex_);
    return ESP_OK;
}

esp_err_t DashboardWeatherService::PersistCache(const DashboardWeatherSnapshot& snapshot) {
    JsonDocument document;
    document["city_name"] = snapshot.city_name;
    document["latitude"] = snapshot.latitude;
    document["longitude"] = snapshot.longitude;
    document["last_success_at"] = snapshot.last_success_at;
    JsonArray days = document["days"].to<JsonArray>();
    for (const auto& day : snapshot.days) {
        JsonObject item = days.add<JsonObject>();
        item["date"] = day.date;
        item["weather_code"] = day.weather_code;
        item["temperature_min_c"] = day.temperature_min_c;
        item["temperature_max_c"] = day.temperature_max_c;
    }
    std::string raw;
    serializeJson(document, raw);
    return storage_->WriteStateTextAtomic(kCacheFile, raw);
}

}  // namespace photopainter::product
