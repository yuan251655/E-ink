#include "dashboard_renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "dashboard_data_service.h"
#include "dashboard_weather_service.h"
#include "display_bsp.h"
#include "fonts.h"

namespace photopainter::product {
namespace {
struct CalendarDate { int year; int month; int day; };
constexpr int kMonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
constexpr bool Leap(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }
constexpr int MonthDays(int year, int month) {
    return month == 2 && Leap(year) ? 29 : kMonthDays[month - 1];
}
CalendarDate ParseDate(const std::string& value) {
    CalendarDate date{};
    if (value.size() == 10) std::sscanf(value.c_str(), "%d-%d-%d", &date.year, &date.month, &date.day);
    return date;
}
constexpr CalendarDate AddDay(CalendarDate date) {
    if (++date.day > MonthDays(date.year, date.month)) {
        date.day = 1;
        if (++date.month > 12) { date.month = 1; ++date.year; }
    }
    return date;
}
constexpr int Weekday(CalendarDate date) {
    if (date.month < 3) { date.month += 12; --date.year; }
    return (date.day + 2 * date.month + 3 * (date.month + 1) / 5 + date.year + date.year / 4 - date.year / 100 + date.year / 400 + 1) % 7;
}
static_assert(Weekday({2026, 8, 10}) == 1);
static_assert(AddDay({2026, 12, 31}).year == 2027 && AddDay({2026, 12, 31}).month == 1 && AddDay({2026, 12, 31}).day == 1);
void Fill(ePaperPort* d, int x, int y, int w, int h, std::uint8_t color) {
    for (int row = std::max(0, y); row < std::min(480, y + h); ++row)
        for (int col = std::max(0, x); col < std::min(800, x + w); ++col) d->EPD_SetPixel(col, row, color);
}
void Line(ePaperPort* d, int x0, int y0, int x1, int y1, std::uint8_t color) {
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        d->EPD_SetPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        const int twice = 2 * error;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}
void Circle(ePaperPort* d, int cx, int cy, int radius, std::uint8_t color, bool filled = false) {
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            const int value = x * x + y * y;
            if ((filled && value <= radius * radius) || (!filled && value <= radius * radius && value >= (radius - 2) * (radius - 2)))
                d->EPD_SetPixel(cx + x, cy + y, color);
        }
    }
}
void Cloud(ePaperPort* d, int cx, int cy) {
    Circle(d, cx - 30, cy + 4, 22, ColorBlack, true);
    Circle(d, cx, cy - 10, 34, ColorBlack, true);
    Circle(d, cx + 35, cy + 5, 25, ColorBlack, true);
    Fill(d, cx - 50, cy, 105, 31, ColorBlack);
    Circle(d, cx - 30, cy + 4, 16, ColorWhite, true);
    Circle(d, cx, cy - 10, 28, ColorWhite, true);
    Circle(d, cx + 35, cy + 5, 19, ColorWhite, true);
    Fill(d, cx - 44, cy + 1, 93, 24, ColorWhite);
    Line(d, cx - 48, cy + 25, cx + 51, cy + 25, ColorBlack);
}
void Sun(ePaperPort* d, int cx, int cy) {
    Circle(d, cx, cy, 22, ColorRed, true);
    for (int delta : {-42, -34, 34, 42}) {
        Line(d, cx + delta, cy, cx + (delta < 0 ? delta + 9 : delta - 9), cy, ColorRed);
        Line(d, cx, cy + delta, cx, cy + (delta < 0 ? delta + 9 : delta - 9), ColorRed);
    }
    Line(d, cx - 29, cy - 29, cx - 23, cy - 23, ColorRed); Line(d, cx + 29, cy - 29, cx + 23, cy - 23, ColorRed);
    Line(d, cx - 29, cy + 29, cx - 23, cy + 23, ColorRed); Line(d, cx + 29, cy + 29, cx + 23, cy + 23, ColorRed);
}
void WeatherIcon(ePaperPort* d, int cx, int cy, int code) {
    if (code == 0) { Sun(d, cx, cy); return; }
    if (code == 1 || code == 2) Sun(d, cx + 25, cy - 25);
    Cloud(d, cx, cy);
    if ((code >= 51 && code <= 65) || code == 80 || code == 81 || code == 82 || code >= 95) {
        for (int x : {-30, 0, 30}) { Line(d, cx + x, cy + 38, cx + x - 5, cy + 51, ColorBlue); }
    }
    if (code >= 95) {
        Line(d, cx + 8, cy + 32, cx - 5, cy + 55, ColorRed);
        Line(d, cx - 5, cy + 55, cx + 9, cy + 51, ColorRed);
        Line(d, cx + 9, cy + 51, cx - 3, cy + 70, ColorRed);
    }
}
const char* WeatherName(int code) {
    if (code == 0) return "晴天";
    if (code == 1 || code == 2) return "多云";
    if (code == 3) return "阴天";
    if (code == 45 || code == 48) return "雾天";
    if (code >= 95) return "雷雨";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "雪天";
    if (code == 63 || code == 81) return "中雨";
    if (code == 65 || code == 82) return "大雨";
    return "小雨";
}
}

