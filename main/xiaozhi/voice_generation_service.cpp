#include "voice_generation_service.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "mcp_server.h"
#include "mode_manager.h"
#include "local_album_playback_runtime.h"
#include "ai_album_playback_runtime.h"
#include "display_runtime.h"
#include "job_runtime.h"
#include "job_service.h"

namespace photopainter::product {
namespace {
constexpr std::size_t kPromptMaxBytes = 320;
constexpr std::uint64_t kConfirmTimeoutMs = 30'000;
constexpr std::uint64_t kAppHeartbeatTimeoutMs = 15'000;

std::uint64_t NowMs() { return static_cast<std::uint64_t>(esp_timer_get_time() / 1000); }
bool IsTerminal(VoiceGenerationState state) {
    return state == VoiceGenerationState::kSuccess || state == VoiceGenerationState::kFailed ||
           state == VoiceGenerationState::kCancelled || state == VoiceGenerationState::kExpired;
}

std::string ShowNextPicture() {
    const auto mode = GetModeManager().GetSnapshot();
    if (mode.state != ModeSnapshot::State::kIdle) return "相框正在切换模式，请稍后再试。";
    if (mode.active_feature == Feature::kInfoDashboard) return "信息看板不支持切换照片。";
    const esp_err_t result = mode.active_feature == Feature::kAiAlbum
        ? GetAiAlbumPlaybackService().RequestNext()
        : GetLocalAlbumPlaybackService().RequestNext();
    if (result == ESP_ERR_NOT_FOUND) return "当前相册里没有其他可切换的照片。";
    if (result != ESP_OK) return "墨水屏正在刷新，请稍后再试。";
    return "正在切换下一张照片，请等待屏幕刷新完成。";
}

std::string SwitchMode(const std::string& target_name) {
    Feature target;
    const char* display_name = nullptr;
    if (target_name == "local_album") {
        target = Feature::kLocalAlbum;
        display_name = "本地相册";
    } else if (target_name == "ai_album") {
        target = Feature::kAiAlbum;
        display_name = "AI 相册";
    } else if (target_name == "info_dashboard") {
        target = Feature::kInfoDashboard;
        display_name = "信息看板";
    } else {
        return "请明确说要切换到本地相册、AI 相册或信息看板。";
    }

    const auto mode = GetModeManager().GetSnapshot();
    if (mode.state != ModeSnapshot::State::kIdle) return "相框正在切换模式，请稍后再试。";
    if (mode.active_feature == target) return std::string("当前已经是") + display_name + "。";

    JobSnapshot job;
    const RequestId request_id = "voice-mode-" + std::to_string(NowMs());
    const std::string fingerprint = "voice-mode:" + target_name + ":" + std::to_string(mode.revision);
    if (GetProductJobService().CreateOrFind(JobKind::kMode, request_id, fingerprint, &job) !=
        JobRegistrationResult::kCreated) {
        return "相框正在执行其他任务，请稍后再试。";
    }
    const esp_err_t result = GetModeManager().BeginSwitch(
        target, mode.revision, job.job_id, &GetProductJobService(), &GetDisplayService());
    if (result != ESP_OK) {
        (void)GetProductJobService().Update(job.job_id, JobState::kFailed, "failed", 0,
                                            result == ESP_ERR_NOT_FOUND ? "mode_cover_unavailable" : "mode_switch_busy");
        return result == ESP_ERR_NOT_FOUND ? "目标模式画面不可用，未执行切换。" : "墨水屏正在刷新，请稍后再试。";
    }
    return std::string("正在切换到") + display_name + "，请等待屏幕刷新完成。";
}
}  // namespace

VoiceGenerationService::VoiceGenerationService() : mutex_(xSemaphoreCreateMutex()) {}

const char* VoiceGenerationService::StateName(VoiceGenerationState state) {
    switch (state) {
        case VoiceGenerationState::kIdle: return "idle";
        case VoiceGenerationState::kAwaitingConfirm: return "awaiting_confirm";
        case VoiceGenerationState::kPendingApp: return "pending_app";
        case VoiceGenerationState::kAppGenerating: return "app_generating";
        case VoiceGenerationState::kUploading: return "uploading";
        case VoiceGenerationState::kDisplaying: return "displaying";
        case VoiceGenerationState::kSuccess: return "success";
        case VoiceGenerationState::kFailed: return "failed";
        case VoiceGenerationState::kCancelled: return "cancelled";
        case VoiceGenerationState::kExpired: return "expired";
    }
    return "unknown";
}

void VoiceGenerationService::ExpireLocked(std::uint64_t now_ms) {
    if (task_.state == VoiceGenerationState::kAwaitingConfirm && task_.expires_at_ms <= now_ms) {
        task_.state = VoiceGenerationState::kExpired;
        task_.error_code = "confirm_timeout";
    }
}

esp_err_t VoiceGenerationService::CreateAwaitingConfirm(const std::string& prompt, VoiceGenerationTask* output) {
    const auto mode = GetModeManager().GetSnapshot();
    if (mode.state != ModeSnapshot::State::kIdle || mode.active_feature != Feature::kAiAlbum) return ESP_ERR_NOT_SUPPORTED;
    if (prompt.empty() || prompt.size() > kPromptMaxBytes) return ESP_ERR_INVALID_ARG;
    if (mutex_ == nullptr) return ESP_ERR_NO_MEM;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    ExpireLocked(NowMs());
    if (!task_.id.empty() && !IsTerminal(task_.state)) { xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_)); return ESP_ERR_INVALID_STATE; }
    task_ = {"voice-" + std::to_string(sequence_++), prompt, VoiceGenerationState::kAwaitingConfirm, "", NowMs() + kConfirmTimeoutMs};
    if (output) *output = task_;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return ESP_OK;
}

