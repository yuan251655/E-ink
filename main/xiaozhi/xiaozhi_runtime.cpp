#include "xiaozhi_runtime.h"

#include "application.h"
#include "device_state.h"
#include "mode_manager.h"
#include "product_network.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace photopainter::product {
namespace {
XiaozhiRuntimeSnapshot snapshot;
bool initialized = false;
bool retry_requested = false;

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
            snapshot.state = "network_unconfigured";
            snapshot.started = app.IsStarted();
            snapshot.wake_word_enabled = false;
        } else if (!network.sta_connected) {
            snapshot.state = "sta_connecting";
            snapshot.started = app.IsStarted();
            snapshot.wake_word_enabled = false;
            app.SetWakeWordEnabled(false);
        } else {
            if (!start_requested || retry_requested) {
                retry_requested = false;
                start_requested = true;
                snapshot.state = "starting";
                // The board override observes product AP+STA and does not run
                // the official WifiStation/SsidManager provisioning path.
                app.Start(true);
            }
            app.SetWakeWordEnabled(ai_active);
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

XiaozhiRuntimeSnapshot GetXiaozhiRuntimeSnapshot() { return snapshot; }

void RetryXiaozhiRuntime() {
    // Application is intentionally single-start. The official protocol owns
    // reconnects; this only requests a fresh supervisor readiness pass.
    retry_requested = true;
}

}  // namespace photopainter::product
