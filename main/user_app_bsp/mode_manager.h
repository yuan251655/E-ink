#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "product_types.h"
namespace photopainter::product {
class DisplayService;
class JobService;

class ModeManager {
public:
    ModeManager();
    ~ModeManager();
    ModeManager(const ModeManager&) = delete;
    ModeManager& operator=(const ModeManager&) = delete;

    void Initialize(Feature feature);
    esp_err_t SetActiveFeature(Feature feature, Revision expected_revision);
    esp_err_t BeginSwitch(Feature target, Revision expected_revision, const JobId& job_id,
                          JobService* jobs, DisplayService* display);
    void CompleteSwitch(const JobId& job_id, Feature target, const std::string& system_asset_id);
    void FailSwitch(const JobId& job_id, Feature target, const std::string& error_code, bool timeout);
    void RecordDisplayedMedia(Feature owner, MediaCategory category, const MediaId& media_id);
    ModeSnapshot GetSnapshot() const;

private:
    static EpochMs NowMs();
    mutable SemaphoreHandle_t mutex_ = nullptr;
    ModeSnapshot snapshot_;
    JobService* jobs_ = nullptr;
};
ModeManager& GetModeManager();
}