esp_err_t VoiceGenerationService::Confirm(VoiceGenerationTask* output) {
    if (mutex_ == nullptr) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY); ExpireLocked(NowMs());
    if (task_.state != VoiceGenerationState::kAwaitingConfirm) { xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_)); return ESP_ERR_INVALID_STATE; }
    const auto mode = GetModeManager().GetSnapshot();
    if (mode.state != ModeSnapshot::State::kIdle || mode.active_feature != Feature::kAiAlbum) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (app_heartbeat_at_ms_ == 0 || NowMs() - app_heartbeat_at_ms_ > kAppHeartbeatTimeoutMs) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
        return ESP_ERR_NOT_FOUND;
    }
    task_.state = VoiceGenerationState::kPendingApp; task_.expires_at_ms = 0;
    if (output) *output = task_;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return ESP_OK;
}

esp_err_t VoiceGenerationService::Cancel(VoiceGenerationTask* output) {
    if (mutex_ == nullptr) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY); ExpireLocked(NowMs());
    if (task_.state != VoiceGenerationState::kAwaitingConfirm && task_.state != VoiceGenerationState::kPendingApp) { xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_)); return ESP_ERR_INVALID_STATE; }
    task_.state = VoiceGenerationState::kCancelled; task_.error_code.clear();
    if (output) *output = task_;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return ESP_OK;
}

esp_err_t VoiceGenerationService::Claim(const std::string& task_id, VoiceGenerationTask* output) {
    if (mutex_ == nullptr) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    if (task_.id != task_id || task_.state != VoiceGenerationState::kPendingApp) { xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_)); return ESP_ERR_INVALID_STATE; }
    task_.state = VoiceGenerationState::kAppGenerating;
    if (output) *output = task_;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return ESP_OK;
}

esp_err_t VoiceGenerationService::Update(const std::string& task_id, VoiceGenerationState state, const std::string& error_code, VoiceGenerationTask* output) {
    if (mutex_ == nullptr || task_id.empty()) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    const bool allowed = state == VoiceGenerationState::kFailed ||
        (task_.state == VoiceGenerationState::kAppGenerating && state == VoiceGenerationState::kUploading) ||
        (task_.state == VoiceGenerationState::kUploading && state == VoiceGenerationState::kDisplaying) ||
        (task_.state == VoiceGenerationState::kUploading && state == VoiceGenerationState::kSuccess && error_code == "saved_not_displayed") ||
        (task_.state == VoiceGenerationState::kDisplaying && state == VoiceGenerationState::kSuccess);
    if (task_.id != task_id || IsTerminal(task_.state) || !allowed) { xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_)); return ESP_ERR_INVALID_STATE; }
    task_.state = state; task_.error_code = error_code;
    if (output) *output = task_;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return ESP_OK;
}

