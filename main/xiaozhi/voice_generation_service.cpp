#include "voice_generation_service.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "mcp_server.h"
#include "mode_manager.h"
#include "local_album_playback_runtime.h"
#include "ai_album_playback_runtime.h"
#include "display_runtime.h"
#include "display_service.h"
#include "job_runtime.h"
#include "job_service.h"
#include "power_service.h"
#include "button_sleep_service.h"
#include "voice_announcement_service.h"

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

const char* FeatureName(Feature feature) {
    switch (feature) {
        case Feature::kLocalAlbum: return "本地相册";
        case Feature::kAiAlbum: return "AI 相册";
        case Feature::kInfoDashboard: return "信息看板";
    }
    return "未知模式";
}

const char* SleepStateName(const std::string& state) {
    if (state == "rtc_unavailable") return "RTC 不可用";
    if (state == "busy") return "设备忙碌";
    if (state == "playback_due") return "轮播即将执行";
    if (state == "waiting_idle") return "等待空闲超时";
    if (state == "ready_to_sleep") return "可以休眠";
    return "未知";
}

const char* GenerationStateName(VoiceGenerationState state) {
    switch (state) {
        case VoiceGenerationState::kIdle: return "无任务";
        case VoiceGenerationState::kAwaitingConfirm: return "等待确认";
        case VoiceGenerationState::kPendingApp: return "等待手机处理";
        case VoiceGenerationState::kAppGenerating: return "正在生成";
        case VoiceGenerationState::kUploading: return "正在上传";
        case VoiceGenerationState::kDisplaying: return "正在显示";
        case VoiceGenerationState::kSuccess: return "已完成";
        case VoiceGenerationState::kFailed: return "失败";
        case VoiceGenerationState::kCancelled: return "已取消";
        case VoiceGenerationState::kExpired: return "已超时";
    }
    return "未知";
}

std::string PlaybackStatus(const PlaybackSnapshot& playback) {
    if (playback.config.mode == PlaybackMode::kPaused) return "轮播已暂停。";
    std::string result = std::string("轮播已开启，") +
        (playback.config.order == PlaybackOrder::kRandom ? "随机播放" : "顺序播放") +
        "，间隔 " + std::to_string(playback.config.interval_seconds / 60) + " 分钟";
    if (playback.has_next_play) {
        result += "，约 " + std::to_string(playback.next_play_in_seconds) + " 秒后切换";
    }
    return result + "。";
}

std::string GetStatus(const std::string& scope) {
    const auto mode = GetModeManager().GetSnapshot();
    if (scope == "summary") {
        const auto power = GetPowerService().GetSnapshot();
        const auto display = GetDisplayService().GetSnapshot();
        std::string result = std::string("当前是") + FeatureName(mode.active_feature) + "，墨水屏" +
            ((display.state == DisplayState::kQueued || display.state == DisplayState::kLoading ||
              display.state == DisplayState::kRefreshing || display.state == DisplayState::kFinalizing)
                 ? "正在刷新"
                 : "空闲");
        if (power.battery_present && power.battery_percent >= 0) {
            result += "，电量 " + std::to_string(power.battery_percent) + "%";
        }
        return result + "。";
    }
    if (scope == "power") {
        const auto power = GetPowerService().GetSnapshot();
        if (!power.pmic_online) return "电源管理芯片当前不可用。";
        if (!power.battery_present) return power.usb_present ? "当前使用 USB 供电，未检测到主电池。" : "未检测到主电池或 USB 供电。";
        std::string result = "主电池电压 " + std::to_string(power.battery_voltage_mv) + " 毫伏";
        if (power.battery_percent >= 0) result += "，电量 " + std::to_string(power.battery_percent) + "%";
        result += power.charging ? "，正在充电" : (power.discharging ? "，正在放电" : "，当前未充电");
        return result + "。";
    }
    if (scope == "playback") {
        if (mode.active_feature == Feature::kInfoDashboard) return "信息看板没有相册轮播。";
        return PlaybackStatus(mode.active_feature == Feature::kAiAlbum
                                  ? GetAiAlbumPlaybackService().GetSnapshot()
                                  : GetLocalAlbumPlaybackService().GetSnapshot());
    }
    if (scope == "sleep") {
        const auto config = GetAutomaticSleepConfig();
        const auto status = GetAutomaticSleepStatus();
        if (!config.enabled) return "自动休眠未开启。";
        return "自动休眠已开启，未操作 " + std::to_string(config.idle_timeout_minutes) +
               " 分钟后休眠，当前状态为" + SleepStateName(status.state) + "。";
    }
    if (scope == "voice_service") {
        const auto task = GetVoiceGenerationService().GetSnapshot();
        return std::string("手机语音生图服务") +
               (GetVoiceGenerationService().IsAppAvailable() ? "已连接" : "未连接") +
               "，生成任务状态为" + GenerationStateName(task.state) + "。";
    }
    return "状态范围只支持 summary、power、playback、sleep 或 voice_service。";
}

