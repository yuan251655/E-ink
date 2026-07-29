#include "product_api.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ArduinoJson.h"
#include "display_runtime.h"
#include "display_service.h"
#include "job_service.h"
#include "job_runtime.h"
#include "media_library.h"
#include "media_upload_service.h"
#include "media_library_runtime.h"
#include "mode_manager.h"
#include "product_network.h"
#include "storage_runtime.h"
#include "storage_service.h"

namespace photopainter::product {
namespace {
esp_err_t SendJson(httpd_req_t* req, const char* body, const char* status = nullptr) {
    httpd_resp_set_type(req, "application/json");
    // Every product API call is deliberately one request per connection.  It
    // prevents a completed TF upload from retaining an HTTP socket and starving
    // the next item in a phone-side serial batch.
    httpd_resp_set_hdr(req, "Connection", "close");
    if (status) httpd_resp_set_status(req, status);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// Newlib nano formatting in this firmware does not support the `ll` printf
// length modifier. Keep 64-bit API values exact without relying on it.
void FormatUInt64(std::uint64_t value, char output[21]) {
    char reverse[20];
    std::size_t count = 0;
    do {
        reverse[count++] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);
    for (std::size_t i = 0; i < count; ++i) output[i] = reverse[count - 1U - i];
    output[count] = '\0';
}

const char* CategoryName(MediaCategory category) {
    switch (category) {
        case MediaCategory::kLocal: return "local";
        case MediaCategory::kAi: return "ai";
        case MediaCategory::kDashboard: return "dashboard";
        case MediaCategory::kSystem: return "system";
    }
    return "unknown";
}

void AppendUInt64(std::string* output, std::uint64_t value) {
    char text[21];
    FormatUInt64(value, text);
    output->append(text);
}

void AppendJsonString(std::string* output, const std::string& value) {
    output->push_back('"');
    static constexpr char kHex[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        switch (c) {
            case '"': output->append("\\\""); break;
            case '\\': output->append("\\\\"); break;
            case '\b': output->append("\\b"); break;
            case '\f': output->append("\\f"); break;
            case '\n': output->append("\\n"); break;
            case '\r': output->append("\\r"); break;
            case '\t': output->append("\\t"); break;
            default:
                if (c < 0x20U) {
                    output->append("\\u00");
                    output->push_back(kHex[c >> 4U]);
                    output->push_back(kHex[c & 0x0fU]);
                } else output->push_back(static_cast<char>(c));
        }
    }
    output->push_back('"');
}

void AppendMediaItemJson(std::string* output, const MediaItem& item) {
    output->append("{\"media_id\":\"").append(item.media_id)
        .append("\",\"display_name\":");
    AppendJsonString(output, item.display_name);
    output->append(",\"category\":\"").append(CategoryName(item.category)).append("\",\"created_at_ms\":");
    AppendUInt64(output, item.created_at_ms);
    output->append(",\"updated_at_ms\":");
    AppendUInt64(output, item.updated_at_ms);
    output->append(",\"display_profile\":{\"width\":").append(std::to_string(item.display_profile.width))
        .append(",\"height\":").append(std::to_string(item.display_profile.height))
        .append(",\"frame_bytes\":").append(std::to_string(item.display_profile.frame_bytes))
        .append(",\"pixel_format\":\"4bpp\",\"palette\":\"six_color_e6\",\"orientation\":\"")
        .append(item.display_profile.orientation == Orientation::kPortrait ? "portrait" : "landscape")
        .append("\",\"rotation_degrees\":").append(std::to_string(item.display_profile.rotation_degrees))
        .append(",\"fit_mode\":\"").append(item.display_profile.fit_mode == FitMode::kCover ? "cover" : "contain")
        .append("\"},\"source\":{\"present\":").append(item.source.present ? "true" : "false")
        .append(",\"mime_type\":");
    AppendJsonString(output, item.source.mime_type);
    output->append(",\"bytes\":");
    AppendUInt64(output, item.source.bytes);
    output->append("},\"preview\":{\"present\":").append(item.preview.present ? "true" : "false")
        .append("},\"frame\":{\"present\":").append(item.frame.present ? "true" : "false")
        .append(",\"bytes\":");
    AppendUInt64(output, item.frame.bytes);
    output->append("},\"manifest_version\":").append(std::to_string(item.manifest_version))
        .append(",\"revision\":");
    AppendUInt64(output, item.revision);
    output->append("}");
}

bool ParseDisplayRequest(httpd_req_t* req, RequestId* request_id, Revision* expected_revision,
                         std::string* after_display) {
    if (req == nullptr || request_id == nullptr || expected_revision == nullptr || after_display == nullptr ||
        req->content_len <= 0 || req->content_len > 512) return false;
    char body[513]{};
    const int read = httpd_req_recv(req, body, req->content_len);
    if (read != req->content_len) return false;
    JsonDocument input;
    if (deserializeJson(input, body) != DeserializationError::Ok) return false;
    *request_id = input["request_id"] | "";
    *expected_revision = input["expected_mode_revision"] | 0ULL;
    *after_display = input["after_display"] | "continue";
    return !request_id->empty() && request_id->size() <= 64 &&
           (*after_display == "continue" || *after_display == "hold");
}

esp_err_t SubmitLocalDisplay(httpd_req_t* req, const MediaId& media_id, const RequestId& request_id,
                             Revision expected_revision, const std::string& after_display) {
    const ModeSnapshot mode = GetModeManager().GetSnapshot();
    if (mode.active_feature != Feature::kLocalAlbum) return SendJson(req, "{\"ok\":false,\"code\":\"mode_changed\"}", "409 Conflict");
    if (expected_revision != mode.revision) return SendJson(req, "{\"ok\":false,\"code\":\"revision_conflict\"}", "409 Conflict");
    MediaItem item;
    if (!GetMediaLibrary().Find(media_id, &item) || item.category != MediaCategory::kLocal ||
        GetMediaLibrary().ValidateFrameForDisplay(media_id) != ESP_OK) {
        return SendJson(req, "{\"ok\":false,\"code\":\"media_invalid\"}", "422 Unprocessable Entity");
    }
    JobSnapshot job;
    const std::string fingerprint = "display:" + media_id + ":" + std::to_string(mode.revision) + ":" + after_display;
    const JobRegistrationResult registration = GetProductJobService().CreateOrFind(JobKind::kDisplay, request_id, fingerprint, &job);
    if (registration == JobRegistrationResult::kRequestIdConflict) return SendJson(req, "{\"ok\":false,\"code\":\"request_id_conflict\"}", "409 Conflict");
    if (registration != JobRegistrationResult::kCreated && registration != JobRegistrationResult::kExisting) return SendJson(req, "{\"ok\":false,\"code\":\"display_busy\"}", "503 Service Unavailable");
    if (registration == JobRegistrationResult::kCreated && GetDisplayService().SubmitLocal(media_id, job.job_id, &GetProductJobService()) != ESP_OK) {
        (void)GetProductJobService().Update(job.job_id, JobState::kFailed, "failed", 0, "display_busy");
        return SendJson(req, "{\"ok\":false,\"code\":\"display_busy\"}", "503 Service Unavailable");
    }
    (void)GetProductJobService().Get(job.job_id, &job);
    char response[256];
    std::snprintf(response, sizeof(response), "{\"ok\":true,\"code\":\"ok\",\"data\":{\"job_id\":\"%s\",\"state\":%u,\"phase\":\"%s\",\"media_id\":\"%s\"}}", job.job_id.c_str(), static_cast<unsigned>(job.state), job.phase.c_str(), media_id.c_str());
    return SendJson(req, response, "202 Accepted");
}

esp_err_t Health(httpd_req_t* req) {
    return SendJson(req, "{\"ok\":true,\"code\":\"ok\",\"message\":\"ready\",\"data\":{\"api_version\":\"v1\"}}");
}

esp_err_t Capabilities(httpd_req_t* req) {
    return SendJson(req, "{\"ok\":true,\"code\":\"ok\",\"data\":{\"api_version\":\"v1\",\"media_upload_modes\":[\"bin_only\"],\"supports_media_preview\":true,\"preview_format\":\"indexed_4bpp_bin\",\"display_width\":800,\"display_height\":480,\"frame_bytes\":192000}}");
}

esp_err_t UploadMedia(httpd_req_t* req) {
    const auto result = ReceiveBinOnlyMultipart(req, GetStorageService(), GetMediaLibrary(), GetProductJobService());
    char body[320];
    std::snprintf(body, sizeof(body), "{\"ok\":%s,\"code\":\"%s\",\"data\":{\"job_id\":\"%s\",\"state\":%u,\"phase\":\"%s\",\"media_id\":\"%s\"}}", result.error == ESP_OK ? "true" : "false", result.code.c_str(), result.job.job_id.c_str(), static_cast<unsigned>(result.job.state), result.job.phase.c_str(), result.job.media_id.c_str());
    if (result.error == ESP_OK) return SendJson(req, body, "202 Accepted");
    if (result.code == "request_id_conflict") return SendJson(req, body, "409 Conflict");
    if (result.code == "checksum_mismatch" || result.code == "media_incomplete" || result.code == "media_invalid") return SendJson(req, body, "422 Unprocessable Entity");
    return SendJson(req, body, result.code == "storage_busy" ? "503 Service Unavailable" : "400 Bad Request");
}

esp_err_t JobStatus(httpd_req_t* req) {
    const char* marker = std::strstr(req->uri, "/api/v1/jobs/");
    if (marker == nullptr || std::strlen(marker + 13) == 0 || std::strlen(marker + 13) > 63) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    JobSnapshot job;
    if (!GetProductJobService().Get(marker + 13, &job)) return SendJson(req, "{\"ok\":false,\"code\":\"media_not_found\"}", "404 Not Found");
    char body[384];
    std::snprintf(body, sizeof(body), "{\"ok\":true,\"code\":\"ok\",\"data\":{\"job_id\":\"%s\",\"state\":%u,\"phase\":\"%s\",\"progress_percent\":%u,\"error_code\":\"%s\",\"media_id\":\"%s\"}}", job.job_id.c_str(), static_cast<unsigned>(job.state), job.phase.c_str(), static_cast<unsigned>(job.progress_percent), job.error_code.c_str(), job.media_id.c_str());
    return SendJson(req, body);
}

esp_err_t ListMedia(httpd_req_t* req) {
    char query[128]{};
    char category[16]{};
    char cursor[16]{};
    char limit_text[8]{};
    if (httpd_req_get_url_query_len(req) >= sizeof(query)) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    if (httpd_req_get_url_query_len(req) > 0) {
        (void)httpd_req_get_url_query_str(req, query, sizeof(query));
        (void)httpd_query_key_value(query, "category", category, sizeof(category));
        (void)httpd_query_key_value(query, "cursor", cursor, sizeof(cursor));
        (void)httpd_query_key_value(query, "limit", limit_text, sizeof(limit_text));
    }
    if (category[0] != '\0' && std::strcmp(category, "local") != 0) return SendJson(req, "{\"ok\":false,\"code\":\"unsupported\"}", "400 Bad Request");
    char* end = nullptr;
    const std::size_t offset = cursor[0] == '\0' ? 0 : static_cast<std::size_t>(std::strtoul(cursor, &end, 10));
    if (cursor[0] != '\0' && (end == cursor || *end != '\0')) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    end = nullptr;
    const std::size_t limit = limit_text[0] == '\0' ? 20 : static_cast<std::size_t>(std::strtoul(limit_text, &end, 10));
    if (limit == 0 || limit > 30 || (limit_text[0] != '\0' && (end == limit_text || *end != '\0'))) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    const auto items = GetMediaLibrary().List(MediaCategory::kLocal, offset, limit);
    const std::size_t total = GetMediaLibrary().Count(MediaCategory::kLocal);
    std::string body = "{\"ok\":true,\"code\":\"ok\",\"data\":{\"items\":[";
    for (std::size_t index = 0; index < items.size(); ++index) { if (index > 0) body.push_back(','); AppendMediaItemJson(&body, items[index]); }
    body.append("],\"next_cursor\":\"");
    if (offset + items.size() < total) body.append(std::to_string(offset + items.size()));
    body.append("\",\"revision\":");
    AppendUInt64(&body, GetMediaLibrary().revision());
    body.append("}}");
    return SendJson(req, body.c_str());
}

esp_err_t GetMediaSource(httpd_req_t* req, const MediaId& media_id);
esp_err_t GetMediaPreview(httpd_req_t* req, const MediaId& media_id);

esp_err_t GetMediaDetail(httpd_req_t* req) {
    constexpr char kPrefix[] = "/api/v1/media/";
    if (std::strncmp(req->uri, kPrefix, sizeof(kPrefix) - 1) != 0) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    const char* media_id = req->uri + sizeof(kPrefix) - 1;
    const std::size_t uri_length = std::strlen(req->uri);
    constexpr char kSourceSuffix[] = "/source";
    constexpr char kPreviewSuffix[] = "/preview";
    if (uri_length > sizeof(kPrefix) + sizeof(kPreviewSuffix) - 2 &&
        std::strcmp(req->uri + uri_length - (sizeof(kPreviewSuffix) - 1), kPreviewSuffix) == 0) {
        const MediaId preview_media_id(media_id, uri_length - (sizeof(kPrefix) - 1) - (sizeof(kPreviewSuffix) - 1));
        if (preview_media_id.empty() || preview_media_id.size() > 64) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
        return GetMediaPreview(req, preview_media_id);
    }
    if (uri_length > sizeof(kPrefix) + sizeof(kSourceSuffix) - 2 &&
        std::strcmp(req->uri + uri_length - (sizeof(kSourceSuffix) - 1), kSourceSuffix) == 0) {
        const MediaId source_media_id(media_id, uri_length - (sizeof(kPrefix) - 1) - (sizeof(kSourceSuffix) - 1));
        if (source_media_id.empty() || source_media_id.size() > 64) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
        return GetMediaSource(req, source_media_id);
    }
    if (*media_id == '\0' || std::strlen(media_id) > 64 || std::strcmp(media_id, "upload") == 0) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    MediaItem item;
    if (!GetMediaLibrary().Find(media_id, &item)) return SendJson(req, "{\"ok\":false,\"code\":\"media_not_found\"}", "404 Not Found");
    std::string body = "{\"ok\":true,\"code\":\"ok\",\"data\":";
    AppendMediaItemJson(&body, item);
    body.append("}");
    return SendJson(req, body.c_str());
}

esp_err_t GetMediaSource(httpd_req_t* req, const MediaId& media_id) {
    MediaItem item;
    if (!GetMediaLibrary().Find(media_id, &item) || !item.source.present ||
        (item.source.mime_type != "image/jpeg" && item.source.mime_type != "image/png") || item.source.bytes == 0) {
        return SendJson(req, "{\"ok\":false,\"code\":\"media_not_found\"}", "404 Not Found");
    }
    const char* source_file = item.source.mime_type == "image/png" ? "source.png" : "source.jpg";
    httpd_resp_set_type(req, item.source.mime_type.c_str());
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t result = GetStorageService().StreamCommittedFile(
        "media/" + item.media_id + "/" + source_file, item.source.bytes,
        [req](const void* data, std::size_t size) { return httpd_resp_send_chunk(req, static_cast<const char*>(data), size); });
    if (result != ESP_OK) return result;
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// A fixed 192000-byte 4bpp frame is compact enough for gallery thumbnails and
// avoids streaming multi-megabyte phone originals through the one-slot server.
esp_err_t GetMediaPreview(httpd_req_t* req, const MediaId& media_id) {
    MediaItem item;
    if (!GetMediaLibrary().Find(media_id, &item) || !item.frame.present || item.frame.bytes != kDisplayFrameBytes) {
        return SendJson(req, "{\"ok\":false,\"code\":\"media_not_found\"}", "404 Not Found");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t result = GetStorageService().StreamCommittedFile(
        "media/" + item.media_id + "/image.bin", item.frame.bytes,
        [req](const void* data, std::size_t size) { return httpd_resp_send_chunk(req, static_cast<const char*>(data), size); });
    if (result != ESP_OK) return result;
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t StepMedia(httpd_req_t* req) {
    if (req == nullptr || req->content_len <= 0 || req->content_len > 512) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    char body[513]{};
    const int read = httpd_req_recv(req, body, req->content_len);
    if (read != req->content_len) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    JsonDocument input;
    if (deserializeJson(input, body) != DeserializationError::Ok) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    const std::string direction = input["direction"] | "";
    const RequestId request_id = input["request_id"] | "";
    const Revision expected_revision = input["expected_mode_revision"] | 0ULL;
    const std::string after_display = input["after_display"] | "hold";
    if ((direction != "next" && direction != "previous") || request_id.empty() || request_id.size() > 64 ||
        (after_display != "continue" && after_display != "hold")) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    const DisplaySnapshot display = GetDisplayService().GetSnapshot();
    if (display.current_media_id.empty()) return SendJson(req, "{\"ok\":false,\"code\":\"no_current_media\"}", "409 Conflict");
    MediaItem target;
    if (!GetMediaLibrary().FindAdjacent(MediaCategory::kLocal, display.current_media_id,
                                        direction == "next" ? 1 : -1, &target)) {
        return SendJson(req, "{\"ok\":false,\"code\":\"media_not_found\"}", "404 Not Found");
    }
    return SubmitLocalDisplay(req, target.media_id, request_id, expected_revision, after_display);
}

esp_err_t DisplayMedia(httpd_req_t* req) {
    constexpr char kPrefix[] = "/api/v1/media/";
    constexpr char kSuffix[] = "/display";
    if (std::strcmp(req->uri, "/api/v1/media/current/step") == 0) return StepMedia(req);
    if (std::strncmp(req->uri, kPrefix, sizeof(kPrefix) - 1) != 0) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    const std::size_t uri_length = std::strlen(req->uri);
    if (uri_length <= sizeof(kPrefix) + sizeof(kSuffix) - 2 || std::strcmp(req->uri + uri_length - (sizeof(kSuffix) - 1), kSuffix) != 0) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    const std::string media_id(req->uri + sizeof(kPrefix) - 1, uri_length - (sizeof(kPrefix) - 1) - (sizeof(kSuffix) - 1));
    RequestId request_id;
    Revision expected_revision = 0;
    std::string after_display;
    if (!ParseDisplayRequest(req, &request_id, &expected_revision, &after_display)) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    return SubmitLocalDisplay(req, media_id, request_id, expected_revision, after_display);
}

bool IsRefreshInProgress(DisplayState state) {
    return state == DisplayState::kQueued || state == DisplayState::kLoading ||
           state == DisplayState::kRefreshing || state == DisplayState::kFinalizing;
}

esp_err_t DeleteMedia(httpd_req_t* req) {
    constexpr char kPrefix[] = "/api/v1/media/";
    if (req == nullptr || std::strncmp(req->uri, kPrefix, sizeof(kPrefix) - 1) != 0 ||
        req->content_len <= 0 || req->content_len > 128) {
        return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    }
    const char* media_id = req->uri + sizeof(kPrefix) - 1;
    if (*media_id == '\0' || std::strlen(media_id) > 64 || std::strchr(media_id, '/') != nullptr ||
        std::strcmp(media_id, "upload") == 0) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    char body[129]{};
    int received_total = 0;
    while (received_total < req->content_len) {
        const int received = httpd_req_recv(req, body + received_total, req->content_len - received_total);
        if (received <= 0) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
        received_total += received;
    }
    JsonDocument input;
    if (deserializeJson(input, body) != DeserializationError::Ok) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    const Revision expected_revision = input["expected_revision"] | 0ULL;
    if (expected_revision == 0) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    MediaItem item;
    if (!GetMediaLibrary().Find(media_id, &item)) return SendJson(req, "{\"ok\":false,\"code\":\"media_not_found\"}", "404 Not Found");
    if (item.revision != expected_revision) return SendJson(req, "{\"ok\":false,\"code\":\"revision_conflict\"}", "409 Conflict");
    const DisplaySnapshot display = GetDisplayService().GetSnapshot();
    if (display.current_media_id == item.media_id || display.queued_target_media_id == item.media_id) {
        return SendJson(req, "{\"ok\":false,\"code\":\"current_media_protected\"}", "409 Conflict");
    }
    if (IsRefreshInProgress(display.state)) return SendJson(req, "{\"ok\":false,\"code\":\"display_busy\"}", "409 Conflict");
    const esp_err_t removed = GetStorageService().DeleteCommittedMedia(item.media_id);
    if (removed == ESP_ERR_NOT_FOUND) return SendJson(req, "{\"ok\":false,\"code\":\"media_not_found\"}", "404 Not Found");
    if (removed == ESP_ERR_INVALID_STATE) return SendJson(req, "{\"ok\":false,\"code\":\"storage_busy\"}", "503 Service Unavailable");
    if (removed != ESP_OK) return SendJson(req, "{\"ok\":false,\"code\":\"storage_delete_failed\"}", "500 Internal Server Error");
    if (!GetMediaLibrary().RemoveCommitted(item.media_id, expected_revision)) {
        return SendJson(req, "{\"ok\":false,\"code\":\"media_index_update_failed\"}", "500 Internal Server Error");
    }
    std::string response = "{\"ok\":true,\"code\":\"media_deleted\",\"data\":{\"media_id\":\"" + item.media_id + "\",\"revision\":";
    AppendUInt64(&response, GetMediaLibrary().revision());
    response.append("}}");
    return SendJson(req, response.c_str());
}

esp_err_t Status(httpd_req_t* req) {
    const auto storage = GetStorageService().GetSnapshot();
    const auto mode = GetModeManager().GetSnapshot();
    const auto display = GetDisplayService().GetSnapshot();
    char mode_revision[21], total_bytes[21], free_bytes[21];
    FormatUInt64(mode.revision, mode_revision);
    FormatUInt64(storage.total_bytes, total_bytes);
    FormatUInt64(storage.free_bytes, free_bytes);
    char body[512];
    std::snprintf(body, sizeof(body), "{\"ok\":true,\"code\":\"ok\",\"data\":{\"api_version\":\"v1\",\"mode\":{\"active_feature\":%u,\"revision\":%s},\"storage\":{\"state\":%u,\"total_bytes\":%s,\"free_bytes\":%s},\"display\":{\"state\":%u,\"current_media_id\":\"%s\"}}}", static_cast<unsigned>(mode.active_feature), mode_revision, static_cast<unsigned>(storage.state), total_bytes, free_bytes, static_cast<unsigned>(display.state), display.current_media_id.c_str());
    return SendJson(req, body);
}

esp_err_t DisplayStatus(httpd_req_t* req) {
    const auto display = GetDisplayService().GetSnapshot();
    char body[256];
    std::snprintf(body, sizeof(body), "{\"ok\":true,\"code\":\"ok\",\"data\":{\"state\":%u,\"current_media_id\":\"%s\",\"last_error_code\":\"%s\"}}", static_cast<unsigned>(display.state), display.current_media_id.c_str(), display.last_error_code.c_str());
    return SendJson(req, body);
}

esp_err_t NetworkStatus(httpd_req_t* req) {
    const auto network = GetProductNetworkSnapshot();
    char revision[21];
    FormatUInt64(network.revision, revision);
    std::string body = "{\"ok\":true,\"code\":\"ok\",\"data\":{\"api_version\":\"v1\",\"device_id\":\"esp32-s3-photopainter\",\"ap\":{\"enabled\":";
    body.append(network.ap_enabled ? "true" : "false").append(",\"ssid\":");
    AppendJsonString(&body, network.ap_ssid);
    body.append(",\"ip\":"); AppendJsonString(&body, network.ap_ip);
    body.append(",\"channel\":").append(std::to_string(network.ap_channel))
        .append(",\"connected_clients\":").append(std::to_string(network.ap_connected_clients)).append("},\"sta\":{\"enabled\":true,\"configured\":")
        .append(network.sta_configured ? "true" : "false").append(",\"state\":\"")
        .append(network.sta_connected ? "connected" : (network.last_error_code == "sta_testing" ? "connecting" : (network.sta_configured ? "failed" : "disabled"))).append("\",\"ssid\":");
    AppendJsonString(&body, network.sta_ssid);
    body.append(",\"ip\":"); AppendJsonString(&body, network.sta_ip);
    body.append(",\"gateway\":"); AppendJsonString(&body, network.sta_gateway);
    if (network.sta_connected) body.append(",\"rssi_dbm\":").append(std::to_string(network.sta_rssi_dbm));
    body.append(",\"last_error_code\":"); AppendJsonString(&body, network.last_error_code);
    body.append("},\"internet\":{\"state\":\"unknown\"},\"revision\":").append(revision).append("}}");
    return SendJson(req, body.c_str());
}

bool DecodeFormValue(const char* input, char* output, std::size_t output_size) {
    std::size_t written = 0;
    for (std::size_t i = 0; input[i] != '\0'; ++i) {
        char value = input[i];
        if (value == '+') value = ' ';
        if (value == '%') {
            const char hi = input[++i], lo = input[++i];
            if (!hi || !lo) return false;
            auto hex = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1)); };
            const int h = hex(hi), l = hex(lo);
            if (h < 0 || l < 0) return false;
            value = static_cast<char>((h << 4) | l);
        }
        if (written + 1 >= output_size) return false;
        output[written++] = value;
    }
    output[written] = '\0';
    return true;
}

esp_err_t ConfigureSta(httpd_req_t* req) {
    if (req->content_len == 0 || req->content_len > 196) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\",\"message\":\"invalid form\"}", "400 Bad Request");
    char form[197]{};
    const int read = httpd_req_recv(req, form, req->content_len);
    if (read != req->content_len) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\",\"message\":\"incomplete form\"}", "400 Bad Request");
    char ssid[33]{}, password[65]{};
    const bool is_json = std::strstr(form, "\"ssid\"") != nullptr;
    if (is_json) {
        JsonDocument input;
        if (deserializeJson(input, form) != DeserializationError::Ok) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
        const std::string parsed_ssid = input["ssid"] | "";
        const std::string parsed_password = input["password"] | "";
        if (parsed_ssid.size() >= sizeof(ssid) || parsed_password.size() >= sizeof(password)) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
        std::strcpy(ssid, parsed_ssid.c_str()); std::strcpy(password, parsed_password.c_str());
    } else {
        char ssid_encoded[100]{}, password_encoded[100]{};
        const char* ssid_start = std::strstr(form, "ssid=");
        const char* password_start = std::strstr(form, "password=");
        if (!ssid_start || !password_start || std::sscanf(ssid_start, "ssid=%99[^&]", ssid_encoded) != 1 || std::sscanf(password_start, "password=%99[^&]", password_encoded) != 1 || !DecodeFormValue(ssid_encoded, ssid, sizeof(ssid)) || !DecodeFormValue(password_encoded, password, sizeof(password))) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\",\"message\":\"ssid and password required\"}", "400 Bad Request");
    }
    if (std::strlen(ssid) == 0 || std::strlen(password) < 8) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\",\"message\":\"invalid Wi-Fi credentials\"}", "400 Bad Request");
    const esp_err_t result = ConfigureProductSta(ssid, password);
    if (result != ESP_OK) return SendJson(req, "{\"ok\":false,\"code\":\"sta_connect_failed\",\"message\":\"unable to start STA connection\"}", "503 Service Unavailable");
    if (!WaitForProductStaConnection(12000)) return SendJson(req, "{\"ok\":false,\"code\":\"sta_connect_failed\",\"message\":\"connection timed out\"}", "503 Service Unavailable");
    const auto network = GetProductNetworkSnapshot();
    char body[192];
    std::snprintf(body, sizeof(body), "{\"ok\":true,\"code\":\"ok\",\"message\":\"connected\",\"data\":{\"sta_ip\":\"%s\"}}", network.sta_ip.c_str());
    return SendJson(req, body);
}

