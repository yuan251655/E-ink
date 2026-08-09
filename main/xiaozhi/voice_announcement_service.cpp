#include "voice_announcement_service.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "application.h"
#include "job_runtime.h"
#include "job_service.h"
#include "assets/lang_config.h"

namespace photopainter::product {
namespace {

constexpr std::size_t kQueueCapacity = 6;

std::string_view SoundFor(VoiceAnnouncement announcement) {
    switch (announcement) {
        case VoiceAnnouncement::kNone: return {};
        case VoiceAnnouncement::kModeLocal: return Lang::Sounds::OGG_VOICE_MODE_LOCAL;
        case VoiceAnnouncement::kModeAi: return Lang::Sounds::OGG_VOICE_MODE_AI;
        case VoiceAnnouncement::kModeDashboard: return Lang::Sounds::OGG_VOICE_MODE_DASHBOARD;
        case VoiceAnnouncement::kNextSuccess: return Lang::Sounds::OGG_VOICE_NEXT_SUCCESS;
        case VoiceAnnouncement::kNextFailed: return Lang::Sounds::OGG_VOICE_NEXT_FAILED;
        case VoiceAnnouncement::kGenerationDisplayed: return Lang::Sounds::OGG_VOICE_GENERATION_DISPLAYED;
        case VoiceAnnouncement::kGenerationSaved: return Lang::Sounds::OGG_VOICE_GENERATION_SAVED;
        case VoiceAnnouncement::kGenerationFailed: return Lang::Sounds::OGG_VOICE_GENERATION_FAILED;
    }
    return {};
}

bool IsTerminal(JobState state) {
    return state == JobState::kSuccess || state == JobState::kFailed ||
           state == JobState::kCancelled || state == JobState::kTimeout;
}

}  // namespace

void VoiceAnnouncementService::Initialize() {
    if (queue_ != nullptr) return;
    queue_ = xQueueCreate(kQueueCapacity, sizeof(VoiceAnnouncement));
    mutex_ = xSemaphoreCreateMutex();
    if (queue_ == nullptr || mutex_ == nullptr) return;
    TaskHandle_t worker = nullptr;
    if (xTaskCreate(WorkerEntry, "voice_announce", 4096, this, 3, &worker) == pdPASS) {
        worker_ = worker;
    }
}

bool VoiceAnnouncementService::Enqueue(VoiceAnnouncement announcement) {
    if (queue_ == nullptr || worker_ == nullptr) return false;
    return xQueueSend(static_cast<QueueHandle_t>(queue_), &announcement, 0) == pdTRUE;
}

bool VoiceAnnouncementService::WatchJob(const std::string& job_id,
                                        VoiceAnnouncement success,
                                        VoiceAnnouncement failure) {
    if (job_id.empty() || mutex_ == nullptr || worker_ == nullptr) return false;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    for (auto& watch : watches_) {
        if (!watch.occupied) {
            watch = {true, job_id, success, failure};
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
            return true;
        }
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
}

bool VoiceAnnouncementService::IsBusy() const {
    if (queue_ == nullptr || mutex_ == nullptr || worker_ == nullptr) return false;
    if (uxQueueMessagesWaiting(static_cast<QueueHandle_t>(queue_)) > 0) return true;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    bool busy = playing_;
    for (const auto& watch : watches_) busy = busy || watch.occupied;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return busy;
}

void VoiceAnnouncementService::WorkerEntry(void* context) {
    static_cast<VoiceAnnouncementService*>(context)->WorkerLoop();
}

void VoiceAnnouncementService::WorkerLoop() {
    while (true) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
        for (auto& watch : watches_) {
            if (!watch.occupied) continue;
            JobSnapshot job;
            if (!GetProductJobService().Get(watch.job_id, &job) || !IsTerminal(job.state)) continue;
            const auto announcement = job.state == JobState::kSuccess ? watch.success : watch.failure;
            watch = {};
            if (announcement != VoiceAnnouncement::kNone) (void)Enqueue(announcement);
        }
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));

        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
        if (playing_ && audio.IsIdle()) playing_ = false;
        const bool can_play = !playing_ && app.GetDeviceState() == kDeviceStateIdle && audio.IsIdle();
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
        if (can_play) {
            VoiceAnnouncement announcement;
            if (xQueueReceive(static_cast<QueueHandle_t>(queue_), &announcement, 0) == pdTRUE) {
                const auto sound = SoundFor(announcement);
                if (!sound.empty()) {
                    app.PlaySound(sound);
                    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
                    playing_ = true;
                    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

VoiceAnnouncementService& GetVoiceAnnouncementService() {
    static VoiceAnnouncementService service;
    return service;
}

}  // namespace photopainter::product
