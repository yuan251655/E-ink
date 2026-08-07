#include "power_service.h"

#include "esp_log.h"
#include "XPowersLib.h"
#include "device_log_service.h"
#include "power_bsp.h"

namespace photopainter::product {
namespace {
constexpr const char* kTag = "power_service";

PowerChargerState ToChargerState(std::uint8_t state) {
    switch (state) {
        case XPOWERS_AXP2101_CHG_TRI_STATE: return PowerChargerState::kTrickle;
        case XPOWERS_AXP2101_CHG_PRE_STATE: return PowerChargerState::kPreCharge;
        case XPOWERS_AXP2101_CHG_CC_STATE: return PowerChargerState::kConstantCurrent;
        case XPOWERS_AXP2101_CHG_CV_STATE: return PowerChargerState::kConstantVoltage;
        case XPOWERS_AXP2101_CHG_DONE_STATE: return PowerChargerState::kCompleted;
        case XPOWERS_AXP2101_CHG_STOP_STATE: return PowerChargerState::kNotCharging;
        default: return PowerChargerState::kUnknown;
    }
}

PowerService g_power_service;
}  // namespace

esp_err_t PowerService::Initialize() {
    const PmicPowerSnapshot pmic = Custom_PmicGetPowerSnapshot();
    if (!pmic.pmic_ready) {
        ESP_LOGW(kTag, "AXP2101 is unavailable; power API will report pmic_online=false");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(kTag, "AXP2101 online; USB=%s battery=%s RTC backup charge=%s safe=%s",
             pmic.usb_vbus_present ? "present" : "absent",
             pmic.battery_present ? "present" : "absent",
             pmic.backup_battery_charge_enabled ? "enabled" : "disabled",
             pmic.backup_battery_charge_safe ? "yes" : "no");
    if (!pmic.backup_battery_charge_safe) {
        GetDeviceLogService().Add(DeviceLogSeverity::kError, "power", "rtc_backup_charge_unsafe",
                                  "RTC backup charging is not verified disabled; do not install CR2032");
        return ESP_ERR_INVALID_STATE;
    }
    GetDeviceLogService().Add(DeviceLogSeverity::kInfo, "power", "rtc_backup_charge_safe",
                              "RTC backup charging is verified disabled");
    return ESP_OK;
}

PowerSnapshot PowerService::GetSnapshot() const {
    const PmicPowerSnapshot pmic = Custom_PmicGetPowerSnapshot();
    PowerSnapshot snapshot;
    snapshot.initialized = true;
    snapshot.pmic_online = pmic.pmic_ready;
    snapshot.usb_present = pmic.usb_vbus_present;
    snapshot.battery_present = pmic.battery_present;
    snapshot.charging = pmic.charging;
    snapshot.discharging = pmic.discharging;
    snapshot.rtc_backup_charge_enabled = pmic.backup_battery_charge_enabled;
    snapshot.rtc_backup_charge_safe = pmic.backup_battery_charge_safe;
    snapshot.charger_state = ToChargerState(pmic.charger_status);
    snapshot.usb_voltage_mv = pmic.vbus_voltage_mv;
    snapshot.system_voltage_mv = pmic.system_voltage_mv;
    snapshot.battery_voltage_mv = pmic.battery_voltage_mv;
    snapshot.battery_percent = pmic.battery_percent;
    snapshot.main_charge_current_setting_ma = pmic.main_charge_current_setting_ma;
    snapshot.main_charge_target_voltage_mv = pmic.main_charge_target_voltage_mv;
    snapshot.main_charge_termination_current_ma = pmic.main_charge_termination_current_ma;
    snapshot.main_charge_termination_enabled = pmic.main_charge_termination_enabled;
    return snapshot;
}

PowerService& GetPowerService() { return g_power_service; }

esp_err_t InitializePowerService() { return g_power_service.Initialize(); }

const char* PowerChargerStateName(PowerChargerState state) {
    switch (state) {
        case PowerChargerState::kNotCharging: return "not_charging";
        case PowerChargerState::kTrickle: return "trickle";
        case PowerChargerState::kPreCharge: return "pre_charge";
        case PowerChargerState::kConstantCurrent: return "constant_current";
        case PowerChargerState::kConstantVoltage: return "constant_voltage";
        case PowerChargerState::kCompleted: return "completed";
        case PowerChargerState::kUnknown: return "unknown";
    }
    return "unknown";
}

}  // namespace photopainter::product
