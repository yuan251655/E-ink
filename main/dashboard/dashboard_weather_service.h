#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace photopainter::product {

class StorageService;

struct DashboardWeatherDay {
    std::string date;
    std::int16_t weather_code = 0;
    std::int16_t temperature_min_c = 0;
    std::int16_t temperature_max_c = 0;
};

struct DashboardWeatherSnapshot {
    std::uint64_t revision = 0;
    std::string state = "location_unconfigured";
    std::string provider = "open_meteo";
    std::string city_name;
    bool has_coordinates = false;
    double latitude = 0.0;
    double longitude = 0.0;
    bool refreshing = false;
    std::uint64_t last_success_at = 0;
    std::string last_error_code;
    std::array<DashboardWeatherDay, 3> days;
};

class DashboardWeatherService {
public:
    esp_err_t Initialize(StorageService* storage);
    DashboardWeatherSnapshot GetSnapshot() const;
    esp_err_t RefreshNow();

private:
    static void TaskEntry(void* context);
    void TaskLoop();
    esp_err_t LoadCache();
    esp_err_t PersistCache(const DashboardWeatherSnapshot& snapshot);
    esp_err_t Fetch(const std::string& city_name, double latitude, double longitude,
                    const std::string& timezone, DashboardWeatherSnapshot* output);

    StorageService* storage_ = nullptr;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    SemaphoreHandle_t fetch_mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;
    DashboardWeatherSnapshot snapshot_;
};

DashboardWeatherService& GetDashboardWeatherService();
esp_err_t InitializeDashboardWeatherService();

}  // namespace photopainter::product
