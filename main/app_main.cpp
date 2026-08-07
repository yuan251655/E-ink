#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "application.h"
#include "system_info.h"
#include "server_app.h"

#include "user_app.h"
#include "storage_runtime.h"
#include "dashboard_data_service.h"
#include "media_library_runtime.h"
#include "local_album_playback_runtime.h"
#include "ai_album_playback_runtime.h"
#include "display_runtime.h"
#include "indicator_service.h"
#include "power_service.h"
#include "device_log_service.h"
#include "mode_manager.h"
#include "xiaozhi_runtime.h"

#define TAG "main"

namespace {
const char* ResetReasonCode() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "other_watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

const char* WakeupCauseCode() {
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_EXT1: return "key";
        case ESP_SLEEP_WAKEUP_TIMER: return "timer";
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "none";
        default: return "other";
    }
}
}  // namespace

extern "C" void app_main(void) {
    photopainter::product::GetDeviceLogService().Add(
        photopainter::product::DeviceLogSeverity::kInfo, "system", ResetReasonCode(),
        "Reset reason recorded");
    photopainter::product::GetDeviceLogService().Add(
        photopainter::product::DeviceLogSeverity::kInfo, "system", "boot", "设备启动");
    photopainter::product::GetDeviceLogService().Add(
        photopainter::product::DeviceLogSeverity::kInfo, "system", WakeupCauseCode(),
        "Wake cause recorded");
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    nvs_handle_t my_handle;
    ret = nvs_open("PhotoPainter", NVS_READWRITE, &my_handle);
    ESP_ERROR_CHECK(ret);
    uint8_t read_value = 0;
    ret                = nvs_get_u8(my_handle, "NetworkMode", &read_value);
    if (ret != ESP_OK) {
        ret = nvs_set_u8(my_handle, "NetworkMode", 0x00);
        ESP_ERROR_CHECK(ret);
        nvs_commit(my_handle);  //Submit the revisions
    }
    uint8_t product_mode_migration = 0;
    ret = nvs_get_u8(my_handle, "ProdMig", &product_mode_migration);
    if (ret != ESP_OK) {
        // Existing official installations may retain a Xiaozhi or Network
        // startup value. Migrate once to the product's local-album default
        // without erasing Wi-Fi credentials or other NVS configuration.
        ESP_ERROR_CHECK(nvs_set_u8(my_handle, "PhotPainterMode", 0x01));
        ESP_ERROR_CHECK(nvs_set_u8(my_handle, "ProdMig", 0x01));
        ESP_ERROR_CHECK(nvs_commit(my_handle));
    }
    ret = nvs_get_u8(my_handle, "PhotPainterMode", &read_value);
    if (ret != ESP_OK) {
        ret = nvs_set_u8(my_handle, "PhotPainterMode", 0x01);
        ESP_ERROR_CHECK(ret);
        nvs_commit(my_handle);  //Submit the revisions
        ret = nvs_get_u8(my_handle, "PhotPainterMode", &read_value);
    }
    uint8_t Mode_value;
    ret = nvs_get_u8(my_handle, "Mode_Flag", &Mode_value);
    if (ret != ESP_OK) {
        ret = nvs_set_u8(my_handle, "Mode_Flag", 0x01);
        ESP_ERROR_CHECK(ret);
        nvs_commit(my_handle);  //Submit the revisions
        ret = nvs_get_u8(my_handle, "Mode_Flag", &Mode_value);
    }
    nvs_close(my_handle);       //Close handle
    ESP_LOGI("Mode_value", "%d", Mode_value);
    /*Button Press Task Creation*/
    if (User_Mode_init() == 0) {
        ESP_LOGE("init", "init Failure");
        return;
    }
    ret = photopainter::product::InitializeIndicatorService();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LED indicator service unavailable: %s", esp_err_to_name(ret));
    }
    ret = photopainter::product::InitializePowerService();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Power service unavailable; continuing without PMIC telemetry: %s", esp_err_to_name(ret));
    }
    bool product_display_ready = false;
    bool product_media_ready = false;
    ret = photopainter::product::InitializeProductStorage(SDPort);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Product storage unavailable; continuing official compatibility flow");
    } else {
        ret = photopainter::product::GetDashboardDataService().Initialize(&photopainter::product::GetStorageService());
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Dashboard data unavailable; continuing without dashboard state: %s", esp_err_to_name(ret));
        }
        ret = photopainter::product::InitializeMediaLibrary();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Media index unavailable; continuing official compatibility flow");
        } else {
            product_media_ready = true;
            // Restore only a completed product snapshot after TF/media index is
            // available.  This does not refresh the e-paper: the panel keeps
            // its pixels through power-off.  Invalid saved media references
            // are sanitized by ModeManager before any API can observe them.
            photopainter::product::GetModeManager().Initialize(
                photopainter::product::Feature::kLocalAlbum,
                &photopainter::product::GetMediaLibrary());
            ret = photopainter::product::InitializeDisplayService();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Display service unavailable; continuing official compatibility flow");
            } else {
                product_display_ready = true;
            }
        }
    }
    if (!product_media_ready) {
        // A missing/degraded TF card has no trustworthy media state.  Keep the
        // product API in its safe first-boot local-album state instead.
        photopainter::product::GetModeManager().Initialize(photopainter::product::Feature::kLocalAlbum);
    }

    // Product firmware never starts the legacy exclusive Mode-3 Xiaozhi or
    // Mode-2 network applications. Keep one product boot path so the local
    // API, AP+STA service and XiaozhiRuntime share the same device state.
    if (read_value != 0x01) {
        ESP_LOGW(TAG, "Migrating legacy startup mode %u to product mode", static_cast<unsigned>(read_value));
        nvs_handle_t handle;
        if (nvs_open("PhotoPainter", NVS_READWRITE, &handle) == ESP_OK) {
            (void)nvs_set_u8(handle, "PhotPainterMode", 0x01);
            (void)nvs_commit(handle);
            nvs_close(handle);
        }
    }
    ESP_LOGW(TAG, "Enter product local-album mode");
    if (product_display_ready) {
        ret = photopainter::product::InitializeLocalAlbumPlaybackService();
        if (ret != ESP_OK) ESP_LOGW(TAG, "Local album playback unavailable; continuing without auto playback");
        ret = photopainter::product::InitializeAiAlbumPlaybackService();
        if (ret != ESP_OK) ESP_LOGW(TAG, "AI album playback unavailable; continuing without auto playback");
    }
    ServerPort_StartProductLocalApi(SDPort);
    photopainter::product::InitializeXiaozhiRuntime();
}
