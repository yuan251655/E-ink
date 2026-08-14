#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace photopainter::product {

// The ESP owns only this tiny hand-off record.  It never sees the App API key
// or model image bytes.
enum class VoiceGenerationState : std::uint8_t {
    kIdle, kAwaitingConfirm, kPendingApp, kAppGenerating, kUploading,
    kDisplaying, kSuccess, kFailed, kCancelled, kExpired,
};

struct VoiceGenerationTask {
    std::string id;
    std::string prompt;
    VoiceGenerationState state = VoiceGenerationState::kIdle;
    std::string error_code;
    std::uint64_t expires_at_ms = 0;
};

class VoiceGenerationService {
public:
    VoiceGenerationService();
    esp_err_t CreateAwaitingConfirm(const std::string& prompt, VoiceGenerationTask* output);
    esp_err_t Confirm(VoiceGenerationTask* output);
    esp_err_t Cancel(VoiceGenerationTask* output);
    esp_err_t Claim(const std::string& task_id, VoiceGenerationTask* output);
    esp_err_t Update(const std::string& task_id, VoiceGenerationState state,
                     const std::string& error_code, VoiceGenerationTask* output);
    void RecordAppHeartbeat();
    bool IsAppAvailable();
    VoiceGenerationTask GetSnapshot();
    static const char* StateName(VoiceGenerationState state);

private:
    void ExpireLocked(std::uint64_t now_ms);
    VoiceGenerationTask task_;
    std::uint32_t sequence_ = 1;
    std::uint64_t app_heartbeat_at_ms_ = 0;
    void* mutex_ = nullptr;
};

VoiceGenerationService& GetVoiceGenerationService();
void RegisterVoiceGenerationMcpTools();
// Returns true when the complete STT utterance is the private birthday
// phrase. The display request is deterministic; cloud MCP remains responsible
// for reading the long message aloud.
bool HandleBirthdayEasterEggStt(const std::string& phrase);
// App fallback for unreliable speech recognition. It uses the same display
// path and asks the connected Xiaozhi session to handle the trigger phrase.
esp_err_t TriggerBirthdayEasterEggFromApp(bool* already_visible);

}  // namespace photopainter::product
