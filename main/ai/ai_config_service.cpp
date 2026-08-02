#include "ai_config_service.h"

#include <cstring>

#include "ArduinoJson.h"
#include "esp_http_client.h"
#include "esp_tls_errors.h"
#include "nvs.h"

namespace photopainter::product {
namespace {
constexpr char kNamespace[] = "ai_provider";
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
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return result;
    std::string key;
    result.configured = ReadString(handle, kEndpoint, kEndpointMax, &result.endpoint) &&
                        ReadString(handle, kModel, kModelMax, &result.model) &&
                        ReadString(handle, kKey, kKeyMax, &key);
    nvs_close(handle);
    if (result.configured && key.size() >= 4) result.key_last4 = key.substr(key.size() - 4);
    else if (!result.configured) { result.endpoint.clear(); result.model.clear(); }
    return result;
}

bool AiConfigService::GetSecret(std::string* endpoint, std::string* model, std::string* key) const {
    if (!endpoint || !model || !key) return false;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    const bool ok = ReadString(handle, kEndpoint, kEndpointMax, endpoint) &&
                    ReadString(handle, kModel, kModelMax, model) && ReadString(handle, kKey, kKeyMax, key);
    nvs_close(handle);
    return ok;
}

esp_err_t AiConfigService::Save(const std::string& endpoint, const std::string& model, const std::string& api_key) {
    if (endpoint.rfind("https://", 0) != 0 || endpoint.size() >= kEndpointMax || model.empty() ||
        model.size() >= kModelMax || api_key.size() >= kKeyMax) return ESP_ERR_INVALID_ARG;
    std::string stored_key = api_key;
    if (stored_key.empty()) {
        std::string old_endpoint, old_model;
        if (!GetSecret(&old_endpoint, &old_model, &stored_key)) return ESP_ERR_INVALID_ARG;
    }
    if (stored_key.size() < 8) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, kEndpoint, endpoint.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, kModel, model.c_str());
    if (err == ESP_OK) err = nvs_set_str(handle, kKey, stored_key.c_str());
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
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
}  // namespace photopainter::product
