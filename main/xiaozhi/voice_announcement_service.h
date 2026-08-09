#pragma once

#include <cstdint>
#include <string>

namespace photopainter::product {

enum class VoiceAnnouncement : std::uint8_t {
    kNone,
    kModeLocal,
    kModeAi,
    kModeDashboard,
    kNextSuccess,
    kNextFailed,
    kGenerationDisplayed,
    kGenerationSaved,
    kGenerationFailed,
};

class VoiceAnnouncementService {
public:
    void Initialize();
    bool Enqueue(VoiceAnnouncement announcement);
    bool WatchJob(const std::string& job_id, VoiceAnnouncement success,
                  VoiceAnnouncement failure);
    bool IsBusy() const;

private:
    struct JobWatch {
        bool occupied = false;
        std::string job_id;
        VoiceAnnouncement success = VoiceAnnouncement::kNextSuccess;
        VoiceAnnouncement failure = VoiceAnnouncement::kNextFailed;
    };

    static void WorkerEntry(void* context);
    void WorkerLoop();

    void* queue_ = nullptr;
    void* mutex_ = nullptr;
    void* worker_ = nullptr;
    bool playing_ = false;
    JobWatch watches_[4];
};

VoiceAnnouncementService& GetVoiceAnnouncementService();

}  // namespace photopainter::product
