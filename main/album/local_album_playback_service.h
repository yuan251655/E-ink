#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "product_types.h"

namespace photopainter::product {

class DisplayService;
class JobService;
class MediaLibrary;

// Device-owned local-album playback policy.  It intentionally has no storage
// or BSP dependency: media selection is delegated to MediaLibrary and every
// physical update is submitted to DisplayService.
enum class PlaybackMode : std::uint8_t { kAuto = 0, kPaused = 1 };
enum class PlaybackOrder : std::uint8_t { kSequential = 0, kRandom = 1 };

struct LocalAlbumPlaybackConfig {
    PlaybackMode mode = PlaybackMode::kPaused;
    std::uint32_t interval_seconds = 1800;
    PlaybackOrder order = PlaybackOrder::kSequential;
    MediaId current_media_id;
    Revision revision = 0;
};

struct LocalAlbumPlaybackSnapshot {
    LocalAlbumPlaybackConfig config;
    // Changes when runtime playback state changes. It is deliberately not a
    // configuration compare-and-swap token: automatic page turns must not
    // make a user's settings form stale.
    Revision state_revision = 0;
    // Internal monotonic deadline. It must never be treated as Unix epoch
    // time: esp_timer resets on every boot and works without RTC/NTP.
    EpochMs next_play_at_ms = 0;
    // API-safe schedule representation. A wall-clock timestamp is not
    // available until the device has a reliable clock source, so clients use
    // this remaining duration together with their own system clock.
    bool has_next_play = false;
    std::uint32_t next_play_in_seconds = 0;
    bool refresh_pending = false;
    MediaId pending_media_id;
    std::string last_error_code;
};

// Short public name used by the API adapter.  The nested `config` field is
// the durable device configuration; scheduling/error fields are runtime only.
using PlaybackSnapshot = LocalAlbumPlaybackSnapshot;

class LocalAlbumPlaybackService {
public:
    LocalAlbumPlaybackService();
    ~LocalAlbumPlaybackService();

    LocalAlbumPlaybackService(const LocalAlbumPlaybackService&) = delete;
    LocalAlbumPlaybackService& operator=(const LocalAlbumPlaybackService&) = delete;

    esp_err_t Initialize(MediaLibrary* library, DisplayService* display, JobService* jobs);
    PlaybackSnapshot GetSnapshot() const;
    // The service is the revision authority. A caller must submit the
    // observed revision; ESP_ERR_INVALID_STATE represents a stale update.
    esp_err_t UpdateConfig(PlaybackMode mode, std::uint32_t interval_seconds,
                           PlaybackOrder order, Revision expected_revision,
                           PlaybackSnapshot* output);
    // Call after a successful user initiated display.  This never changes a
    // paused configuration into automatic playback.
    void NotifyManualDisplaySuccess(const MediaId& media_id);
    // Keep a persisted cursor and a shuffled queue from retaining IDs whose
    // media transaction was already removed.
    void NotifyMediaDeleted(const MediaId& media_id);

    static bool IsAllowedInterval(std::uint32_t interval_seconds);

private:
    static void WorkerEntry(void* context);
    void WorkerLoop();
    void Tick();
    bool SelectNextLocked(MediaId* output);
    void RebuildRandomQueueLocked();
    void ObserveExternalDisplayLocked();
    void HandlePendingCompletionLocked();
    PlaybackSnapshot SnapshotLocked() const;
    esp_err_t PersistLocked();
    void LoadPersistedLocked();
    static EpochMs NowMs();

    MediaLibrary* library_ = nullptr;
    DisplayService* display_ = nullptr;
    JobService* jobs_ = nullptr;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    LocalAlbumPlaybackSnapshot snapshot_;
    JobId pending_job_id_;
    std::vector<MediaId> random_queue_;
    std::uint32_t request_sequence_ = 1;
    bool local_mode_was_active_ = true;
};

}  // namespace photopainter::product