std::string SetPlayback(const std::string& action) {
    if (action != "pause" && action != "resume") return "操作只支持 pause 或 resume。";
    const auto mode = GetModeManager().GetSnapshot();
    if (mode.state != ModeSnapshot::State::kIdle) return "相框正在切换模式，请稍后再试。";
    if (mode.active_feature == Feature::kInfoDashboard) return "信息看板没有相册轮播，不能暂停或继续。";
    const PlaybackMode target = action == "pause" ? PlaybackMode::kPaused : PlaybackMode::kAuto;
    PlaybackSnapshot current = mode.active_feature == Feature::kAiAlbum
        ? GetAiAlbumPlaybackService().GetSnapshot()
        : GetLocalAlbumPlaybackService().GetSnapshot();
    if (current.config.mode == target) return target == PlaybackMode::kPaused ? "当前轮播已经暂停。" : "当前轮播已经开启。";
    PlaybackSnapshot updated;
    const esp_err_t result = mode.active_feature == Feature::kAiAlbum
        ? GetAiAlbumPlaybackService().UpdateConfig(target, current.config.interval_seconds,
                                                   current.config.order, current.config.revision, &updated)
        : GetLocalAlbumPlaybackService().UpdateConfig(target, current.config.interval_seconds,
                                                      current.config.order, current.config.revision, &updated);
    if (result != ESP_OK) return "轮播设置刚刚发生变化，请再试一次。";
    return target == PlaybackMode::kPaused ? "已暂停当前相册轮播。" : "已继续当前相册轮播，并重新开始计时。";
}

