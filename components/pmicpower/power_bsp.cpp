#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include "power_bsp.h"
#include "XPowersLib.h"

const char *TAG = "axp2101";

static XPowersPMU axp2101;

static I2cMasterBus           *i2cbus_   = NULL;
static i2c_master_dev_handle_t i2cPMICdev = NULL;
static uint8_t                 i2cPMICAddress;
static bool                    pmic_ready = false;
static bool                    backup_battery_charge_verified_disabled = false;

static int16_t AXP2101ChargeCurrentSettingMilliamp(uint8_t raw) {
    switch (raw) {
        case XPOWERS_AXP2101_CHG_CUR_0MA: return 0;
        case XPOWERS_AXP2101_CHG_CUR_100MA: return 100;
        case XPOWERS_AXP2101_CHG_CUR_125MA: return 125;
        case XPOWERS_AXP2101_CHG_CUR_150MA: return 150;
        case XPOWERS_AXP2101_CHG_CUR_175MA: return 175;
        case XPOWERS_AXP2101_CHG_CUR_200MA: return 200;
        case XPOWERS_AXP2101_CHG_CUR_300MA: return 300;
        case XPOWERS_AXP2101_CHG_CUR_400MA: return 400;
        case XPOWERS_AXP2101_CHG_CUR_500MA: return 500;
        case XPOWERS_AXP2101_CHG_CUR_600MA: return 600;
        case XPOWERS_AXP2101_CHG_CUR_700MA: return 700;
        case XPOWERS_AXP2101_CHG_CUR_800MA: return 800;
        case XPOWERS_AXP2101_CHG_CUR_900MA: return 900;
        case XPOWERS_AXP2101_CHG_CUR_1000MA: return 1000;
        default: return -1;
    }
}

static int16_t AXP2101ChargeTargetVoltageMillivolt(uint8_t raw) {
    switch (raw) {
        case XPOWERS_AXP2101_CHG_VOL_4V: return 4000;
        case XPOWERS_AXP2101_CHG_VOL_4V1: return 4100;
        case XPOWERS_AXP2101_CHG_VOL_4V2: return 4200;
        case XPOWERS_AXP2101_CHG_VOL_4V35: return 4350;
        case XPOWERS_AXP2101_CHG_VOL_4V4: return 4400;
        default: return -1;
    }
}

static int16_t AXP2101ChargeTerminationCurrentMilliamp(uint8_t raw) {
    switch (raw) {
        case XPOWERS_AXP2101_CHG_ITERM_0MA: return 0;
        case XPOWERS_AXP2101_CHG_ITERM_25MA: return 25;
        case XPOWERS_AXP2101_CHG_ITERM_50MA: return 50;
        case XPOWERS_AXP2101_CHG_ITERM_75MA: return 75;
        case XPOWERS_AXP2101_CHG_ITERM_100MA: return 100;
        case XPOWERS_AXP2101_CHG_ITERM_125MA: return 125;
        case XPOWERS_AXP2101_CHG_ITERM_150MA: return 150;
        case XPOWERS_AXP2101_CHG_ITERM_175MA: return 175;
        case XPOWERS_AXP2101_CHG_ITERM_200MA: return 200;
        default: return -1;
    }
}

static int AXP2101_SLAVE_Read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    int ret;
    uint8_t count = 3;
    do
    {
        ret = (i2cbus_->i2c_read_buff(i2cPMICdev, regAddr, data, len) == ESP_OK) ? 0 : -1;
        if (ret == 0)
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
        count--;
    } while (count);
    return ret;
}

static int AXP2101_SLAVE_Write(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    int ret;
    uint8_t count = 3;
    do
    {
        ret = (i2cbus_->i2c_write_buff(i2cPMICdev, regAddr, data, len) == ESP_OK) ? 0 : -1;
        if (ret == 0)
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
        count--;
    } while (count);
    return ret;
}

void Custom_PmicPortGpioInit() {
    gpio_config_t io_conf = {};
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << AXP2101_iqr_PIN) | (1ULL << AXP2101_CHGLED_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
}

