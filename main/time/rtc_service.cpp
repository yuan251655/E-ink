#include "rtc_service.h"

#include <cstring>

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "user_app.h"

namespace photopainter::product {
namespace {

// PCF85063 stores the wall-clock fields supplied by the App (China Standard
// Time). Convert that local calendar to UTC before exposing Unix timestamps.
// This keeps App formatting and RTC scheduling on the same absolute timeline.
constexpr std::int64_t kLocalTimezoneOffsetSeconds = 8 * 60 * 60;
constexpr const char* kTag = "rtc_service";
constexpr std::uint8_t kAddress = 0x51;
constexpr std::uint8_t kControl2Register = 0x01;
constexpr std::uint8_t kTimeRegister = 0x04;
constexpr std::uint8_t kTimerValueRegister = 0x10;
constexpr std::uint8_t kTimerModeRegister = 0x11;
constexpr gpio_num_t kInterruptPin = GPIO_NUM_6;
i2c_master_dev_handle_t g_rtc_device = nullptr;
bool g_present = false;

std::uint8_t BcdToBinary(std::uint8_t value) {
    return static_cast<std::uint8_t>((value >> 4) * 10 + (value & 0x0f));
}

bool InRange(std::uint8_t value, std::uint8_t min, std::uint8_t max) {
    return value >= min && value <= max;
}

bool IsLeapYear(std::uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

std::uint8_t DaysInMonth(std::uint16_t year, std::uint8_t month) {
    constexpr std::uint8_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && IsLeapYear(year)) return 29;
    return month >= 1 && month <= 12 ? kDays[month - 1] : 0;
}

esp_err_t ClearPendingInterrupts() {
    const std::uint8_t clear_flags[] = {kControl2Register, 0x00};
    esp_err_t result = i2c_master_transmit(g_rtc_device, clear_flags, sizeof(clear_flags), 500);
    if (result != ESP_OK) return result;
    const std::uint8_t disable_timer[] = {kTimerModeRegister, 0x18};
    return i2c_master_transmit(g_rtc_device, disable_timer, sizeof(disable_timer), 500);
}

std::uint8_t BinaryToBcd(std::uint8_t value) {
    return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
}
}  // namespace

RtcService g_rtc_service;

esp_err_t RtcService::Initialize() {
    (void)rtc_gpio_hold_dis(kInterruptPin);
    (void)rtc_gpio_deinit(kInterruptPin);
    if (g_rtc_device == nullptr) {
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = kAddress,
            .scl_speed_hz = 100000,
        };
        esp_err_t result = i2c_master_bus_add_device(I2cBus.Get_I2cBusHandle(), &config, &g_rtc_device);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "PCF85063 device add failed: %s", esp_err_to_name(result));
            return result;
        }
    }

    esp_err_t result = ClearPendingInterrupts();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "PCF85063 interrupt cleanup failed: %s", esp_err_to_name(result));
        return result;
    }
    RtcSnapshot snapshot;
    result = ReadSnapshot(&snapshot);
    g_present = result == ESP_OK;
    if (!g_present) {
        ESP_LOGW(kTag, "PCF85063 not responding: %s", esp_err_to_name(result));
        return result;
    }
    gpio_config_t gpio = {};
    gpio.pin_bit_mask = 1ULL << static_cast<unsigned>(kInterruptPin);
    gpio.mode = GPIO_MODE_INPUT;
    gpio.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio.intr_type = GPIO_INTR_DISABLE;
    (void)gpio_config(&gpio);
    ESP_LOGI(kTag, "PCF85063 online; time_valid=%s", snapshot.valid ? "yes" : "no");
    return ESP_OK;
}

esp_err_t RtcService::ArmInterruptDiagnostic(std::uint8_t seconds) {
    if (g_rtc_device == nullptr || seconds == 0) return ESP_ERR_INVALID_ARG;
    const std::uint8_t clear_flags[] = {kControl2Register, 0x00};
    esp_err_t result = i2c_master_transmit(g_rtc_device, clear_flags, sizeof(clear_flags), 500);
    if (result != ESP_OK) return result;
    const std::uint8_t disable_timer[] = {kTimerModeRegister, 0x18};
    result = i2c_master_transmit(g_rtc_device, disable_timer, sizeof(disable_timer), 500);
    if (result != ESP_OK) return result;
    const std::uint8_t arm[] = {kTimerValueRegister, seconds, 0x16};
    return i2c_master_transmit(g_rtc_device, arm, sizeof(arm), 500);
}

