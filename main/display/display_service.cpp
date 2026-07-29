#include "display_service.h"
#include <cstring>
#include "display_bsp.h"
#include "job_service.h"
#include "indicator_service.h"
#include "media_library.h"
#include "storage_service.h"

namespace photopainter::product {
esp_err_t DisplayService::Initialize(StorageService* storage, MediaLibrary* library, ePaperPort* display, SemaphoreHandle_t legacy_mutex) {
    if (!storage || !library || !display || !legacy_mutex) return ESP_ERR_INVALID_ARG;
    storage_ = storage; library_ = library; display_ = display; legacy_mutex_ = legacy_mutex;
    queue_ = xQueueCreate(1, sizeof(WorkItem));
    if (!queue_) return ESP_ERR_NO_MEM;
    return xTaskCreate(WorkerEntry, "display_service", 6144, this, 4, &worker_) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t DisplayService::SubmitLocal(const MediaId& media_id, const JobId& job_id, JobService* jobs) {
    if (!queue_ || !jobs || media_id.empty() || job_id.empty() || media_id.size() >= 65 || job_id.size() >= 65) return ESP_ERR_INVALID_ARG;
    if (snapshot_.state == DisplayState::kQueued || snapshot_.state == DisplayState::kLoading ||
        snapshot_.state == DisplayState::kRefreshing || snapshot_.state == DisplayState::kFinalizing) return ESP_ERR_INVALID_STATE;
    WorkItem item{}; std::strncpy(item.media_id, media_id.c_str(), sizeof(item.media_id) - 1); std::strncpy(item.job_id, job_id.c_str(), sizeof(item.job_id) - 1);
    jobs_ = jobs;
    snapshot_.state = DisplayState::kQueued; snapshot_.queued_target_media_id = media_id;
    return xQueueOverwrite(queue_, &item) == pdPASS ? ESP_OK : ESP_FAIL;
}
void DisplayService::WorkerEntry(void* context) { static_cast<DisplayService*>(context)->WorkerLoop(); }
void DisplayService::WorkerLoop() {
    WorkItem item{};
    while (true) {
        xQueueReceive(queue_, &item, portMAX_DELAY);
        const MediaId media_id(item.media_id); const JobId job_id(item.job_id); snapshot_.state = DisplayState::kLoading;
        if (jobs_) (void)jobs_->Update(job_id, JobState::kRunning, "loading", 15);
        if (library_->ValidateFrameForDisplay(media_id) != ESP_OK || xSemaphoreTake(legacy_mutex_, portMAX_DELAY) != pdTRUE) {
            snapshot_.state = DisplayState::kFailed; snapshot_.last_error_code = "media_invalid"; if (jobs_) (void)jobs_->Update(job_id, JobState::kFailed, "failed", 0, "media_invalid"); continue;
        }
        // Product local-album mode does not start the legacy button task that
        // normally initializes the panel. Initialize lazily under the same
        // display lock before sending a frame; repeated calls are harmless in
        // the official BSP.
        display_->EPD_Init();
        esp_err_t result = storage_->ReadCommittedFile("media/" + media_id + "/image.bin", display_->EPD_GetIMGBuffer(), kDisplayFrameBytes);
        bool refresh_indicator_active = false;
        if (result == ESP_OK) {
            snapshot_.state = DisplayState::kRefreshing;
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
            result = display_->EPD_DisplayWithTimeout(40000) ? ESP_OK : ESP_FAIL;
        }
        if (refresh_indicator_active) GetIndicatorService().SetRefreshActive(false);
        xSemaphoreGive(legacy_mutex_);
        if (result == ESP_OK) { snapshot_.state = DisplayState::kSuccess; snapshot_.current_media_id = media_id; snapshot_.last_successful_media_id = media_id; snapshot_.last_error_code.clear(); if (jobs_) (void)jobs_->CompleteSuccess(job_id, media_id); }
        else { snapshot_.state = DisplayState::kFailed; snapshot_.last_error_code = result == ESP_ERR_TIMEOUT ? "display_timeout" : "display_failed"; if (jobs_) (void)jobs_->Update(job_id, result == ESP_ERR_TIMEOUT ? JobState::kTimeout : JobState::kFailed, "failed", 0, snapshot_.last_error_code); }
    }
}
}  // namespace photopainter::product
