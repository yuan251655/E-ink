#pragma once

#include <cstdint>

#include "esp_err.h"

namespace photopainter::product {
esp_err_t InitializeDashboardAutoRefreshService();
std::uint64_t DashboardAutoRefreshDeadlineEpoch();
}
