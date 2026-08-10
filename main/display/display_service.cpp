#include "display_service.h"
#include <cstring>
#include "display_bsp.h"
#include "device_log_service.h"
#include "job_service.h"
#include "indicator_service.h"
#include "media_library.h"
#include "mode_cover_assets.h"
#include "mode_manager.h"
#include "button_sleep_service.h"
#include "power_service.h"
#include "storage_service.h"

namespace photopainter::product {
namespace {
constexpr int kBatteryX = 16;
constexpr int kBatteryY = 16;
constexpr int kBatteryBodyWidth = 78;
constexpr int kBatteryBodyHeight = 36;
constexpr int kBatteryInnerWidth = 64;

constexpr int BatteryFillWidth(int percent) {
    return percent <= 0 ? 0 : percent >= 100 ? kBatteryInnerWidth
                                             : kBatteryInnerWidth * percent / 100;
}

static_assert(BatteryFillWidth(0) == 0);
static_assert(BatteryFillWidth(50) == 32);
static_assert(BatteryFillWidth(100) == 64);
static_assert(kBatteryX + kBatteryBodyWidth + 10 <= 800);
static_assert(kBatteryY + kBatteryBodyHeight <= 480);

void FillRect(ePaperPort* display, int x, int y, int width, int height, std::uint8_t color) {
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column) {
            display->EPD_SetPixel(column, row, color);
        }
    }
}

void DrawBatteryIcon(ePaperPort* display, int percent) {
    // Two overlapping rectangles give the body and terminal softly clipped
    // corners without adding a bitmap asset or graphics dependency.
    FillRect(display, kBatteryX + 3, kBatteryY, kBatteryBodyWidth - 6, kBatteryBodyHeight, ColorBlack);
    FillRect(display, kBatteryX, kBatteryY + 3, kBatteryBodyWidth, kBatteryBodyHeight - 6, ColorBlack);
    FillRect(display, kBatteryX + 5, kBatteryY + 5, kBatteryBodyWidth - 10, kBatteryBodyHeight - 10, ColorWhite);
    FillRect(display, kBatteryX + kBatteryBodyWidth, kBatteryY + 10, 10, 16, ColorBlack);
    const int fill_width = BatteryFillWidth(percent);
    if (fill_width > 0) FillRect(display, kBatteryX + 7, kBatteryY + 7, fill_width, 22, ColorBlack);
}
}  // namespace

