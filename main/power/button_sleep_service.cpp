#include "button_sleep_service.h"

#include <cstdint>
#include <memory>

#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "button.h"
#include "device_log_service.h"
#include "display_runtime.h"
#include "display_service.h"
#include "storage_runtime.h"
#include "storage_service.h"

namespace photopainter::product {
namespace {
constexpr const char* kTag = "button_sleep";
constexpr gpio_num_t kBootPin = GPIO_NUM_0;
constexpr gpio_num_t kKeyWakePin = GPIO_NUM_4;
constexpr std::uint16_t kLongPressMs = 3000;

std::unique_ptr<Button> g_boot_button;
bool g_sleep_pending = false;

bool IsDisplayBusy(DisplayState state) {
    return state == DisplayState::kQueued || state == DisplayState::kLoading ||
           state == DisplayState::kRefreshing || state == DisplayState::kFinalizing;
}

void EnterSleepTask(void*) {
    const auto display = GetDisplayService().GetSnapshot();
    if (IsDisplayBusy(display.state) || GetStorageService().HasActiveWriteTransaction()) {
        g_sleep_pending = false;
        GetDeviceLogService().Add(DeviceLogSeverity::kWarning, "button", "sleep_rejected_busy",
                                  "BOOT sleep ignored while display or TF is busy");
        return;
    }

    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    const esp_err_t wake = esp_sleep_enable_ext1_wakeup_io(
        1ULL << static_cast<unsigned>(kKeyWakePin), ESP_EXT1_WAKEUP_ANY_LOW);
    if (wake != ESP_OK) {
        g_sleep_pending = false;
        ESP_LOGE(kTag, "KEY wake configuration failed: %s", esp_err_to_name(wake));
        GetDeviceLogService().Add(DeviceLogSeverity::kError, "button", "sleep_wake_config_failed",
                                  "KEY wake source could not be configured");
        return;
    }

    GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "button", "sleep_entering",
                              "BOOT long press accepted; KEY wakes deep sleep");
    ESP_LOGI(kTag, "Entering deep sleep; KEY GPIO%d wakes", static_cast<int>(kKeyWakePin));
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_deep_sleep_start();
}

void RequestSleep() {
    if (g_sleep_pending) return;
    g_sleep_pending = true;
    if (xTaskCreate(EnterSleepTask, "button_sleep", 4096, nullptr, 3, nullptr) != pdPASS) {
        g_sleep_pending = false;
        GetDeviceLogService().Add(DeviceLogSeverity::kError, "button", "sleep_task_failed",
                                  "Unable to start BOOT sleep task");
    }
}
}  // namespace

esp_err_t InitializeButtonSleepService() {
    if (g_boot_button) return ESP_OK;
    g_boot_button = std::make_unique<Button>(kBootPin, false, kLongPressMs);
    g_boot_button->OnLongPress(RequestSleep);
    GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "button", "boot_sleep_ready",
                              "BOOT hold enters deep sleep; KEY wakes");
    return ESP_OK;
}

}  // namespace photopainter::product
