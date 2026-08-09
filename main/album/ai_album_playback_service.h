#pragma once

#include <vector>

#include "local_album_playback_service.h"

namespace photopainter::product {

// AI-album playback intentionally owns a separate NVS namespace and runtime
// cursor.  It only shares the public configuration vocabulary with local
// playback so the App can use the same API shape.
class AiAlbumPlaybackService {
public:
    AiAlbumPlaybackService();
    ~AiAlbumPlaybackService();
    AiAlbumPlaybackService(const AiAlbumPlaybackService&) = delete;
    AiAlbumPlaybackService& operator=(const AiAlbumPlaybackService&) = delete;

    esp_err_t Initialize(MediaLibrary* library, DisplayService* display, JobService* jobs);
    PlaybackSnapshot GetSnapshot() const;
    esp_err_t UpdateConfig(PlaybackMode mode, std::uint32_t interval_seconds,
                           PlaybackOrder order, Revision expected_revision,
                           PlaybackSnapshot* output);
    void NotifyManualDisplaySuccess(const MediaId& media_id);
    esp_err_t RequestNext();
    void NotifyMediaDeleted(const MediaId& media_id);
    static bool IsAllowedInterval(std::uint32_t seconds);

private:
    static void WorkerEntry(void* context);
    void WorkerLoop();
    void Tick();
    bool SelectNextLocked(MediaId* output);
    void RebuildRandomQueueLocked();
    void ObserveExternalDisplayLocked();
    void HandlePendingCompletionLocked();
    void ScheduleNextLocked(std::uint32_t delay_seconds);
    PlaybackSnapshot SnapshotLocked() const;
    esp_err_t PersistLocked();
    void LoadPersistedLocked();
    static EpochMs NowMs();

    MediaLibrary* library_ = nullptr;
    DisplayService* display_ = nullptr;
    JobService* jobs_ = nullptr;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    PlaybackSnapshot snapshot_;
    JobId pending_job_id_;
    std::vector<MediaId> random_queue_;
    std::uint32_t request_sequence_ = 1;
    bool ai_mode_was_active_ = false;
    bool manual_next_requested_ = false;
};

}  // namespace photopainter::product
