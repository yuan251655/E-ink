#pragma once

#include <cstdint>
#include <mutex>

#include "esp_err.h"
#include "product_types.h"

namespace photopainter::product {

enum class PowerChargerState : std::uint8_t {
    kUnknown,
    kNotCharging,
    kTrickle,
    kPreCharge,
    kConstantCurrent,
    kConstantVoltage,
    kCompleted,
};

struct PowerSnapshot {
    bool initialized = false;
    bool pmic_online = false;
    bool usb_present = false;
    bool battery_present = false;
    bool charging = false;
    bool discharging = false;
    // Stage P0 safety invariant: this remains false until a compatible
    // rechargeable backup cell and its policy are explicitly confirmed.
    bool rtc_backup_charge_enabled = false;
    bool rtc_backup_charge_safe = false;
    PowerChargerState charger_state = PowerChargerState::kUnknown;
    std::uint16_t usb_voltage_mv = 0;
    std::uint16_t system_voltage_mv = 0;
    std::uint16_t battery_voltage_mv = 0;
    std::int16_t battery_percent = -1;
    std::int16_t main_charge_current_setting_ma = -1;
    std::int16_t main_charge_target_voltage_mv = -1;
    std::int16_t main_charge_termination_current_ma = -1;
    bool main_charge_termination_enabled = false;
};

struct BatteryDisplayConfig {
    bool enabled = true;
    bool visible = false;
    Revision revision = 0;
};

class PowerService {
public:
    esp_err_t Initialize();
    PowerSnapshot GetSnapshot() const;
    BatteryDisplayConfig GetBatteryDisplayConfig() const;
    bool ShouldShowBatteryIcon(const PowerSnapshot& power);
    esp_err_t UpdateBatteryDisplayConfig(bool enabled, Revision expected_revision,
                                         BatteryDisplayConfig* updated);
private:
    mutable std::mutex battery_display_mutex_;
};

PowerService& GetPowerService();
esp_err_t InitializePowerService();
const char* PowerChargerStateName(PowerChargerState state);

}  // namespace photopainter::product
