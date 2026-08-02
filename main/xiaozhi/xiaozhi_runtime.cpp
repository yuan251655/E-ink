#include "xiaozhi_runtime.h"

#include "application.h"
#include "device_state.h"
#include "mode_manager.h"
#include "product_network.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <algorithm>
#include <deque>
#include <mutex>

namespace photopainter::product {
namespace {
XiaozhiRuntimeSnapshot snapshot;
bool initialized = false;
bool retry_requested = false;
constexpr std::size_t kConversationCapacity = 48;
constexpr std::size_t kConversationTextMaxBytes = 480;
std::deque<XiaozhiConversationEvent> conversation;
std::uint64_t next_conversation_seq = 1;
std::mutex runtime_mutex;

std::string SanitizeText(const std::string& input) {
    std::string output;
    output.reserve(std::min(input.size(), kConversationTextMaxBytes));
    for (const char value : input) {
        const unsigned char byte = static_cast<unsigned char>(value);
        if (byte < 0x20U && value != '\n' && value != '\t') continue;
        output.push_back(value);
        if (output.size() >= kConversationTextMaxBytes) break;
    }
    return output;
}

void AppendConversationEvent(const char* type, const char* role, const std::string& text) {
    XiaozhiConversationEvent event;
    event.timestamp_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
    event.type = type;
    event.role = role;
    event.text = SanitizeText(text);
    std::lock_guard<std::mutex> lock(runtime_mutex);
    event.seq = next_conversation_seq++;
    if (conversation.size() >= kConversationCapacity) conversation.pop_front();
    conversation.push_back(std::move(event));
}

const char* StateName(DeviceState state) {
    switch (state) {
        case kDeviceStateActivating: return "activation_required";
        case kDeviceStateConnecting: return "connecting";
        case kDeviceStateListening: return "listening";
        case kDeviceStateSpeaking: return "speaking";
        case kDeviceStateIdle: return "ready";
        case kDeviceStateStarting: return "starting";
        default: return "starting";
    }
}

void RuntimeTask(void*) {
    bool start_requested = false;
    while (true) {
        const auto network = GetProductNetworkSnapshot();
        const auto mode = GetModeManager().GetSnapshot();
        const bool ai_active = mode.state == ModeSnapshot::State::kIdle && mode.active_feature == Feature::kAiAlbum;
        auto& app = Application::GetInstance();

        if (!network.sta_configured) {
            std::lock_guard<std::mutex> lock(runtime_mutex);
            snapshot.state = "network_unconfigured";
            snapshot.started = app.IsStarted();
            snapshot.wake_word_enabled = false;
        } else if (!network.sta_connected) {
            std::lock_guard<std::mutex> lock(runtime_mutex);
            snapshot.state = "sta_connecting";
            snapshot.started = app.IsStarted();
            snapshot.wake_word_enabled = false;
            app.SetWakeWordEnabled(false);
        } else {
            if (!start_requested || retry_requested) {
                retry_requested = false;
                start_requested = true;
                { std::lock_guard<std::mutex> lock(runtime_mutex); snapshot.state = "starting"; }
                // The board override observes product AP+STA and does not run
                // the official WifiStation/SsidManager provisioning path.
                app.Start(true);
            }
            app.SetWakeWordEnabled(ai_active);
            std::lock_guard<std::mutex> lock(runtime_mutex);
            snapshot.state = StateName(app.GetDeviceState());
            snapshot.started = app.IsStarted();
            snapshot.wake_word_enabled = ai_active && app.IsWakeWordEnabled();
            snapshot.activation_code = app.GetActivationCode();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
}  // namespace

void InitializeXiaozhiRuntime() {
    if (initialized) return;
    initialized = true;
    xTaskCreate(RuntimeTask, "xiaozhi_runtime", 8192, nullptr, 3, nullptr);
}

XiaozhiRuntimeSnapshot GetXiaozhiRuntimeSnapshot() {
    std::lock_guard<std::mutex> lock(runtime_mutex);
    return snapshot;
}

void RetryXiaozhiRuntime() {
    // Application is intentionally single-start. The official protocol owns
    // reconnects; this only requests a fresh supervisor readiness pass.
    retry_requested = true;
}

XiaozhiConversationPage GetXiaozhiConversation(std::uint64_t after_seq, std::size_t limit) {
    XiaozhiConversationPage page;
    limit = std::min<std::size_t>(std::max<std::size_t>(limit, 1), kConversationCapacity);
    std::lock_guard<std::mutex> lock(runtime_mutex);
    page.latest_seq = next_conversation_seq == 0 ? 0 : next_conversation_seq - 1;
    for (const auto& event : conversation) {
        if (event.seq <= after_seq) continue;
        page.events.push_back(event);
        if (page.events.size() >= limit) break;
    }
    return page;
}

void RecordXiaozhiStateEvent(const char* state) {
    if (state == nullptr) return;
    AppendConversationEvent("state", "system", state);
}

void RecordXiaozhiMessageEvent(const char* role, const std::string& text) {
    if (role == nullptr || text.empty()) return;
    AppendConversationEvent("message", role, text);
}

void SetXiaozhiTtsPlaybackEnabled(bool enabled) {
    // Product policy: Xiaozhi reply audio is always enabled while the AI album
    // runtime is active. Do not allow legacy callers or old App builds to mute
    // the cloud reply path.
    (void)enabled;
    constexpr bool kAlwaysEnabled = true;
    {
        std::lock_guard<std::mutex> lock(runtime_mutex);
        snapshot.tts_playback_enabled = kAlwaysEnabled;
    }
    Application::GetInstance().SetTtsPlaybackEnabled(kAlwaysEnabled);
}

bool IsXiaozhiTtsPlaybackEnabled() {
    std::lock_guard<std::mutex> lock(runtime_mutex);
    return snapshot.tts_playback_enabled;
}

}  // namespace photopainter::product
