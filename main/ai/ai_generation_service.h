#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "esp_err.h"
#include "product_types.h"
namespace photopainter::product {
class AiConfigService; class StorageService; class MediaLibrary; class JobService; class DisplayService;
class AiGenerationService {
public:
    struct PreviewSnapshot {
        bool ready = false;
        bool expired = false;
        std::string job_id;
        std::string prompt;
        std::uint64_t expires_at_ms = 0;
        std::uint64_t source_bytes = 0;
    };
    // A deliberately redacted view for recovery after the App process is
    // restarted. It never contains API credentials or the original prompt.
    struct ActiveTaskSnapshot {
        bool has_active_task = false;
        std::string job_id;
        std::string state;
        std::string phase;
        std::string kind;
        std::string prompt_summary;
    };
    // The last terminal task is persisted only as a privacy-safe diagnostic
    // record. It intentionally cannot be used to recreate a generation.
    struct LastTaskSnapshot {
        bool available = false;
        std::string job_id;
        std::string kind;
        std::string state;
        std::string phase;
        std::string error_code;
        std::uint64_t finished_at_ms = 0;
        std::string profile_id;
        std::string profile_name;
        std::string prompt_summary;
    };
    AiGenerationService(AiConfigService* config, StorageService* storage, MediaLibrary* library, JobService* jobs, DisplayService* display);
    // Generates a temporary preview only. It never writes a formal AI media item.
    esp_err_t Create(const RequestId& request_id, const std::string& prompt, bool ignored_display_flag, JobSnapshot* job, std::string* code);
    esp_err_t ConfirmSave(const RequestId& request_id, const std::string& preview_job_id, JobSnapshot* job, std::string* code);
    bool GetPreview(const std::string& preview_job_id, PreviewSnapshot* output);
    ActiveTaskSnapshot GetActiveTaskSnapshot() const;
    LastTaskSnapshot GetLastTaskSnapshot() const;
    esp_err_t StreamPreview(const std::string& preview_job_id, const std::function<esp_err_t(const void*, std::size_t)>& consume);
private:
    enum class WorkKind : std::uint8_t { kGeneratePreview, kConfirmSave };
    struct Work { WorkKind kind; char job_id[65]; char prompt[769]; char preview_job_id[65]; };
    static void WorkerEntry(void* context); void WorkerLoop();
    struct PreviewState { std::string job_id; std::string prompt; std::uint64_t expires_at_ms = 0; std::uint64_t source_bytes = 0; bool ready = false; };
    AiConfigService* config_; StorageService* storage_; MediaLibrary* library_; JobService* jobs_; DisplayService* display_;
    void UpdateActivePhaseLocked(const char* phase);
    static std::string SummarizePrompt(const char* prompt);
    void LoadLastTask();
    void RecordTerminalTask(const Work& work, bool success, const std::string& error_code);
    void* queue_ = nullptr; void* worker_ = nullptr; void* mutex_ = nullptr; bool active_ = false; PreviewState preview_; ActiveTaskSnapshot active_task_; LastTaskSnapshot last_task_;
};
}  // namespace photopainter::product
