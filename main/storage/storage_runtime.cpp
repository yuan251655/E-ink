#include "storage_runtime.h"

#include "storage_service.h"

namespace photopainter::product {

StorageService& GetStorageService() {
    static StorageService storage_service;
    return storage_service;
}

esp_err_t InitializeProductStorage(CustomSDPort* sd_port) {
    return GetStorageService().Initialize(sd_port);
}

}  // namespace photopainter::product
