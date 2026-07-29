#include "media_library_runtime.h"

#include "media_library.h"
#include "storage_runtime.h"

namespace photopainter::product {

MediaLibrary& GetMediaLibrary() {
    static MediaLibrary media_library;
    return media_library;
}

esp_err_t InitializeMediaLibrary() {
    return GetMediaLibrary().Initialize(&GetStorageService());
}

}  // namespace photopainter::product
