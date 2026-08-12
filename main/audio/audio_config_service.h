#pragma once

#include <esp_err.h>

namespace photopainter::product {

struct AudioConfigSnapshot {
    int master_volume = 70;
    bool muted = false;
    bool output_enabled = false;
    bool playing = false;
    const char* source = "idle";
};

AudioConfigSnapshot GetAudioConfigSnapshot();
esp_err_t UpdateAudioConfig(int master_volume, bool muted, AudioConfigSnapshot* output = nullptr);
esp_err_t StartSpeakerTest();
bool IsProductAudioBusy();

}  // namespace photopainter::product
