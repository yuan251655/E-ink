#include "display_runtime.h"
#include "display_service.h"
#include "media_library_runtime.h"
#include "storage_runtime.h"
#include "user_app.h"
namespace photopainter::product {
DisplayService& GetDisplayService() { static DisplayService service; return service; }
esp_err_t InitializeDisplayService() { return GetDisplayService().Initialize(&GetStorageService(), &GetMediaLibrary(), &ePaperDisplay, epaper_gui_semapHandle); }
}
