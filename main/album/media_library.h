#pragma once

#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "product_types.h"

namespace photopainter::product {

class StorageService;

class MediaLibrary {
public:
    MediaLibrary();
    ~MediaLibrary();

    MediaLibrary(const MediaLibrary&) = delete;
    MediaLibrary& operator=(const MediaLibrary&) = delete;

    esp_err_t Initialize(StorageService* storage);
    // Returns a copy so HTTP and display workers never hold a pointer into an
    // index that can change when an upload is committed.
    bool Find(const MediaId& media_id, MediaItem* output) const;
    std::vector<MediaItem> List(MediaCategory category, std::size_t offset, std::size_t limit) const;
    std::size_t Count(MediaCategory category) const;
    // The gallery order is authoritative (newest committed first). direction
    // is +1 for the next item and -1 for the previous item; it wraps.
    bool FindAdjacent(MediaCategory category, const MediaId& current_media_id,
                      int direction, MediaItem* output) const;

    // Admit one already atomically committed /media/<media_id> directory.
    // The item becomes visible only after its manifest and 192000-byte frame
    // have both been validated.  It is safe to call after a successful retry.
    esp_err_t RegisterCommitted(const MediaId& media_id);
    // Removes an item from the in-memory index only when the caller still
    // holds the revision observed before destructive storage work.
    bool RemoveCommitted(const MediaId& media_id, Revision expected_item_revision);

    esp_err_t ValidateFrameForDisplay(const MediaId& media_id) const;
    Revision revision() const;

private:
    esp_err_t LoadManifest(const MediaId& expected_id, MediaItem* output) const;
    esp_err_t ValidateFrameForDisplay(const MediaItem& item) const;
    void SortItemsLocked();

    StorageService* storage_ = nullptr;
    std::vector<MediaItem> items_;
    Revision revision_ = 0;
    mutable SemaphoreHandle_t mutex_ = nullptr;
};

}  // namespace photopainter::product
