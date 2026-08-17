#include "voice_generation_service.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_log.h>

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
#include "audio_config_service.h"
#include "application.h"
#include "xiaozhi_runtime.h"

namespace photopainter::product {
namespace {
constexpr std::size_t kPromptMaxBytes = 320;
constexpr std::uint64_t kConfirmTimeoutMs = 30'000;
constexpr std::uint64_t kAppHeartbeatTimeoutMs = 15'000;

constexpr char kBirthdayEasterEgg[] = R"EASTER(彩蛋被菁菁小姐姐发现啦。接下来，是你的宝宝专门留给你的一段话。

二十四岁生日快乐呀！

亲爱的菁菁：

不知不觉已经是陪你度过的第五次生日了，先祝你二十四岁生日快乐呀！人生的每一岁都很珍贵，愿你在新的一岁里心想事成、快乐常在。不过不管多少岁，我的宝宝在我心里永远都是十八岁！

从二零二零年第一次在西南石油大学见到你，到后来我们成为恋人，再到今天，我们已经一起留下了许多珍贵的回忆。我做这台相框送给你，不只是想送你一件生日礼物，更想为我们准备一个收藏时光的小地方。以后，我们一起拍下的照片、去过的地方、经历的快乐，还有那些看起来平凡却值得记住的瞬间，都可以慢慢装进这里。

当你一个人想得太多，或者偶尔感到难过的时候，希望你看到相框里的照片，能够想起我们一起经历过的快乐。距离可能会让我们暂时不能时时待在彼此身边，但它不会改变我对你的喜欢和牵挂。这是我们异地生活的最后一年。明年这个时候，我们一定会在一起，迎接一段全新的生活。我们也会继续努力，成为更好的自己。

你之前经常问我，我看上你什么，其实我一直都觉得自己真的很幸运，能够遇到一个漂亮、可爱、体贴、温柔、聪明、三观正的知心小姐姐。跟你在一起，带给我很多之前从未体验的感受。在我们相处的日子里，我不断感受着你的细腻与温暖。和你在一起的每一个瞬间，都令人难忘、充满意义。

希望未来，无论发生什么事情，我们都能认真倾听对方的内心。良辰好景总会有的，我相信我们会一起前往，一起去欣赏，我们会在流动的时辰里面慢慢积攒对彼此的爱。

生日快乐，我的宝宝。我希望这个相册能够陪我们一起记录生活的点点滴滴。在我们暂时还不能时时陪在彼此身边的这不到一年里，也希望它能够替我安静地陪着你，也希望未来每一段幸福的回忆里面，都有我们。

最后，祝我们在各自的成长路上渐入佳境，祝你在崭新的一岁里面快乐常在，幸运依旧！

爱你爱你超级爱你的宝宝。

二零二六年八月二十四日。

菁菁小姐姐，生日快乐。这就是你的宝宝藏在相框里，专门留给你的话。)EASTER";

bool IsBirthdayEasterEggPhrase(const std::string& phrase) {
    std::string normalized;
    normalized.reserve(phrase.size());
    for (const unsigned char value : phrase) {
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n') normalized.push_back(value);
    }
    for (const std::string suffix : {"。", "！", "!", "？", "?", "，", ","}) {
        if (normalized.ends_with(suffix)) normalized.resize(normalized.size() - suffix.size());
    }
    return normalized == "生日快乐";
}

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

esp_err_t RequestBirthdayEasterEggDisplay(bool* already_visible = nullptr) {
    if (already_visible != nullptr) *already_visible = false;
    const auto display = GetDisplayService().GetSnapshot();
    if (display.current_media_id == "birthday_easter_egg") {
        if (already_visible != nullptr) *already_visible = true;
        ESP_LOGI("birthday_egg", "Display already visible; skipped duplicate refresh");
        return ESP_OK;
    }
    const auto mode = GetModeManager().GetSnapshot();
    JobSnapshot job;
    const RequestId request_id = "birthday-egg-" + std::to_string(NowMs());
    if (GetProductJobService().CreateOrFind(JobKind::kDisplay, request_id,
                                            "birthday-easter-egg", &job) !=
        JobRegistrationResult::kCreated) {
        ESP_LOGW("birthday_egg", "Job admission failed; skipped birthday refresh");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t submitted = GetDisplayService().SubmitBirthdayEasterEgg(
        mode.active_feature, job.job_id, &GetProductJobService());
    if (submitted == ESP_OK) {
        ESP_LOGI("birthday_egg", "Submitted birthday easter egg display");
        return ESP_OK;
    }
    (void)GetProductJobService().Update(job.job_id, JobState::kFailed, "failed", 0,
                                        "display_busy");
    ESP_LOGW("birthday_egg", "Display busy; skipped birthday refresh");
    return submitted;
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
        const std::string timeout = config.idle_timeout_seconds < 60
            ? std::to_string(config.idle_timeout_seconds) + " 秒"
            : std::to_string(config.idle_timeout_seconds / 60) + " 分钟";
        return "自动休眠已开启，未操作 " + timeout + "后休眠，当前状态为" + SleepStateName(status.state) + "。";
    }
    if (scope == "audio") {
        const auto audio = GetAudioConfigSnapshot();
        if (audio.muted) return "相框当前已静音，保留音量为 " + std::to_string(audio.master_volume) + "%。";
        return "相框当前音量为 " + std::to_string(audio.master_volume) + "%。";
    }
    if (scope == "voice_service") {
        const auto task = GetVoiceGenerationService().GetSnapshot();
        return std::string("手机语音生图服务") +
               (GetVoiceGenerationService().IsAppAvailable() ? "已连接" : "未连接") +
               "，生成任务状态为" + GenerationStateName(task.state) + "。";
    }
    return "状态范围只支持 summary、power、playback、sleep 或 voice_service。";
}

std::string SetAudioVolume(int volume) {
    if (volume < 1 || volume > 100) return "音量只支持 1 到 100。";
    if (UpdateAudioConfig(volume, false) != ESP_OK) return "音量设置失败，请稍后再试。";
    return "已将音量调到 " + std::to_string(volume) + "%。";
}

std::string SetAudioMuted(bool muted) {
    const auto current = GetAudioConfigSnapshot();
    if (current.muted == muted) return muted ? "当前已经静音。" : "当前已经取消静音。";
    if (UpdateAudioConfig(current.master_volume, muted) != ESP_OK) return "静音设置失败，请稍后再试。";
    return muted ? "已静音。" : "已取消静音，恢复到之前的音量。";
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

esp_err_t TriggerBirthdayEasterEggFromApp(bool* already_visible) {
    if (already_visible == nullptr) return ESP_ERR_INVALID_ARG;
    return RequestBirthdayEasterEggDisplay(already_visible);
}

bool HandleBirthdayEasterEggStt(const std::string& phrase) {
    if (!IsBirthdayEasterEggPhrase(phrase)) return false;
    RecordAutomaticSleepActivity();
    ESP_LOGI("birthday_egg", "Matched birthday phrase directly from STT");
    (void)RequestBirthdayEasterEggDisplay();
    return true;
}

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
    mcp.SetToolAllowlist({"self.photo_frame.next_picture", "self.photo_frame.switch_mode", "self.photo_frame.get_status", "self.photo_frame.set_playback", "self.photo_frame.set_automatic_sleep", "self.photo_frame.set_volume", "self.photo_frame.set_mute", "self.photo_frame.create_image", "self.photo_frame.confirm_image", "self.photo_frame.cancel_image", "self.photo_frame.play_birthday_easter_egg"});
    mcp.AddTool("self.photo_frame.next_picture", "Show the next photo in the currently active local or AI album. Never switch device mode.", PropertyList(), [](const PropertyList&) -> ReturnValue {
        RecordAutomaticSleepActivity();
        return ShowNextPicture();
    });
    mcp.AddTool("self.photo_frame.switch_mode", "Switch to exactly one requested photo-frame mode. target must be local_album, ai_album, or info_dashboard. If the user did not name a target, ask which mode instead of calling this tool.", PropertyList({Property("target", kPropertyTypeString)}), [](const PropertyList& args) -> ReturnValue {
        RecordAutomaticSleepActivity();
        return SwitchMode(args["target"].value<std::string>());
    });
    mcp.AddTool("self.photo_frame.get_status", "Read photo-frame status without changing settings or refreshing the screen. scope must be summary, power, playback, sleep, audio, or voice_service.", PropertyList({Property("scope", kPropertyTypeString)}), [](const PropertyList& args) -> ReturnValue {
        RecordAutomaticSleepActivity();
        return GetStatus(args["scope"].value<std::string>());
    });
    mcp.AddTool("self.photo_frame.set_playback", "Pause or resume only the currently active local or AI album. action must be pause or resume. Never change interval or order.", PropertyList({Property("action", kPropertyTypeString)}), [](const PropertyList& args) -> ReturnValue {
        RecordAutomaticSleepActivity();
        return SetPlayback(args["action"].value<std::string>());
    });
    mcp.AddTool("self.photo_frame.set_automatic_sleep", "Enable or disable automatic sleep only when the user explicitly asks to turn automatic sleep on or off. Set enabled true to enable and false to disable. This never enters sleep immediately and never changes the saved idle timeout or any album playback setting.", PropertyList({Property("enabled", kPropertyTypeBoolean)}), [](const PropertyList& args) -> ReturnValue {
        auto config = GetAutomaticSleepConfig();
        const bool enabled = args["enabled"].value<bool>();
        if (config.enabled == enabled) {
            if (enabled) RecordAutomaticSleepActivity();
            return enabled ? std::string("自动休眠已经开启，已重新开始空闲计时。")
                           : std::string("自动休眠已经关闭。");
        }
        config.enabled = enabled;
        if (UpdateAutomaticSleepConfig(config) != ESP_OK) return std::string("自动休眠设置保存失败，请稍后再试。");
        if (enabled) RecordAutomaticSleepActivity();
        return enabled ? std::string("已开启自动休眠，并从现在重新开始空闲计时。")
                       : std::string("已关闭自动休眠。");
    });
    mcp.AddTool("self.photo_frame.set_volume", "Set the photo-frame speaker volume to an exact value from 1 to 100. This also cancels mute. Use only when the user explicitly requests a volume value.", PropertyList({Property("volume", kPropertyTypeInteger, 1, 100)}), [](const PropertyList& args) -> ReturnValue {
        RecordAutomaticSleepActivity();
        return SetAudioVolume(args["volume"].value<int>());
    });
    mcp.AddTool("self.photo_frame.set_mute", "Mute or unmute the photo-frame speaker. Muting preserves the saved volume and unmuting restores it. Use only when the user explicitly requests mute or unmute.", PropertyList({Property("muted", kPropertyTypeBoolean)}), [](const PropertyList& args) -> ReturnValue {
        RecordAutomaticSleepActivity();
        return SetAudioMuted(args["muted"].value<bool>());
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
    mcp.AddTool(
        "self.photo_frame.play_birthday_easter_egg",
        "Call this tool every time the user's latest complete utterance is exactly '生日快乐', including repeated occurrences "
        "in the same conversation. Pass that exact latest "
        "utterance in phrase. Never reuse an earlier result and never call for partial, similar, guessed, "
        "requested, or quoted phrases. After calling, speak the returned Chinese text verbatim at roughly "
        "0.8x normal speed, with a short pause between paragraphs. Do not summarize, rewrite, or add anything.",
        PropertyList({Property("phrase", kPropertyTypeString)}), [](const PropertyList& args) -> ReturnValue {
            const auto phrase = args["phrase"].value<std::string>();
            if (!IsBirthdayEasterEggPhrase(phrase)) {
                ESP_LOGW("birthday_egg", "Rejected non-matching phrase");
                return std::string("未触发彩蛋。");
            }
            RecordAutomaticSleepActivity();
            (void)RequestBirthdayEasterEggDisplay();
            return std::string(kBirthdayEasterEgg);
        });
}
}  // namespace photopainter::product
