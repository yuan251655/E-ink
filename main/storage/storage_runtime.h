#pragma once

#include "esp_err.h"

class CustomSDPort;

namespace photopainter::product {

class StorageService;

// Called once after the official user application has mounted the TF card.
// The returned singleton is the only StorageService instance for product code.
esp_err_t InitializeProductStorage(CustomSDPort* sd_port);
StorageService& GetStorageService();

}  // namespace photopainter::product
