#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace photopainter::product {

struct XiaozhiRuntimeSnapshot {
    std::string state = "network_unconfigured";
    bool started = false;
    bool wake_word_enabled = false;
    bool active_only = true;
    // Controls only cloud TTS packets. Product prompt sounds remain enabled.
    bool tts_playback_enabled = true;
    std::string activation_code;
    std::string last_error_code;
};

// A bounded, RAM-only mirror of official Xiaozhi state/STT/TTS callbacks.
// It deliberately contains text only: never raw audio, protocol credentials,
// or persisted history.
struct XiaozhiConversationEvent {
    std::uint64_t seq = 0;
    std::uint64_t timestamp_ms = 0;
    std::string type;  // state | message
    std::string role;  // system | user | assistant
    std::string text;
};

struct XiaozhiConversationPage {
    std::vector<XiaozhiConversationEvent> events;
    std::uint64_t latest_seq = 0;
};

// Starts the product supervisor. It never initializes Wi-Fi and it never
// changes any physical button mapping.
void InitializeXiaozhiRuntime();
XiaozhiRuntimeSnapshot GetXiaozhiRuntimeSnapshot();
void RetryXiaozhiRuntime();
XiaozhiConversationPage GetXiaozhiConversation(std::uint64_t after_seq, std::size_t limit);
void RecordXiaozhiStateEvent(const char* state);
void RecordXiaozhiMessageEvent(const char* role, const std::string& text);
void SetXiaozhiTtsPlaybackEnabled(bool enabled);
bool IsXiaozhiTtsPlaybackEnabled();

}  // namespace photopainter::product
