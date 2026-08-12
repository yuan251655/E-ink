#include "dashboard_auto_refresh_service.h"

#include <string>

#include "ArduinoJson.h"
#include "dashboard_data_service.h"
#include "dashboard_weather_service.h"
#include "display_runtime.h"
#include "display_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "job_runtime.h"
#include "job_service.h"
#include "mode_manager.h"
#include "rtc_service.h"
#include "storage_runtime.h"
#include "storage_service.h"

namespace photopainter::product {
namespace {
constexpr char kTag[] = "dashboard_refresh";
constexpr char kStateFile[] = "dashboard_refresh.json";
constexpr std::uint64_t kRetrySeconds = 30 * 60;
SemaphoreHandle_t g_mutex = nullptr;
TaskHandle_t g_task = nullptr;
std::uint64_t g_next_refresh_at = 0;

void Persist(std::uint64_t next) {
    JsonDocument document;
    document["next_refresh_at"] = next;
    std::string raw;
    serializeJson(document, raw);
    (void)GetStorageService().WriteStateTextAtomic(kStateFile, raw);
}

void SetNext(std::uint64_t next) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_next_refresh_at = next;
    xSemaphoreGive(g_mutex);
    Persist(next);
}

void Load() {
    std::string raw;
    if (GetStorageService().ReadStateText(kStateFile, 256, &raw) != ESP_OK) return;
    JsonDocument document;
    if (deserializeJson(document, raw) == DeserializationError::Ok) g_next_refresh_at = document["next_refresh_at"] | 0ULL;
}

void Worker(void*) {
    std::uint64_t observed_revision = GetDashboardDataService().GetSnapshot().revision;
    while (true) {
        const auto dashboard = GetDashboardDataService().GetSnapshot();
        std::uint64_t now = 0;
        if (GetRtcService().GetUnixTimeSeconds(&now)) {
            if (!dashboard.auto_refresh_enabled || dashboard.layout_id != "weather_date") {
                if (DashboardAutoRefreshDeadlineEpoch() != 0) SetNext(0);
            } else {
                if (dashboard.revision != observed_revision) {
                    observed_revision = dashboard.revision;
                    SetNext(now + dashboard.auto_refresh_interval_seconds);
                } else if (DashboardAutoRefreshDeadlineEpoch() == 0) {
                    SetNext(now + dashboard.auto_refresh_interval_seconds);
                } else if (now >= DashboardAutoRefreshDeadlineEpoch()) {
                    const auto mode = GetModeManager().GetSnapshot();
                    if (mode.state == ModeSnapshot::State::kIdle && mode.active_feature == Feature::kInfoDashboard) {
                        const esp_err_t weather = GetDashboardWeatherService().RefreshNow();
                        if (weather == ESP_OK) {
                            JobSnapshot job;
                            const std::string request = "dashboard-auto-" + std::to_string(now);
                            const auto registered = GetProductJobService().CreateOrFind(
                                JobKind::kDisplay, request, "dashboard_auto_refresh", &job);
                            if (registered == JobRegistrationResult::kCreated &&
                                GetDisplayService().SubmitDashboard(job.job_id, &GetProductJobService()) == ESP_OK) {
                                SetNext(now + dashboard.auto_refresh_interval_seconds);
                            } else {
                                SetNext(now + kRetrySeconds);
                            }
                        } else {
                            ESP_LOGW(kTag, "Weather refresh failed; keeping old dashboard frame");
                            SetNext(now + kRetrySeconds);
                        }
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
}

esp_err_t InitializeDashboardAutoRefreshService() {
    if (g_task != nullptr) return ESP_OK;
    g_mutex = xSemaphoreCreateMutex();
    if (g_mutex == nullptr) return ESP_ERR_NO_MEM;
    Load();
    return xTaskCreate(Worker, "dashboard_refresh", 6144, nullptr, 3, &g_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

std::uint64_t DashboardAutoRefreshDeadlineEpoch() {
    if (g_mutex == nullptr) return 0;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const auto value = g_next_refresh_at;
    xSemaphoreGive(g_mutex);
    return value;
}
}
