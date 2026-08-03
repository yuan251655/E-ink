#include "ai_config_service.h"

#include <cstring>
#include <algorithm>
#include <cstdio>

#include "ArduinoJson.h"
#include "esp_http_client.h"
#include "esp_tls_errors.h"
#include "nvs.h"

namespace photopainter::product {
namespace {
constexpr char kNamespace[] = "ai_provider";
constexpr char kProfilesNamespace[] = "ai_profiles";
constexpr char kProfilesMigrated[] = "migrated";
constexpr char kProfileCount[] = "count";
constexpr char kActiveProfile[] = "active";
constexpr std::size_t kMaxProfiles = 5;
constexpr char kEndpoint[] = "endpoint";
constexpr char kModel[] = "model";
constexpr char kKey[] = "api_key";
constexpr std::size_t kEndpointMax = 256;
constexpr std::size_t kModelMax = 96;
constexpr std::size_t kKeyMax = 320;
constexpr std::size_t kMaxPreflightResponseBytes = 8U * 1024U;
extern const uint8_t ark_vol_pem_start[] asm("_binary_ark_vol_pem_start");

bool ReadString(nvs_handle_t handle, const char* name, std::size_t maximum, std::string* out) {
    std::size_t length = 0;
    if (nvs_get_str(handle, name, nullptr, &length) != ESP_OK || length == 0 || length > maximum) return false;
    std::string value(length, '\0');
    if (nvs_get_str(handle, name, value.data(), &length) != ESP_OK) return false;
    value.resize(std::strlen(value.c_str()));
    *out = value;
    return true;
}

bool IsSafeProfileId(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    for (const char ch : id) {
        const bool alpha_num = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                               (ch >= '0' && ch <= '9');
        if (!alpha_num && ch != '-' && ch != '_') return false;
    }
    return true;
}

void ProfileKey(char output[16], const char* field, std::size_t slot) {
    std::snprintf(output, 16, "%s%u", field, static_cast<unsigned>(slot));
}

struct StoredProfile {
    std::string id, name, endpoint, model, key;
};

bool ReadProfile(nvs_handle_t handle, std::size_t slot, StoredProfile* output) {
    if (!output) return false;
    char id[16], name[16], endpoint[16], model[16], key[16];
    ProfileKey(id, "id", slot); ProfileKey(name, "name", slot); ProfileKey(endpoint, "endpoint", slot);
    ProfileKey(model, "model", slot); ProfileKey(key, "key", slot);
    return ReadString(handle, id, 33, &output->id) && ReadString(handle, name, 65, &output->name) &&
        ReadString(handle, endpoint, kEndpointMax, &output->endpoint) && ReadString(handle, model, kModelMax, &output->model) &&
        ReadString(handle, key, kKeyMax, &output->key);
}

esp_err_t WriteProfile(nvs_handle_t handle, std::size_t slot, const StoredProfile& profile) {
    char id[16], name[16], endpoint[16], model[16], key[16];
    ProfileKey(id, "id", slot); ProfileKey(name, "name", slot); ProfileKey(endpoint, "endpoint", slot);
    ProfileKey(model, "model", slot); ProfileKey(key, "key", slot);
    esp_err_t err = nvs_set_str(handle, id, profile.id.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, name, profile.name.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, endpoint, profile.endpoint.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, model, profile.model.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, key, profile.key.c_str());
    return err;
}

void EraseProfile(nvs_handle_t handle, std::size_t slot) {
    char value[16];
    for (const char* field : {"id", "name", "endpoint", "model", "key"}) {
        ProfileKey(value, field, slot);
        (void)nvs_erase_key(handle, value);
    }
}

// Migrate the original one-provider namespace once.  It is intentionally
// copied rather than erased so old firmware can still start safely after a
// temporary rollback.
esp_err_t EnsureProfileMigration() {
    nvs_handle_t profiles;
    esp_err_t err = nvs_open(kProfilesNamespace, NVS_READWRITE, &profiles);
    if (err != ESP_OK) return err;
    std::uint8_t migrated = 0;
    if (nvs_get_u8(profiles, kProfilesMigrated, &migrated) == ESP_OK && migrated == 1) {
        nvs_close(profiles); return ESP_OK;
    }
    nvs_handle_t legacy;
    if (nvs_open(kNamespace, NVS_READONLY, &legacy) == ESP_OK) {
        StoredProfile old;
        if (ReadString(legacy, kEndpoint, kEndpointMax, &old.endpoint) &&
            ReadString(legacy, kModel, kModelMax, &old.model) && ReadString(legacy, kKey, kKeyMax, &old.key)) {
            old.id = "default"; old.name = "默认模型";
            err = WriteProfile(profiles, 0, old);
            if (err == ESP_OK) err = nvs_set_u8(profiles, kProfileCount, 1);
            if (err == ESP_OK) err = nvs_set_str(profiles, kActiveProfile, old.id.c_str());
        }
        nvs_close(legacy);
    }
    if (err == ESP_OK) err = nvs_set_u8(profiles, kProfilesMigrated, 1);
    if (err == ESP_OK) err = nvs_commit(profiles);
    nvs_close(profiles);
    return err;
}

std::vector<StoredProfile> ReadAllProfiles(nvs_handle_t handle) {
    std::vector<StoredProfile> output;
    std::uint8_t count = 0;
    (void)nvs_get_u8(handle, kProfileCount, &count);
    count = std::min<std::uint8_t>(count, static_cast<std::uint8_t>(kMaxProfiles));
    for (std::size_t slot = 0; slot < count; ++slot) { StoredProfile profile; if (ReadProfile(handle, slot, &profile)) output.push_back(std::move(profile)); }
    return output;
}

struct ResponseBuffer { std::string data; bool overflow = false; };

esp_err_t CollectResponse(esp_http_client_event_t* event) {
    auto* response = static_cast<ResponseBuffer*>(event->user_data);
    if (!response || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    if (response->data.size() + static_cast<std::size_t>(event->data_len) > kMaxPreflightResponseBytes) {
        response->overflow = true;
        return ESP_ERR_NO_MEM;
    }
    response->data.append(static_cast<const char*>(event->data), event->data_len);
    return ESP_OK;
}

std::string TestFailureCode(esp_err_t transport, int tls_error, int http_status) {
    if (http_status == 401) return "ai_http_401";
    if (http_status == 403) return "ai_http_403";
    if (http_status == 404) return "ai_http_404";
    if (http_status == 429) return "ai_http_429";
    if (http_status >= 500 && http_status <= 599) return "ai_http_5xx";
    if (tls_error != 0 || (transport >= ESP_ERR_ESP_TLS_BASE && transport < ESP_ERR_ESP_TLS_BASE + 0x100)) return "ai_tls_failed";
    if (transport == ESP_ERR_TIMEOUT || transport == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT || transport == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT) return "ai_request_timeout";
    if (transport == ESP_ERR_HTTP_CONNECT || transport == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME || transport == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST) return "ai_network_failed";
    return "ai_service_unavailable";
}

std::string ProviderErrorMessage(const std::string& response) {
    JsonDocument parsed;
    if (deserializeJson(parsed, response) != DeserializationError::Ok) return {};
    const char* message = parsed["error"]["message"] | parsed["message"] | "";
    std::string safe = message ? message : "";
    for (char& ch : safe) if (ch == '\r' || ch == '\n') ch = ' ';
    if (safe.size() > 160) safe.resize(160);
    return safe;
}
}

AiProviderConfig AiConfigService::GetPublicConfig() const {
    AiProviderConfig result;
    const auto profiles = ListProfiles();
    for (const auto& profile : profiles) {
        if (!profile.active) continue;
        result.configured = true;
        result.profile_id = profile.id;
        result.profile_name = profile.name;
        result.endpoint = profile.endpoint;
        result.model = profile.model;
        result.key_last4 = profile.key_last4;
        break;
    }
    return result;
}

bool AiConfigService::GetSecret(std::string* endpoint, std::string* model, std::string* key) const {
    if (!endpoint || !model || !key) return false;
    if (EnsureProfileMigration() != ESP_OK) return false;
    nvs_handle_t handle;
    if (nvs_open(kProfilesNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    std::string active;
    (void)ReadString(handle, kActiveProfile, 33, &active);
    bool ok = false;
    for (const auto& profile : ReadAllProfiles(handle)) {
        if (profile.id == active) { *endpoint = profile.endpoint; *model = profile.model; *key = profile.key; ok = true; break; }
    }
    nvs_close(handle);
    return ok;
}

esp_err_t AiConfigService::Save(const std::string& endpoint, const std::string& model, const std::string& api_key) {
    const auto current = GetPublicConfig();
    AiProviderProfile output;
    return SaveProfile(current.profile_id.empty() ? "default" : current.profile_id,
                       current.profile_name.empty() ? "默认模型" : current.profile_name,
                       endpoint, model, api_key, &output);
}

std::vector<AiProviderProfile> AiConfigService::ListProfiles() const {
    std::vector<AiProviderProfile> output;
    if (EnsureProfileMigration() != ESP_OK) return output;
    nvs_handle_t handle;
    if (nvs_open(kProfilesNamespace, NVS_READONLY, &handle) != ESP_OK) return output;
    std::string active;
    (void)ReadString(handle, kActiveProfile, 33, &active);
    for (const auto& stored : ReadAllProfiles(handle)) {
        AiProviderProfile profile;
        profile.id = stored.id; profile.name = stored.name; profile.endpoint = stored.endpoint; profile.model = stored.model;
        profile.active = stored.id == active;
        if (stored.key.size() >= 4) profile.key_last4 = stored.key.substr(stored.key.size() - 4);
        output.push_back(std::move(profile));
    }
    nvs_close(handle);
    return output;
}

esp_err_t AiConfigService::SaveProfile(const std::string& id, const std::string& name, const std::string& endpoint,
                                       const std::string& model, const std::string& api_key, AiProviderProfile* output) {
    if (!IsSafeProfileId(id) || name.empty() || name.size() > 64 || endpoint.rfind("https://", 0) != 0 ||
        endpoint.size() >= kEndpointMax || model.empty() || model.size() >= kModelMax || api_key.size() >= kKeyMax) return ESP_ERR_INVALID_ARG;
    esp_err_t err = EnsureProfileMigration(); if (err != ESP_OK) return err;
    nvs_handle_t handle; err = nvs_open(kProfilesNamespace, NVS_READWRITE, &handle); if (err != ESP_OK) return err;
    const auto existing = ReadAllProfiles(handle);
    std::size_t slot = existing.size(); std::string key = api_key; bool found = false;
    for (std::size_t i = 0; i < existing.size(); ++i) if (existing[i].id == id) { slot = i; found = true; if (key.empty()) key = existing[i].key; break; }
    if (slot >= kMaxProfiles || key.size() < 8) { nvs_close(handle); return ESP_ERR_INVALID_ARG; }
    StoredProfile saved{id, name, endpoint, model, key};
    err = WriteProfile(handle, slot, saved);
    if (err == ESP_OK && !found) err = nvs_set_u8(handle, kProfileCount, static_cast<std::uint8_t>(existing.size() + 1));
    std::string active;
    (void)ReadString(handle, kActiveProfile, 33, &active);
    if (err == ESP_OK && active.empty()) err = nvs_set_str(handle, kActiveProfile, id.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK && output) { output->id = id; output->name = name; output->endpoint = endpoint; output->model = model; output->key_last4 = key.substr(key.size() - 4); output->active = active.empty() || active == id; }
    return err;
}

esp_err_t AiConfigService::ActivateProfile(const std::string& id) {
    if (!IsSafeProfileId(id) || EnsureProfileMigration() != ESP_OK) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle; esp_err_t err = nvs_open(kProfilesNamespace, NVS_READWRITE, &handle); if (err != ESP_OK) return err;
    bool exists = false; for (const auto& profile : ReadAllProfiles(handle)) if (profile.id == id) { exists = true; break; }
    if (!exists) { nvs_close(handle); return ESP_ERR_NOT_FOUND; }
    err = nvs_set_str(handle, kActiveProfile, id.c_str()); if (err == ESP_OK) err = nvs_commit(handle); nvs_close(handle); return err;
}

esp_err_t AiConfigService::DeleteProfile(const std::string& id) {
    if (!IsSafeProfileId(id) || EnsureProfileMigration() != ESP_OK) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle; esp_err_t err = nvs_open(kProfilesNamespace, NVS_READWRITE, &handle); if (err != ESP_OK) return err;
    auto profiles = ReadAllProfiles(handle); std::size_t remove = profiles.size();
    for (std::size_t i = 0; i < profiles.size(); ++i) if (profiles[i].id == id) { remove = i; break; }
    if (remove == profiles.size()) { nvs_close(handle); return ESP_ERR_NOT_FOUND; }
    std::string active; (void)ReadString(handle, kActiveProfile, 33, &active);
    for (std::size_t i = remove; i + 1 < profiles.size(); ++i) { err = WriteProfile(handle, i, profiles[i + 1]); if (err != ESP_OK) break; }
    if (err == ESP_OK) EraseProfile(handle, profiles.size() - 1);
    if (err == ESP_OK) err = nvs_set_u8(handle, kProfileCount, static_cast<std::uint8_t>(profiles.size() - 1));
    if (err == ESP_OK && active == id) { if (profiles.size() > 1) err = nvs_set_str(handle, kActiveProfile, profiles[remove == 0 ? 1 : 0].id.c_str()); else err = nvs_erase_key(handle, kActiveProfile); }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t AiConfigService::TestConnection(bool allow_billable_test, AiConfigTestResult* result) const {
    if (!result) return ESP_ERR_INVALID_ARG;
    *result = {};
    std::string endpoint, model, key;
    if (!GetSecret(&endpoint, &model, &key)) { result->code = "ai_not_configured"; return ESP_ERR_INVALID_STATE; }
    result->configured = true;
    ResponseBuffer response;
    esp_http_client_config_t config{};
    config.url = endpoint.c_str();
    config.cert_pem = reinterpret_cast<const char*>(ark_vol_pem_start);
    config.event_handler = CollectResponse;
    config.user_data = &response;
    // Keep this aligned with the official PhotoPainter Ark client. Image
    // generation commonly takes longer than a normal local API request, and
    // a short timeout wrongly reports a reachable service as offline.
    config.timeout_ms = 180000;
    config.buffer_size = 4096;
    auto client = esp_http_client_init(&config);
    if (!client) { result->code = "ai_client_init_failed"; return ESP_ERR_NO_MEM; }
    // A zero-cost probe must not pretend that a 400 for an empty prompt proves
    // the credential or model works. A user-approved billable probe sends a
    // minimal real request but deliberately never downloads its result.
    JsonDocument request;
    request["model"] = model;
    request["prompt"] = allow_billable_test ? "A minimal abstract color test pattern" : "";
    // Seedream 5.0 Pro's official request profile. Do not carry over legacy
    // PhotoPainter-only fields because newer Ark models can reject them.
    request["response_format"] = "url";
    request["size"] = "2K";
    request["output_format"] = "png";
    request["watermark"] = false;
    std::string body;
    serializeJson(request, body);
    const std::string auth = "Bearer " + key;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body.data(), body.size());
    const esp_err_t transport = esp_http_client_perform(client);
    result->http_status = esp_http_client_get_status_code(client);
    int tls_error = 0, tls_flags = 0;
    (void)esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
    esp_http_client_cleanup(client);
    if (transport != ESP_OK || response.overflow) {
        result->code = TestFailureCode(transport, tls_error, result->http_status);
        return transport == ESP_OK ? ESP_FAIL : transport;
    }
    result->network_reachable = true;
    result->endpoint_reachable = true;
    result->provider_message = ProviderErrorMessage(response.data);
    if (allow_billable_test && result->http_status >= 200 && result->http_status < 300) {
        JsonDocument parsed;
        if (deserializeJson(parsed, response.data) == DeserializationError::Ok &&
            parsed["data"][0]["url"].is<const char*>()) {
            result->authenticated = true;
            result->model_available = true;
            result->code = "ok";
            return ESP_OK;
        }
        result->code = "ai_invalid_provider_response";
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!allow_billable_test && result->http_status == 400) {
        result->code = "model_requires_generation_test";
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (allow_billable_test && result->http_status == 400) {
        result->code = "ai_http_400";
        return ESP_FAIL;
    }
    if (result->http_status >= 200 && result->http_status < 300) {
        result->code = "model_requires_generation_test";
        return ESP_ERR_INVALID_RESPONSE;
    }
    result->code = TestFailureCode(transport, tls_error, result->http_status);
    return ESP_FAIL;
}

esp_err_t AiConfigService::Clear() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kProfilesNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    // Do not re-import a deliberately deleted legacy configuration on the
    // next read. The legacy namespace itself is retained for rollback safety.
    if (err == ESP_OK) err = nvs_set_u8(handle, kProfilesMigrated, 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
}  // namespace photopainter::product
