#pragma once
#include "esp_err.h"
namespace photopainter::product { class DisplayService; esp_err_t InitializeDisplayService(); DisplayService& GetDisplayService(); }
