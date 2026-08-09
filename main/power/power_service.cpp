#include "power_service.h"

#include "esp_log.h"
#include "nvs.h"
#include "XPowersLib.h"
#include "device_log_service.h"
#include "power_bsp.h"

namespace photopainter::product {
namespace {
constexpr const char* kTag = "power_service";
constexpr const char* kBatteryDisplayNamespace = "batt_display";
constexpr const char* kBatteryDisplayEnabledKey = "enabled";
constexpr const char* kBatteryDisplayVisibleKey = "visible";
constexpr const char* kBatteryDisplayRevisionKey = "revision";
constexpr int kBatteryDisplayShowPercent = 30;
constexpr int kBatteryDisplayHidePercent = 35;

constexpr bool BatteryIconVisibleAt(int percent, bool was_visible) {
    return percent <= kBatteryDisplayShowPercent ? true
         : percent >= kBatteryDisplayHidePercent ? false
         : was_visible;
}

static_assert(BatteryIconVisibleAt(30, false));
static_assert(BatteryIconVisibleAt(31, true));
static_assert(!BatteryIconVisibleAt(34, false));
static_assert(!BatteryIconVisibleAt(35, true));

BatteryDisplayConfig ReadBatteryDisplayConfig() {
    BatteryDisplayConfig config;
    nvs_handle_t handle{};
    if (nvs_open(kBatteryDisplayNamespace, NVS_READONLY, &handle) != ESP_OK) return config;
    std::uint8_t enabled = config.enabled ? 1 : 0;
    std::uint8_t visible = config.visible ? 1 : 0;
    std::uint64_t revision = 0;
    (void)nvs_get_u8(handle, kBatteryDisplayEnabledKey, &enabled);
    (void)nvs_get_u8(handle, kBatteryDisplayVisibleKey, &visible);
    (void)nvs_get_u64(handle, kBatteryDisplayRevisionKey, &revision);
    nvs_close(handle);
    config.enabled = enabled != 0;
    config.visible = visible != 0;
    config.revision = revision;
    return config;
}

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

BatteryDisplayConfig PowerService::GetBatteryDisplayConfig() const {
    std::lock_guard<std::mutex> lock(battery_display_mutex_);
    return ReadBatteryDisplayConfig();
}

bool PowerService::ShouldShowBatteryIcon(const PowerSnapshot& power) {
    if (!power.battery_present || power.battery_percent < 0) return false;
    std::lock_guard<std::mutex> lock(battery_display_mutex_);
    BatteryDisplayConfig config = ReadBatteryDisplayConfig();
    const bool visible = BatteryIconVisibleAt(power.battery_percent, config.visible);
    if (visible != config.visible) {
        nvs_handle_t handle{};
        if (nvs_open(kBatteryDisplayNamespace, NVS_READWRITE, &handle) == ESP_OK) {
            if (nvs_set_u8(handle, kBatteryDisplayVisibleKey, visible ? 1 : 0) == ESP_OK) {
                (void)nvs_commit(handle);
            }
            nvs_close(handle);
        }
    }
    return config.enabled && visible;
}

esp_err_t PowerService::UpdateBatteryDisplayConfig(bool enabled, Revision expected_revision,
                                                    BatteryDisplayConfig* updated) {
    if (!updated) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(battery_display_mutex_);
    const BatteryDisplayConfig current = ReadBatteryDisplayConfig();
    if (current.revision != expected_revision) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle{};
    esp_err_t result = nvs_open(kBatteryDisplayNamespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_set_u8(handle, kBatteryDisplayEnabledKey, enabled ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u64(handle, kBatteryDisplayRevisionKey, current.revision + 1);
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle) nvs_close(handle);
    if (result != ESP_OK) return result;

    updated->enabled = enabled;
    updated->visible = current.visible;
    updated->revision = current.revision + 1;
    return ESP_OK;
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
