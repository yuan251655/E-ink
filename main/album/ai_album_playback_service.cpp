#include "ai_album_playback_service.h"

#include <algorithm>
#include <cstdio>
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "display_service.h"
#include "job_service.h"
#include "media_library.h"
#include "mode_manager.h"
#include "rtc_service.h"

namespace photopainter::product {
namespace {
constexpr char kNvsNamespace[] = "ai_playback";
constexpr char kModeKey[] = "mode";
constexpr char kIntervalKey[] = "interval";
constexpr char kOrderKey[] = "order";
constexpr char kCurrentKey[] = "current";
constexpr char kRevisionKey[] = "revision";
constexpr char kNextEpochKey[] = "next_epoch";
constexpr std::uint32_t kRetrySeconds = 15;
}

AiAlbumPlaybackService::AiAlbumPlaybackService() { mutex_ = xSemaphoreCreateMutex(); }
AiAlbumPlaybackService::~AiAlbumPlaybackService() {
    if (worker_ != nullptr) vTaskDelete(worker_);
    if (mutex_ != nullptr) vSemaphoreDelete(mutex_);
}
bool AiAlbumPlaybackService::IsAllowedInterval(std::uint32_t seconds) {
    return LocalAlbumPlaybackService::IsAllowedInterval(seconds);
}
esp_err_t AiAlbumPlaybackService::Initialize(MediaLibrary* library, DisplayService* display, JobService* jobs) {
    if (library == nullptr || display == nullptr || jobs == nullptr || mutex_ == nullptr) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (worker_ != nullptr) { xSemaphoreGive(mutex_); return ESP_OK; }
    library_ = library; display_ = display; jobs_ = jobs; LoadPersistedLocked();
    if (snapshot_.config.mode == PlaybackMode::kAuto) {
        std::uint64_t now = 0;
        if (GetRtcService().GetUnixTimeSeconds(&now) && snapshot_.next_play_at_epoch_seconds != 0) {
            snapshot_.next_play_at_ms = snapshot_.next_play_at_epoch_seconds > now
                ? NowMs() + (snapshot_.next_play_at_epoch_seconds - now) * 1000ULL : NowMs();
        } else ScheduleNextLocked(snapshot_.config.interval_seconds);
    }
    // The active album's deadline above is already restored from RTC/NVS.
    // Do not let its first worker tick replace it with a fresh full interval.
    const auto mode = GetModeManager().GetSnapshot();
    ai_mode_was_active_ = mode.active_feature == Feature::kAiAlbum &&
                          mode.state == ModeSnapshot::State::kIdle;
    snapshot_.state_revision = 1;
    const BaseType_t created = xTaskCreate(WorkerEntry, "ai_playback", 4096, this, 3, &worker_);
    xSemaphoreGive(mutex_);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
PlaybackSnapshot AiAlbumPlaybackService::GetSnapshot() const {
    PlaybackSnapshot output; if (mutex_ == nullptr) return output;
    xSemaphoreTake(mutex_, portMAX_DELAY); output = SnapshotLocked(); xSemaphoreGive(mutex_); return output;
}
PlaybackSnapshot AiAlbumPlaybackService::SnapshotLocked() const {
    PlaybackSnapshot output = snapshot_; output.has_next_play = snapshot_.config.mode == PlaybackMode::kAuto;
    output.next_play_in_seconds = 0;
    if (output.has_next_play && snapshot_.next_play_at_ms > NowMs())
        output.next_play_in_seconds = static_cast<std::uint32_t>((snapshot_.next_play_at_ms - NowMs() + 999U) / 1000U);
    return output;
}
esp_err_t AiAlbumPlaybackService::UpdateConfig(PlaybackMode mode, std::uint32_t seconds, PlaybackOrder order, Revision expected, PlaybackSnapshot* output) {
    if (!IsAllowedInterval(seconds) || mutex_ == nullptr) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (expected != snapshot_.config.revision) { if (output) *output = SnapshotLocked(); xSemaphoreGive(mutex_); return ESP_ERR_INVALID_STATE; }
    const PlaybackSnapshot previous = snapshot_; const auto previous_queue = random_queue_;
    snapshot_.config.mode = mode; snapshot_.config.interval_seconds = seconds; snapshot_.config.order = order;
    ++snapshot_.config.revision; ++snapshot_.state_revision; random_queue_.clear();
    if (mode == PlaybackMode::kAuto) ScheduleNextLocked(seconds);
    else { snapshot_.next_play_at_ms = 0; snapshot_.next_play_at_epoch_seconds = 0; }
    const esp_err_t saved = PersistLocked();
    if (saved != ESP_OK) { snapshot_ = previous; random_queue_ = previous_queue; snapshot_.last_error_code = "playback_persist_failed"; if (output) *output = SnapshotLocked(); xSemaphoreGive(mutex_); return saved; }
    if (output) *output = SnapshotLocked();
    xSemaphoreGive(mutex_);
    return ESP_OK;
}
void AiAlbumPlaybackService::NotifyManualDisplaySuccess(const MediaId& id) {
    if (id.empty() || mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    snapshot_.config.current_media_id = id; ++snapshot_.state_revision; random_queue_.clear();
    if (snapshot_.config.mode == PlaybackMode::kAuto) ScheduleNextLocked(snapshot_.config.interval_seconds);
    if (PersistLocked() != ESP_OK) snapshot_.last_error_code = "playback_persist_failed";
    xSemaphoreGive(mutex_);
}
esp_err_t AiAlbumPlaybackService::RequestNext() {
    if (mutex_ == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    HandlePendingCompletionLocked(); ObserveExternalDisplayLocked();
    const auto mode = GetModeManager().GetSnapshot();
    if (mode.active_feature != Feature::kAiAlbum || mode.state != ModeSnapshot::State::kIdle ||
        snapshot_.refresh_pending || manual_next_requested_) {
        xSemaphoreGive(mutex_); return ESP_ERR_INVALID_STATE;
    }
    if (library_ == nullptr || library_->Count(MediaCategory::kAi) <= 1U) {
        xSemaphoreGive(mutex_); return ESP_ERR_NOT_FOUND;
    }
    manual_next_requested_ = true; ++snapshot_.state_revision;
    xSemaphoreGive(mutex_); return ESP_OK;
}
void AiAlbumPlaybackService::NotifyMediaDeleted(const MediaId& id) {
    if (id.empty() || mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    random_queue_.erase(std::remove(random_queue_.begin(), random_queue_.end(), id), random_queue_.end());
    if (snapshot_.pending_media_id != id && snapshot_.config.current_media_id == id) { snapshot_.config.current_media_id.clear(); ++snapshot_.state_revision; if (PersistLocked() != ESP_OK) snapshot_.last_error_code = "playback_persist_failed"; }
    xSemaphoreGive(mutex_);
}
void AiAlbumPlaybackService::WorkerEntry(void* ctx) { static_cast<AiAlbumPlaybackService*>(ctx)->WorkerLoop(); }
void AiAlbumPlaybackService::WorkerLoop() { while (true) { Tick(); vTaskDelay(pdMS_TO_TICKS(1000)); } }
void AiAlbumPlaybackService::Tick() {
    if (mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    HandlePendingCompletionLocked();
    ObserveExternalDisplayLocked();
    const auto mode = GetModeManager().GetSnapshot(); const bool active = mode.active_feature == Feature::kAiAlbum && mode.state == ModeSnapshot::State::kIdle;
    if (!active) { manual_next_requested_ = false; ai_mode_was_active_ = false; xSemaphoreGive(mutex_); return; }
    if (!ai_mode_was_active_) { ai_mode_was_active_ = true; if (snapshot_.config.mode == PlaybackMode::kAuto) ScheduleNextLocked(snapshot_.config.interval_seconds); ++snapshot_.state_revision; }
    const bool manual_next = manual_next_requested_;
    if (snapshot_.refresh_pending || (!manual_next && (snapshot_.config.mode != PlaybackMode::kAuto || NowMs() < snapshot_.next_play_at_ms))) { xSemaphoreGive(mutex_); return; }
    manual_next_requested_ = false;
    const auto media_count = library_ == nullptr ? 0U : library_->Count(MediaCategory::kAi);
    if (media_count <= 1U) {
        // Keep automatic playback enabled but do not repeatedly refresh the
        // same image.  The next normal interval will pick up a newly saved
        // AI image without forcing a noisy retry loop.
        snapshot_.last_error_code = media_count == 0U ? "media_not_found" : "insufficient_media";
        ScheduleNextLocked(snapshot_.config.interval_seconds);
        ++snapshot_.state_revision;
        xSemaphoreGive(mutex_);
        return;
    }
    MediaId target;
    if (!SelectNextLocked(&target)) { snapshot_.last_error_code = "media_not_found"; ScheduleNextLocked(kRetrySeconds); ++snapshot_.state_revision; xSemaphoreGive(mutex_); return; }
    char request[48]; std::snprintf(request, sizeof(request), "ai-playback-%lu", static_cast<unsigned long>(request_sequence_++)); JobSnapshot job;
    const auto registration = jobs_->CreateOrFind(JobKind::kDisplay, request, "ai-playback", &job);
    if (registration != JobRegistrationResult::kCreated || display_->SubmitMedia(Feature::kAiAlbum, MediaCategory::kAi, target, job.job_id, jobs_) != ESP_OK) {
        if (registration == JobRegistrationResult::kCreated) (void)jobs_->Update(job.job_id, JobState::kFailed, "rejected", 0, "display_busy");
        snapshot_.last_error_code = "display_busy"; ScheduleNextLocked(kRetrySeconds); ++snapshot_.state_revision; xSemaphoreGive(mutex_); return;
    }
    snapshot_.refresh_pending = true; snapshot_.pending_media_id = target; pending_job_id_ = job.job_id; snapshot_.last_error_code.clear(); ++snapshot_.state_revision; xSemaphoreGive(mutex_);
}
void AiAlbumPlaybackService::ObserveExternalDisplayLocked() {
    if (display_ == nullptr || snapshot_.refresh_pending) return;
    const auto display = display_->GetSnapshot();
    if (display.state != DisplayState::kSuccess || display.current_media_id.empty() || display.current_media_id == snapshot_.config.current_media_id) return;
    MediaItem item; if (!library_->Find(display.current_media_id, &item) || item.category != MediaCategory::kAi) return;
    snapshot_.config.current_media_id = display.current_media_id; ++snapshot_.state_revision; random_queue_.clear();
    if (snapshot_.config.mode == PlaybackMode::kAuto) ScheduleNextLocked(snapshot_.config.interval_seconds);
    if (PersistLocked() != ESP_OK) snapshot_.last_error_code = "playback_persist_failed";
}
bool AiAlbumPlaybackService::SelectNextLocked(MediaId* output) {
    if (output == nullptr || library_ == nullptr) return false;
    const auto count = library_->Count(MediaCategory::kAi);
    // A one-item library is already stable: refreshing the identical image
    // wastes a full 25-second e-paper cycle without changing the result.
    if (count <= 1) return false;
    const auto items = library_->List(MediaCategory::kAi, 0, count); if (items.empty()) return false;
    if (snapshot_.config.order == PlaybackOrder::kRandom) { if (random_queue_.empty()) RebuildRandomQueueLocked(); while (!random_queue_.empty()) { MediaItem item; if (library_->Find(random_queue_.front(), &item) && item.category == MediaCategory::kAi) { *output = random_queue_.front(); return true; } random_queue_.erase(random_queue_.begin()); } return false; }
    if (snapshot_.config.current_media_id.empty()) { *output = items.front().media_id; return true; }
    MediaItem adjacent; if (!library_->FindAdjacent(MediaCategory::kAi, snapshot_.config.current_media_id, 1, &adjacent)) { *output = items.front().media_id; return true; } *output = adjacent.media_id; return true;
}
void AiAlbumPlaybackService::RebuildRandomQueueLocked() {
    random_queue_.clear(); if (library_ == nullptr) return; const auto items = library_->List(MediaCategory::kAi, 0, library_->Count(MediaCategory::kAi));
    for (const auto& item : items) random_queue_.push_back(item.media_id);
    for (std::size_t i = random_queue_.size(); i > 1; --i) std::swap(random_queue_[i - 1], random_queue_[esp_random() % i]);
    if (random_queue_.size() > 1 && random_queue_.front() == snapshot_.config.current_media_id) std::swap(random_queue_.front(), random_queue_.back());
}
void AiAlbumPlaybackService::HandlePendingCompletionLocked() {
    if (!snapshot_.refresh_pending || jobs_ == nullptr) return;
    JobSnapshot job;
    if (!jobs_->Get(pending_job_id_, &job) || job.state == JobState::kQueued || job.state == JobState::kRunning) return;
    snapshot_.refresh_pending = false; pending_job_id_.clear();
    if (job.state == JobState::kSuccess) { snapshot_.config.current_media_id = snapshot_.pending_media_id; ++snapshot_.state_revision; if (snapshot_.config.order == PlaybackOrder::kRandom && !random_queue_.empty() && random_queue_.front() == snapshot_.pending_media_id) random_queue_.erase(random_queue_.begin()); snapshot_.last_error_code.clear(); ScheduleNextLocked(snapshot_.config.interval_seconds); if (PersistLocked() != ESP_OK) snapshot_.last_error_code = "playback_persist_failed"; }
    else { ++snapshot_.state_revision; snapshot_.last_error_code = job.error_code.empty() ? "display_failed" : job.error_code; ScheduleNextLocked(kRetrySeconds); }
    snapshot_.pending_media_id.clear();
}
void AiAlbumPlaybackService::LoadPersistedLocked() {
    nvs_handle_t h; if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return; std::uint8_t mode = static_cast<std::uint8_t>(PlaybackMode::kPaused), order = static_cast<std::uint8_t>(PlaybackOrder::kSequential); std::uint32_t interval = snapshot_.config.interval_seconds, revision = 0;
    (void)nvs_get_u8(h, kModeKey, &mode); (void)nvs_get_u8(h, kOrderKey, &order); (void)nvs_get_u32(h, kIntervalKey, &interval); (void)nvs_get_u32(h, kRevisionKey, &revision); (void)nvs_get_u64(h, kNextEpochKey, &snapshot_.next_play_at_epoch_seconds); size_t size = 0;
    if (nvs_get_str(h, kCurrentKey, nullptr, &size) == ESP_OK && size > 0 && size <= 65) { std::vector<char> value(size); if (nvs_get_str(h, kCurrentKey, value.data(), &size) == ESP_OK) snapshot_.config.current_media_id = value.data(); } nvs_close(h);
    snapshot_.config.mode = mode == static_cast<std::uint8_t>(PlaybackMode::kAuto) ? PlaybackMode::kAuto : PlaybackMode::kPaused; snapshot_.config.order = order == static_cast<std::uint8_t>(PlaybackOrder::kRandom) ? PlaybackOrder::kRandom : PlaybackOrder::kSequential; snapshot_.config.interval_seconds = IsAllowedInterval(interval) ? interval : 1800; snapshot_.config.revision = revision;
}
esp_err_t AiAlbumPlaybackService::PersistLocked() {
    nvs_handle_t h; esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &h); if (result != ESP_OK) return result;
    result = nvs_set_u8(h, kModeKey, static_cast<std::uint8_t>(snapshot_.config.mode)); if (result == ESP_OK) result = nvs_set_u8(h, kOrderKey, static_cast<std::uint8_t>(snapshot_.config.order)); if (result == ESP_OK) result = nvs_set_u32(h, kIntervalKey, snapshot_.config.interval_seconds); if (result == ESP_OK) result = nvs_set_u32(h, kRevisionKey, static_cast<std::uint32_t>(snapshot_.config.revision)); if (result == ESP_OK) result = nvs_set_u64(h, kNextEpochKey, snapshot_.next_play_at_epoch_seconds); if (result == ESP_OK) result = nvs_set_str(h, kCurrentKey, snapshot_.config.current_media_id.c_str()); if (result == ESP_OK) result = nvs_commit(h); nvs_close(h); return result;
}
EpochMs AiAlbumPlaybackService::NowMs() { return static_cast<EpochMs>(esp_timer_get_time() / 1000); }
void AiAlbumPlaybackService::ScheduleNextLocked(std::uint32_t delay_seconds) {
    snapshot_.next_play_at_ms = NowMs() + static_cast<EpochMs>(delay_seconds) * 1000ULL;
    std::uint64_t now = 0;
    snapshot_.next_play_at_epoch_seconds = GetRtcService().GetUnixTimeSeconds(&now) ? now + delay_seconds : 0;
}
}  // namespace photopainter::product
