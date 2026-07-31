#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "application.h"
#include "system_info.h"
#include "server_app.h"

#include "user_app.h"
#include "storage_runtime.h"
#include "media_library_runtime.h"
#include "local_album_playback_runtime.h"
#include "display_runtime.h"
#include "indicator_service.h"
#include "device_log_service.h"
#include "mode_manager.h"

#define TAG "main"

extern "C" void app_main(void) {
    photopainter::product::GetDeviceLogService().Add(
        photopainter::product::DeviceLogSeverity::kInfo, "system", "boot", "设备启动");
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
    bool product_display_ready = false;
    ret = photopainter::product::InitializeProductStorage(SDPort);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Product storage unavailable; continuing official compatibility flow");
    } else {
        ret = photopainter::product::InitializeMediaLibrary();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Media index unavailable; continuing official compatibility flow");
        } else {
            photopainter::product::GetModeManager().Initialize(photopainter::product::Feature::kLocalAlbum);
            ret = photopainter::product::InitializeDisplayService();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Display service unavailable; continuing official compatibility flow");
            } else {
                product_display_ready = true;
            }
        }
    }

    if (read_value == 0x03) {
        ESP_LOGW("main","Enter xiaozhi mode");
        auto &app = Application::GetInstance();
        app.Start();
    } else if (read_value == 0x01) {
        ESP_LOGW("main","Enter local album product mode");
        if (product_display_ready) {
            ret = photopainter::product::InitializeLocalAlbumPlaybackService();
            if (ret != ESP_OK) ESP_LOGW(TAG, "Local album playback unavailable; continuing without auto playback");
        }
        ServerPort_StartProductLocalApi(SDPort);
    } else if (read_value == 0x02) {
        ESP_LOGW("main","Enter Network mode");
        User_Network_mode_app_init();
    } else if (read_value == 0x04) {
        ESP_LOGW("main","Enter Mode Selection");
        Mode_Selection_Init();
    }
}
