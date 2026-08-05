#pragma once

#include <string>

#include "esp_err.h"
#include "esp_http_server.h"
#include "product_types.h"

namespace photopainter::product {

class JobService;
class MediaLibrary;
class StorageService;

// The API handler owns HTTP status mapping.  This worker only receives the
// request, atomically admits a valid local or AI media item, and returns a
// stable product error code plus the upload job snapshot.
struct MediaUploadResult {
    esp_err_t error = ESP_FAIL;
    std::string code = "invalid_request";
    JobSnapshot job;
};

// Receives exactly two multipart parts in this order: metadata (JSON),
// image_bin. The request body is streamed to TF; no phone image or display
// frame is retained in RAM. The function is synchronous because the
// HTTP server owns the socket, while the returned JobSnapshot remains the
// authoritative result for subsequent GET /jobs/{job_id} requests.
MediaUploadResult ReceiveBinOnlyMultipart(httpd_req_t* request,
                                           StorageService& storage,
                                           MediaLibrary& media_library,
                                           JobService& jobs);

}  // namespace photopainter::product
