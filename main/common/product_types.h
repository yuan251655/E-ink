#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace photopainter::product {

using RequestId = std::string;
using JobId = std::string;
using MediaId = std::string;
using TransactionId = std::string;
using Revision = std::uint64_t;
using EpochMs = std::uint64_t;

constexpr std::uint16_t kDisplayWidth = 800;
constexpr std::uint16_t kDisplayHeight = 480;
constexpr std::size_t kDisplayFrameBytes = 192000;

enum class Feature : std::uint8_t {
    kLocalAlbum,
    kAiAlbum,
    kInfoDashboard,
};

enum class MediaCategory : std::uint8_t {
    kLocal,
    kAi,
    kDashboard,
    kSystem,
};

enum class UploadMode : std::uint8_t {
    // The only accepted mode in the first hardware product loop.
    kSourcePlusBin,
    // Reserved for future on-device conversion; currently rejected as unsupported.
    kSourceOnly,
};

enum class Orientation : std::uint8_t {
    kLandscape,
    kPortrait,
};

enum class FitMode : std::uint8_t {
    kContain,
    kCover,
};

enum class PixelFormat : std::uint8_t {
    kIndexed4Bpp,
};

enum class Palette : std::uint8_t {
    kSixColorE6,
};

enum class AfterDisplay : std::uint8_t {
    kContinue,
    kHold,
};

enum class JobKind : std::uint8_t {
    kUpload,
    kDisplay,
    kMode,
};

enum class JobState : std::uint8_t {
    kQueued,
    kRunning,
    kSuccess,
    kFailed,
    kCancelled,
    kTimeout,
};

enum class StorageState : std::uint8_t {
    kUninitialized,
    kMounting,
    kReady,
    kDegraded,
    kMissing,
    kErrorBackoff,
    kRemountPending,
};

enum class DisplayState : std::uint8_t {
    kIdle,
    kQueued,
    kLoading,
    kRefreshing,
    kFinalizing,
    kSuccess,
    kFailed,
};

struct DisplayProfile {
    std::uint16_t width = kDisplayWidth;
    std::uint16_t height = kDisplayHeight;
    std::size_t frame_bytes = kDisplayFrameBytes;
    PixelFormat pixel_format = PixelFormat::kIndexed4Bpp;
    Palette palette = Palette::kSixColorE6;
    Orientation orientation = Orientation::kLandscape;
    std::int16_t rotation_degrees = 0;
    FitMode fit_mode = FitMode::kContain;
    std::string converter_version;
};

struct FileDescriptor {
    bool present = false;
    std::string mime_type;
    std::uint64_t bytes = 0;
    std::string sha256;
};

struct MediaItem {
    MediaId media_id;
    // User-facing file label supplied at admission time.  It is metadata only:
    // callers never use it to construct a TF path.
    std::string display_name;
    MediaCategory category = MediaCategory::kLocal;
    DisplayProfile display_profile;
    EpochMs created_at_ms = 0;
    EpochMs updated_at_ms = 0;
    FileDescriptor source;
    FileDescriptor preview;
    FileDescriptor frame;
    std::uint32_t manifest_version = 1;
    Revision revision = 0;
};

struct StorageSnapshot {
    StorageState state = StorageState::kUninitialized;
    std::uint64_t total_bytes = 0;
    std::uint64_t free_bytes = 0;
    std::uint64_t reserve_bytes = 0;
    TransactionId active_transaction_id;
    std::string last_error_code;
    Revision revision = 0;
};

struct ModeSnapshot {
    Feature active_feature = Feature::kLocalAlbum;
    Revision revision = 0;
    EpochMs updated_at_ms = 0;
};

struct DisplayRequest {
    RequestId request_id;
    MediaId media_id;
    Feature feature = Feature::kLocalAlbum;
    AfterDisplay after_display = AfterDisplay::kContinue;
    Revision expected_mode_revision = 0;
};

struct DisplaySnapshot {
    DisplayState state = DisplayState::kIdle;
    MediaId current_media_id;
    MediaId last_successful_media_id;
    JobId active_job_id;
    MediaId queued_target_media_id;
    std::string last_error_code;
    EpochMs updated_at_ms = 0;
};

struct JobSnapshot {
    JobId job_id;
    JobKind kind = JobKind::kUpload;
    JobState state = JobState::kQueued;
    std::string phase;
    RequestId request_id;
    EpochMs created_at_ms = 0;
    EpochMs started_at_ms = 0;
    EpochMs finished_at_ms = 0;
    std::uint8_t progress_percent = 0;
    std::string error_code;
    // Upload jobs expose the committed device-generated MediaId only after
    // successful admission. Other job kinds leave this empty.
    MediaId media_id;
    JobId superseded_by_job_id;
};

}  // namespace photopainter::product