esp_err_t RtcService::ArmWakeAfterSeconds(std::uint32_t seconds) {
    if (g_rtc_device == nullptr || seconds == 0) return ESP_ERR_INVALID_ARG;
    std::uint8_t value = 0;
    std::uint8_t mode = 0;
    if (seconds <= 255) {
        value = static_cast<std::uint8_t>(seconds);
        mode = 0x16;  // 1 Hz, timer enabled, timer interrupt enabled.
    } else if (seconds % 60 == 0 && seconds / 60 <= 255) {
        value = static_cast<std::uint8_t>(seconds / 60);
        mode = 0x1e;  // 1/60 Hz, timer enabled, timer interrupt enabled.
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t clear = ClearPendingInterrupts();
    if (clear != ESP_OK) return clear;
    const std::uint8_t arm[] = {kTimerValueRegister, value, mode};
    return i2c_master_transmit(g_rtc_device, arm, sizeof(arm), 500);
}

esp_err_t RtcService::DisarmWakeTimer() {
    if (g_rtc_device == nullptr) return ESP_ERR_INVALID_STATE;
    return ClearPendingInterrupts();
}

bool RtcService::GetUnixTimeSeconds(std::uint64_t* output) const {
    if (output == nullptr) return false;
    const RtcSnapshot value = GetSnapshot();
    if (!value.valid || value.year < 1970 || value.day > DaysInMonth(value.year, value.month)) return false;
    std::uint64_t days = 0;
    for (std::uint16_t year = 1970; year < value.year; ++year) days += IsLeapYear(year) ? 366 : 365;
    for (std::uint8_t month = 1; month < value.month; ++month) days += DaysInMonth(value.year, month);
    days += value.day - 1;
    const std::int64_t local_seconds = static_cast<std::int64_t>(days * 86400ULL) +
        value.hour * 3600LL + value.minute * 60LL + value.second;
    if (local_seconds < kLocalTimezoneOffsetSeconds) return false;
    *output = static_cast<std::uint64_t>(local_seconds - kLocalTimezoneOffsetSeconds);
    return true;
}

int RtcService::ReadInterruptLevel() const { return gpio_get_level(kInterruptPin); }

esp_err_t RtcService::ReadSnapshot(RtcSnapshot* snapshot) const {
    if (snapshot == nullptr || g_rtc_device == nullptr) return ESP_ERR_INVALID_ARG;
    std::uint8_t reg = kTimeRegister;
    std::uint8_t data[7] = {};
    const esp_err_t result = i2c_master_transmit_receive(g_rtc_device, &reg, 1, data, sizeof(data), 500);
    if (result != ESP_OK) return result;

    snapshot->initialized = true;
    snapshot->present = true;
    snapshot->second = BcdToBinary(data[0] & 0x7f);
    snapshot->minute = BcdToBinary(data[1] & 0x7f);
    snapshot->hour = BcdToBinary(data[2] & 0x3f);
    snapshot->day = BcdToBinary(data[3] & 0x3f);
    snapshot->weekday = data[4] & 0x07;
    snapshot->month = BcdToBinary(data[5] & 0x1f);
    snapshot->year = static_cast<std::uint16_t>(2000 + BcdToBinary(data[6]));
    snapshot->valid = (data[0] & 0x80) == 0 &&
        InRange(snapshot->second, 0, 59) && InRange(snapshot->minute, 0, 59) &&
        InRange(snapshot->hour, 0, 23) && InRange(snapshot->day, 1, 31) &&
        InRange(snapshot->month, 1, 12);
    return ESP_OK;
}

RtcSnapshot RtcService::GetSnapshot() const {
    RtcSnapshot snapshot;
    snapshot.present = g_present;
    if (g_present) (void)ReadSnapshot(&snapshot);
    return snapshot;
}

esp_err_t RtcService::SetTime(const RtcSnapshot& snapshot) {
    if (g_rtc_device == nullptr || snapshot.year < 2000 || snapshot.year > 2099 ||
        !InRange(snapshot.month, 1, 12) || !InRange(snapshot.day, 1, 31) ||
        !InRange(snapshot.weekday, 0, 6) || !InRange(snapshot.hour, 0, 23) ||
        !InRange(snapshot.minute, 0, 59) || !InRange(snapshot.second, 0, 59)) {
        return ESP_ERR_INVALID_ARG;
    }
    const std::uint8_t data[] = {
        kTimeRegister, BinaryToBcd(snapshot.second), BinaryToBcd(snapshot.minute),
        BinaryToBcd(snapshot.hour), BinaryToBcd(snapshot.day), snapshot.weekday,
        BinaryToBcd(snapshot.month), BinaryToBcd(static_cast<std::uint8_t>(snapshot.year - 2000)),
    };
    const esp_err_t result = i2c_master_transmit(g_rtc_device, data, sizeof(data), 500);
    g_present = result == ESP_OK;
    return result;
}

RtcService& GetRtcService() { return g_rtc_service; }
esp_err_t InitializeRtcService() { return g_rtc_service.Initialize(); }

}  // namespace photopainter::product
