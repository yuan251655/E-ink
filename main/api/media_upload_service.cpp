#include "media_upload_service.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "ArduinoJson.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "job_service.h"
#include "mbedtls/sha256.h"
#include "media_library.h"
#include "storage_service.h"

namespace photopainter::product {
namespace {

constexpr std::size_t kMaxRequestBytes = 6U * 1024U * 1024U;
constexpr std::size_t kMaxMetadataBytes = 4U * 1024U;
constexpr std::size_t kMaxParts = 2;
constexpr std::size_t kReadBufferBytes = 2048;
constexpr std::size_t kHeaderLineBytes = 512;
constexpr std::size_t kWriteBufferBytes = 16U * 1024U;
constexpr char kTag[] = "MediaUpload";

struct Metadata {
    RequestId request_id;
    std::string display_name;
    std::string frame_sha256;
    DisplayProfile profile;
};

class RequestReader {
public:
    explicit RequestReader(httpd_req_t* request) : request_(request), remaining_(request == nullptr ? 0 : request->content_len) {}

    bool ReadByte(char* output) {
        if (output == nullptr || remaining_ <= 0) return false;
        if (offset_ == available_) {
            int received = 0;
            do {
                received = httpd_req_recv(request_, buffer_,
                                          remaining_ < static_cast<int>(sizeof(buffer_)) ? remaining_ : static_cast<int>(sizeof(buffer_)));
                if (received == HTTPD_SOCK_ERR_TIMEOUT && ++timeout_retries_ < kMaximumReceiveTimeoutRetries) continue;
                break;
            } while (true);
            if (received <= 0) {
                ESP_LOGE(kTag, "request receive failed ret=%d remaining=%d timeouts=%u", received, remaining_,
                         static_cast<unsigned>(timeout_retries_));
                return false;
            }
            timeout_retries_ = 0;
            offset_ = 0;
            available_ = static_cast<std::size_t>(received);
        }
        *output = buffer_[offset_++];
        --remaining_;
        return true;
    }

    bool ReadLine(std::string* output) {
        if (output == nullptr) return false;
        output->clear();
        char byte = 0;
        while (ReadByte(&byte)) {
            if (output->size() >= kHeaderLineBytes) return false;
            output->push_back(byte);
            const std::size_t n = output->size();
            if (n >= 2 && (*output)[n - 2] == '\r' && (*output)[n - 1] == '\n') {
                output->resize(n - 2);
                return true;
            }
        }
        return false;
    }

    bool Drain() {
        char ignored = 0;
        while (remaining_ > 0 && ReadByte(&ignored)) {}
        return remaining_ == 0;
    }

