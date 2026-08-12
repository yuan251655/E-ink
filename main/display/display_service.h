#pragma once

#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "product_types.h"

class ePaperPort;
namespace photopainter::product { class StorageService; class MediaLibrary;
class JobService;

class DisplayService {
public:
    esp_err_t Initialize(StorageService* storage, MediaLibrary* library, ePaperPort* display, SemaphoreHandle_t legacy_mutex);
    // A display request never writes TF. It runs the validated frame through
    // the sole e-paper worker and records its lifecycle in the shared job.
    esp_err_t SubmitMedia(Feature feature, MediaCategory category, const MediaId& media_id,
                          const JobId& job_id, JobService* jobs);
    esp_err_t SubmitLocal(const MediaId& media_id, const JobId& job_id, JobService* jobs);
    // ModeManager is the only caller allowed to submit a system mode cover.
    // The worker reports completion back to ModeManager, which commits the
    // new active feature only after the physical refresh succeeds.
    esp_err_t SubmitModeCover(Feature feature, const JobId& job_id, JobService* jobs);
    esp_err_t SubmitDashboard(const JobId& job_id, JobService* jobs);
    DisplaySnapshot GetSnapshot() const;
private:
    enum class WorkKind : std::uint8_t { kMedia, kModeCover, kDashboard };
    struct WorkItem {
        WorkKind kind = WorkKind::kMedia;
        Feature feature = Feature::kLocalAlbum;
        MediaCategory category = MediaCategory::kLocal;
        char media_id[65];
        char job_id[65];
    };
    static void WorkerEntry(void* context);
    void WorkerLoop();
    StorageService* storage_ = nullptr; MediaLibrary* library_ = nullptr; ePaperPort* display_ = nullptr;
    JobService* jobs_ = nullptr;
    SemaphoreHandle_t legacy_mutex_ = nullptr; SemaphoreHandle_t state_mutex_ = nullptr;
    QueueHandle_t queue_ = nullptr; TaskHandle_t worker_ = nullptr;
    DisplaySnapshot snapshot_;
};
}  // namespace photopainter::product