std::string ShowNextPicture() {
    const auto mode = GetModeManager().GetSnapshot();
    if (mode.state != ModeSnapshot::State::kIdle) return "相框正在切换模式，请稍后再试。";
    if (mode.active_feature == Feature::kInfoDashboard) return "信息看板不支持切换照片。";
    const esp_err_t result = mode.active_feature == Feature::kAiAlbum
        ? GetAiAlbumPlaybackService().RequestNext(true)
        : GetLocalAlbumPlaybackService().RequestNext(true);
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
    VoiceAnnouncement announcement = VoiceAnnouncement::kModeLocal;
    if (target == Feature::kAiAlbum) announcement = VoiceAnnouncement::kModeAi;
    else if (target == Feature::kInfoDashboard) announcement = VoiceAnnouncement::kModeDashboard;
    (void)GetVoiceAnnouncementService().WatchJob(job.job_id, announcement, VoiceAnnouncement::kNone);
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
    if (state == VoiceGenerationState::kSuccess) {
        (void)GetVoiceAnnouncementService().Enqueue(
            error_code == "saved_not_displayed" ? VoiceAnnouncement::kGenerationSaved
                                                 : VoiceAnnouncement::kGenerationDisplayed);
    } else if (state == VoiceGenerationState::kFailed) {
        (void)GetVoiceAnnouncementService().Enqueue(VoiceAnnouncement::kGenerationFailed);
    }
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
    GetVoiceAnnouncementService().Initialize();
    auto& mcp = McpServer::GetInstance();
    mcp.SetToolAllowlist({"self.photo_frame.next_picture", "self.photo_frame.switch_mode", "self.photo_frame.get_status", "self.photo_frame.set_playback", "self.photo_frame.create_image", "self.photo_frame.confirm_image", "self.photo_frame.cancel_image"});
    mcp.AddTool("self.photo_frame.next_picture", "Show the next photo in the currently active local or AI album. Never switch device mode.", PropertyList(), [](const PropertyList&) -> ReturnValue {
        RecordAutomaticSleepActivity();
        return ShowNextPicture();
    });
    mcp.AddTool("self.photo_frame.switch_mode", "Switch to exactly one requested photo-frame mode. target must be local_album, ai_album, or info_dashboard. If the user did not name a target, ask which mode instead of calling this tool.", PropertyList({Property("target", kPropertyTypeString)}), [](const PropertyList& args) -> ReturnValue {
        RecordAutomaticSleepActivity();
        return SwitchMode(args["target"].value<std::string>());
    });
    mcp.AddTool("self.photo_frame.get_status", "Read photo-frame status without changing settings or refreshing the screen. scope must be summary, power, playback, sleep, or voice_service.", PropertyList({Property("scope", kPropertyTypeString)}), [](const PropertyList& args) -> ReturnValue {
        RecordAutomaticSleepActivity();
        return GetStatus(args["scope"].value<std::string>());
    });
    mcp.AddTool("self.photo_frame.set_playback", "Pause or resume only the currently active local or AI album. action must be pause or resume. Never change interval or order.", PropertyList({Property("action", kPropertyTypeString)}), [](const PropertyList& args) -> ReturnValue {
        RecordAutomaticSleepActivity();
        return SetPlayback(args["action"].value<std::string>());
    });
    mcp.AddTool("self.photo_frame.create_image", "Create one image only while AI album is active. Call this when the user requests image generation; pass the exact Chinese image description. The result asks the user to confirm.", PropertyList({Property("prompt", kPropertyTypeString)}), [](const PropertyList& args) -> ReturnValue {
        RecordAutomaticSleepActivity();
        VoiceGenerationTask task; const auto result = GetVoiceGenerationService().CreateAwaitingConfirm(args["prompt"].value<std::string>(), &task);
        if (result == ESP_ERR_NOT_SUPPORTED) return std::string("当前不是 AI 相册模式，不能语音生成图片。请先切换到 AI 相册。");
        if (result != ESP_OK) return std::string("当前已有生成任务，请先确认或取消它。");
        return std::string("请向用户确认：是否生成图片：") + task.prompt;
    });
    mcp.AddTool("self.photo_frame.confirm_image", "Confirm the one pending image request after the user clearly says confirm.", PropertyList(), [](const PropertyList&) -> ReturnValue {
        RecordAutomaticSleepActivity();
        VoiceGenerationTask task;
        const auto result = GetVoiceGenerationService().Confirm(&task);
        if (result == ESP_ERR_NOT_FOUND) return std::string("手机语音生图服务未连接，请先在手机上开启该服务。");
        if (result == ESP_ERR_NOT_SUPPORTED) return std::string("当前不是 AI 相册模式，不能确认语音生图。");
        return result == ESP_OK ? std::string("已确认，正在生成图片，请等待。") : std::string("没有等待确认的生成请求。");
    });
    mcp.AddTool("self.photo_frame.cancel_image", "Cancel the one pending image request before generation starts.", PropertyList(), [](const PropertyList&) -> ReturnValue {
        RecordAutomaticSleepActivity();
        VoiceGenerationTask task; return GetVoiceGenerationService().Cancel(&task) == ESP_OK ? std::string("已取消生成图片。") : std::string("没有可取消的生成请求。");
    });
}
}  // namespace photopainter::product