    bool FinishAfterFinalBoundary() {
        if (remaining_ == 0) return true;
        char carriage_return = 0;
        char line_feed = 0;
        return ReadByte(&carriage_return) && ReadByte(&line_feed) &&
               carriage_return == '\r' && line_feed == '\n' && remaining_ == 0;
    }

private:
    static constexpr std::uint8_t kMaximumReceiveTimeoutRetries = 10;
    httpd_req_t* request_ = nullptr;
    int remaining_ = 0;
    char buffer_[kReadBufferBytes]{};
    std::size_t offset_ = 0;
    std::size_t available_ = 0;
    std::uint8_t timeout_retries_ = 0;
};

bool IsHexHash(const std::string& value) {
    if (value.size() != 64) return false;
    for (const char c : value) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

bool IsValidUtf8(const std::string& value) {
    for (std::size_t i = 0; i < value.size();) {
        const unsigned char lead = static_cast<unsigned char>(value[i]);
        if (lead < 0x80U) { ++i; continue; }
        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        if (lead >= 0xc2U && lead <= 0xdfU) { continuation = 1; codepoint = lead & 0x1fU; }
        else if (lead >= 0xe0U && lead <= 0xefU) { continuation = 2; codepoint = lead & 0x0fU; }
        else if (lead >= 0xf0U && lead <= 0xf4U) { continuation = 3; codepoint = lead & 0x07U; }
        else return false;
        if (i + continuation >= value.size()) return false;
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            const unsigned char part = static_cast<unsigned char>(value[i + offset]);
            if ((part & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (part & 0x3fU);
        }
        if ((continuation == 1 && codepoint < 0x80U) || (continuation == 2 && codepoint < 0x800U) ||
            (continuation == 3 && codepoint < 0x10000U) || (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            codepoint > 0x10ffffU) return false;
        i += continuation + 1U;
    }
    return true;
}

std::string Lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string HexDigest(const unsigned char digest[32]) {
    char hex[65]{};
    for (std::size_t i = 0; i < 32; ++i) std::snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", digest[i]);
    return hex;
}

std::string HeaderParameter(const std::string& header, const char* key) {
    const std::string needle = std::string(key) + "=\"";
    const std::size_t begin = header.find(needle);
    if (begin == std::string::npos) return {};
    const std::size_t value_begin = begin + needle.size();
    const std::size_t end = header.find('"', value_begin);
    return end == std::string::npos ? std::string{} : header.substr(value_begin, end - value_begin);
}

std::string Trim(std::string value) {
    const std::size_t begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    const std::size_t end = value.find_last_not_of(" \t");
    return value.substr(begin, end - begin + 1);
}

bool ExtractBoundary(httpd_req_t* request, std::string* boundary) {
    if (request == nullptr || boundary == nullptr) return false;
    const std::size_t length = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (length == 0 || length > 256) return false;
    std::string type(length + 1, '\0');
    if (httpd_req_get_hdr_value_str(request, "Content-Type", type.data(), type.size()) != ESP_OK) return false;
    type.resize(std::strlen(type.c_str()));
    const std::size_t marker = type.find("boundary=");
    if (marker == std::string::npos) return false;
    std::string value = type.substr(marker + 9);
    const std::size_t semicolon = value.find(';');
    if (semicolon != std::string::npos) value.resize(semicolon);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    if (value.empty() || value.size() > 70 || value.find_first_of("\r\n") != std::string::npos) return false;
    *boundary = std::move(value);
    return true;
}

bool ParseMetadata(const std::string& json, Metadata* output, std::string* code) {
    if (output == nullptr || code == nullptr || json.empty() || json.size() > kMaxMetadataBytes) return false;
    // JsonDocument owns its pool on the heap.  The HTTP server task must not
    // reserve a multi-kilobyte JSON pool on its limited stack.
    JsonDocument document;
    if (deserializeJson(document, json) != DeserializationError::Ok) { *code = "invalid_request"; return false; }
    JsonObjectConst root = document.as<JsonObjectConst>();
    const std::string category = root["category"] | "";
    const std::string mode = root["upload_mode"] | "";
    if (category != "local") { *code = "unsupported"; return false; }
    if (mode != "bin_only") { *code = "unsupported"; return false; }

    Metadata parsed;
    parsed.request_id = root["request_id"] | "";
    if (parsed.request_id.empty() || parsed.request_id.size() > 64) { *code = "invalid_request"; return false; }
    parsed.display_name = root["display_name"] | "";
    if (parsed.display_name.size() > 128 || !IsValidUtf8(parsed.display_name)) { *code = "invalid_request"; return false; }
    for (const unsigned char c : parsed.display_name) {
        if (c < 0x20U || c == 0x7fU) { *code = "invalid_request"; return false; }
    }
    JsonObjectConst frame = root["image_bin"].as<JsonObjectConst>();
    if (frame.isNull()) frame = root["files"]["frame"].as<JsonObjectConst>();
    const std::uint64_t frame_bytes = frame["bytes"] | 0ULL;
    parsed.frame_sha256 = Lower(frame["sha256"] | "");
    JsonObjectConst profile = root["display_profile"].as<JsonObjectConst>();
    parsed.profile.width = profile["width"] | 0U;
    parsed.profile.height = profile["height"] | 0U;
    parsed.profile.frame_bytes = profile["frame_bytes"] | 0U;
    const std::string pixel_format = profile["pixel_format"] | "";
    const std::string palette = profile["palette"] | "";
    const std::string orientation = profile["orientation"] | "";
    const std::string fit_mode = profile["fit_mode"] | "";
    parsed.profile.rotation_degrees = profile["rotation_degrees"] | 0;
    parsed.profile.converter_version = profile["converter_version"] | "";
    if (parsed.profile.converter_version.empty()) {
        parsed.profile.converter_version = root["client_algorithm_version"] | "";
    }
    if (frame_bytes != kDisplayFrameBytes || !IsHexHash(parsed.frame_sha256) ||
        parsed.profile.width != kDisplayWidth || parsed.profile.height != kDisplayHeight ||
        parsed.profile.frame_bytes != kDisplayFrameBytes || pixel_format != "4bpp" || palette != "six_color_e6" ||
        (orientation != "landscape" && orientation != "portrait") ||
        (fit_mode != "contain" && fit_mode != "cover") || parsed.profile.converter_version.empty() ||
        parsed.profile.converter_version.size() > 64) {
        *code = "invalid_request";
        return false;
    }
    parsed.profile.pixel_format = PixelFormat::kIndexed4Bpp;
    parsed.profile.palette = Palette::kSixColorE6;
    parsed.profile.orientation = orientation == "portrait" ? Orientation::kPortrait : Orientation::kLandscape;
    parsed.profile.fit_mode = fit_mode == "cover" ? FitMode::kCover : FitMode::kContain;
    *output = std::move(parsed);
    return true;
}

std::string Fingerprint(const Metadata& metadata) {
    char profile[128]{};
    std::snprintf(profile, sizeof(profile), "%u:%u:%u:%d", metadata.profile.width, metadata.profile.height,
                  static_cast<unsigned>(metadata.profile.frame_bytes), metadata.profile.rotation_degrees);
    const std::string canonical = std::string("bin_only:") + metadata.frame_sha256 + ":" + profile + ":" +
                                  metadata.profile.converter_version + ":" + metadata.display_name;
    unsigned char digest[32]{};
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    const bool valid = mbedtls_sha256_starts(&hash, false) == 0 &&
                       mbedtls_sha256_update(&hash, reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size()) == 0 &&
                       mbedtls_sha256_finish(&hash, digest) == 0;
    mbedtls_sha256_free(&hash);
    return valid ? HexDigest(digest) : std::string{};
}

std::string NewSafeId(const char* prefix) {
    char id[32]{};
    std::snprintf(id, sizeof(id), "%s-%08lx", prefix, static_cast<unsigned long>(esp_random()));
    return id;
}

bool ReceivePartBody(RequestReader* reader, const std::string& boundary,
                     const std::function<bool(const char*, std::size_t)>& consume,
                     bool* final_boundary) {
    if (reader == nullptr || final_boundary == nullptr) return false;
    const std::string marker = "\r\n--" + boundary;
    std::string pending;
    pending.reserve(marker.size() + 1);
    // The HTTP server task has a deliberately small stack.  Keep the large
    // upload coalescing buffer on the heap instead of corrupting its stack
    // during a normal 192000-byte frame upload.
    std::vector<char> output(kWriteBufferBytes);
    std::size_t output_size = 0;
    auto flush = [&]() { return output_size == 0 || consume(output.data(), output_size); };
    char byte = 0;
    while (reader->ReadByte(&byte)) {
        pending.push_back(byte);
        if (pending.size() > marker.size()) {
            output[output_size++] = pending.front();
            pending.erase(0, 1);
            if (output_size == output.size()) {
                if (!flush()) return false;
                output_size = 0;
            }
        }
        if (pending == marker) {
            if (!flush()) return false;
            char suffix[2]{};
            if (!reader->ReadByte(&suffix[0]) || !reader->ReadByte(&suffix[1])) return false;
            if (suffix[0] == '-' && suffix[1] == '-') {
                *final_boundary = true;
                // A conforming client may append a final CRLF; it is harmless.
                return true;
            }
            if (suffix[0] == '\r' && suffix[1] == '\n') {
                *final_boundary = false;
                return true;
            }
            return false;
        }
    }
    return false;
}

void FailJob(JobService& jobs, const JobSnapshot& job, const std::string& code) {
    if (!job.job_id.empty()) (void)jobs.Update(job.job_id, JobState::kFailed, "failed", 0, code);
}

}  // namespace

MediaUploadResult ReceiveBinOnlyMultipart(httpd_req_t* request,
                                           StorageService& storage,
                                           MediaLibrary& media_library,
                                           JobService& jobs) {
    MediaUploadResult result;
    if (request == nullptr || request->content_len <= 0 || request->content_len > static_cast<int>(kMaxRequestBytes)) {
        result.code = "invalid_request";
        result.error = ESP_ERR_INVALID_SIZE;
        return result;
    }
    std::string boundary;
    if (!ExtractBoundary(request, &boundary)) { result.code = "invalid_request"; result.error = ESP_ERR_INVALID_ARG; return result; }
    RequestReader reader(request);
    std::string line;
    if (!reader.ReadLine(&line) || line != "--" + boundary) { result.code = "invalid_request"; result.error = ESP_ERR_INVALID_ARG; return result; }

    Metadata metadata;
    bool transaction_started = false;
    bool frame_received = false;
    std::size_t part_count = 0;
    std::string failure_code = "invalid_request";
    esp_err_t failure = ESP_FAIL;

    while (true) {
        if (++part_count > kMaxParts) { failure = ESP_ERR_INVALID_SIZE; break; }
        std::string disposition;
        std::string content_type;
        while (reader.ReadLine(&line)) {
            if (line.empty()) break;
            if (line.rfind("Content-Disposition:", 0) == 0) disposition = line;
            else if (line.rfind("Content-Type:", 0) == 0) content_type = line.substr(std::strlen("Content-Type:"));
        }
        if (line.empty() == false || disposition.empty()) { failure = ESP_ERR_INVALID_ARG; break; }
        const std::string name = HeaderParameter(disposition, "name");
        bool final_boundary = false;

        if (part_count == 1) {
            if (name != "metadata" || Trim(content_type).rfind("application/json", 0) != 0) { failure = ESP_ERR_INVALID_ARG; break; }
            std::string raw_metadata;
            const bool received = ReceivePartBody(&reader, boundary, [&](const char* data, std::size_t size) {
                if (raw_metadata.size() + size > kMaxMetadataBytes) return false;
                raw_metadata.append(data, size);
                return true;
            }, &final_boundary);
            if (!received || final_boundary || !ParseMetadata(raw_metadata, &metadata, &failure_code)) {
                failure = ESP_ERR_INVALID_ARG;
                break;
            }
            const JobRegistrationResult registration = jobs.CreateOrFind(JobKind::kUpload, metadata.request_id, Fingerprint(metadata), &result.job);
            if (registration == JobRegistrationResult::kExisting) {
                (void)reader.Drain();
                result.error = ESP_OK;
                result.code = "ok";
                return result;
            }
            if (registration != JobRegistrationResult::kCreated) {
                result.code = registration == JobRegistrationResult::kRequestIdConflict ? "request_id_conflict" : "storage_busy";
                result.error = registration == JobRegistrationResult::kRequestIdConflict ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM;
                return result;
            }
            (void)jobs.Update(result.job.job_id, JobState::kRunning, "receiving", 5);
            const std::uint64_t required = kDisplayFrameBytes + kMaxMetadataBytes;
            failure = storage.BeginWriteTransaction(NewSafeId("txn"), required);
            if (failure != ESP_OK) { failure_code = failure == ESP_ERR_NO_MEM ? "storage_no_space" : (failure == ESP_ERR_INVALID_STATE ? "storage_busy" : "storage_unavailable"); break; }
            transaction_started = true;
        } else if (part_count == 2) {
            if (name != "image_bin" || Trim(content_type) != "application/octet-stream") { failure = ESP_ERR_INVALID_ARG; break; }
            mbedtls_sha256_context hash;
            mbedtls_sha256_init(&hash);
            const bool started = mbedtls_sha256_starts(&hash, false) == 0;
            std::uint64_t bytes = 0;
            bool first = true;
            esp_err_t write_error = ESP_OK;
            const bool received = started && ReceivePartBody(&reader, boundary, [&](const char* data, std::size_t size) {
                if (bytes + size > kDisplayFrameBytes || mbedtls_sha256_update(&hash, reinterpret_cast<const unsigned char*>(data), size) != 0) return false;
                const esp_err_t write = storage.AppendStagedFile("image.bin", data, size, first);
                first = false;
                if (write != ESP_OK) { write_error = write; return false; }
                bytes += size;
                return true;
            }, &final_boundary);
            unsigned char digest[32]{};
            const bool finished = started && mbedtls_sha256_finish(&hash, digest) == 0;
            mbedtls_sha256_free(&hash);
            const esp_err_t finalized = storage.FinalizeStagedFile("image.bin", kDisplayFrameBytes);
            if (!received || !final_boundary || !finished || bytes != kDisplayFrameBytes || HexDigest(digest) != metadata.frame_sha256 || finalized != ESP_OK) {
                ESP_LOGE(kTag, "frame rejected recv=%d final=%d hash=%d bytes=%llu/%u finalize=%s write=%s", received, final_boundary, finished && HexDigest(digest) == metadata.frame_sha256, static_cast<unsigned long long>(bytes), static_cast<unsigned>(kDisplayFrameBytes), esp_err_to_name(finalized), esp_err_to_name(write_error));
                failure_code = write_error == ESP_OK ? ((bytes == kDisplayFrameBytes && finished && HexDigest(digest) != metadata.frame_sha256) ? "checksum_mismatch" : "media_incomplete") : "storage_write_failed"; failure = write_error == ESP_OK ? ESP_ERR_INVALID_SIZE : write_error; break; }
            if (!reader.FinishAfterFinalBoundary()) { failure = ESP_ERR_INVALID_ARG; break; }
            frame_received = true;
            break;
        } else {
            failure = ESP_ERR_INVALID_SIZE;
            break;
        }
    }

    if (transaction_started && frame_received && part_count == kMaxParts) {
        (void)jobs.Update(result.job.job_id, JobState::kRunning, "committing", 85);
        const MediaId media_id = NewSafeId("local");
        const EpochMs now = static_cast<EpochMs>(esp_timer_get_time() / 1000);
        JsonDocument manifest;
        manifest["media_id"] = media_id;
        manifest["display_name"] = metadata.display_name;
        manifest["category"] = "local";
        manifest["created_at_ms"] = now;
        manifest["updated_at_ms"] = now;
        manifest["manifest_version"] = 2;
        manifest["revision"] = 1;
        JsonObject profile = manifest.createNestedObject("display_profile");
        profile["width"] = metadata.profile.width; profile["height"] = metadata.profile.height;
        profile["frame_bytes"] = metadata.profile.frame_bytes; profile["pixel_format"] = "4bpp";
        profile["palette"] = "six_color_e6"; profile["orientation"] = metadata.profile.orientation == Orientation::kPortrait ? "portrait" : "landscape";
        profile["rotation_degrees"] = metadata.profile.rotation_degrees;
        profile["fit_mode"] = metadata.profile.fit_mode == FitMode::kCover ? "cover" : "contain";
        profile["converter_version"] = metadata.profile.converter_version;
        JsonObject files = manifest.createNestedObject("files");
        JsonObject frame = files.createNestedObject("frame");
        frame["present"] = true; frame["mime_type"] = "application/octet-stream"; frame["bytes"] = kDisplayFrameBytes; frame["sha256"] = metadata.frame_sha256;
        std::string manifest_json;
        serializeJson(manifest, manifest_json);
        ESP_LOGI(kTag, "admitting media=%s manifest=%u", media_id.c_str(), static_cast<unsigned>(manifest_json.size()));
        failure = storage.AppendStagedFile("manifest.json", manifest_json.data(), manifest_json.size(), true);
        if (failure == ESP_OK) failure = storage.FinalizeStagedFile("manifest.json", manifest_json.size());
        if (failure == ESP_OK) {
            failure = storage.CommitTransaction("media/" + media_id);
            if (failure != ESP_OK) ESP_LOGE(kTag, "media commit failed: %s", esp_err_to_name(failure));
        }
        if (failure == ESP_OK) {
            failure = media_library.RegisterCommitted(media_id);
            if (failure != ESP_OK) ESP_LOGE(kTag, "media register failed: %s", esp_err_to_name(failure));
        }
        if (failure == ESP_OK && jobs.CompleteSuccess(result.job.job_id, media_id)) {
            (void)jobs.Get(result.job.job_id, &result.job);
            result.error = ESP_OK;
            result.code = "ok";
            return result;
        }
        failure_code = failure == ESP_OK ? "storage_write_failed" : "storage_write_failed";
    }

    if (transaction_started) (void)storage.RollbackTransaction();
    FailJob(jobs, result.job, failure_code);
    (void)jobs.Get(result.job.job_id, &result.job);
    result.error = failure == ESP_OK ? ESP_FAIL : failure;
    result.code = failure_code;
    return result;
}

}  // namespace photopainter::product
