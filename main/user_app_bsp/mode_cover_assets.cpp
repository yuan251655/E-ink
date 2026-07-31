#include "mode_cover_assets.h"

namespace {
extern const std::uint8_t local_start[] asm("_binary_mode_cover_local_album_bin_start");
extern const std::uint8_t local_end[] asm("_binary_mode_cover_local_album_bin_end");
extern const std::uint8_t ai_start[] asm("_binary_mode_cover_ai_album_bin_start");
extern const std::uint8_t ai_end[] asm("_binary_mode_cover_ai_album_bin_end");
extern const std::uint8_t dashboard_start[] asm("_binary_mode_cover_info_dashboard_bin_start");
extern const std::uint8_t dashboard_end[] asm("_binary_mode_cover_info_dashboard_bin_end");
}

namespace photopainter::product {

esp_err_t GetModeCoverAsset(Feature feature, ModeCoverAsset* output) {
    if (output == nullptr) return ESP_ERR_INVALID_ARG;
    const std::uint8_t* start = nullptr;
    const std::uint8_t* end = nullptr;
    const char* id = nullptr;
    switch (feature) {
        case Feature::kLocalAlbum:
            start = local_start; end = local_end; id = "mode_cover_local_album"; break;
        case Feature::kAiAlbum:
            start = ai_start; end = ai_end; id = "mode_cover_ai_album"; break;
        case Feature::kInfoDashboard:
            start = dashboard_start; end = dashboard_end; id = "mode_cover_info_dashboard"; break;
    }
    if (start == nullptr || end == nullptr || end < start ||
        static_cast<std::size_t>(end - start) != kDisplayFrameBytes) return ESP_ERR_INVALID_SIZE;
    output->data = start;
    output->size = static_cast<std::size_t>(end - start);
    output->system_asset_id = id;
    return ESP_OK;
}

}  // namespace photopainter::product
