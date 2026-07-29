#include "media_library.h"

#include <algorithm>

#include "ArduinoJson.h"
#include "storage_service.h"

namespace photopainter::product {
namespace {
constexpr std::size_t kManifestMaximumBytes = 4096;

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

    std::vector<MediaId> ids;
    esp_err_t result = storage->ListCommittedMediaIds(&ids);
    if (result != ESP_OK) return result;
    std::vector<MediaItem> rebuilt;
    for (const auto& id : ids) {
        MediaItem item;
        if (LoadManifest(id, &item) != ESP_OK) continue;
        if (ValidateFrameForDisplay(item) != ESP_OK) continue;
        rebuilt.push_back(std::move(item));
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

esp_err_t MediaLibrary::RegisterCommitted(const MediaId& media_id) {
    if (media_id.empty() || mutex_ == nullptr) return ESP_ERR_INVALID_ARG;

    MediaItem item;
    esp_err_t result = LoadManifest(media_id, &item);
    if (result != ESP_OK) return result;
    result = ValidateFrameForDisplay(item);
    if (result != ESP_OK) return result;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    const auto existing = std::find_if(items_.begin(), items_.end(), [&media_id](const MediaItem& candidate) {
        return candidate.media_id == media_id;
    });
    if (existing == items_.end()) {
        items_.push_back(std::move(item));
    } else {
        *existing = std::move(item);
    }
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
    return storage->GetCommittedFileSize("media/" + item.media_id + "/image.bin", &actual_bytes) == ESP_OK &&
                   actual_bytes == kDisplayFrameBytes ? ESP_OK : ESP_ERR_INVALID_SIZE;
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

esp_err_t MediaLibrary::LoadManifest(const MediaId& expected_id, MediaItem* output) const {
    if (output == nullptr || mutex_ == nullptr) return ESP_ERR_INVALID_ARG;
    StorageService* storage = nullptr;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    storage = storage_;
    xSemaphoreGive(mutex_);
    if (storage == nullptr) return ESP_ERR_INVALID_STATE;
    std::string json;
    esp_err_t result = storage->ReadCommittedText("media/" + expected_id + "/manifest.json", kManifestMaximumBytes, &json);
    if (result != ESP_OK) return result;
    // Admission can run in the HTTP upload task after multipart parsing; keep
    // this manifest pool off that task's limited stack.
    JsonDocument document;
    if (deserializeJson(document, json) != DeserializationError::Ok) return ESP_ERR_INVALID_ARG;
    JsonObjectConst root = document.as<JsonObjectConst>();
    const std::string manifest_media_id = root["media_id"] | "";
    const std::string category = root["category"] | "";
    if (manifest_media_id != expected_id || category != "local") return ESP_ERR_INVALID_ARG;
    MediaItem item;
    item.media_id = expected_id;
    item.display_name = root["display_name"] | "";
    if (item.display_name.size() > 128) return ESP_ERR_INVALID_SIZE;
    item.category = MediaCategory::kLocal;
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
        (fit_mode != "contain" && fit_mode != "cover")) return ESP_ERR_INVALID_ARG;
    item.display_profile.pixel_format = PixelFormat::kIndexed4Bpp;
    item.display_profile.palette = Palette::kSixColorE6;
    item.display_profile.orientation = orientation == "portrait" ? Orientation::kPortrait : Orientation::kLandscape;
    item.display_profile.fit_mode = fit_mode == "cover" ? FitMode::kCover : FitMode::kContain;
    JsonObjectConst files = root["files"].as<JsonObjectConst>();
    if (item.manifest_version != 1 || !ReadDescriptor(files["source"].as<JsonObjectConst>(), &item.source) ||
        !ReadDescriptor(files["preview"].as<JsonObjectConst>(), &item.preview) ||
        !ReadDescriptor(files["frame"].as<JsonObjectConst>(), &item.frame)) return ESP_ERR_INVALID_ARG;
    *output = std::move(item);
    return ESP_OK;
}

}  // namespace photopainter::product
