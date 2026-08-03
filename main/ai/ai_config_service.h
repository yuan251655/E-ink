#pragma once

#include <string>
#include <vector>

#include "esp_err.h"

namespace photopainter::product {

// Device-local provider configuration. The API key never leaves this service
// except in the HTTPS Authorization header assembled by the generation worker.
struct AiProviderConfig {
    bool configured = false;
    std::string profile_id;
    std::string profile_name;
    std::string endpoint;
    std::string model;
    std::string key_last4;
};

// Never use this type to return credentials. `key_last4` is the maximum
// credential detail permitted through the product API or device logs.
struct AiProviderProfile {
    std::string id;
    std::string name;
    std::string endpoint;
    std::string model;
    std::string key_last4;
    bool active = false;
};

// Result intentionally exposes no credential, provider response body, or URL
// query data. This request only reads the provider model catalogue and never
// starts image generation, writes TF, or refreshes the panel.
struct AiConfigTestResult {
    bool configured = false;
    bool network_reachable = false;
    bool endpoint_reachable = false;
    bool authenticated = false;
    bool model_available = false;
    int http_status = 0;
    std::string code;
    std::string provider_message;
};

class AiConfigService {
public:
    AiProviderConfig GetPublicConfig() const;
    bool GetSecret(std::string* endpoint, std::string* model, std::string* key) const;
    esp_err_t Save(const std::string& endpoint, const std::string& model, const std::string& api_key);
    std::vector<AiProviderProfile> ListProfiles() const;
    esp_err_t SaveProfile(const std::string& id, const std::string& name, const std::string& endpoint,
                          const std::string& model, const std::string& api_key, AiProviderProfile* output);
    esp_err_t ActivateProfile(const std::string& id);
    esp_err_t DeleteProfile(const std::string& id);
    esp_err_t TestConnection(bool allow_billable_test, AiConfigTestResult* result) const;
    esp_err_t Clear();
};

}  // namespace photopainter::product
