#include "button_sleep_service.h"

#include <cstdint>
#include <memory>

#include <driver/rtc_io.h>
#include <esp_attr.h>
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
#include "rtc_service.h"

namespace photopainter::product {
namespace {
constexpr const char* kTag = "button_sleep";
constexpr gpio_num_t kBootPin = GPIO_NUM_0;
constexpr gpio_num_t kKeyWakePin = GPIO_NUM_4;
constexpr gpio_num_t kRtcWakePin = GPIO_NUM_6;
constexpr std::uint16_t kLongPressMs = 3000;
constexpr std::uint8_t kRtcTestCycles = 10;
constexpr std::uint8_t kRtcTestSeconds = 10;

std::unique_ptr<Button> g_boot_button;
bool g_sleep_pending = false;
RTC_DATA_ATTR std::uint8_t g_rtc_test_remaining = 0;
RTC_DATA_ATTR std::uint8_t g_rtc_test_completed = 0;

bool IsDisplayBusy(DisplayState state) {
    return state == DisplayState::kQueued || state == DisplayState::kLoading ||
           state == DisplayState::kRefreshing || state == DisplayState::kFinalizing;
}

void CancelRtcTest(const char* code, const char* message) {
    g_rtc_test_remaining = 0;
    GetDeviceLogService().Add(DeviceLogSeverity::kWarning, "rtc", code, message);
}

esp_err_t ConfigureWakeSources(bool include_rtc) {
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    std::uint64_t mask = 1ULL << static_cast<unsigned>(kKeyWakePin);
    if (include_rtc) {
        esp_err_t result = rtc_gpio_init(kRtcWakePin);
        if (result != ESP_OK) return result;
        result = rtc_gpio_set_direction(kRtcWakePin, RTC_GPIO_MODE_INPUT_ONLY);
        if (result != ESP_OK) return result;
        (void)rtc_gpio_pulldown_dis(kRtcWakePin);
        result = rtc_gpio_pullup_en(kRtcWakePin);
        if (result != ESP_OK) return result;
        result = rtc_gpio_hold_en(kRtcWakePin);
        if (result != ESP_OK) return result;
        mask |= 1ULL << static_cast<unsigned>(kRtcWakePin);
    }
    return esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
}

void EnterSleepTask(void*) {
    const auto display = GetDisplayService().GetSnapshot();
    if (IsDisplayBusy(display.state) || GetStorageService().HasActiveWriteTransaction()) {
        g_sleep_pending = false;
        GetDeviceLogService().Add(DeviceLogSeverity::kWarning, "button", "sleep_rejected_busy",
                                  "BOOT sleep ignored while display or TF is busy");
        return;
    }

    const esp_err_t wake = ConfigureWakeSources(false);
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

void EnterRtcTestSleepTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(700));
    const auto display = GetDisplayService().GetSnapshot();
    if (IsDisplayBusy(display.state) || GetStorageService().HasActiveWriteTransaction()) {
        CancelRtcTest("rtc_wake_test_busy", "RTC wake verification stopped because display or TF is busy");
        return;
    }
    esp_err_t result = GetRtcService().ArmInterruptDiagnostic(kRtcTestSeconds);
    if (result == ESP_OK) result = ConfigureWakeSources(true);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "RTC wake test setup failed: %s", esp_err_to_name(result));
        CancelRtcTest("rtc_wake_test_setup_failed", "RTC wake verification setup failed");
        return;
    }
    GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "rtc", "rtc_wake_test_sleep",
                              "RTC wake verification entering 10-second deep sleep; KEY cancels");
    ESP_LOGI(kTag, "RTC wake verification: completed=%u remaining=%u", g_rtc_test_completed,
             g_rtc_test_remaining);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_deep_sleep_start();
}

esp_err_t QueueRtcTestSleep() {
    return xTaskCreate(EnterRtcTestSleepTask, "rtc_wake_test", 4096, nullptr, 3, nullptr) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
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

esp_err_t StartRtcWakeVerification() {
    if (g_rtc_test_remaining != 0) return ESP_ERR_INVALID_STATE;
    if (!GetRtcService().GetSnapshot().present) return ESP_ERR_NOT_FOUND;
    g_rtc_test_completed = 0;
    g_rtc_test_remaining = kRtcTestCycles;
    const esp_err_t result = QueueRtcTestSleep();
    if (result != ESP_OK) g_rtc_test_remaining = 0;
    return result;
}

void ResumeRtcWakeVerification() {
    if (g_rtc_test_remaining == 0) return;
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const std::uint64_t pins = cause == ESP_SLEEP_WAKEUP_EXT1 ? esp_sleep_get_ext1_wakeup_status() : 0;
    if ((pins & (1ULL << static_cast<unsigned>(kRtcWakePin))) == 0) {
        CancelRtcTest("rtc_wake_test_cancelled", "RTC wake verification cancelled by KEY or non-RTC reset");
        return;
    }
    --g_rtc_test_remaining;
    ++g_rtc_test_completed;
    if (g_rtc_test_remaining == 0) {
        GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "rtc", "rtc_wake_test_passed",
                                  "RTC wake verification completed 10 consecutive cycles");
        return;
    }
    if (QueueRtcTestSleep() != ESP_OK) {
        CancelRtcTest("rtc_wake_test_task_failed", "RTC wake verification could not schedule the next cycle");
    }
}

RtcWakeVerificationSnapshot GetRtcWakeVerificationSnapshot() {
    return {
        .active = g_rtc_test_remaining != 0,
        .completed = g_rtc_test_completed,
        .remaining = g_rtc_test_remaining,
    };
}

}  // namespace photopainter::product