void VoiceGenerationService::RecordAppHeartbeat() {
    if (mutex_ == nullptr) return;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    app_heartbeat_at_ms_ = NowMs();
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

bool VoiceGenerationService::IsAppAvailable() {
    if (mutex_ == nullptr) return false;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    const bool available = app_heartbeat_at_ms_ != 0 && NowMs() - app_heartbeat_at_ms_ <= kAppHeartbeatTimeoutMs;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return available;
}

VoiceGenerationTask VoiceGenerationService::GetSnapshot() {
    if (mutex_ == nullptr) return {};
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY); ExpireLocked(NowMs()); const auto copy = task_; xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_)); return copy;
}

VoiceGenerationService& GetVoiceGenerationService() { static VoiceGenerationService service; return service; }

void RegisterVoiceGenerationMcpTools() {
    static bool registered = false; if (registered) return; registered = true;
    auto& mcp = McpServer::GetInstance();
    mcp.SetToolAllowlist({"self.photo_frame.next_picture", "self.photo_frame.switch_mode", "self.photo_frame.create_image", "self.photo_frame.confirm_image", "self.photo_frame.cancel_image"});
    mcp.AddTool("self.photo_frame.next_picture", "Show the next photo in the currently active local or AI album. Never switch device mode.", PropertyList(), [](const PropertyList&) -> ReturnValue {
        return ShowNextPicture();
    });
    mcp.AddTool("self.photo_frame.switch_mode", "Switch to exactly one requested photo-frame mode. target must be local_album, ai_album, or info_dashboard. If the user did not name a target, ask which mode instead of calling this tool.", PropertyList({Property("target", kPropertyTypeString)}), [](const PropertyList& args) -> ReturnValue {
        return SwitchMode(args["target"].value<std::string>());
    });
    mcp.AddTool("self.photo_frame.create_image", "Create one image only while AI album is active. Call this when the user requests image generation; pass the exact Chinese image description. The result asks the user to confirm.", PropertyList({Property("prompt", kPropertyTypeString)}), [](const PropertyList& args) -> ReturnValue {
        VoiceGenerationTask task; const auto result = GetVoiceGenerationService().CreateAwaitingConfirm(args["prompt"].value<std::string>(), &task);
        if (result == ESP_ERR_NOT_SUPPORTED) return std::string("当前不是 AI 相册模式，不能语音生成图片。请先切换到 AI 相册。");
        if (result != ESP_OK) return std::string("当前已有生成任务，请先确认或取消它。");
        return std::string("请向用户确认：是否生成图片：") + task.prompt;
    });
    mcp.AddTool("self.photo_frame.confirm_image", "Confirm the one pending image request after the user clearly says confirm.", PropertyList(), [](const PropertyList&) -> ReturnValue {
        VoiceGenerationTask task;
        const auto result = GetVoiceGenerationService().Confirm(&task);
        if (result == ESP_ERR_NOT_FOUND) return std::string("手机语音生图服务未连接，请先在手机上开启该服务。");
        if (result == ESP_ERR_NOT_SUPPORTED) return std::string("当前不是 AI 相册模式，不能确认语音生图。");
        return result == ESP_OK ? std::string("已确认，正在生成图片，请等待。") : std::string("没有等待确认的生成请求。");
    });
    mcp.AddTool("self.photo_frame.cancel_image", "Cancel the one pending image request before generation starts.", PropertyList(), [](const PropertyList&) -> ReturnValue {
        VoiceGenerationTask task; return GetVoiceGenerationService().Cancel(&task) == ESP_OK ? std::string("已取消生成图片。") : std::string("没有可取消的生成请求。");
    });
}
}  // namespace photopainter::product
