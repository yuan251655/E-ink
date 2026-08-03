#pragma once
#include "esp_err.h"
#include "ai_album_playback_service.h"
namespace photopainter::product {
AiAlbumPlaybackService& GetAiAlbumPlaybackService();
esp_err_t InitializeAiAlbumPlaybackService();
}  // namespace photopainter::product
