#include "job_service.h"

#include <cstdio>

#include "esp_timer.h"

namespace photopainter::product {
namespace {

bool IsAllowedTransition(JobState previous, JobState next) {
    if (previous == JobState::kQueued) {
        return next == JobState::kQueued || next == JobState::kRunning ||
               next == JobState::kCancelled || next == JobState::kFailed ||
               next == JobState::kTimeout;
    }
    if (previous == JobState::kRunning) {
        return next == JobState::kRunning || next == JobState::kSuccess ||
               next == JobState::kCancelled || next == JobState::kFailed ||
               next == JobState::kTimeout;
    }
    return false;
}

}  // namespace

JobService::JobService() {
    mutex_ = xSemaphoreCreateMutex();
}

JobService::~JobService() {
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
    }
}

JobRegistrationResult JobService::CreateOrFind(JobKind kind,
                                                const RequestId& request_id,
                                                const std::string& fingerprint,
                                                JobSnapshot* output) {
    if (mutex_ == nullptr || output == nullptr || !IsValidBounded(request_id, kMaxRequestIdLength) ||
        !IsValidBounded(fingerprint, kMaxFingerprintLength)) {
        return JobRegistrationResult::kInvalidArgument;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    const std::size_t existing = FindByRequestIdLocked(request_id);
    if (existing != kNoSlot) {
        *output = entries_[existing].snapshot;
        const bool matching = entries_[existing].fingerprint == fingerprint;
        xSemaphoreGive(mutex_);
        return matching ? JobRegistrationResult::kExisting : JobRegistrationResult::kRequestIdConflict;
    }

    const std::size_t slot = FindAvailableSlotLocked();
    if (slot == kNoSlot) {
        xSemaphoreGive(mutex_);
        return JobRegistrationResult::kCapacityFull;
    }

    const EpochMs now = NowMs();
    Entry& entry = entries_[slot];
    entry.occupied = true;
    entry.fingerprint = fingerprint;
    entry.snapshot = {};
    entry.snapshot.job_id = NextJobIdLocked();
    entry.snapshot.kind = kind;
    entry.snapshot.state = JobState::kQueued;
    entry.snapshot.phase = "queued";
    entry.snapshot.request_id = request_id;
    entry.snapshot.created_at_ms = now;
    entry.snapshot.progress_percent = 0;
    *output = entry.snapshot;
    xSemaphoreGive(mutex_);
    return JobRegistrationResult::kCreated;
}

bool JobService::Get(const JobId& job_id, JobSnapshot* output) const {
    if (mutex_ == nullptr || output == nullptr || job_id.empty()) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const std::size_t slot = FindByJobIdLocked(job_id);
    if (slot == kNoSlot) {
        xSemaphoreGive(mutex_);
        return false;
    }
    *output = entries_[slot].snapshot;
    xSemaphoreGive(mutex_);
    return true;
}

bool JobService::Update(const JobId& job_id,
                        JobState state,
                        const std::string& phase,
                        std::uint8_t progress_percent,
                        const std::string& error_code) {
    if (mutex_ == nullptr || job_id.empty() || !IsValidBounded(phase, kMaxPhaseLength) ||
        error_code.size() > kMaxErrorCodeLength) {
        return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const std::size_t slot = FindByJobIdLocked(job_id);
    if (slot == kNoSlot || !IsAllowedTransition(entries_[slot].snapshot.state, state)) {
        xSemaphoreGive(mutex_);
        return false;
    }

    JobSnapshot& snapshot = entries_[slot].snapshot;
    const EpochMs now = NowMs();
    if (state == JobState::kRunning && snapshot.started_at_ms == 0) {
        snapshot.started_at_ms = now;
    }
    snapshot.state = state;
    snapshot.phase = phase;
    snapshot.progress_percent = progress_percent;
    snapshot.error_code = error_code;
    if (IsTerminal(state)) {
        snapshot.finished_at_ms = now;
        if (state == JobState::kSuccess) snapshot.progress_percent = 100;
    }
    xSemaphoreGive(mutex_);
    return true;
}

bool JobService::CompleteSuccess(const JobId& job_id, const MediaId& media_id) {
    if (mutex_ == nullptr || job_id.empty() || media_id.size() > kMaxMediaIdLength) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const std::size_t slot = FindByJobIdLocked(job_id);
    if (slot == kNoSlot || !IsAllowedTransition(entries_[slot].snapshot.state, JobState::kSuccess)) {
        xSemaphoreGive(mutex_);
        return false;
    }
    JobSnapshot& snapshot = entries_[slot].snapshot;
    const EpochMs now = NowMs();
    if (snapshot.started_at_ms == 0) snapshot.started_at_ms = now;
    snapshot.state = JobState::kSuccess;
    snapshot.phase = "completed";
    snapshot.progress_percent = 100;
    snapshot.error_code.clear();
    snapshot.media_id = media_id;
    snapshot.finished_at_ms = now;
    xSemaphoreGive(mutex_);
    return true;
}

bool JobService::IsTerminal(JobState state) {
    return state == JobState::kSuccess || state == JobState::kFailed ||
           state == JobState::kCancelled || state == JobState::kTimeout;
}

EpochMs JobService::NowMs() {
    return static_cast<EpochMs>(esp_timer_get_time() / 1000);
}

bool JobService::IsValidBounded(const std::string& value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum;
}

std::size_t JobService::FindByRequestIdLocked(const RequestId& request_id) const {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].occupied && entries_[index].snapshot.request_id == request_id) return index;
    }
    return kNoSlot;
}

std::size_t JobService::FindByJobIdLocked(const JobId& job_id) const {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].occupied && entries_[index].snapshot.job_id == job_id) return index;
    }
    return kNoSlot;
}

std::size_t JobService::FindAvailableSlotLocked() const {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (!entries_[index].occupied) return index;
    }

    // Keep every completed job visible for a bounded period. In particular a
    // mode-cover refresh may take tens of seconds, so replacing its terminal
    // record immediately makes a client see a misleading "not found" while
    // it is still polling the request it just submitted.
    const EpochMs now = NowMs();
    std::size_t oldest_terminal = kNoSlot;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const JobSnapshot& candidate = entries_[index].snapshot;
        if (!IsTerminal(candidate.state) || candidate.finished_at_ms == 0 ||
            now - candidate.finished_at_ms < kTerminalRetentionMs) continue;
        if (oldest_terminal == kNoSlot ||
            candidate.finished_at_ms < entries_[oldest_terminal].snapshot.finished_at_ms) {
            oldest_terminal = index;
        }
    }
    return oldest_terminal;
}

JobId JobService::NextJobIdLocked() {
    char value[24];
    std::snprintf(value, sizeof(value), "job-%lu", static_cast<unsigned long>(next_sequence_++));
    return value;
}

}  // namespace photopainter::product
