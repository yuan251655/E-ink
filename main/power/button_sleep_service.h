#pragma once

#include "esp_err.h"

namespace photopainter::product {

// BOOT long-press requests ESP deep sleep. KEY wakes it. GPIO5 is SYS_OUT
// and must never be used as a button.
esp_err_t InitializeButtonSleepService();

}  // namespace photopainter::product
