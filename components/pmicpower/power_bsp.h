#pragma once

#include "i2c_bsp.h"

#define AXP2101_iqr_PIN             GPIO_NUM_21
#define AXP2101_CHGLED_PIN          GPIO_NUM_3

typedef struct {
    char isCharging[32];
    char chargeStatus[45];
    char batteryVoltage[30];
    char batteryPercent[30];
} PmicRegisterConfig;

// This is intentionally a read-only hardware snapshot.  Product code must
// not infer a battery is present from voltage or percentage alone: AXP2101
// exposes an explicit battery-present bit which is authoritative here.
typedef struct {
    bool pmic_ready;
    bool usb_vbus_present;
    bool battery_present;
    bool charging;
    bool discharging;
    bool backup_battery_charge_enabled;
    bool backup_battery_charge_safe;
    uint8_t charger_status;
    uint16_t vbus_voltage_mv;
    uint16_t system_voltage_mv;
    uint16_t battery_voltage_mv;
    int16_t battery_percent;
    int16_t main_charge_current_setting_ma;
    int16_t main_charge_target_voltage_mv;
    int16_t main_charge_termination_current_ma;
    bool main_charge_termination_enabled;
} PmicPowerSnapshot;

void Custom_PmicPortInit(I2cMasterBus *i2cbus,uint8_t dev_addr);
void Custom_PmicRegisterInit(void);
void Axp2101_isChargingTask(void *arg);
PmicRegisterConfig Custom_PmicGetBatteryInfo(void);
PmicPowerSnapshot Custom_PmicGetPowerSnapshot(void);