void Custom_PmicPortInit(I2cMasterBus *i2cbus,uint8_t dev_addr) {
    if(i2cbus_ == NULL) {
        i2cbus_ = i2cbus;
    }
    if(i2cPMICdev == NULL) {
        i2c_master_bus_handle_t BusHandle = i2cbus_->Get_I2cBusHandle();
        i2c_device_config_t     dev_cfg   = {};
        dev_cfg.dev_addr_length           = I2C_ADDR_BIT_LEN_7;
        dev_cfg.scl_speed_hz              = 100000;
        dev_cfg.device_address            = dev_addr;
        ESP_ERROR_CHECK(i2c_master_bus_add_device(BusHandle, &dev_cfg, &i2cPMICdev));
        i2cPMICAddress = dev_addr;
    }
    pmic_ready = axp2101.begin(i2cPMICAddress, AXP2101_SLAVE_Read, AXP2101_SLAVE_Write);
    if (pmic_ready) {
        ESP_LOGI(TAG, "Init PMU SUCCESS!");
    } else {
        ESP_LOGE(TAG, "Init PMU FAILED!");
        return;
    }
    Custom_PmicPortGpioInit();
    Custom_PmicRegisterInit();
}

void Custom_PmicRegisterInit(void) {
    if (!pmic_ready) return;
    axp2101.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_2000MA);

    if(axp2101.getDC1Voltage() != 3300) {
        axp2101.setDC1Voltage(3300);
        ESP_LOGW("axp2101_init_log","Set DCDC1 to output 3V3");
    }
    if(axp2101.getALDO1Voltage() != 3300) {
        axp2101.setALDO1Voltage(3300);
        ESP_LOGW("axp2101_init_log","Set ALDO1 to output 3V3");
    }
    if(axp2101.getALDO2Voltage() != 3300) {
        axp2101.setALDO2Voltage(3300);
        ESP_LOGW("axp2101_init_log","Set ALDO2 to output 3V3");
    }
    if(axp2101.getALDO3Voltage() != 3300) {
        axp2101.setALDO3Voltage(3300);
        ESP_LOGW("axp2101_init_log","Set ALDO3 to output 3V3");
    }
    if(axp2101.getALDO4Voltage() != 3300) {
        axp2101.setALDO4Voltage(3300);
        ESP_LOGW("axp2101_init_log","Set ALDO4 to output 3V3");
    }

    // Temporary safety cap: the real pack reached 4.20 V while the PMIC still
    // reported constant-current charging. Hold at 4.10 V until termination
    // behavior is verified with hardware measurements.
    axp2101.enableCellbatteryCharge();
    axp2101.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_100MA);
    if (!axp2101.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_200MA)) {
        ESP_LOGE(TAG, "Failed to set main battery charge current");
    }
    if (!axp2101.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V1)) {
        ESP_LOGE(TAG, "Failed to set main battery charge target voltage");
    }
    axp2101.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    axp2101.enableChargerTerminationLimit();

    // CR2032 is non-rechargeable. Disable VBACKUP charging and verify the
    // register before the firmware may report that installation is safe.
    backup_battery_charge_verified_disabled = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        (void)axp2101.disableButtonBatteryCharge();
        if (!axp2101.isEnableButtonBatteryCharge()) {
            backup_battery_charge_verified_disabled = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!backup_battery_charge_verified_disabled) {
        ESP_LOGE(TAG, "RTC backup charging could not be disabled; do not install CR2032");
    }
    axp2101.enableSystemVoltageMeasure();
    axp2101.enableVbusVoltageMeasure();
    axp2101.enableBattVoltageMeasure();
    axp2101.enableBattDetection();
}

void Axp2101_isChargingTask(void *arg) {
    // Retained only for source compatibility with the official BSP. Product
    // firmware no longer creates this task; clients query PowerService.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        ESP_LOGI(TAG, "isCharging: %s", axp2101.isCharging() ? "YES" : "NO");
        uint8_t charge_status = axp2101.getChargerStatus();
        if (charge_status == XPOWERS_AXP2101_CHG_TRI_STATE) {
            ESP_LOGI(TAG, "Charger Status: tri_charge");
        } else if (charge_status == XPOWERS_AXP2101_CHG_PRE_STATE) {
            ESP_LOGI(TAG, "Charger Status: pre_charge");
        } else if (charge_status == XPOWERS_AXP2101_CHG_CC_STATE) {
            ESP_LOGI(TAG, "Charger Status: constant charge");
        } else if (charge_status == XPOWERS_AXP2101_CHG_CV_STATE) {
            ESP_LOGI(TAG, "Charger Status: constant voltage");
        } else if (charge_status == XPOWERS_AXP2101_CHG_DONE_STATE) {
            ESP_LOGI(TAG, "Charger Status: charge done");
        } else if (charge_status == XPOWERS_AXP2101_CHG_STOP_STATE) {
            ESP_LOGI(TAG, "Charger Status: not charge");
        }
        ESP_LOGI(TAG, "getBattVoltage: %dmV", axp2101.getBattVoltage());
        ESP_LOGI(TAG, "getBatteryPercent: %d%%", axp2101.getBatteryPercent());
    }
}

