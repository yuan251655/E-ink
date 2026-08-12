#pragma once

#include "esp_err.h"

class ePaperPort;

namespace photopainter::product {
struct DashboardDataSnapshot;
struct DashboardWeatherSnapshot;

esp_err_t RenderDashboardFrame(ePaperPort* display, const DashboardDataSnapshot& dashboard,
                               const DashboardWeatherSnapshot& weather);
}
