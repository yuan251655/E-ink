#include "local_album_playback_service.h"

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
#include "voice_announcement_service.h"

namespace photopainter::product {
namespace {

constexpr char kNvsNamespace[] = "local_playback";
constexpr char kModeKey[] = "mode";
constexpr char kIntervalKey[] = "interval";
constexpr char kOrderKey[] = "order";
constexpr char kCurrentKey[] = "current";
constexpr char kRevisionKey[] = "revision";
constexpr char kNextEpochKey[] = "next_epoch";
constexpr std::uint32_t kRetrySeconds = 15;

}  // namespace

LocalAlbumPlaybackService::LocalAlbumPlaybackService() {
    mutex_ = xSemaphoreCreateMutex();
}

LocalAlbumPlaybackService::~LocalAlbumPlaybackService() {
    if (worker_ != nullptr) vTaskDelete(worker_);
    if (mutex_ != nullptr) vSemaphoreDelete(mutex_);
}

bool LocalAlbumPlaybackService::IsAllowedInterval(std::uint32_t seconds) {
    switch (seconds) {
        case 5 * 60: case 15 * 60: case 30 * 60: case 60 * 60:
        case 3 * 60 * 60: case 6 * 60 * 60: case 12 * 60 * 60: case 24 * 60 * 60:
            return true;
        default:
            return false;
    }
}

esp_err_t LocalAlbumPlaybackService::Initialize(MediaLibrary* library, DisplayService* display, JobService* jobs) {
    if (library == nullptr || display == nullptr || jobs == nullptr || mutex_ == nullptr) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (worker_ != nullptr) {
        xSemaphoreGive(mutex_);
        return ESP_OK;
    }
    library_ = library;
    display_ = display;
    jobs_ = jobs;
    LoadPersistedLocked();
    // esp_timer is monotonic only for this boot.  In always-on mode a reboot
    // starts a fresh interval from boot rather than pretending a wall-clock
    // deadline survived without RTC/NTP participation.
    if (snapshot_.config.mode == PlaybackMode::kAuto) {
        std::uint64_t now = 0;
        if (GetRtcService().GetUnixTimeSeconds(&now) && snapshot_.next_play_at_epoch_seconds != 0) {
            snapshot_.next_play_at_ms = snapshot_.next_play_at_epoch_seconds > now
                ? NowMs() + (snapshot_.next_play_at_epoch_seconds - now) * 1000ULL : NowMs();
        } else ScheduleNextLocked(snapshot_.config.interval_seconds);
    }
    // The active album's deadline above is already restored from RTC/NVS.
    // Do not let its first worker tick replace it with a fresh full interval.
    const ModeSnapshot mode = GetModeManager().GetSnapshot();
    local_mode_was_active_ = mode.active_feature == Feature::kLocalAlbum &&
                             mode.state == ModeSnapshot::State::kIdle;
    snapshot_.state_revision = 1;
    const BaseType_t created = xTaskCreate(WorkerEntry, "album_playback", 4096, this, 3, &worker_);
    xSemaphoreGive(mutex_);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

PlaybackSnapshot LocalAlbumPlaybackService::GetSnapshot() const {
    PlaybackSnapshot output;
    if (mutex_ == nullptr) return output;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    output = SnapshotLocked();
    xSemaphoreGive(mutex_);
    return output;
}

PlaybackSnapshot LocalAlbumPlaybackService::SnapshotLocked() const {
    PlaybackSnapshot output = snapshot_;
    output.has_next_play = snapshot_.config.mode == PlaybackMode::kAuto;
    output.next_play_in_seconds = 0;
    if (output.has_next_play && snapshot_.next_play_at_ms > NowMs()) {
        const EpochMs remaining_ms = snapshot_.next_play_at_ms - NowMs();
        // Round upward so a client does not announce a switch before the
        // worker can actually submit it on the next one-second tick.
        output.next_play_in_seconds = static_cast<std::uint32_t>((remaining_ms + 999U) / 1000U);
    }
    return output;
}

esp_err_t LocalAlbumPlaybackService::UpdateConfig(PlaybackMode mode, std::uint32_t interval_seconds,
                                                   PlaybackOrder order, Revision expected_revision,
                                                   PlaybackSnapshot* output) {
    if (!IsAllowedInterval(interval_seconds) || (mode != PlaybackMode::kAuto && mode != PlaybackMode::kPaused) ||
        (order != PlaybackOrder::kSequential && order != PlaybackOrder::kRandom) || mutex_ == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (expected_revision != snapshot_.config.revision) {
        if (output != nullptr) *output = SnapshotLocked();
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    const LocalAlbumPlaybackSnapshot previous = snapshot_;
    const std::vector<MediaId> previous_random_queue = random_queue_;
    snapshot_.config.mode = mode;
    snapshot_.config.interval_seconds = interval_seconds;
    snapshot_.config.order = order;
    ++snapshot_.config.revision;
    ++snapshot_.state_revision;
    random_queue_.clear();
    if (mode == PlaybackMode::kAuto) ScheduleNextLocked(interval_seconds);
    else { snapshot_.next_play_at_ms = 0; snapshot_.next_play_at_epoch_seconds = 0; }
    const esp_err_t persisted = PersistLocked();
    if (persisted != ESP_OK) {
        snapshot_ = previous;
        random_queue_ = previous_random_queue;
        snapshot_.last_error_code = "playback_persist_failed";
        if (output != nullptr) *output = SnapshotLocked();
        xSemaphoreGive(mutex_);
        return persisted;
    }
    if (output != nullptr) *output = SnapshotLocked();
    xSemaphoreGive(mutex_);
    return ESP_OK;
}

void LocalAlbumPlaybackService::NotifyManualDisplaySuccess(const MediaId& media_id) {
    if (media_id.empty() || mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    snapshot_.config.current_media_id = media_id;
    ++snapshot_.state_revision;
    random_queue_.clear();
    if (snapshot_.config.mode == PlaybackMode::kAuto) ScheduleNextLocked(snapshot_.config.interval_seconds);
    if (PersistLocked() != ESP_OK) snapshot_.last_error_code = "playback_persist_failed";
    xSemaphoreGive(mutex_);
}

void LocalAlbumPlaybackService::NotifyTemporarySystemDisplaySuccess() {
    if (mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (snapshot_.config.mode == PlaybackMode::kAuto) {
        ScheduleNextLocked(snapshot_.config.interval_seconds);
        ++snapshot_.state_revision;
        if (PersistLocked() != ESP_OK) snapshot_.last_error_code = "playback_persist_failed";
    }
    xSemaphoreGive(mutex_);
}

esp_err_t LocalAlbumPlaybackService::RequestNext(bool announce_completion) {
    if (mutex_ == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    HandlePendingCompletionLocked();
    ObserveExternalDisplayLocked();
    const auto mode = GetModeManager().GetSnapshot();
    if (mode.active_feature != Feature::kLocalAlbum || mode.state != ModeSnapshot::State::kIdle ||
        snapshot_.refresh_pending || manual_next_requested_) {
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    if (library_ == nullptr || library_->Count(MediaCategory::kLocal) <= 1U) {
        xSemaphoreGive(mutex_);
        return ESP_ERR_NOT_FOUND;
    }
    manual_next_requested_ = true;
    announce_manual_next_ = announce_completion;
    ++snapshot_.state_revision;
    xSemaphoreGive(mutex_);
    return ESP_OK;
}

void LocalAlbumPlaybackService::TriggerScheduledWake() {
    if (mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (snapshot_.config.mode == PlaybackMode::kAuto) {
        snapshot_.next_play_at_ms = NowMs();
        ++snapshot_.state_revision;
    }
    xSemaphoreGive(mutex_);
}

void LocalAlbumPlaybackService::NotifyMediaDeleted(const MediaId& media_id) {
    if (media_id.empty() || mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    random_queue_.erase(std::remove(random_queue_.begin(), random_queue_.end(), media_id), random_queue_.end());
    if (snapshot_.pending_media_id == media_id) {
        // DeleteMedia rejects the target of a queued refresh, but retain this
        // guard for callers outside the HTTP adapter.
        xSemaphoreGive(mutex_);
        return;
    }
    if (snapshot_.config.current_media_id == media_id) {
        snapshot_.config.current_media_id.clear();
        ++snapshot_.state_revision;
        if (PersistLocked() != ESP_OK) snapshot_.last_error_code = "playback_persist_failed";
    }
    xSemaphoreGive(mutex_);
}

void LocalAlbumPlaybackService::WorkerEntry(void* context) {
    static_cast<LocalAlbumPlaybackService*>(context)->WorkerLoop();
}

void LocalAlbumPlaybackService::WorkerLoop() {
    while (true) {
        Tick();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void LocalAlbumPlaybackService::Tick() {
    if (mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    HandlePendingCompletionLocked();
    ObserveExternalDisplayLocked();
    const ModeSnapshot mode = GetModeManager().GetSnapshot();
    const bool local_mode_active = mode.active_feature == Feature::kLocalAlbum &&
                                   mode.state == ModeSnapshot::State::kIdle;
    if (!local_mode_active) {
        manual_next_requested_ = false;
        announce_manual_next_ = false;
        local_mode_was_active_ = false;
        xSemaphoreGive(mutex_);
        return;
    }
    if (!local_mode_was_active_) {
        // A mode cover must remain visible for one complete configured
        // interval after switching back to local album. Do not immediately
        // replace it merely because the old countdown expired while another
        // feature was active.
        local_mode_was_active_ = true;
        if (snapshot_.config.mode == PlaybackMode::kAuto) ScheduleNextLocked(snapshot_.config.interval_seconds);
        ++snapshot_.state_revision;
    }
    const bool manual_next = manual_next_requested_;
    const bool announce_completion = manual_next && announce_manual_next_;
    if (snapshot_.refresh_pending ||
        (!manual_next && (snapshot_.config.mode != PlaybackMode::kAuto || NowMs() < snapshot_.next_play_at_ms))) {
        xSemaphoreGive(mutex_);
        return;
    }
    manual_next_requested_ = false;
    announce_manual_next_ = false;

    MediaId target;
    if (!SelectNextLocked(&target)) {
        if (announce_completion) (void)GetVoiceAnnouncementService().Enqueue(VoiceAnnouncement::kNextFailed);
        snapshot_.last_error_code = "media_not_found";
        ScheduleNextLocked(kRetrySeconds);
        ++snapshot_.state_revision;
        xSemaphoreGive(mutex_);
        return;
    }

    char request[48];
    std::snprintf(request, sizeof(request), "playback-%lu", static_cast<unsigned long>(request_sequence_++));
    JobSnapshot job;
    const auto registration = jobs_->CreateOrFind(JobKind::kDisplay, request, "local-playback", &job);
    if (registration != JobRegistrationResult::kCreated || display_->SubmitLocal(target, job.job_id, jobs_) != ESP_OK) {
        if (registration == JobRegistrationResult::kCreated) {
            (void)jobs_->Update(job.job_id, JobState::kFailed, "rejected", 0, "display_busy");
        }
        snapshot_.last_error_code = "display_busy";
        if (announce_completion) (void)GetVoiceAnnouncementService().Enqueue(VoiceAnnouncement::kNextFailed);
        ScheduleNextLocked(kRetrySeconds);
        ++snapshot_.state_revision;
        xSemaphoreGive(mutex_);
        return;
    }
    snapshot_.refresh_pending = true;
    snapshot_.pending_media_id = target;
    pending_job_id_ = job.job_id;
    if (announce_completion) {
        (void)GetVoiceAnnouncementService().WatchJob(
            job.job_id, VoiceAnnouncement::kNextSuccess, VoiceAnnouncement::kNextFailed);
    }
    snapshot_.last_error_code.clear();
    ++snapshot_.state_revision;
    xSemaphoreGive(mutex_);
}

void LocalAlbumPlaybackService::ObserveExternalDisplayLocked() {
    if (display_ == nullptr || snapshot_.refresh_pending) return;
    const DisplaySnapshot display = display_->GetSnapshot();
    // A user-requested display is completed by DisplayService outside this
    // service. Adopt it only after its physical refresh succeeds, so auto
    // playback restarts from the image the user actually saw. In paused mode
    // this updates the remembered current image without scheduling a timer.
    if (display.state != DisplayState::kSuccess || display.current_media_id.empty() ||
        display.current_media_id == snapshot_.config.current_media_id) return;
    MediaItem item;
    if (!library_->Find(display.current_media_id, &item) || item.category != MediaCategory::kLocal) return;
    snapshot_.config.current_media_id = display.current_media_id;
    ++snapshot_.state_revision;
    random_queue_.clear();
    if (snapshot_.config.mode == PlaybackMode::kAuto) ScheduleNextLocked(snapshot_.config.interval_seconds);
    if (PersistLocked() != ESP_OK) snapshot_.last_error_code = "playback_persist_failed";
}

bool LocalAlbumPlaybackService::SelectNextLocked(MediaId* output) {
    if (output == nullptr || library_ == nullptr) return false;
    const std::size_t count = library_->Count(MediaCategory::kLocal);
    if (count <= 1) return false;
    const auto items = library_->List(MediaCategory::kLocal, 0, count);
    if (items.empty()) return false;
    if (snapshot_.config.order == PlaybackOrder::kRandom) {
        if (random_queue_.empty()) RebuildRandomQueueLocked();
        // A deletion can happen after the queue was shuffled. Skip stale IDs
        // instead of repeatedly submitting a permanently invalid display job.
        while (!random_queue_.empty()) {
            MediaItem candidate;
            if (library_->Find(random_queue_.front(), &candidate) && candidate.category == MediaCategory::kLocal) {
                *output = random_queue_.front();
                return true;
            }
            random_queue_.erase(random_queue_.begin());
        }
        return false;
    }
    if (snapshot_.config.current_media_id.empty()) {
        *output = items.front().media_id;
        return true;
    }
    MediaItem adjacent;
    if (!library_->FindAdjacent(MediaCategory::kLocal, snapshot_.config.current_media_id, 1, &adjacent)) {
        *output = items.front().media_id;
        return true;
    }
    *output = adjacent.media_id;
    return true;
}

void LocalAlbumPlaybackService::RebuildRandomQueueLocked() {
    random_queue_.clear();
    if (library_ == nullptr) return;
    const std::size_t count = library_->Count(MediaCategory::kLocal);
    const auto items = library_->List(MediaCategory::kLocal, 0, count);
    for (const auto& item : items) random_queue_.push_back(item.media_id);
    for (std::size_t index = random_queue_.size(); index > 1; --index) {
        std::swap(random_queue_[index - 1], random_queue_[esp_random() % index]);
    }
    // Avoid an immediate repeat when there is another candidate.
    if (random_queue_.size() > 1 && random_queue_.front() == snapshot_.config.current_media_id) {
        std::swap(random_queue_.front(), random_queue_.back());
    }
}

void LocalAlbumPlaybackService::HandlePendingCompletionLocked() {
    if (!snapshot_.refresh_pending || jobs_ == nullptr) return;
    JobSnapshot job;
    if (!jobs_->Get(pending_job_id_, &job)) return;
    if (job.state == JobState::kQueued || job.state == JobState::kRunning) return;

    snapshot_.refresh_pending = false;
    pending_job_id_.clear();
    if (job.state == JobState::kSuccess) {
        snapshot_.config.current_media_id = snapshot_.pending_media_id;
        ++snapshot_.state_revision;
        if (snapshot_.config.order == PlaybackOrder::kRandom && !random_queue_.empty() &&
            random_queue_.front() == snapshot_.pending_media_id) {
            random_queue_.erase(random_queue_.begin());
        }
        snapshot_.last_error_code.clear();
        ScheduleNextLocked(snapshot_.config.interval_seconds);
        if (PersistLocked() != ESP_OK) snapshot_.last_error_code = "playback_persist_failed";
    } else {
        ++snapshot_.state_revision;
        snapshot_.last_error_code = job.error_code.empty() ? "display_failed" : job.error_code;
        ScheduleNextLocked(kRetrySeconds);
    }
    snapshot_.pending_media_id.clear();
}

void LocalAlbumPlaybackService::LoadPersistedLocked() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    std::uint8_t mode = static_cast<std::uint8_t>(PlaybackMode::kPaused);
    std::uint8_t order = static_cast<std::uint8_t>(PlaybackOrder::kSequential);
    std::uint32_t interval = snapshot_.config.interval_seconds;
    std::uint32_t revision = 0;
    (void)nvs_get_u8(handle, kModeKey, &mode);
    (void)nvs_get_u8(handle, kOrderKey, &order);
    (void)nvs_get_u32(handle, kIntervalKey, &interval);
    (void)nvs_get_u32(handle, kRevisionKey, &revision);
    (void)nvs_get_u64(handle, kNextEpochKey, &snapshot_.next_play_at_epoch_seconds);
    size_t current_size = 0;
    if (nvs_get_str(handle, kCurrentKey, nullptr, &current_size) == ESP_OK && current_size > 0 && current_size <= 65) {
        std::vector<char> current(current_size);
        if (nvs_get_str(handle, kCurrentKey, current.data(), &current_size) == ESP_OK) snapshot_.config.current_media_id = current.data();
    }
    nvs_close(handle);
    snapshot_.config.mode = mode == static_cast<std::uint8_t>(PlaybackMode::kAuto) ? PlaybackMode::kAuto : PlaybackMode::kPaused;
    snapshot_.config.order = order == static_cast<std::uint8_t>(PlaybackOrder::kRandom) ? PlaybackOrder::kRandom : PlaybackOrder::kSequential;
    snapshot_.config.interval_seconds = IsAllowedInterval(interval) ? interval : 1800;
    snapshot_.config.revision = revision;
}

esp_err_t LocalAlbumPlaybackService::PersistLocked() {
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_u8(handle, kModeKey, static_cast<std::uint8_t>(snapshot_.config.mode));
    if (result == ESP_OK) result = nvs_set_u8(handle, kOrderKey, static_cast<std::uint8_t>(snapshot_.config.order));
    if (result == ESP_OK) result = nvs_set_u32(handle, kIntervalKey, snapshot_.config.interval_seconds);
    if (result == ESP_OK) result = nvs_set_u32(handle, kRevisionKey, static_cast<std::uint32_t>(snapshot_.config.revision));
    if (result == ESP_OK) result = nvs_set_u64(handle, kNextEpochKey, snapshot_.next_play_at_epoch_seconds);
    if (result == ESP_OK) result = nvs_set_str(handle, kCurrentKey, snapshot_.config.current_media_id.c_str());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

EpochMs LocalAlbumPlaybackService::NowMs() {
    return static_cast<EpochMs>(esp_timer_get_time() / 1000);
}

void LocalAlbumPlaybackService::ScheduleNextLocked(std::uint32_t delay_seconds) {
    snapshot_.next_play_at_ms = NowMs() + static_cast<EpochMs>(delay_seconds) * 1000ULL;
    std::uint64_t now = 0;
    snapshot_.next_play_at_epoch_seconds = GetRtcService().GetUnixTimeSeconds(&now) ? now + delay_seconds : 0;
}

}  // namespace photopainter::product
