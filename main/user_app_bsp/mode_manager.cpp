#include "mode_manager.h"
#include "esp_timer.h"
#include "display_service.h"
#include "job_service.h"
namespace photopainter::product {
ModeManager::ModeManager() { mutex_ = xSemaphoreCreateMutex(); }
ModeManager::~ModeManager() { if (mutex_ != nullptr) vSemaphoreDelete(mutex_); }

EpochMs ModeManager::NowMs() { return static_cast<EpochMs>(esp_timer_get_time() / 1000); }

void ModeManager::Initialize(Feature feature) {
    if (mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    snapshot_ = {};
    snapshot_.active_feature = feature;
    snapshot_.current_content_owner = feature;
    snapshot_.current_content_category = feature == Feature::kAiAlbum ? MediaCategory::kAi :
        (feature == Feature::kInfoDashboard ? MediaCategory::kDashboard : MediaCategory::kLocal);
    snapshot_.revision = 1;
    snapshot_.updated_at_ms = NowMs();
    jobs_ = nullptr;
    xSemaphoreGive(mutex_);
}

esp_err_t ModeManager::SetActiveFeature(Feature feature, Revision expected_revision) {
    if (mutex_ == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (expected_revision != snapshot_.revision || snapshot_.state == ModeSnapshot::State::kSwitching) {
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    snapshot_.active_feature = feature;
    ++snapshot_.revision;
    snapshot_.updated_at_ms = NowMs();
    xSemaphoreGive(mutex_);
    return ESP_OK;
}

esp_err_t ModeManager::BeginSwitch(Feature target, Revision expected_revision, const JobId& job_id,
                                   JobService* jobs, DisplayService* display) {
    if (mutex_ == nullptr || jobs == nullptr || display == nullptr || job_id.empty()) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (expected_revision != snapshot_.revision || snapshot_.state == ModeSnapshot::State::kSwitching) {
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    snapshot_.state = ModeSnapshot::State::kSwitching;
    snapshot_.has_pending_feature = true;
    snapshot_.pending_feature = target;
    snapshot_.switch_job_id = job_id;
    snapshot_.updated_at_ms = NowMs();
    jobs_ = jobs;
    xSemaphoreGive(mutex_);

    (void)jobs->Update(job_id, JobState::kRunning, "preparing", 10);
    const esp_err_t submit = display->SubmitModeCover(target, job_id, jobs);
    if (submit != ESP_OK) {
        FailSwitch(job_id, target,
                   submit == ESP_ERR_NOT_FOUND ? "mode_cover_unavailable" : "mode_switch_busy", false);
    }
    return submit;
}

void ModeManager::CompleteSwitch(const JobId& job_id, Feature target, const std::string& system_asset_id) {
    if (mutex_ == nullptr) return;
    JobService* jobs = nullptr;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (snapshot_.state == ModeSnapshot::State::kSwitching && snapshot_.switch_job_id == job_id &&
        snapshot_.has_pending_feature && snapshot_.pending_feature == target) {
        jobs = jobs_;
        if (jobs != nullptr) (void)jobs->Update(job_id, JobState::kRunning, "finalizing", 95);
        snapshot_.active_feature = target;
        snapshot_.state = ModeSnapshot::State::kIdle;
        snapshot_.has_pending_feature = false;
        snapshot_.switch_job_id.clear();
        snapshot_.current_content_kind = ModeSnapshot::ContentKind::kModeCover;
        snapshot_.current_content_owner = target;
        snapshot_.current_content_category = MediaCategory::kSystem;
        snapshot_.current_media_id.clear();
        snapshot_.current_system_asset_id = system_asset_id;
        ++snapshot_.revision;
        snapshot_.updated_at_ms = NowMs();
        jobs_ = nullptr;
    }
    xSemaphoreGive(mutex_);
    if (jobs != nullptr) (void)jobs->CompleteSuccess(job_id);
}

void ModeManager::FailSwitch(const JobId& job_id, Feature target, const std::string& error_code, bool timeout) {
    if (mutex_ == nullptr) return;
    JobService* jobs = nullptr;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (snapshot_.state == ModeSnapshot::State::kSwitching && snapshot_.switch_job_id == job_id &&
        snapshot_.has_pending_feature && snapshot_.pending_feature == target) {
        jobs = jobs_;
        snapshot_.state = ModeSnapshot::State::kIdle;
        snapshot_.has_pending_feature = false;
        snapshot_.switch_job_id.clear();
        snapshot_.updated_at_ms = NowMs();
        jobs_ = nullptr;
    }
    xSemaphoreGive(mutex_);
    if (jobs != nullptr) {
        (void)jobs->Update(job_id, timeout ? JobState::kTimeout : JobState::kFailed,
                           timeout ? "timeout" : "failed", 0, error_code);
    }
}

void ModeManager::RecordDisplayedMedia(Feature owner, MediaCategory category, const MediaId& media_id) {
    if (mutex_ == nullptr || media_id.empty()) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (snapshot_.active_feature == owner && snapshot_.state == ModeSnapshot::State::kIdle) {
        snapshot_.current_content_kind = ModeSnapshot::ContentKind::kMedia;
        snapshot_.current_content_owner = owner;
        snapshot_.current_content_category = category;
        snapshot_.current_media_id = media_id;
        snapshot_.current_system_asset_id.clear();
        snapshot_.updated_at_ms = NowMs();
    }
    xSemaphoreGive(mutex_);
}

ModeSnapshot ModeManager::GetSnapshot() const {
    ModeSnapshot result;
    if (mutex_ == nullptr) return result;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    result = snapshot_;
    xSemaphoreGive(mutex_);
    return result;
}

ModeManager& GetModeManager() { static ModeManager manager; return manager; }
}
