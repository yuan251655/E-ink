#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "product_types.h"

namespace photopainter::product {

// Result of request_id/fingerprint admission. A matching request_id and
// fingerprint returns the original snapshot; a reused request_id with a
// different fingerprint is explicitly rejected.
enum class JobRegistrationResult : std::uint8_t {
    kCreated,
    kExisting,
    kRequestIdConflict,
    kCapacityFull,
    kInvalidArgument,
};

// A small in-memory registry for device operations. It deliberately has a
// fixed capacity: active jobs are never evicted. Completed jobs are retained
// long enough for an App that is polling a slow e-paper refresh to observe
// the terminal result. If all slots are protected, admission fails explicitly
// instead of silently making an existing job disappear from GET /jobs/{id}.
// Callers must persist any durable domain state (for example a committed
// MediaItem) separately.
class JobService {
public:
    static constexpr std::size_t kCapacity = 12;

    JobService();
    ~JobService();

    JobService(const JobService&) = delete;
    JobService& operator=(const JobService&) = delete;

    JobRegistrationResult CreateOrFind(JobKind kind,
                                       const RequestId& request_id,
                                       const std::string& fingerprint,
                                       JobSnapshot* output);
    bool Get(const JobId& job_id, JobSnapshot* output) const;

    // Update a non-terminal job. Terminal states set finished_at_ms and cannot
    // be moved back to an active state.
    bool Update(const JobId& job_id,
                JobState state,
                const std::string& phase,
                std::uint8_t progress_percent,
                const std::string& error_code = {});

    // Marks a job successful. Upload callers may provide the newly committed
    // MediaId, which becomes visible with the completed job result.
    bool CompleteSuccess(const JobId& job_id, const MediaId& media_id = {});

private:
    struct Entry {
        bool occupied = false;
        std::string fingerprint;
        JobSnapshot snapshot;
    };

    static bool IsTerminal(JobState state);
    static EpochMs NowMs();
    static bool IsValidBounded(const std::string& value, std::size_t maximum);
    std::size_t FindByRequestIdLocked(const RequestId& request_id) const;
    std::size_t FindByJobIdLocked(const JobId& job_id) const;
    std::size_t FindAvailableSlotLocked() const;
    JobId NextJobIdLocked();

    static constexpr std::size_t kNoSlot = kCapacity;
    static constexpr std::size_t kMaxRequestIdLength = 64;
    static constexpr std::size_t kMaxFingerprintLength = 128;
    static constexpr std::size_t kMaxPhaseLength = 48;
    static constexpr std::size_t kMaxErrorCodeLength = 64;
    static constexpr std::size_t kMaxMediaIdLength = 64;
    static constexpr EpochMs kTerminalRetentionMs = 10ULL * 60ULL * 1000ULL;

    mutable SemaphoreHandle_t mutex_ = nullptr;
    std::array<Entry, kCapacity> entries_{};
    std::uint32_t boot_nonce_ = 0;
    std::uint32_t next_sequence_ = 1;
};

}  // namespace photopainter::product