esp_err_t ScanWifi(httpd_req_t* req) {
    if (req->content_len > 2) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    std::vector<ScannedWifiNetwork> networks;
    const esp_err_t result = ScanProductWifi24Ghz(&networks);
    if (result != ESP_OK) return SendJson(req, "{\"ok\":false,\"code\":\"network_busy\"}", "503 Service Unavailable");
    std::string body = "{\"ok\":true,\"code\":\"ok\",\"data\":{\"networks\":[";
    for (std::size_t index = 0; index < networks.size(); ++index) {
        if (index != 0) body.push_back(',');
        body.append("{\"ssid\":"); AppendJsonString(&body, networks[index].ssid);
        body.append(",\"rssi_dbm\":").append(std::to_string(networks[index].rssi_dbm))
            .append(",\"channel\":").append(std::to_string(networks[index].channel)).append(",\"security\":");
        AppendJsonString(&body, networks[index].security); body.push_back('}');
    }
    body.append("]}}");
    return SendJson(req, body.c_str());
}

bool ParseJsonCredentials(httpd_req_t* req, std::string* ssid, std::string* password) {
    if (req == nullptr || req->content_len <= 0 || req->content_len > 196) return false;
    char body[197]{}; int total = 0;
    while (total < req->content_len) { const int read = httpd_req_recv(req, body + total, req->content_len - total); if (read <= 0) return false; total += read; }
    JsonDocument input;
    if (deserializeJson(input, body) != DeserializationError::Ok) return false;
    *ssid = input["ssid"] | ""; *password = input["password"] | "";
    return true;
}

