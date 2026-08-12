#include "audio_config_service.h"

#include <algorithm>

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "settings.h"

namespace photopainter::product {
namespace {
constexpr char kSettingsNamespace[] = "audio";
constexpr char kMasterVolumeKey[] = "master_volume";
constexpr char kLegacyVolumeKey[] = "output_volume";
constexpr char kMutedKey[] = "muted";

int StoredMasterVolume() {
    Settings settings(kSettingsNamespace, false);
    return std::clamp<int>(
        settings.GetInt(kMasterVolumeKey, settings.GetInt(kLegacyVolumeKey, 70)), 1, 100);
}

bool StoredMuted() {
    Settings settings(kSettingsNamespace, false);
    return settings.GetBool(kMutedKey, false);
}
}  // namespace

AudioConfigSnapshot GetAudioConfigSnapshot() {
    AudioConfigSnapshot snapshot;
    snapshot.master_volume = StoredMasterVolume();
    snapshot.muted = StoredMuted();
    auto& app = Application::GetInstance();
    if (app.IsStarted()) {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr) snapshot.output_enabled = codec->output_enabled();
        snapshot.playing = !app.GetAudioService().IsIdle();
        if (app.GetDeviceState() == kDeviceStateListening) snapshot.source = "xiaozhi_listening";
        else if (app.GetDeviceState() == kDeviceStateSpeaking) snapshot.source = "xiaozhi";
        else if (snapshot.playing) snapshot.source = "system_audio";
    }
    return snapshot;
}

esp_err_t UpdateAudioConfig(int master_volume, bool muted, AudioConfigSnapshot* output) {
    if (master_volume < 1 || master_volume > 100) return ESP_ERR_INVALID_ARG;
    {
        Settings settings(kSettingsNamespace, true);
        settings.SetInt(kMasterVolumeKey, master_volume);
        settings.SetBool(kMutedKey, muted);
    }
    auto& app = Application::GetInstance();
    if (app.IsStarted()) {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) return ESP_ERR_NOT_SUPPORTED;
        codec->SetOutputVolume(muted ? 0 : master_volume);
    }
    if (output != nullptr) *output = GetAudioConfigSnapshot();
    return ESP_OK;
}

esp_err_t StartSpeakerTest() {
    const auto config = GetAudioConfigSnapshot();
    if (config.muted) return ESP_ERR_INVALID_STATE;
    auto& app = Application::GetInstance();
    if (!app.IsStarted()) return ESP_ERR_NOT_FINISHED;
    if (app.GetDeviceState() != kDeviceStateIdle || !app.GetAudioService().IsIdle()) return ESP_ERR_TIMEOUT;
    app.PlaySound(Lang::Sounds::OGG_SUCCESS);
    return ESP_OK;
}

bool IsProductAudioBusy() {
    auto& app = Application::GetInstance();
    return app.IsStarted() && !app.GetAudioService().IsIdle();
}

}  // namespace photopainter::product
