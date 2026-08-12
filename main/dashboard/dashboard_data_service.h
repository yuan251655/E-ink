#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace photopainter::product {

class StorageService;

struct DashboardTodoState {
    std::string id;
    std::string title;
    bool completed = false;
    std::uint32_t position = 0;
};

struct DashboardDataSnapshot {
    std::uint64_t revision = 1;
    std::string layout_id = "weather_memo_todo";
    std::string timezone = "Asia/Shanghai";
    std::string city_name;
    bool has_coordinates = false;
    double latitude = 0.0;
    double longitude = 0.0;
    std::string memo;
    std::vector<DashboardTodoState> todos;
    bool auto_refresh_enabled = false;
    std::uint32_t auto_refresh_interval_seconds = 3 * 60 * 60;
};

class DashboardDataService {
public:
    esp_err_t Initialize(StorageService* storage);
    DashboardDataSnapshot GetSnapshot() const;
    esp_err_t Replace(const DashboardDataSnapshot& candidate, std::uint64_t expected_revision,
                      DashboardDataSnapshot* updated);

private:
    bool IsValid(const DashboardDataSnapshot& candidate) const;
    esp_err_t Load();
    esp_err_t Persist(const DashboardDataSnapshot& snapshot);

    StorageService* storage_ = nullptr;
    DashboardDataSnapshot snapshot_;
    mutable SemaphoreHandle_t mutex_ = nullptr;
};

DashboardDataService& GetDashboardDataService();

}  // namespace photopainter::product