esp_err_t ConfigureAp(httpd_req_t* req) {
    std::string ssid, password;
    if (!ParseJsonCredentials(req, &ssid, &password)) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    const esp_err_t result = ConfigureProductAp(ssid, password);
    if (result == ESP_ERR_INVALID_ARG) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_ap_credentials\"}", "400 Bad Request");
    if (result != ESP_OK) return SendJson(req, "{\"ok\":false,\"code\":\"ap_save_failed\"}", "500 Internal Server Error");
    const auto snapshot = GetProductNetworkSnapshot();
    std::string body = "{\"ok\":true,\"code\":\"ap_saved\",\"data\":{\"ssid\":"; AppendJsonString(&body, snapshot.ap_ssid); body.append(",\"ip\":"); AppendJsonString(&body, snapshot.ap_ip); body.append("}}");
    return SendJson(req, body.c_str());
}

esp_err_t RestoreDefaultAp(httpd_req_t* req) {
    if (req->content_len > 2) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    if (RestoreDefaultProductAp() != ESP_OK) return SendJson(req, "{\"ok\":false,\"code\":\"ap_restore_failed\"}", "500 Internal Server Error");
    return SendJson(req, "{\"ok\":true,\"code\":\"ap_restored\",\"data\":{\"ssid\":\"esp_network\",\"ip\":\"192.168.4.1\"}}");
}

