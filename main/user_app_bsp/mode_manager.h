#pragma once
#include "esp_err.h"
#include "product_types.h"
namespace photopainter::product {
class ModeManager { public: void Initialize(Feature feature); esp_err_t SetActiveFeature(Feature feature, Revision expected_revision); ModeSnapshot GetSnapshot() const { return snapshot_; } private: ModeSnapshot snapshot_; };
ModeManager& GetModeManager();
}
