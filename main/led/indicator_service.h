#pragma once

#include "esp_err.h"

namespace photopainter::product {

// Product-facing LED adapter. The official LED worker tasks remain the only
// GPIO writers so legacy modes and product services never contend for pins.
class IndicatorService {
public:
    esp_err_t Initialize();
    void SetRefreshActive(bool active);
    void RunSelfTest();

private:
    bool initialized_ = false;
};

IndicatorService& GetIndicatorService();
esp_err_t InitializeIndicatorService();

}  // namespace photopainter::product
