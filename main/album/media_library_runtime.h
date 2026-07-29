#pragma once

#include "esp_err.h"

namespace photopainter::product {

class MediaLibrary;

esp_err_t InitializeMediaLibrary();
MediaLibrary& GetMediaLibrary();

}  // namespace photopainter::product