PmicRegisterConfig Custom_PmicGetBatteryInfo(void) {
    PmicRegisterConfig config = {};
    const PmicPowerSnapshot snapshot = Custom_PmicGetPowerSnapshot();
    bool is_charging = snapshot.charging;
    if(is_charging) {
        strcpy(config.isCharging, "Battery Status : Charging");
    } else {
        strcpy(config.isCharging, "Battery Status : Not Charging");
    }
    uint8_t charge_status = snapshot.charger_status;
    if (charge_status == XPOWERS_AXP2101_CHG_TRI_STATE) {
        strcpy(config.chargeStatus, "Charging Status : Tri_Charge");
    } else if (charge_status == XPOWERS_AXP2101_CHG_PRE_STATE) {
        strcpy(config.chargeStatus, "Charging Status : Pre_Charge");
    } else if (charge_status == XPOWERS_AXP2101_CHG_CC_STATE) {
        strcpy(config.chargeStatus, "Charging Status : Constant_Charge");
    } else if (charge_status == XPOWERS_AXP2101_CHG_CV_STATE) {
        strcpy(config.chargeStatus, "Charging Status : Constant_Voltage");
    } else if (charge_status == XPOWERS_AXP2101_CHG_DONE_STATE) {
        strcpy(config.chargeStatus, "Charging Status : Charge_Done");
    } else if (charge_status == XPOWERS_AXP2101_CHG_STOP_STATE) {
        strcpy(config.chargeStatus, "Charging Status : Not_Charging");
    }
    uint16_t battery_voltage = snapshot.battery_voltage_mv;
    snprintf(config.batteryVoltage, sizeof(config.batteryVoltage), "Battery Voltage : %dmV", battery_voltage);
    int battery_percent = snapshot.battery_percent;
    snprintf(config.batteryPercent, sizeof(config.batteryPercent), "Battery Percent : %d%%", battery_percent);
    return config;
}

PmicPowerSnapshot Custom_PmicGetPowerSnapshot(void) {
    PmicPowerSnapshot snapshot = {};
    snapshot.battery_percent = -1;
    snapshot.main_charge_current_setting_ma = -1;
    snapshot.main_charge_target_voltage_mv = -1;
    snapshot.main_charge_termination_current_ma = -1;
    snapshot.pmic_ready = pmic_ready;
    if (!pmic_ready) return snapshot;

    snapshot.usb_vbus_present = axp2101.isVbusIn();
    snapshot.battery_present = axp2101.isBatteryConnect();
    snapshot.charging = axp2101.isCharging();
    snapshot.discharging = axp2101.isDischarge();
    snapshot.backup_battery_charge_enabled = axp2101.isEnableButtonBatteryCharge();
    snapshot.backup_battery_charge_safe =
        backup_battery_charge_verified_disabled && !snapshot.backup_battery_charge_enabled;
    snapshot.charger_status = axp2101.getChargerStatus();
    snapshot.vbus_voltage_mv = axp2101.getVbusVoltage();
    snapshot.system_voltage_mv = axp2101.getSystemVoltage();
    snapshot.main_charge_current_setting_ma = AXP2101ChargeCurrentSettingMilliamp(axp2101.getChargerConstantCurr());
    snapshot.main_charge_target_voltage_mv = AXP2101ChargeTargetVoltageMillivolt(axp2101.getChargeTargetVoltage());
    snapshot.main_charge_termination_current_ma = AXP2101ChargeTerminationCurrentMilliamp(axp2101.getChargerTerminationCurr());
    snapshot.main_charge_termination_enabled = axp2101.isChargerTerminationLimit();
    if (snapshot.battery_present) {
        snapshot.battery_voltage_mv = axp2101.getBattVoltage();
        snapshot.battery_percent = axp2101.getBatteryPercent();
    }
    return snapshot;
}
