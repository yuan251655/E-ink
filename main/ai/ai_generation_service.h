#pragma once
#include <string>
#include "esp_err.h"
#include "product_types.h"
namespace photopainter::product {
class AiConfigService; class StorageService; class MediaLibrary; class JobService; class DisplayService;
class AiGenerationService {
public:
    AiGenerationService(AiConfigService* config, StorageService* storage, MediaLibrary* library, JobService* jobs, DisplayService* display);
    // Admission is nonblocking. A second active job is rejected deterministically.
    esp_err_t Create(const RequestId& request_id, const std::string& prompt, bool display_when_active, JobSnapshot* job, std::string* code);
private:
    struct Work { char job_id[65]; char prompt[769]; bool display_when_active; };
    static void WorkerEntry(void* context); void WorkerLoop();
    AiConfigService* config_; StorageService* storage_; MediaLibrary* library_; JobService* jobs_; DisplayService* display_;
    void* queue_ = nullptr; void* worker_ = nullptr; void* mutex_ = nullptr; bool active_ = false;
};
}  // namespace photopainter::product
