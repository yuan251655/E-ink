#pragma once

#include "esp_err.h"
#include "local_album_playback_service.h"

namespace photopainter::product {

LocalAlbumPlaybackService& GetLocalAlbumPlaybackService();
esp_err_t InitializeLocalAlbumPlaybackService();

}  // namespace photopainter::product
