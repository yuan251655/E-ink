#include "indicator_service.h"

#include "button_bsp.h"
#include "led_bsp.h"
#include "user_app.h"

namespace photopainter::product {

esp_err_t IndicatorService::Initialize() {
    if (initialized_) return ESP_OK;
    if (Green_led_Mode_queue == nullptr || Red_led_Mode_queue == nullptr) return ESP_ERR_INVALID_STATE;

    // The official PWR LED is active-low. Route this through its existing
    // worker task so the product layer never writes the board GPIO directly.
    xEventGroupSetBits(Red_led_Mode_queue, set_bit_button(0));
    initialized_ = true;
    return ESP_OK;
}

void IndicatorService::SetRefreshActive(bool active) {
    if (!initialized_) return;
    if (active) {
        // Bit 6 is the official continuous green-blink pattern. The worker
        // owns the actual GPIO and stops within one blink period after false.
        Green_led_arg = 1;
        xEventGroupSetBits(Green_led_Mode_queue, set_bit_button(6));
    } else {
        Green_led_arg = 0;
    }
}

void IndicatorService::RunSelfTest() {
    if (!initialized_) return;
    xEventGroupSetBits(Red_led_Mode_queue, GroupBit1);
    xEventGroupSetBits(Green_led_Mode_queue, set_bit_button(7));
}

IndicatorService& GetIndicatorService() {
    static IndicatorService service;
    return service;
}

esp_err_t InitializeIndicatorService() {
    return GetIndicatorService().Initialize();
}

}  // namespace photopainter::product
