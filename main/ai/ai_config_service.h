#pragma once

#include <string>

#include "esp_err.h"

namespace photopainter::product {

// Device-local provider configuration. The API key never leaves this service
// except in the HTTPS Authorization header assembled by the generation worker.
struct AiProviderConfig {
    bool configured = false;
    std::string endpoint;
    std::string model;
    std::string key_last4;
};

// Result intentionally exposes no credential, provider response body, or URL
// query data. This request only reads the provider model catalogue and never
// starts image generation, writes TF, or refreshes the panel.
struct AiConfigTestResult {
    bool configured = false;
    bool endpoint_reachable = false;
    bool authenticated = false;
    bool model_available = false;
    int http_status = 0;
    std::string code;
};

class AiConfigService {
public:
    AiProviderConfig GetPublicConfig() const;
    bool GetSecret(std::string* endpoint, std::string* model, std::string* key) const;
    esp_err_t Save(const std::string& endpoint, const std::string& model, const std::string& api_key);
    esp_err_t TestConnection(AiConfigTestResult* result) const;
    esp_err_t Clear();
};

}  // namespace photopainter::product