esp_err_t ForgetSta(httpd_req_t* req) {
    if (req->content_len > 2) return SendJson(req, "{\"ok\":false,\"code\":\"invalid_request\"}", "400 Bad Request");
    if (ForgetProductSta() != ESP_OK) return SendJson(req, "{\"ok\":false,\"code\":\"network_busy\"}", "503 Service Unavailable");
    return SendJson(req, "{\"ok\":true,\"code\":\"sta_forgotten\"}");
}

esp_err_t ProvisionPage(httpd_req_t* req) {
    static constexpr char page[] = R"HTML(<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><title>E-ink Wi-Fi Setup</title><style>body{font-family:sans-serif;max-width:420px;margin:48px auto;padding:0 20px;color:#33242c}input,button{box-sizing:border-box;width:100%;padding:13px;margin:8px 0;font-size:16px}button{background:#d95788;color:white;border:0;border-radius:8px}.ok{color:#16803c}.bad{color:#bd263c}</style><h2>墨水屏相册配网</h2><p>仅支持 2.4 GHz Wi-Fi；esp_network 会保留。</p><form id="f"><input name="ssid" placeholder="Wi-Fi 名称 (SSID)" required maxlength="32"><input name="password" type="password" placeholder="Wi-Fi 密码" maxlength="64"><button id="b">连接 Wi-Fi</button></form><p id="r">请输入信息后连接。</p><script>let r=document.querySelector('#r'),b=document.querySelector('#b');f.onsubmit=async e=>{e.preventDefault();b.disabled=true;r.className='';r.textContent='正在连接，最多等待 12 秒…';try{let x=await fetch('/api/v1/network/sta',{method:'POST',body:new URLSearchParams(new FormData(f))}),j=await x.json();if(!x.ok)throw 0;r.className='ok';r.textContent='连接成功，设备局域网地址：'+j.data.sta_ip}catch(e){r.className='bad';r.textContent='连接未成功：请确认 2.4 GHz、名称和密码'}b.disabled=false};</script>)HTML";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}
}

