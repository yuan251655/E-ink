#include "ai_album_playback_runtime.h"
#include "display_runtime.h"
#include "job_runtime.h"
#include "media_library_runtime.h"
namespace photopainter::product {
AiAlbumPlaybackService& GetAiAlbumPlaybackService() { static AiAlbumPlaybackService service; return service; }
esp_err_t InitializeAiAlbumPlaybackService() {
    return GetAiAlbumPlaybackService().Initialize(&GetMediaLibrary(), &GetDisplayService(), &GetProductJobService());
}
}  // namespace photopainter::product
