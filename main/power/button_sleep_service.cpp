#include "button_sleep_service.h"

#include <cstdint>
#include <memory>

#include <driver/rtc_io.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include "button.h"
#include "device_log_service.h"
#include "display_runtime.h"
#include "display_service.h"
#include "storage_runtime.h"
#include "storage_service.h"
#include "rtc_service.h"
#include "local_album_playback_runtime.h"
#include "ai_album_playback_runtime.h"
#include "mode_manager.h"
#include "xiaozhi_runtime.h"

namespace photopainter::product {
namespace {
constexpr const char* kTag = "button_sleep";
// GPIO5 is the board SYS_OUT power-control line, not a button input.
constexpr gpio_num_t kBootPin = GPIO_NUM_0;
constexpr gpio_num_t kKeyWakePin = GPIO_NUM_4;
constexpr gpio_num_t kRtcWakePin = GPIO_NUM_6;
constexpr std::uint16_t kLongPressMs = 3000;
constexpr std::uint8_t kRtcTestCycles = 10;
constexpr std::uint8_t kRtcTestSeconds = 10;
constexpr char kSleepConfigNamespace[] = "sleep_config";
constexpr char kSleepEnabledKey[] = "enabled";
constexpr char kSleepTimeoutKey[] = "idle_min";
constexpr char kSleepPlaybackKey[] = "playback";
constexpr char kScheduledNamespace[] = "sleep_schedule";
constexpr char kScheduledLocalKey[] = "local";
constexpr char kActivityNamespace[] = "sleep_activity";
constexpr char kActivityEpochKey[] = "last_epoch";

std::unique_ptr<Button> g_boot_button;
bool g_sleep_pending = false;
RTC_DATA_ATTR std::uint8_t g_rtc_test_remaining = 0;
RTC_DATA_ATTR std::uint8_t g_rtc_test_completed = 0;
AutomaticSleepConfig g_automatic_sleep_config;
bool g_scheduled_sleep_pending = false;
std::uint32_t g_scheduled_sleep_delay_seconds = 0;
TaskHandle_t g_automatic_sleep_worker = nullptr;
bool g_automatic_sleep_pending = false;
std::uint64_t g_last_activity_epoch_seconds = 0;

bool IsRtcWake() {
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return false;
    return (esp_sleep_get_ext1_wakeup_status() & (1ULL << static_cast<unsigned>(kRtcWakePin))) != 0;
}

void PersistScheduledLocalPlayback(bool pending) {
    nvs_handle_t handle;
    if (nvs_open(kScheduledNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_set_u8(handle, kScheduledLocalKey, pending ? 1 : 0);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

bool TakeScheduledLocalPlayback() {
    nvs_handle_t handle;
    if (nvs_open(kScheduledNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    std::uint8_t pending = 0;
    (void)nvs_get_u8(handle, kScheduledLocalKey, &pending);
    (void)nvs_set_u8(handle, kScheduledLocalKey, 0);
    (void)nvs_commit(handle);
    nvs_close(handle);
    return pending != 0;
}

void LoadAutomaticSleepConfig() {
    nvs_handle_t handle;
    if (nvs_open(kSleepConfigNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    std::uint8_t enabled = 0;
    std::uint8_t timeout = g_automatic_sleep_config.idle_timeout_minutes;
    std::uint8_t playback = 1;
    (void)nvs_get_u8(handle, kSleepEnabledKey, &enabled);
    (void)nvs_get_u8(handle, kSleepTimeoutKey, &timeout);
    (void)nvs_get_u8(handle, kSleepPlaybackKey, &playback);
    nvs_close(handle);
    g_automatic_sleep_config.enabled = enabled != 0;
    g_automatic_sleep_config.idle_timeout_minutes = IsAllowedAutomaticSleepTimeout(timeout) ? timeout : 15;
    g_automatic_sleep_config.wake_for_playback = playback != 0;
}

void PersistActivityEpoch(std::uint64_t value) {
    nvs_handle_t handle;
    if (nvs_open(kActivityNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_set_u64(handle, kActivityEpochKey, value);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

void LoadActivityEpoch() {
    nvs_handle_t handle;
    if (nvs_open(kActivityNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    (void)nvs_get_u64(handle, kActivityEpochKey, &g_last_activity_epoch_seconds);
    nvs_close(handle);
}

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

void EnterScheduledLocalPlaybackSleepTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(700));
    g_scheduled_sleep_pending = false;
    const auto config = GetAutomaticSleepConfig();
    const auto display = GetDisplayService().GetSnapshot();
    const auto rtc = GetRtcService().GetSnapshot();
    if (!config.enabled || !config.wake_for_playback || !rtc.present || !rtc.valid ||
        IsDisplayBusy(display.state) || GetStorageService().HasActiveWriteTransaction()) {
        GetDeviceLogService().Add(DeviceLogSeverity::kWarning, "power", "scheduled_sleep_rejected",
                                  "Local playback sleep rejected because configuration, RTC, display, or TF is not ready");
        return;
    }
    esp_err_t result = GetRtcService().ArmWakeAfterSeconds(g_scheduled_sleep_delay_seconds);
    if (result == ESP_OK) result = ConfigureWakeSources(true);
    if (result != ESP_OK) {
        (void)GetRtcService().DisarmWakeTimer();
        GetDeviceLogService().Add(DeviceLogSeverity::kWarning, "power", "scheduled_sleep_setup_failed",
                                  "Local playback stays awake because RTC wake could not be configured");
        return;
    }
    PersistScheduledLocalPlayback(true);
    GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "power", "scheduled_sleep_entering",
                              "Local playback is sleeping until the next RTC refresh");
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_deep_sleep_start();
}

bool IsAutomaticSleepBusy() {
    const auto display = GetDisplayService().GetSnapshot();
    if (IsDisplayBusy(display.state) || GetStorageService().HasActiveWriteTransaction()) return true;
    const auto mode = GetModeManager().GetSnapshot();
    if (mode.state != ModeSnapshot::State::kIdle) return true;
    if (mode.active_feature == Feature::kAiAlbum) {
        const auto ai = GetXiaozhiRuntimeSnapshot();
        if (ai.state == "listening" || ai.state == "speaking" || ai.state == "connecting" || ai.state == "starting") return true;
    }
    return false;
}

std::uint64_t ActivePlaybackDeadlineEpoch() {
    const auto mode = GetModeManager().GetSnapshot();
    if (mode.state != ModeSnapshot::State::kIdle) return 0;
    if (mode.active_feature == Feature::kLocalAlbum) {
        const auto playback = GetLocalAlbumPlaybackService().GetSnapshot();
        return playback.config.mode == PlaybackMode::kAuto ? playback.next_play_at_epoch_seconds : 0;
    }
    if (mode.active_feature == Feature::kAiAlbum) {
        const auto playback = GetAiAlbumPlaybackService().GetSnapshot();
        return playback.config.mode == PlaybackMode::kAuto ? playback.next_play_at_epoch_seconds : 0;
    }
    return 0;
}

void EnterAutomaticSleepTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(250));
    g_automatic_sleep_pending = false;
    const auto config = GetAutomaticSleepConfig();
    std::uint64_t now = 0;
    if (!config.enabled || !GetRtcService().GetUnixTimeSeconds(&now) || IsAutomaticSleepBusy()) return;
    if (g_last_activity_epoch_seconds == 0 || now < g_last_activity_epoch_seconds + config.idle_timeout_minutes * 60ULL) return;
    const std::uint64_t deadline = config.wake_for_playback ? ActivePlaybackDeadlineEpoch() : 0;
    if (deadline != 0 && deadline <= now) return;
    const std::uint64_t remaining = deadline == 0 ? 0 : deadline - now;
    // PCF85063 timer supports at most 255 minutes. If a later playback cannot
    // be armed exactly, remain awake rather than losing its saved schedule.
    if (remaining > 255ULL * 60ULL) return;
    esp_err_t result = deadline == 0 ? ConfigureWakeSources(false)
                                      : GetRtcService().ArmWakeAfterSeconds(static_cast<std::uint32_t>(remaining));
    if (result == ESP_OK && deadline != 0) result = ConfigureWakeSources(true);
    if (result != ESP_OK) {
        if (deadline != 0) (void)GetRtcService().DisarmWakeTimer();
        GetDeviceLogService().Add(DeviceLogSeverity::kWarning, "power", "automatic_sleep_setup_failed",
                                  "Global automatic sleep stayed awake because wake setup failed");
        return;
    }
    GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "power", "automatic_sleep_entering",
                              deadline == 0 ? "Idle sleep entered; KEY wakes" : "Idle sleep entered; RTC preserves playback deadline");
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_deep_sleep_start();
}

void AutomaticSleepWorker(void*) {
    while (true) {
        const auto config = GetAutomaticSleepConfig();
        std::uint64_t now = 0;
        if (config.enabled && !g_automatic_sleep_pending && GetRtcService().GetUnixTimeSeconds(&now)) {
            if (g_last_activity_epoch_seconds == 0) {
                g_last_activity_epoch_seconds = now;
                PersistActivityEpoch(now);
            }
            if (now >= g_last_activity_epoch_seconds + config.idle_timeout_minutes * 60ULL && !IsAutomaticSleepBusy()) {
                g_automatic_sleep_pending = true;
                if (xTaskCreate(EnterAutomaticSleepTask, "auto_sleep", 4096, nullptr, 3, nullptr) != pdPASS) {
                    g_automatic_sleep_pending = false;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
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
    LoadAutomaticSleepConfig();
    g_boot_button = std::make_unique<Button>(kBootPin, false, kLongPressMs);
    g_boot_button->OnLongPress(RequestSleep);
    GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "button", "boot_sleep_ready",
                              "BOOT hold enters deep sleep; KEY wakes");
    return ESP_OK;
}

void RecordAutomaticSleepActivity() {
    std::uint64_t now = 0;
    if (!GetRtcService().GetUnixTimeSeconds(&now)) return;
    g_last_activity_epoch_seconds = now;
    PersistActivityEpoch(now);
}

esp_err_t InitializeAutomaticSleepService() {
    if (g_automatic_sleep_worker != nullptr) return ESP_OK;
    LoadActivityEpoch();
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause != ESP_SLEEP_WAKEUP_EXT1 ||
        (esp_sleep_get_ext1_wakeup_status() & (1ULL << static_cast<unsigned>(kRtcWakePin))) == 0) {
        RecordAutomaticSleepActivity();
    }
    return xTaskCreate(AutomaticSleepWorker, "auto_sleep_policy", 4096, nullptr, 3, &g_automatic_sleep_worker) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}

AutomaticSleepStatus GetAutomaticSleepStatus() {
    AutomaticSleepStatus status;
    const auto config = GetAutomaticSleepConfig();
    status.enabled = config.enabled;
    std::uint64_t now = 0;
    status.rtc_valid = GetRtcService().GetUnixTimeSeconds(&now);
    status.last_activity_epoch_seconds = g_last_activity_epoch_seconds;
    status.idle_sleep_at_epoch_seconds = g_last_activity_epoch_seconds == 0 ? 0
        : g_last_activity_epoch_seconds + config.idle_timeout_minutes * 60ULL;
    status.next_play_at_epoch_seconds = ActivePlaybackDeadlineEpoch();
    status.busy = IsAutomaticSleepBusy();
    if (!status.enabled) status.state = "disabled";
    else if (!status.rtc_valid) status.state = "rtc_unavailable";
    else if (status.busy) status.state = "busy";
    else if (status.next_play_at_epoch_seconds != 0 && status.next_play_at_epoch_seconds <= now) status.state = "playback_due";
    else if (status.idle_sleep_at_epoch_seconds != 0 && now < status.idle_sleep_at_epoch_seconds) status.state = "waiting_idle";
    else status.state = "ready_to_sleep";
    return status;
}

bool IsAllowedAutomaticSleepTimeout(std::uint8_t minutes) {
    return minutes == 1 || minutes == 2 || minutes == 5 || minutes == 10 || minutes == 15 || minutes == 30 || minutes == 60;
}

AutomaticSleepConfig GetAutomaticSleepConfig() {
    return g_automatic_sleep_config;
}

esp_err_t UpdateAutomaticSleepConfig(const AutomaticSleepConfig& config) {
    if (!IsAllowedAutomaticSleepTimeout(config.idle_timeout_minutes)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kSleepConfigNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_u8(handle, kSleepEnabledKey, config.enabled ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u8(handle, kSleepTimeoutKey, config.idle_timeout_minutes);
    if (result == ESP_OK) result = nvs_set_u8(handle, kSleepPlaybackKey, config.wake_for_playback ? 1 : 0);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) return result;
    g_automatic_sleep_config = config;
    GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "power", "automatic_sleep_config_saved",
                              config.enabled ? "Automatic sleep configuration enabled" : "Automatic sleep configuration disabled");
    return ESP_OK;
}

esp_err_t RequestScheduledLocalPlaybackSleep(std::uint32_t delay_seconds) {
    const auto config = GetAutomaticSleepConfig();
    if (!config.enabled || !config.wake_for_playback || delay_seconds == 0) return ESP_ERR_INVALID_STATE;
    if (g_scheduled_sleep_pending) return ESP_ERR_INVALID_STATE;
    g_scheduled_sleep_pending = true;
    g_scheduled_sleep_delay_seconds = delay_seconds;
    if (xTaskCreate(EnterScheduledLocalPlaybackSleepTask, "local_sleep", 4096, nullptr, 3, nullptr) != pdPASS) {
        g_scheduled_sleep_pending = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool ConsumeScheduledLocalPlaybackWake() {
    const bool was_pending = TakeScheduledLocalPlayback();
    if (!was_pending) return false;
    if (!IsRtcWake()) {
        GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "power", "scheduled_sleep_cancelled",
                                  "Local playback RTC plan cancelled by KEY or non-RTC boot");
        return false;
    }
    GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "power", "scheduled_sleep_woke",
                              "RTC woke local playback for the next refresh");
    return true;
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