esp_err_t RegisterProductApi(httpd_handle_t server) {
    const httpd_uri_t routes[] = {
        {.uri="/", .method=HTTP_GET, .handler=ProvisionPage, .user_ctx=nullptr},
        {.uri="/setup", .method=HTTP_GET, .handler=ProvisionPage, .user_ctx=nullptr},
        {.uri="/index.html", .method=HTTP_GET, .handler=ProvisionPage, .user_ctx=nullptr},
        {.uri="/api/v1/health", .method=HTTP_GET, .handler=Health, .user_ctx=nullptr},
        {.uri="/api/v1/device/capabilities", .method=HTTP_GET, .handler=Capabilities, .user_ctx=nullptr},
        {.uri="/api/v1/device/status", .method=HTTP_GET, .handler=Status, .user_ctx=nullptr},
        {.uri="/api/v1/display/status", .method=HTTP_GET, .handler=DisplayStatus, .user_ctx=nullptr},
        {.uri="/api/v1/network/status", .method=HTTP_GET, .handler=NetworkStatus, .user_ctx=nullptr},
        {.uri="/api/v1/network/scan", .method=HTTP_POST, .handler=ScanWifi, .user_ctx=nullptr},
        {.uri="/api/v1/network/sta", .method=HTTP_POST, .handler=ConfigureSta, .user_ctx=nullptr},
        {.uri="/api/v1/network/sta", .method=HTTP_DELETE, .handler=ForgetSta, .user_ctx=nullptr},
        {.uri="/api/v1/network/ap", .method=HTTP_POST, .handler=ConfigureAp, .user_ctx=nullptr},
        {.uri="/api/v1/network/ap/restore-default", .method=HTTP_POST, .handler=RestoreDefaultAp, .user_ctx=nullptr},
        {.uri="/api/v1/media/upload", .method=HTTP_POST, .handler=UploadMedia, .user_ctx=nullptr},
        {.uri="/api/v1/media", .method=HTTP_GET, .handler=ListMedia, .user_ctx=nullptr},
        {.uri="/api/v1/media/*", .method=HTTP_GET, .handler=GetMediaDetail, .user_ctx=nullptr},
        {.uri="/api/v1/media/*", .method=HTTP_POST, .handler=DisplayMedia, .user_ctx=nullptr},
        {.uri="/api/v1/media/*", .method=HTTP_DELETE, .handler=DeleteMedia, .user_ctx=nullptr},
        {.uri="/api/v1/jobs/*", .method=HTTP_GET, .handler=JobStatus, .user_ctx=nullptr},
    };
    for (const auto& route : routes) { const esp_err_t result = httpd_register_uri_handler(server, &route); if (result != ESP_OK) return result; }
    return ESP_OK;
}
}  // namespace photopainter::product
