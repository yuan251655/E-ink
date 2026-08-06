#include "media_library.h"

#include <algorithm>
#include <unordered_map>

#include "ArduinoJson.h"
#include "esp_log.h"
#include "media_location_policy.h"
#include "storage_service.h"

namespace photopainter::product {
namespace {
constexpr std::size_t kManifestMaximumBytes = 4096;
constexpr char kTag[] = "media_library";

struct MediaLocationGroup {
    std::vector<CommittedMediaLocation> categorized;
    bool has_legacy = false;
    CommittedMediaLocation legacy;
};

bool ReadDescriptor(JsonObjectConst object, FileDescriptor* output) {
    if (output == nullptr) return false;
    output->present = object["present"] | false;
    output->mime_type = object["mime_type"] | "";
    output->bytes = object["bytes"] | 0ULL;
    output->sha256 = object["sha256"] | "";
    return !output->present || (!output->mime_type.empty() && output->bytes > 0 && !output->sha256.empty());
}
}

MediaLibrary::MediaLibrary() {
    mutex_ = xSemaphoreCreateMutex();
}

MediaLibrary::~MediaLibrary() {
    if (mutex_ != nullptr) vSemaphoreDelete(mutex_);
}

esp_err_t MediaLibrary::Initialize(StorageService* storage) {
    if (storage == nullptr || mutex_ == nullptr) return ESP_ERR_INVALID_ARG;

    // Build a complete replacement index off-lock.  TF reads can take time;
    // readers keep using the last known-good in-memory index until the new one
    // has been fully validated.
    xSemaphoreTake(mutex_, portMAX_DELAY);
    storage_ = storage;
    xSemaphoreGive(mutex_);

    std::vector<CommittedMediaLocation> locations;
    esp_err_t result = storage->ListCommittedMedia(&locations);
    if (result != ESP_OK) return result;

    std::unordered_map<MediaId, MediaLocationGroup> grouped;
    for (const auto& location : locations) {
        auto& group = grouped[location.media_id];
        if (location.relative_directory == "media/" + location.media_id) {
            group.has_legacy = true;
            group.legacy = location;
        } else {
            group.categorized.push_back(location);
        }
    }

    std::vector<MediaItem> rebuilt;
    for (const auto& entry : grouped) {
        const MediaId& media_id = entry.first;
        const MediaLocationGroup& group = entry.second;
        if (group.categorized.size() > 1U) {
            ESP_LOGE(kTag, "rejecting categorized media id collision: %s", media_id.c_str());
            continue;
        }

        MediaItem categorized_item;
        const bool categorized_valid = group.categorized.size() == 1U &&
            LoadManifest(group.categorized.front(), &categorized_item) == ESP_OK &&
            ValidateCommittedItem(categorized_item) == ESP_OK;
        MediaItem legacy_item;
        const bool legacy_valid = !categorized_valid && group.has_legacy &&
            LoadManifest(group.legacy, &legacy_item) == ESP_OK &&
            ValidateCommittedItem(legacy_item) == ESP_OK;

        switch (DecideMediaLocation(group.categorized.size(), categorized_valid, legacy_valid)) {
            case MediaLocationDecision::kUseCategorized:
                rebuilt.push_back(std::move(categorized_item));
                break;
            case MediaLocationDecision::kUseLegacy:
                ESP_LOGW(kTag, "using valid legacy fallback for media id: %s", media_id.c_str());
                rebuilt.push_back(std::move(legacy_item));
                break;
            case MediaLocationDecision::kRejectCategorizedCollision:
            case MediaLocationDecision::kNone:
                break;
        }
    }

    std::sort(rebuilt.begin(), rebuilt.end(), [](const MediaItem& left, const MediaItem& right) {
        return left.created_at_ms > right.created_at_ms;
    });

    xSemaphoreTake(mutex_, portMAX_DELAY);
    storage_ = storage;
    items_ = std::move(rebuilt);
    ++revision_;
    xSemaphoreGive(mutex_);
    return ESP_OK;
}

bool MediaLibrary::Find(const MediaId& media_id, MediaItem* output) const {
    if (output == nullptr || mutex_ == nullptr) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const auto it = std::find_if(items_.begin(), items_.end(), [&media_id](const MediaItem& item) { return item.media_id == media_id; });
    const bool found = it != items_.end();
    if (found) *output = *it;
    xSemaphoreGive(mutex_);
    return found;
}

std::vector<MediaItem> MediaLibrary::List(MediaCategory category, std::size_t offset, std::size_t limit) const {
    std::vector<MediaItem> page;
    if (mutex_ == nullptr || limit == 0) return page;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    std::size_t skipped = 0;
    for (const auto& item : items_) {
        if (item.category != category) continue;
        if (skipped < offset) {
            ++skipped;
            continue;
        }
        if (page.size() == limit) break;
        page.push_back(item);
    }
    xSemaphoreGive(mutex_);
    return page;
}

std::size_t MediaLibrary::Count(MediaCategory category) const {
    if (mutex_ == nullptr) return 0;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const std::size_t count = static_cast<std::size_t>(std::count_if(items_.begin(), items_.end(),
        [category](const MediaItem& item) { return item.category == category; }));
    xSemaphoreGive(mutex_);
    return count;
}

bool MediaLibrary::FindAdjacent(MediaCategory category, const MediaId& current_media_id,
                                int direction, MediaItem* output) const {
    if (output == nullptr || mutex_ == nullptr || current_media_id.empty() ||
        (direction != -1 && direction != 1)) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    std::vector<const MediaItem*> category_items;
    for (const auto& item : items_) if (item.category == category) category_items.push_back(&item);
    const auto current = std::find_if(category_items.begin(), category_items.end(),
        [&current_media_id](const MediaItem* item) { return item->media_id == current_media_id; });
    if (category_items.empty() || current == category_items.end()) {
        xSemaphoreGive(mutex_);
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(current - category_items.begin());
    const std::size_t next = direction > 0 ? (index + 1U) % category_items.size()
                                           : (index + category_items.size() - 1U) % category_items.size();
    *output = *category_items[next];
    xSemaphoreGive(mutex_);
    return true;
}

esp_err_t MediaLibrary::RegisterCommitted(MediaCategory category, const MediaId& media_id) {
    if (media_id.empty() || mutex_ == nullptr) return ESP_ERR_INVALID_ARG;

    const char* category_directory = category == MediaCategory::kLocal ? "local" :
        (category == MediaCategory::kAi ? "ai" :
         (category == MediaCategory::kDashboard ? "dashboard" : nullptr));
    if (category_directory == nullptr) return ESP_ERR_INVALID_ARG;
    const CommittedMediaLocation location{
        category,
        media_id,
        std::string("media/") + category_directory + "/" + media_id,
    };
    StorageService* storage = nullptr;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    storage = storage_;
    const bool already_indexed = std::any_of(items_.begin(), items_.end(), [&media_id](const MediaItem& candidate) {
        return candidate.media_id == media_id;
    });
    xSemaphoreGive(mutex_);
    if (storage == nullptr) return ESP_ERR_INVALID_STATE;
    if (already_indexed) {
        ESP_LOGE(kTag, "refusing to overwrite indexed media id: %s", media_id.c_str());
        return ESP_ERR_INVALID_STATE;
    }
    std::vector<CommittedMediaLocation> locations;
    esp_err_t result = storage->ListCommittedMedia(&locations);
    if (result != ESP_OK) return result;
    const bool conflict = std::any_of(locations.begin(), locations.end(), [&location](const CommittedMediaLocation& candidate) {
        return candidate.media_id == location.media_id && candidate.relative_directory != location.relative_directory;
    });
    if (conflict) {
        ESP_LOGE(kTag, "refusing media id conflict during registration: %s", media_id.c_str());
        return ESP_ERR_INVALID_STATE;
    }
    MediaItem item;
    result = LoadManifest(location, &item);
    if (result != ESP_OK) return result;
    result = ValidateCommittedItem(item);
    if (result != ESP_OK) return result;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    const auto existing = std::find_if(items_.begin(), items_.end(), [&media_id](const MediaItem& candidate) {
        return candidate.media_id == media_id;
    });
    if (existing != items_.end()) {
        ESP_LOGE(kTag, "refusing to overwrite indexed media id: %s", media_id.c_str());
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    items_.push_back(std::move(item));
    SortItemsLocked();
    ++revision_;
    xSemaphoreGive(mutex_);
    return ESP_OK;
}

esp_err_t MediaLibrary::RestoreCommitted(const MediaItem& removed_item) {
    if (removed_item.media_id.empty() || removed_item.storage_relative_directory.empty() || mutex_ == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const CommittedMediaLocation location{
        removed_item.category,
        removed_item.media_id,
        removed_item.storage_relative_directory,
    };
    MediaItem restored;
    esp_err_t result = LoadManifest(location, &restored);
    if (result != ESP_OK) return result;
    result = ValidateCommittedItem(restored);
    if (result != ESP_OK) return result;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool conflict = std::any_of(items_.begin(), items_.end(), [&restored](const MediaItem& candidate) {
        return candidate.media_id == restored.media_id;
    });
    if (conflict) {
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    items_.push_back(std::move(restored));
    SortItemsLocked();
    ++revision_;
    xSemaphoreGive(mutex_);
    return ESP_OK;
}

esp_err_t MediaLibrary::RestoreSnapshot(const MediaItem& removed_item) {
    if (mutex_ == nullptr || !IsValidSnapshotLocation(removed_item) ||
        removed_item.display_profile.width != kDisplayWidth ||
        removed_item.display_profile.height != kDisplayHeight ||
        removed_item.display_profile.frame_bytes != kDisplayFrameBytes ||
        !removed_item.frame.present || removed_item.frame.bytes != kDisplayFrameBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool conflict = std::any_of(items_.begin(), items_.end(), [&removed_item](const MediaItem& candidate) {
        return candidate.media_id == removed_item.media_id;
    });
    if (conflict) {
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    items_.push_back(removed_item);
    SortItemsLocked();
    ++revision_;
    xSemaphoreGive(mutex_);
    return ESP_OK;
}

bool MediaLibrary::RemoveCommitted(const MediaId& media_id, Revision expected_item_revision) {
    if (media_id.empty() || mutex_ == nullptr) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const auto item = std::find_if(items_.begin(), items_.end(), [&media_id](const MediaItem& candidate) {
        return candidate.media_id == media_id;
    });
    if (item == items_.end() || item->revision != expected_item_revision) {
        xSemaphoreGive(mutex_);
        return false;
    }
    items_.erase(item);
    ++revision_;
    xSemaphoreGive(mutex_);
    return true;
}

esp_err_t MediaLibrary::ValidateFrameForDisplay(const MediaId& media_id) const {
    MediaItem item;
    if (!Find(media_id, &item)) return ESP_ERR_NOT_FOUND;
    return ValidateFrameForDisplay(item);
}

esp_err_t MediaLibrary::ValidateFrameForDisplay(const MediaItem& item) const {
    if (item.display_profile.width != kDisplayWidth || item.display_profile.height != kDisplayHeight ||
        item.display_profile.frame_bytes != kDisplayFrameBytes || !item.frame.present ||
        item.frame.bytes != kDisplayFrameBytes) return ESP_ERR_INVALID_SIZE;
    StorageService* storage = nullptr;
    if (mutex_ == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    storage = storage_;
    xSemaphoreGive(mutex_);
    if (storage == nullptr) return ESP_ERR_INVALID_STATE;
    std::uint64_t actual_bytes = 0;
    if (item.storage_relative_directory.empty()) return ESP_ERR_INVALID_STATE;
    return storage->GetCommittedFileSize(item.storage_relative_directory + "/image.bin", &actual_bytes) == ESP_OK &&
                   actual_bytes == kDisplayFrameBytes ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t MediaLibrary::ValidateCommittedItem(const MediaItem& item) const {
    esp_err_t result = ValidateFrameForDisplay(item);
    if (result != ESP_OK) return result;
    StorageService* storage = nullptr;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    storage = storage_;
    xSemaphoreGive(mutex_);
    if (storage == nullptr) return ESP_ERR_INVALID_STATE;
    const auto validate_optional = [storage, &item](const FileDescriptor& descriptor,
                                                    const std::string& filename) -> esp_err_t {
        if (!descriptor.present) return ESP_OK;
        std::uint64_t actual_bytes = 0;
        return storage->GetCommittedFileSize(item.storage_relative_directory + "/" + filename, &actual_bytes) == ESP_OK &&
                       actual_bytes == descriptor.bytes ? ESP_OK : ESP_ERR_INVALID_SIZE;
    };
    if (item.source.present) {
        const char* source_name = item.source.mime_type == "image/png" ? "source.png" :
            (item.source.mime_type == "image/jpeg" ? "source.jpg" : nullptr);
        if (source_name == nullptr) return ESP_ERR_INVALID_ARG;
        result = validate_optional(item.source, source_name);
        if (result != ESP_OK) return result;
    }
    if (item.preview.present) {
        if (item.preview.mime_type != "image/png") return ESP_ERR_INVALID_ARG;
        result = validate_optional(item.preview, "preview.png");
    }
    return result;
}

bool MediaLibrary::IsValidSnapshotLocation(const MediaItem& item) const {
    if (item.media_id.empty()) return false;
    if (item.category == MediaCategory::kLocal && item.feature == Feature::kLocalAlbum) {
        return item.storage_relative_directory == "media/local/" + item.media_id ||
               item.storage_relative_directory == "media/" + item.media_id;
    }
    if (item.category == MediaCategory::kAi && item.feature == Feature::kAiAlbum) {
        return item.storage_relative_directory == "media/ai/" + item.media_id;
    }
    return item.category == MediaCategory::kDashboard && item.feature == Feature::kInfoDashboard &&
           item.storage_relative_directory == "media/dashboard/" + item.media_id;
}

Revision MediaLibrary::revision() const {
    if (mutex_ == nullptr) return 0;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const Revision value = revision_;
    xSemaphoreGive(mutex_);
    return value;
}

void MediaLibrary::SortItemsLocked() {
    std::sort(items_.begin(), items_.end(), [](const MediaItem& left, const MediaItem& right) {
        if (left.created_at_ms != right.created_at_ms) return left.created_at_ms > right.created_at_ms;
        return left.media_id > right.media_id;
    });
}

esp_err_t MediaLibrary::LoadManifest(const CommittedMediaLocation& location, MediaItem* output) const {
    if (output == nullptr || mutex_ == nullptr || location.media_id.empty() ||
        location.relative_directory.empty()) return ESP_ERR_INVALID_ARG;
    StorageService* storage = nullptr;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    storage = storage_;
    xSemaphoreGive(mutex_);
    if (storage == nullptr) return ESP_ERR_INVALID_STATE;
    std::string json;
    esp_err_t result = storage->ReadCommittedText(location.relative_directory + "/manifest.json",
                                                  kManifestMaximumBytes, &json);
    if (result != ESP_OK) return result;
    // Admission can run in the HTTP upload task after multipart parsing; keep
    // this manifest pool off that task's limited stack.
    JsonDocument document;
    if (deserializeJson(document, json) != DeserializationError::Ok) return ESP_ERR_INVALID_ARG;
    JsonObjectConst root = document.as<JsonObjectConst>();
    const std::string manifest_media_id = root["media_id"] | "";
    const std::string category = root["category"] | "";
    const MediaCategory manifest_category = category == "local" ? MediaCategory::kLocal :
        (category == "ai" ? MediaCategory::kAi :
         (category == "dashboard" ? MediaCategory::kDashboard : MediaCategory::kSystem));
    if (manifest_media_id != location.media_id || manifest_category != location.category ||
        (manifest_category != MediaCategory::kLocal && manifest_category != MediaCategory::kAi &&
         manifest_category != MediaCategory::kDashboard)) {
        return ESP_ERR_INVALID_ARG;
    }
    MediaItem item;
    item.media_id = location.media_id;
    item.display_name = root["display_name"] | "";
    if (item.display_name.size() > 128) return ESP_ERR_INVALID_SIZE;
    item.category = manifest_category;
    item.feature = manifest_category == MediaCategory::kAi ? Feature::kAiAlbum :
        (manifest_category == MediaCategory::kDashboard ? Feature::kInfoDashboard : Feature::kLocalAlbum);
    item.storage_relative_directory = location.relative_directory;
    item.created_at_ms = root["created_at_ms"] | 0ULL;
    item.updated_at_ms = root["updated_at_ms"] | item.created_at_ms;
    item.manifest_version = root["manifest_version"] | 0U;
    item.revision = root["revision"] | 0ULL;
    JsonObjectConst profile = root["display_profile"].as<JsonObjectConst>();
    item.display_profile.width = profile["width"] | 0U;
    item.display_profile.height = profile["height"] | 0U;
    item.display_profile.frame_bytes = profile["frame_bytes"] | 0U;
    item.display_profile.rotation_degrees = profile["rotation_degrees"] | 0;
    item.display_profile.converter_version = profile["converter_version"] | "";
    const std::string pixel_format = profile["pixel_format"] | "";
    const std::string palette = profile["palette"] | "";
    const std::string orientation = profile["orientation"] | "";
    const std::string fit_mode = profile["fit_mode"] | "";
    if (pixel_format != "4bpp" || palette != "six_color_e6" ||
        (orientation != "landscape" && orientation != "portrait") ||
        // AI images use this explicit label for the CropToFill conversion.
        // It has the same display semantics as the existing `cover` enum;
        // accepting it keeps manifest metadata truthful without excluding a
        // successfully committed TF item from the authoritative library.
        (fit_mode != "contain" && fit_mode != "cover" && fit_mode != "crop_to_fill")) return ESP_ERR_INVALID_ARG;
    item.display_profile.pixel_format = PixelFormat::kIndexed4Bpp;
    item.display_profile.palette = Palette::kSixColorE6;
    item.display_profile.orientation = orientation == "portrait" ? Orientation::kPortrait : Orientation::kLandscape;
    item.display_profile.fit_mode = (fit_mode == "cover" || fit_mode == "crop_to_fill")
        ? FitMode::kCover : FitMode::kContain;
    JsonObjectConst files = root["files"].as<JsonObjectConst>();
    if (item.manifest_version == 1) {
        if (!ReadDescriptor(files["source"].as<JsonObjectConst>(), &item.source) ||
            !ReadDescriptor(files["preview"].as<JsonObjectConst>(), &item.preview) ||
            !ReadDescriptor(files["frame"].as<JsonObjectConst>(), &item.frame)) return ESP_ERR_INVALID_ARG;
    } else if (item.manifest_version == 2) {
        if (!ReadDescriptor(files["frame"].as<JsonObjectConst>(), &item.frame) || !item.frame.present ||
            item.frame.bytes != kDisplayFrameBytes) return ESP_ERR_INVALID_ARG;
        item.source = {};
        item.preview = {};
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    *output = std::move(item);
    return ESP_OK;
}

}  // namespace photopainter::product
