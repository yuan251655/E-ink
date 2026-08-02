#include "mode_manager.h"
#include <cstdint>
#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "media_library.h"
#include "display_service.h"
#include "device_log_service.h"
#include "job_service.h"
namespace photopainter::product {
namespace {
constexpr const char* kTag = "mode_manager";
// Keep product runtime state isolated from the official legacy Mode_Flag and
// PhotPainterMode keys, which deliberately remain in the PhotoPainter NVS
// namespace for compatibility.
constexpr const char* kNvsNamespace = "product_runtime";
constexpr const char* kNvsRuntimeVersion = "ProdRtVer";
constexpr const char* kNvsActiveFeature = "ProdActive";
constexpr const char* kNvsContentKind = "ProdContK";
constexpr const char* kNvsContentOwner = "ProdContO";
constexpr const char* kNvsContentCategory = "ProdContC";
constexpr const char* kNvsMediaId = "ProdMedia";
constexpr const char* kNvsSystemAssetId = "ProdAsset";
constexpr const char* kNvsRuntimeCheck = "ProdRtCk";
constexpr std::uint8_t kRuntimeVersion = 1;
constexpr std::uint8_t kFeatureCheckSeed = 0xA5;
constexpr std::size_t kMaxPersistedIdLength = 64;

std::uint8_t EncodeFeature(Feature feature) {
    return static_cast<std::uint8_t>(feature);
}

std::uint8_t SnapshotCheck(const ModeSnapshot& snapshot) {
    std::uint8_t check = kFeatureCheckSeed ^ kRuntimeVersion ^ EncodeFeature(snapshot.active_feature) ^
                         static_cast<std::uint8_t>(snapshot.current_content_kind) ^
                         EncodeFeature(snapshot.current_content_owner) ^
                         static_cast<std::uint8_t>(snapshot.current_content_category);
    for (const char value : snapshot.current_media_id) check ^= static_cast<std::uint8_t>(value);
    for (const char value : snapshot.current_system_asset_id) check ^= static_cast<std::uint8_t>(value);
    return check;
}

bool ReadString(nvs_handle_t handle, const char* key, std::string* output) {
    if (output == nullptr) return false;
    char value[kMaxPersistedIdLength + 1]{};
    size_t size = sizeof(value);
    if (nvs_get_str(handle, key, value, &size) != ESP_OK || size == 0 || size > sizeof(value)) return false;
    *output = value;
    return true;
}
}  // namespace

ModeManager::ModeManager() { mutex_ = xSemaphoreCreateMutex(); }
ModeManager::~ModeManager() { if (mutex_ != nullptr) vSemaphoreDelete(mutex_); }

EpochMs ModeManager::NowMs() { return static_cast<EpochMs>(esp_timer_get_time() / 1000); }

void ModeManager::Initialize(Feature fallback_feature, MediaLibrary* media_library) {
    if (mutex_ == nullptr) return;
    if (!IsPersistableFeature(fallback_feature)) fallback_feature = Feature::kLocalAlbum;
    ModeSnapshot restored;
    const bool has_persisted_snapshot = LoadPersistedSnapshot(fallback_feature, &restored);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    snapshot_ = has_persisted_snapshot ? restored : ModeSnapshot{};
    if (!has_persisted_snapshot) {
        snapshot_.active_feature = fallback_feature;
        snapshot_.current_content_owner = fallback_feature;
        snapshot_.current_content_category = fallback_feature == Feature::kAiAlbum ? MediaCategory::kAi :
            (fallback_feature == Feature::kInfoDashboard ? MediaCategory::kDashboard : MediaCategory::kLocal);
        snapshot_.current_content_kind = ModeSnapshot::ContentKind::kUnknown;
    }
    // A reboot never restores a queued/running job.  The persisted format has
    // no pending job fields, and this explicitly re-establishes that boundary.
    snapshot_.state = ModeSnapshot::State::kIdle;
    snapshot_.has_pending_feature = false;
    snapshot_.switch_job_id.clear();
    const bool sanitized = SanitizeRestoredSnapshotLocked(media_library);
    snapshot_.revision = 1;
    snapshot_.updated_at_ms = NowMs();
    jobs_ = nullptr;
    const ModeSnapshot sanitized_snapshot = snapshot_;
    xSemaphoreGive(mutex_);
    // Persist a one-time safety repair for deleted/corrupt media.  This is not
    // a new display operation: it merely removes a stale reference so every
    // subsequent boot reports the same safe mode-cover state.
    if (sanitized && PersistSnapshot(sanitized_snapshot) != ESP_OK) {
        ESP_LOGW(kTag, "Unable to persist restored-media safety fallback");
    }
    if (has_persisted_snapshot) {
        ESP_LOGI(kTag, "Restored completed product runtime state");
    } else {
        ESP_LOGI(kTag, "No valid product runtime state; using local album fallback");
    }
}

bool ModeManager::IsPersistableFeature(Feature feature) {
    return feature == Feature::kLocalAlbum || feature == Feature::kAiAlbum ||
           feature == Feature::kInfoDashboard;
}

bool ModeManager::IsValidSnapshot(const ModeSnapshot& snapshot) {
    if (!IsPersistableFeature(snapshot.active_feature) || snapshot.state != ModeSnapshot::State::kIdle ||
        snapshot.has_pending_feature || !snapshot.switch_job_id.empty() ||
        snapshot.current_content_owner != snapshot.active_feature) return false;
    if (snapshot.current_content_kind == ModeSnapshot::ContentKind::kUnknown) return true;
    if (snapshot.current_content_kind == ModeSnapshot::ContentKind::kModeCover) {
        return snapshot.current_content_category == MediaCategory::kSystem && snapshot.current_media_id.empty() &&
               !snapshot.current_system_asset_id.empty();
    }
    if (snapshot.current_content_kind == ModeSnapshot::ContentKind::kMedia) {
        const bool category_matches =
            (snapshot.active_feature == Feature::kLocalAlbum && snapshot.current_content_category == MediaCategory::kLocal) ||
            (snapshot.active_feature == Feature::kAiAlbum && snapshot.current_content_category == MediaCategory::kAi);
        return category_matches && !snapshot.current_media_id.empty() && snapshot.current_system_asset_id.empty();
    }
    return snapshot.current_content_kind == ModeSnapshot::ContentKind::kDashboard &&
           snapshot.active_feature == Feature::kInfoDashboard &&
           snapshot.current_content_category == MediaCategory::kDashboard;
}

bool ModeManager::LoadPersistedSnapshot(Feature fallback_feature, ModeSnapshot* output) {
    if (output == nullptr) return false;
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    std::uint8_t version = 0;
    std::uint8_t active = 0;
    std::uint8_t kind = 0;
    std::uint8_t owner = 0;
    std::uint8_t category = 0;
    std::uint8_t check = 0;
    const bool values_ok = nvs_get_u8(handle, kNvsRuntimeVersion, &version) == ESP_OK &&
                           nvs_get_u8(handle, kNvsActiveFeature, &active) == ESP_OK &&
                           nvs_get_u8(handle, kNvsContentKind, &kind) == ESP_OK &&
                           nvs_get_u8(handle, kNvsContentOwner, &owner) == ESP_OK &&
                           nvs_get_u8(handle, kNvsContentCategory, &category) == ESP_OK &&
                           nvs_get_u8(handle, kNvsRuntimeCheck, &check) == ESP_OK;
    ModeSnapshot restored;
    if (values_ok) {
        restored.active_feature = static_cast<Feature>(active);
        restored.current_content_kind = static_cast<ModeSnapshot::ContentKind>(kind);
        restored.current_content_owner = static_cast<Feature>(owner);
        restored.current_content_category = static_cast<MediaCategory>(category);
        bool strings_ok = ReadString(handle, kNvsMediaId, &restored.current_media_id) &&
                          ReadString(handle, kNvsSystemAssetId, &restored.current_system_asset_id);
        nvs_close(handle);
        if (version == kRuntimeVersion && strings_ok && IsValidSnapshot(restored) && check == SnapshotCheck(restored)) {
            *output = restored;
            return true;
        }
        return false;
    }
    nvs_close(handle);
    (void)fallback_feature;
    return false;
}

esp_err_t ModeManager::PersistSnapshot(const ModeSnapshot& snapshot) {
    if (!IsValidSnapshot(snapshot) || snapshot.current_media_id.size() > kMaxPersistedIdLength ||
        snapshot.current_system_asset_id.size() > kMaxPersistedIdLength) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_u8(handle, kNvsRuntimeVersion, kRuntimeVersion);
    if (result == ESP_OK) result = nvs_set_u8(handle, kNvsActiveFeature, EncodeFeature(snapshot.active_feature));
    if (result == ESP_OK) result = nvs_set_u8(handle, kNvsContentKind, static_cast<std::uint8_t>(snapshot.current_content_kind));
    if (result == ESP_OK) result = nvs_set_u8(handle, kNvsContentOwner, EncodeFeature(snapshot.current_content_owner));
    if (result == ESP_OK) result = nvs_set_u8(handle, kNvsContentCategory, static_cast<std::uint8_t>(snapshot.current_content_category));
    if (result == ESP_OK) result = nvs_set_str(handle, kNvsMediaId, snapshot.current_media_id.c_str());
    if (result == ESP_OK) result = nvs_set_str(handle, kNvsSystemAssetId, snapshot.current_system_asset_id.c_str());
    if (result == ESP_OK) result = nvs_set_u8(handle, kNvsRuntimeCheck, SnapshotCheck(snapshot));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

bool ModeManager::SanitizeRestoredSnapshotLocked(MediaLibrary* media_library) {
    if (snapshot_.current_content_kind != ModeSnapshot::ContentKind::kMedia || media_library == nullptr) return false;
    MediaItem item;
    const bool valid = media_library->Find(snapshot_.current_media_id, &item) &&
        item.category == snapshot_.current_content_category &&
        media_library->ValidateFrameForDisplay(snapshot_.current_media_id) == ESP_OK;
    if (valid) return false;

    // The panel retains its prior pixels through power-off, but an invalid TF
    // reference must not leak through API state.  Represent the corresponding
    // built-in cover instead; this is a safe in-memory fallback and does not
    // rewrite NVS during boot.
    snapshot_.current_content_kind = ModeSnapshot::ContentKind::kModeCover;
    snapshot_.current_content_category = MediaCategory::kSystem;
    snapshot_.current_media_id.clear();
    switch (snapshot_.active_feature) {
        case Feature::kAiAlbum: snapshot_.current_system_asset_id = "mode_cover_ai_album"; break;
        case Feature::kInfoDashboard: snapshot_.current_system_asset_id = "mode_cover_info_dashboard"; break;
        case Feature::kLocalAlbum:
        default: snapshot_.current_system_asset_id = "mode_cover_local_album"; break;
    }
    GetDeviceLogService().Add(DeviceLogSeverity::kWarning, "mode", "restored_media_invalid",
                              "Saved media was unavailable; using the mode cover state");
    return true;
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

    GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "mode", "switch_queued",
                              "Mode cover refresh has been queued");
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
    ModeSnapshot committed;
    bool completed = false;
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
        committed = snapshot_;
        completed = true;
    }
    xSemaphoreGive(mutex_);
    // A physical display success is the persistence boundary. Failed, timed
    // out, queued, and interrupted switches deliberately leave NVS untouched.
    if (jobs != nullptr && completed) {
        const esp_err_t persisted = PersistSnapshot(committed);
        if (persisted != ESP_OK) {
            ESP_LOGW(kTag, "Mode switch succeeded but NVS persistence failed: %s", esp_err_to_name(persisted));
            GetDeviceLogService().Add(DeviceLogSeverity::kWarning, "mode", "persist_failed",
                                      "Mode is active but may not survive a reboot");
        }
    }
    if (jobs != nullptr) {
        GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "mode", "switch_completed",
                                  "Mode cover was refreshed and the new mode is active");
        (void)jobs->CompleteSuccess(job_id);
    }
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
        GetDeviceLogService().Add(timeout ? DeviceLogSeverity::kWarning : DeviceLogSeverity::kError,
                                  "mode", timeout ? "switch_display_timeout" : "switch_failed",
                                  timeout ? "Mode cover refresh timed out" : "Mode cover refresh failed");
        (void)jobs->Update(job_id, timeout ? JobState::kTimeout : JobState::kFailed,
                           timeout ? "timeout" : "failed", 0, error_code);
    }
}

void ModeManager::RecordDisplayedMedia(Feature owner, MediaCategory category, const MediaId& media_id) {
    if (mutex_ == nullptr || media_id.empty()) return;
    bool committed = false;
    ModeSnapshot persisted;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (snapshot_.active_feature == owner && snapshot_.state == ModeSnapshot::State::kIdle) {
        snapshot_.current_content_kind = ModeSnapshot::ContentKind::kMedia;
        snapshot_.current_content_owner = owner;
        snapshot_.current_content_category = category;
        snapshot_.current_media_id = media_id;
        snapshot_.current_system_asset_id.clear();
        snapshot_.updated_at_ms = NowMs();
        persisted = snapshot_;
        committed = true;
    }
    xSemaphoreGive(mutex_);
    if (committed) {
        const esp_err_t result = PersistSnapshot(persisted);
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "Displayed media is active but runtime persistence failed: %s", esp_err_to_name(result));
            GetDeviceLogService().Add(DeviceLogSeverity::kWarning, "mode", "persist_failed",
                                      "Displayed media may not survive a reboot");
        }
    }
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
