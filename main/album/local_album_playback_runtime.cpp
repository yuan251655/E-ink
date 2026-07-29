#include "local_album_playback_runtime.h"

#include "display_runtime.h"
#include "job_runtime.h"
#include "local_album_playback_service.h"
#include "media_library_runtime.h"

namespace photopainter::product {

LocalAlbumPlaybackService& GetLocalAlbumPlaybackService() {
    static LocalAlbumPlaybackService service;
    return service;
}

esp_err_t InitializeLocalAlbumPlaybackService() {
    return GetLocalAlbumPlaybackService().Initialize(
        &GetMediaLibrary(), &GetDisplayService(), &GetProductJobService());
}

}  // namespace photopainter::product
