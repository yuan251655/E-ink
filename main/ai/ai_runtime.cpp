#include "ai_runtime.h"
#include "ai_config_service.h"
#include "ai_generation_service.h"
#include "display_runtime.h"
#include "job_runtime.h"
#include "media_library_runtime.h"
#include "storage_runtime.h"
namespace photopainter::product {
AiConfigService& GetAiConfigService() { static AiConfigService service; return service; }
AiGenerationService& GetAiGenerationService() { static AiGenerationService service(&GetAiConfigService(), &GetStorageService(), &GetMediaLibrary(), &GetProductJobService(), &GetDisplayService()); return service; }
}  // namespace photopainter::product