esp_err_t DisplayService::Initialize(StorageService* storage, MediaLibrary* library, ePaperPort* display, SemaphoreHandle_t legacy_mutex) {
    if (!storage || !library || !display || !legacy_mutex) return ESP_ERR_INVALID_ARG;
    storage_ = storage; library_ = library; display_ = display; legacy_mutex_ = legacy_mutex;
    state_mutex_ = xSemaphoreCreateMutex();
    if (!state_mutex_) return ESP_ERR_NO_MEM;
    queue_ = xQueueCreate(1, sizeof(WorkItem));
    if (!queue_) return ESP_ERR_NO_MEM;
    return xTaskCreate(WorkerEntry, "display_service", 6144, this, 4, &worker_) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t DisplayService::SubmitLocal(const MediaId& media_id, const JobId& job_id, JobService* jobs) {
    return SubmitMedia(Feature::kLocalAlbum, MediaCategory::kLocal, media_id, job_id, jobs);
}

esp_err_t DisplayService::SubmitMedia(Feature feature, MediaCategory category, const MediaId& media_id,
                                      const JobId& job_id, JobService* jobs) {
    const bool valid_owner = (feature == Feature::kLocalAlbum && category == MediaCategory::kLocal) ||
                             (feature == Feature::kAiAlbum && category == MediaCategory::kAi) ||
                             (feature == Feature::kInfoDashboard && category == MediaCategory::kDashboard);
    if (!valid_owner) return ESP_ERR_INVALID_ARG;
    if (!queue_ || !state_mutex_ || !jobs || media_id.empty() || job_id.empty() || media_id.size() >= 65 || job_id.size() >= 65) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    if (snapshot_.state == DisplayState::kQueued || snapshot_.state == DisplayState::kLoading ||
        snapshot_.state == DisplayState::kRefreshing || snapshot_.state == DisplayState::kFinalizing) {
        xSemaphoreGive(state_mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    WorkItem item{}; item.kind = WorkKind::kMedia; item.feature = feature; item.category = category;
    std::strncpy(item.media_id, media_id.c_str(), sizeof(item.media_id) - 1); std::strncpy(item.job_id, job_id.c_str(), sizeof(item.job_id) - 1);
    jobs_ = jobs;
    snapshot_.state = DisplayState::kQueued; snapshot_.queued_target_media_id = media_id; snapshot_.active_job_id = job_id;
    xSemaphoreGive(state_mutex_);
    return xQueueOverwrite(queue_, &item) == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t DisplayService::SubmitModeCover(Feature feature, const JobId& job_id, JobService* jobs) {
    if (!queue_ || !state_mutex_ || !jobs || job_id.empty() || job_id.size() >= 65) return ESP_ERR_INVALID_ARG;
    ModeCoverAsset asset;
    if (GetModeCoverAsset(feature, &asset) != ESP_OK) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    if (snapshot_.state == DisplayState::kQueued || snapshot_.state == DisplayState::kLoading ||
        snapshot_.state == DisplayState::kRefreshing || snapshot_.state == DisplayState::kFinalizing) {
        xSemaphoreGive(state_mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    WorkItem item{};
    item.kind = WorkKind::kModeCover;
    item.feature = feature;
    std::strncpy(item.job_id, job_id.c_str(), sizeof(item.job_id) - 1);
    jobs_ = jobs;
    snapshot_.state = DisplayState::kQueued;
    snapshot_.queued_target_media_id = asset.system_asset_id;
    snapshot_.active_job_id = job_id;
    xSemaphoreGive(state_mutex_);
    return xQueueOverwrite(queue_, &item) == pdPASS ? ESP_OK : ESP_FAIL;
}
DisplaySnapshot DisplayService::GetSnapshot() const {
    DisplaySnapshot result;
    if (!state_mutex_) return result;
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    result = snapshot_;
    xSemaphoreGive(state_mutex_);
    return result;
}
void DisplayService::WorkerEntry(void* context) { static_cast<DisplayService*>(context)->WorkerLoop(); }
void DisplayService::WorkerLoop() {
    WorkItem item{};
    while (true) {
        xQueueReceive(queue_, &item, portMAX_DELAY);
        const MediaId media_id(item.media_id); const JobId job_id(item.job_id);
        xSemaphoreTake(state_mutex_, portMAX_DELAY); snapshot_.state = DisplayState::kLoading; xSemaphoreGive(state_mutex_);
        const bool mode_cover = item.kind == WorkKind::kModeCover;
        if (mode_cover) {
            GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "display", "mode_cover_loading",
                                      "Mode cover is being prepared for refresh");
        }
        if (jobs_) (void)jobs_->Update(job_id, JobState::kRunning, mode_cover ? "preparing" : "loading", 15);
        ModeCoverAsset asset;
        const esp_err_t asset_result = mode_cover ? GetModeCoverAsset(item.feature, &asset) : ESP_OK;
        MediaItem media;
        const bool media_valid = mode_cover ||
            (library_->Find(media_id, &media) && media.feature == item.feature && media.category == item.category &&
             library_->ValidateFrameForDisplay(media_id) == ESP_OK);
        if (!media_valid || (mode_cover && asset_result != ESP_OK) ||
            xSemaphoreTake(legacy_mutex_, portMAX_DELAY) != pdTRUE) {
            const std::string error = mode_cover ? "mode_cover_unavailable" : "media_invalid";
            xSemaphoreTake(state_mutex_, portMAX_DELAY);
            snapshot_.state = DisplayState::kFailed; snapshot_.last_error_code = error;
            snapshot_.active_job_id.clear(); snapshot_.queued_target_media_id.clear();
            xSemaphoreGive(state_mutex_);
            if (mode_cover) GetModeManager().FailSwitch(job_id, item.feature, error, false);
            else if (jobs_) (void)jobs_->Update(job_id, JobState::kFailed, "failed", 0, error);
            continue;
        }
        // Product local-album mode does not start the legacy button task that
        // normally initializes the panel. Initialize lazily under the same
        // display lock before sending a frame; repeated calls are harmless in
        // the official BSP.
        display_->EPD_Init();
        esp_err_t result = ESP_OK;
        if (mode_cover) std::memcpy(display_->EPD_GetIMGBuffer(), asset.data, asset.size);
        else result = storage_->ReadCommittedFile(media.storage_relative_directory + "/image.bin",
                                                  display_->EPD_GetIMGBuffer(), kDisplayFrameBytes);
        bool refresh_indicator_active = false;
        if (result == ESP_OK) {
            const PowerSnapshot power = GetPowerService().GetSnapshot();
            if (GetPowerService().ShouldShowBatteryIcon(power)) {
                DrawBatteryIcon(display_, power.battery_percent);
            }
            xSemaphoreTake(state_mutex_, portMAX_DELAY); snapshot_.state = DisplayState::kRefreshing; xSemaphoreGive(state_mutex_);
            if (jobs_) (void)jobs_->Update(job_id, JobState::kRunning, "refreshing", 60);
            GetIndicatorService().SetRefreshActive(true);
            refresh_indicator_active = true;
            // The 7.3-inch PhotoPainter panel is installed 180 degrees from
            // the logical 800x480 media frame.  This is the same correction
            // the official BSP applies to a landscape BMP.  Keep it solely
            // in the physical display layer: the stored BIN and App preview
            // remain in their normal user-facing orientation.
            display_->Set_Rotation(2);
            // The official BSP returns bool here, not esp_err_t.  Mapping it
            // explicitly avoids treating a successful `true` value (1) as an
            // ESP-IDF error code.
            result = display_->EPD_DisplayWithTimeout(40000) ? ESP_OK : ESP_ERR_TIMEOUT;
        }
        if (refresh_indicator_active) GetIndicatorService().SetRefreshActive(false);
        xSemaphoreGive(legacy_mutex_);
        xSemaphoreTake(state_mutex_, portMAX_DELAY);
        snapshot_.active_job_id.clear(); snapshot_.queued_target_media_id.clear();
        xSemaphoreGive(state_mutex_);
        if (result == ESP_OK) {
            // Keep admission closed until the domain owner has committed its
            // authoritative state. Otherwise a local display could slip into
            // the queue after the cover refresh but before active_feature is
            // changed by ModeManager.
            xSemaphoreTake(state_mutex_, portMAX_DELAY);
            snapshot_.state = DisplayState::kFinalizing;
            snapshot_.current_media_id = mode_cover ? asset.system_asset_id : media_id;
            snapshot_.last_successful_media_id = snapshot_.current_media_id;
            snapshot_.last_error_code.clear();
            xSemaphoreGive(state_mutex_);
            if (mode_cover) GetModeManager().CompleteSwitch(job_id, item.feature, asset.system_asset_id);
            else {
                GetModeManager().RecordDisplayedMedia(media.feature, media.category, media_id);
                if (jobs_) (void)jobs_->CompleteSuccess(job_id, media_id);
            }
            xSemaphoreTake(state_mutex_, portMAX_DELAY); snapshot_.state = DisplayState::kSuccess; xSemaphoreGive(state_mutex_);
            RecordAutomaticSleepActivity();
        } else {
            const std::string error = result == ESP_ERR_TIMEOUT ? "display_timeout" : "display_failed";
            if (mode_cover) {
                GetDeviceLogService().Add(result == ESP_ERR_TIMEOUT ? DeviceLogSeverity::kWarning : DeviceLogSeverity::kError,
                                          "display", result == ESP_ERR_TIMEOUT ? "mode_cover_timeout" : "mode_cover_failed",
                                          result == ESP_ERR_TIMEOUT ? "Mode cover refresh timed out" : "Mode cover refresh failed");
            }
            xSemaphoreTake(state_mutex_, portMAX_DELAY);
            snapshot_.state = DisplayState::kFailed;
            snapshot_.last_error_code = error;
            xSemaphoreGive(state_mutex_);
            if (mode_cover) GetModeManager().FailSwitch(job_id, item.feature, error, result == ESP_ERR_TIMEOUT);
            else if (jobs_) (void)jobs_->Update(job_id, result == ESP_ERR_TIMEOUT ? JobState::kTimeout : JobState::kFailed, "failed", 0, error);
        }
    }
}
}  // namespace photopainter::product
