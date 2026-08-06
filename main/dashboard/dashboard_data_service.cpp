#include "dashboard_data_service.h"

#include <algorithm>

#include "ArduinoJson.h"
#include "storage_service.h"

namespace photopainter::product {
namespace {
constexpr char kStateFile[] = "dashboard.json";
constexpr std::size_t kMaxTodos = 50;
constexpr std::size_t kMaxMemoBytes = 120;
constexpr std::size_t kMaxTodoBytes = 80;
constexpr std::size_t kMaxTextBytes = 4096;

bool ValidText(const std::string& value, std::size_t maximum) {
    return value.size() <= maximum && value.find_first_of("\r\n\0") == std::string::npos;
}
}  // namespace

DashboardDataService& GetDashboardDataService() {
    static DashboardDataService service;
    return service;
}

esp_err_t DashboardDataService::Initialize(StorageService* storage) {
    if (storage == nullptr) return ESP_ERR_INVALID_ARG;
    storage_ = storage;
    const esp_err_t result = Load();
    return result == ESP_ERR_NOT_FOUND ? Persist(snapshot_) : result;
}

DashboardDataSnapshot DashboardDataService::GetSnapshot() const { return snapshot_; }

esp_err_t DashboardDataService::Replace(const DashboardDataSnapshot& candidate, std::uint64_t expected_revision,
                                        DashboardDataSnapshot* updated) {
    if (storage_ == nullptr || updated == nullptr || !IsValid(candidate)) return ESP_ERR_INVALID_ARG;
    if (expected_revision != snapshot_.revision) return ESP_ERR_INVALID_STATE;
    DashboardDataSnapshot next = candidate;
    next.revision = snapshot_.revision + 1;
    const esp_err_t result = Persist(next);
    if (result != ESP_OK) return result;
    snapshot_ = next;
    *updated = snapshot_;
    return ESP_OK;
}

bool DashboardDataService::IsValid(const DashboardDataSnapshot& candidate) const {
    if (candidate.layout_id != "weather_date" && candidate.layout_id != "date_memo_todo" &&
        candidate.layout_id != "weather_memo_todo") return false;
    if (!ValidText(candidate.timezone, 64) || !ValidText(candidate.city_name, 120) ||
        !ValidText(candidate.memo, kMaxMemoBytes) || candidate.todos.size() > kMaxTodos) return false;
    for (std::size_t index = 0; index < candidate.todos.size(); ++index) {
        const auto& todo = candidate.todos[index];
        if (todo.id.empty() || todo.id.size() > 64 || !ValidText(todo.title, kMaxTodoBytes) || todo.position != index) return false;
    }
    return true;
}

esp_err_t DashboardDataService::Load() {
    std::string raw;
    const esp_err_t result = storage_->ReadStateText(kStateFile, kMaxTextBytes, &raw);
    if (result != ESP_OK) return result;
    JsonDocument document;
    if (deserializeJson(document, raw) != DeserializationError::Ok) return ESP_ERR_INVALID_RESPONSE;
    DashboardDataSnapshot loaded;
    loaded.revision = document["revision"] | 1ULL;
    loaded.layout_id = document["layout_id"] | "weather_memo_todo";
    loaded.timezone = document["timezone"] | "Asia/Shanghai";
    loaded.city_name = document["city_name"] | "";
    loaded.memo = document["memo"] | "";
    JsonArrayConst todos = document["todos"].as<JsonArrayConst>();
    for (JsonObjectConst value : todos) {
        DashboardTodoState todo;
        todo.id = value["id"] | "";
        todo.title = value["title"] | "";
        todo.completed = value["completed"] | false;
        todo.position = value["position"] | static_cast<std::uint32_t>(loaded.todos.size());
        loaded.todos.push_back(std::move(todo));
    }
    if (!IsValid(loaded)) return ESP_ERR_INVALID_RESPONSE;
    snapshot_ = std::move(loaded);
    return ESP_OK;
}

esp_err_t DashboardDataService::Persist(const DashboardDataSnapshot& state) {
    JsonDocument document;
    document["revision"] = state.revision;
    document["layout_id"] = state.layout_id;
    document["timezone"] = state.timezone;
    document["city_name"] = state.city_name;
    document["memo"] = state.memo;
    JsonArray todos = document["todos"].to<JsonArray>();
    for (const auto& todo : state.todos) {
        JsonObject item = todos.add<JsonObject>();
        item["id"] = todo.id;
        item["title"] = todo.title;
        item["completed"] = todo.completed;
        item["position"] = todo.position;
    }
    std::string raw;
    serializeJson(document, raw);
    return storage_->WriteStateTextAtomic(kStateFile, raw);
}

}  // namespace photopainter::product
