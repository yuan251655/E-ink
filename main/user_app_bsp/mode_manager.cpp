#include "mode_manager.h"
#include "esp_timer.h"
namespace photopainter::product {
void ModeManager::Initialize(Feature feature) { snapshot_.active_feature = feature; snapshot_.revision = 1; snapshot_.updated_at_ms = esp_timer_get_time() / 1000; }
esp_err_t ModeManager::SetActiveFeature(Feature feature, Revision expected_revision) { if (expected_revision != snapshot_.revision) return ESP_ERR_INVALID_STATE; snapshot_.active_feature = feature; ++snapshot_.revision; snapshot_.updated_at_ms = esp_timer_get_time() / 1000; return ESP_OK; }
ModeManager& GetModeManager() { static ModeManager manager; return manager; }
}