esp_err_t RenderDashboardFrame(ePaperPort* display, const DashboardDataSnapshot& dashboard,
                               const DashboardWeatherSnapshot& weather) {
    if (display == nullptr || dashboard.layout_id != "weather_date" || weather.last_success_at == 0) return ESP_ERR_INVALID_STATE;
    std::memset(display->EPD_GetIMGBuffer(), 0x11, 192000);
    const std::string city = dashboard.city_name.empty() ? weather.city_name : dashboard.city_name;
    display->EPD_DrawStringCN(34, 32, city.c_str(), &Font22CN, ColorBlack, ColorWhite);
    char updated[40]{};
    const unsigned local_seconds = static_cast<unsigned>((weather.last_success_at + 8ULL * 3600ULL) % 86400ULL);
    std::snprintf(updated, sizeof(updated), "UPDATE %02u:%02u", local_seconds / 3600U, (local_seconds / 60U) % 60U);
    display->EPD_DrawStringEN(34, 78, updated, &Font24, ColorBlack, ColorWhite);

    CalendarDate date = ParseDate(weather.days[0].date);
    static const char* weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    for (int i = 0; i < 5; ++i) {
        const int x = 305 + i * 96;
        if (i == 0) Fill(display, x, 25, 88, 94, ColorRed);
        display->EPD_DrawStringCN(x + 24, 37, weekdays[Weekday(date)], &Font14CN,
                                  i == 0 ? ColorWhite : ColorBlack, i == 0 ? ColorRed : ColorWhite);
        char date_text[8]{};
        std::snprintf(date_text, sizeof(date_text), "%d/%d", date.month, date.day);
        display->EPD_DrawStringEN(x + 16, 68, date_text, &Font24, i == 0 ? ColorWhite : ColorBlack, i == 0 ? ColorRed : ColorWhite);
        date = AddDay(date);
    }

    static const char* labels[] = {"今天", "明天", "后天"};
    for (int i = 0; i < 3; ++i) {
        const int left = 30 + i * 245;
        for (int x = left; x <= left + 222; ++x) { display->EPD_SetPixel(x, 140, ColorBlack); display->EPD_SetPixel(x, 462, ColorBlack); }
        for (int y = 140; y <= 462; ++y) { display->EPD_SetPixel(left, y, ColorBlack); display->EPD_SetPixel(left + 222, y, ColorBlack); }
        display->EPD_DrawStringCN(left + 82, 158, labels[i], &Font22CN, i == 0 ? ColorRed : ColorBlue, ColorWhite);
        WeatherIcon(display, left + 111, 265, weather.days[i].weather_code);
        display->EPD_DrawStringCN(left + 79, 345, WeatherName(weather.days[i].weather_code), &Font22CN, ColorBlack, ColorWhite);
        char temperature[24]{};
        std::snprintf(temperature, sizeof(temperature), "%d / %d C", weather.days[i].temperature_max_c, weather.days[i].temperature_min_c);
        display->EPD_DrawStringEN(left + 52, 397, temperature, &Font24, ColorBlack, ColorWhite);
        display->EPD_DrawStringEN(left + 42, 432, weather.days[i].date.c_str(), &Font24, ColorBlack, ColorWhite);
    }
    return ESP_OK;
}
}
