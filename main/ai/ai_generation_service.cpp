#include "ai_generation_service.h"
#include <cstring>
#include <cstdio>
#include <string>
#include "ArduinoJson.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_tls_errors.h"
#include "mbedtls/sha256.h"
#include "imgdecode_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "ai_config_service.h"
#include "display_service.h"
#include "job_service.h"
#include "media_library.h"
#include "mode_manager.h"
#include "storage_service.h"
namespace photopainter::product {
namespace {
// Seedream 2k JPEG is retained only in the staging transaction. The decoder
// immediately scales it to 800x480, so no full RGB888 2k frame is allocated.
constexpr std::size_t kMaxSourceBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaxResponseBytes = 8192U;
constexpr std::uint64_t kRequiredBytes = kMaxSourceBytes + kDisplayFrameBytes + 4096U;
extern const uint8_t ark_vol_pem_start[] asm("_binary_ark_vol_pem_start");
extern const uint8_t ark_volces_chain_pem_start[] asm("_binary_volces_chain_pem_start");

std::string NewId(const char* prefix) { char value[40]; std::snprintf(value, sizeof(value), "%s-%08lx-%08lx", prefix, static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random())); return value; }
std::string Hex(const unsigned char digest[32]) { static constexpr char kHex[]="0123456789abcdef"; std::string value(64,'0'); for (std::size_t i=0;i<32;++i) { value[i*2]=kHex[digest[i]>>4]; value[i*2+1]=kHex[digest[i]&15]; } return value; }
uint8_t NibbleForRgb(const uint8_t* rgb) { if (rgb[0]==0 && rgb[1]==0 && rgb[2]==0) return 0; if (rgb[0]==255 && rgb[1]==255 && rgb[2]==255) return 1; if (rgb[0]==255 && rgb[1]==255 && rgb[2]==0) return 2; if (rgb[0]==255 && rgb[1]==0 && rgb[2]==0) return 3; if (rgb[0]==0 && rgb[1]==0 && rgb[2]==255) return 5; if (rgb[0]==0 && rgb[1]==255 && rgb[2]==0) return 6; return 1; }
struct ResponseBuffer { std::string data; };
esp_err_t ResponseEvent(esp_http_client_event_t* event) { auto* result=static_cast<ResponseBuffer*>(event->user_data); if (event->event_id==HTTP_EVENT_ON_DATA && event->data_len>0) { if (!result || result->data.size()+static_cast<std::size_t>(event->data_len)>kMaxResponseBytes) return ESP_ERR_NO_MEM; result->data.append(static_cast<const char*>(event->data), event->data_len); } return ESP_OK; }
struct ImageUrlRequestResult { std::string code = "ai_request_failed"; int http_status = 0; esp_err_t transport_error = ESP_FAIL; int tls_error = 0; };
const char* RequestFailureCode(const ImageUrlRequestResult& result) {
    if (result.http_status == 401) return "ai_http_401";
    if (result.http_status == 403) return "ai_http_403";
    if (result.http_status == 404) return "ai_http_404";
    if (result.http_status == 429) return "ai_http_429";
    if (result.http_status >= 500 && result.http_status <= 599) return "ai_http_5xx";
    if (result.tls_error != 0 || (result.transport_error >= ESP_ERR_ESP_TLS_BASE && result.transport_error < ESP_ERR_ESP_TLS_BASE + 0x100)) return "ai_tls_failed";
    if (result.transport_error == ESP_ERR_TIMEOUT || result.transport_error == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT || result.transport_error == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT) return "ai_request_timeout";
    if (result.transport_error == ESP_ERR_HTTP_CONNECT || result.transport_error == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME || result.transport_error == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST) return "ai_network_failed";
    return "ai_request_failed";
}
struct DownloadContext { StorageService* storage=nullptr; std::uint64_t bytes=0; bool first=true; esp_err_t error=ESP_OK; mbedtls_sha256_context sha{}; };
esp_err_t DownloadEvent(esp_http_client_event_t* event) { auto* context=static_cast<DownloadContext*>(event->user_data); if (event->event_id!=HTTP_EVENT_ON_DATA || event->data_len<=0) return ESP_OK; if (!context || !context->storage || context->bytes+static_cast<std::size_t>(event->data_len)>kMaxSourceBytes) return ESP_ERR_INVALID_SIZE; if (mbedtls_sha256_update(&context->sha, static_cast<const unsigned char*>(event->data), event->data_len)!=0) return ESP_FAIL; context->error=context->storage->AppendStagedFile("source.jpg", event->data, event->data_len, context->first); context->first=false; if (context->error!=ESP_OK) return context->error; context->bytes += static_cast<std::size_t>(event->data_len); return ESP_OK; }
bool RequestImageUrl(const std::string& endpoint, const std::string& model, const std::string& key, const char* prompt, std::string* url, ImageUrlRequestResult* result) {
    if (!url || !result) return false;
    // Keep real generation aligned with the verified Seedream 5.0 Pro model
    // configuration request. Do not inherit legacy-only stream flags.
    // The preview transaction, preview endpoint and validated official
    // decoder all use JPEG (`source.jpg`). Keep the provider response in the
    // same format so the App never receives PNG bytes labelled as JPEG.
    JsonDocument request; request["model"]=model; request["prompt"]=prompt; request["response_format"]="url"; request["size"]="2K"; request["output_format"]="jpeg"; request["watermark"]=false;
    std::string body; serializeJson(request,body); ResponseBuffer response; esp_http_client_config_t config{};
    config.url=endpoint.c_str(); config.cert_pem=reinterpret_cast<const char*>(ark_vol_pem_start); config.event_handler=ResponseEvent; config.user_data=&response;
    config.timeout_ms=180000; config.buffer_size=4096; config.buffer_size_tx=2048;
    auto client=esp_http_client_init(&config); if(!client) { result->code="ai_client_init_failed"; result->transport_error=ESP_ERR_NO_MEM; return false; }
    std::string auth="Bearer "+key; esp_http_client_set_method(client,HTTP_METHOD_POST); esp_http_client_set_header(client,"Content-Type","application/json"); esp_http_client_set_header(client,"Authorization",auth.c_str()); esp_http_client_set_post_field(client,body.data(),body.size());
    result->transport_error=esp_http_client_perform(client); result->http_status=esp_http_client_get_status_code(client);
    int tls_flags=0; (void)esp_http_client_get_and_clear_last_tls_error(client, &result->tls_error, &tls_flags);
    esp_http_client_cleanup(client);
    if(result->transport_error!=ESP_OK || result->http_status<200 || result->http_status>=300) {
        result->code=RequestFailureCode(*result);
        ESP_LOGW("ai_generation", "image request failed: code=%s http=%d transport=%s tls=%d", result->code.c_str(), result->http_status, esp_err_to_name(result->transport_error), result->tls_error);
        return false;
    }
    JsonDocument decoded; if(deserializeJson(decoded,response.data)!=DeserializationError::Ok) { result->code="ai_invalid_provider_response"; return false; }
    const char* parsed=decoded["data"][0]["url"]; if(!parsed || std::strlen(parsed)>1024) { result->code="ai_invalid_provider_response"; return false; }
    *url=parsed; result->code="ok"; return true;
}
esp_err_t DownloadSource(StorageService* storage, const std::string& url, std::string* sha256, std::uint64_t* bytes) { DownloadContext context; context.storage=storage; mbedtls_sha256_init(&context.sha); if(mbedtls_sha256_starts(&context.sha,false)!=0) { mbedtls_sha256_free(&context.sha); return ESP_FAIL; } esp_http_client_config_t config{}; config.url=url.c_str(); config.cert_pem=reinterpret_cast<const char*>(ark_volces_chain_pem_start); config.event_handler=DownloadEvent; config.user_data=&context; config.timeout_ms=60000; config.buffer_size=4096; auto client=esp_http_client_init(&config); if(!client){mbedtls_sha256_free(&context.sha);return ESP_ERR_NO_MEM;} esp_http_client_set_method(client,HTTP_METHOD_GET); const esp_err_t err=esp_http_client_perform(client); const int status=esp_http_client_get_status_code(client); esp_http_client_cleanup(client); unsigned char digest[32]{}; const bool hash_ok=mbedtls_sha256_finish(&context.sha,digest)==0; mbedtls_sha256_free(&context.sha); if(err!=ESP_OK||status<200||status>=300||context.error!=ESP_OK||context.bytes==0||!hash_ok) return context.error==ESP_OK?ESP_FAIL:context.error; const esp_err_t finalize=storage->FinalizeStagedFile("source.jpg",context.bytes); if(finalize!=ESP_OK)return finalize; *sha256=Hex(digest);*bytes=context.bytes;return ESP_OK; }
esp_err_t ConvertToFrame(StorageService* storage, const std::string& absolute_source, std::string* sha256) { ImgDecodeDither decoder; uint8_t* decoded=nullptr; int decoded_bytes=0; const esp_err_t decode=decoder.ImgDecode_TFOneJPGPictureScaled(absolute_source.c_str(),kDisplayWidth,kDisplayHeight,&decoded,&decoded_bytes); if(decode!=ESP_OK||!decoded||decoded_bytes!=kDisplayWidth*kDisplayHeight*3){if(decoded)decoder.ImgDecode_JPGBufferFree(decoded);return ESP_ERR_INVALID_RESPONSE;} uint8_t* dithered=static_cast<uint8_t*>(heap_caps_malloc(kDisplayWidth*kDisplayHeight*3,MALLOC_CAP_SPIRAM)); if(!dithered){decoder.ImgDecode_JPGBufferFree(decoded);return ESP_ERR_NO_MEM;} decoder.ImgDecode_DitherRgb888(decoded,dithered,kDisplayWidth,kDisplayHeight); decoder.ImgDecode_JPGBufferFree(decoded); mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);mbedtls_sha256_starts(&hash,false); std::uint8_t row[kDisplayWidth/2]; bool first=true; esp_err_t result=ESP_OK; for(int y=0;y<kDisplayHeight&&result==ESP_OK;++y){for(int x=0;x<kDisplayWidth;x+=2){const uint8_t a=NibbleForRgb(dithered+(y*kDisplayWidth+x)*3);const uint8_t b=NibbleForRgb(dithered+(y*kDisplayWidth+x+1)*3);row[x/2]=static_cast<uint8_t>((a<<4)|b);} if(mbedtls_sha256_update(&hash,row,sizeof(row))!=0) result=ESP_FAIL; if(result==ESP_OK)result=storage->AppendStagedFile("image.bin",row,sizeof(row),first); first=false;} heap_caps_free(dithered); unsigned char digest[32]{}; const bool hash_ok=mbedtls_sha256_finish(&hash,digest)==0;mbedtls_sha256_free(&hash);if(result!=ESP_OK||!hash_ok)return result==ESP_OK?ESP_FAIL:result;result=storage->FinalizeStagedFile("image.bin",kDisplayFrameBytes);if(result==ESP_OK)*sha256=Hex(digest);return result; }
}
AiGenerationService::AiGenerationService(AiConfigService* config, StorageService* storage, MediaLibrary* library, JobService* jobs, DisplayService* display)
 : config_(config), storage_(storage), library_(library), jobs_(jobs), display_(display) {
    queue_ = xQueueCreate(1, sizeof(Work)); mutex_ = xSemaphoreCreateMutex();
    if (queue_) xTaskCreate(WorkerEntry, "ai_generation", 6144, this, 4, reinterpret_cast<TaskHandle_t*>(&worker_));
}
esp_err_t AiGenerationService::Create(const RequestId& request_id, const std::string& prompt, bool /*ignored_display_flag*/, JobSnapshot* job, std::string* code) {
    if (!job || !code || prompt.empty() || prompt.size() > 768 || request_id.empty() || request_id.size() > 64) { if (code) *code="invalid_request"; return ESP_ERR_INVALID_ARG; }
    if (!config_->GetPublicConfig().configured) { *code="ai_not_configured"; return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(reinterpret_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    if (active_) { xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_)); *code="ai_job_busy"; return ESP_ERR_INVALID_STATE; }
    const auto admission = jobs_->CreateOrFind(JobKind::kAiGeneration, request_id, "ai:" + prompt, job);
    if (admission == JobRegistrationResult::kExisting) { xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_)); *code="ok"; return ESP_OK; }
    if (admission != JobRegistrationResult::kCreated) { xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_)); *code=admission==JobRegistrationResult::kRequestIdConflict?"request_id_conflict":"ai_job_busy"; return ESP_ERR_INVALID_STATE; }
    Work work{}; work.kind=WorkKind::kGeneratePreview; std::strncpy(work.job_id, job->job_id.c_str(), sizeof(work.job_id)-1); std::strncpy(work.prompt, prompt.c_str(), sizeof(work.prompt)-1);
    active_=true;
    if (xQueueSend(reinterpret_cast<QueueHandle_t>(queue_), &work, 0) != pdTRUE) { active_=false; (void)jobs_->Update(job->job_id, JobState::kFailed,"failed",0,"ai_job_busy"); xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_)); *code="ai_job_busy"; return ESP_ERR_INVALID_STATE; }
    xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_)); *code="accepted"; return ESP_OK;
}
esp_err_t AiGenerationService::ConfirmSave(const RequestId& request_id, const std::string& preview_job_id, JobSnapshot* job, std::string* code) {
    if (!job || !code || request_id.empty() || preview_job_id.empty()) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(reinterpret_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    const std::uint64_t now=static_cast<std::uint64_t>(esp_timer_get_time()/1000);
    if (active_) { xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_)); *code="ai_job_busy"; return ESP_ERR_INVALID_STATE; }
    if (!preview_.ready || preview_.job_id!=preview_job_id) { xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_)); *code="preview_not_found"; return ESP_ERR_NOT_FOUND; }
    if (now>=preview_.expires_at_ms) { preview_.ready=false; xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_)); (void)storage_->DeletePreview(preview_job_id); *code="preview_expired"; return ESP_ERR_INVALID_STATE; }
    const auto admission=jobs_->CreateOrFind(JobKind::kAiGeneration,request_id,"ai-save:"+preview_job_id,job);
    if (admission!=JobRegistrationResult::kCreated) { xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_)); *code=admission==JobRegistrationResult::kExisting?"ok":"ai_job_busy"; return admission==JobRegistrationResult::kExisting?ESP_OK:ESP_ERR_INVALID_STATE; }
    Work work{}; work.kind=WorkKind::kConfirmSave; std::strncpy(work.job_id,job->job_id.c_str(),sizeof(work.job_id)-1); std::strncpy(work.preview_job_id,preview_job_id.c_str(),sizeof(work.preview_job_id)-1); std::strncpy(work.prompt,preview_.prompt.c_str(),sizeof(work.prompt)-1);
    active_=true; if(xQueueSend(reinterpret_cast<QueueHandle_t>(queue_),&work,0)!=pdTRUE){active_=false;(void)jobs_->Update(job->job_id,JobState::kFailed,"failed",0,"ai_job_busy");xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_));*code="ai_job_busy";return ESP_ERR_INVALID_STATE;} xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_));*code="accepted";return ESP_OK;
}
bool AiGenerationService::GetPreview(const std::string& preview_job_id, PreviewSnapshot* output) { if(!output)return false; xSemaphoreTake(reinterpret_cast<SemaphoreHandle_t>(mutex_),portMAX_DELAY); const std::uint64_t now=static_cast<std::uint64_t>(esp_timer_get_time()/1000); output->job_id=preview_job_id; output->expired=preview_.ready&&preview_.job_id==preview_job_id&&now>=preview_.expires_at_ms; output->ready=preview_.ready&&preview_.job_id==preview_job_id&&!output->expired; output->prompt=output->ready?preview_.prompt:""; output->expires_at_ms=preview_.expires_at_ms; output->source_bytes=preview_.source_bytes; xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_)); return output->ready; }
esp_err_t AiGenerationService::StreamPreview(const std::string& preview_job_id, const std::function<esp_err_t(const void*, std::size_t)>& consume) { PreviewSnapshot preview; if(!GetPreview(preview_job_id,&preview)) return preview.expired?ESP_ERR_INVALID_STATE:ESP_ERR_NOT_FOUND; return storage_->StreamPreviewFile(preview_job_id,consume); }
void AiGenerationService::WorkerEntry(void* context) { static_cast<AiGenerationService*>(context)->WorkerLoop(); }
void AiGenerationService::WorkerLoop() {
    Work work{};
    for (;;) {
        if (xQueueReceive(reinterpret_cast<QueueHandle_t>(queue_), &work, portMAX_DELAY) != pdTRUE) continue;
        std::string endpoint, model, key, url, source_hash, frame_hash, failure = "ai_request_failed";
        const std::string transaction = NewId("ai-txn");
        std::uint64_t source_bytes = 0;
        esp_err_t result = ESP_FAIL;
        bool transaction_started = false;
        MediaId media_id;

        do {
            if (work.kind == WorkKind::kConfirmSave) {
                (void)jobs_->Update(work.job_id, JobState::kRunning, "converting", 35);
                result = storage_->BeginWriteTransaction(NewId("ai-save"), kDisplayFrameBytes + 8192U);
                if (result != ESP_OK) { failure = result == ESP_ERR_NO_MEM ? "storage_no_space" : "storage_busy"; break; }
                transaction_started = true;
                result = ConvertToFrame(storage_, "/sdcard/.ai_preview/" + std::string(work.preview_job_id) + "/source.jpg", &frame_hash);
                if (result != ESP_OK) { failure = result == ESP_ERR_NO_MEM ? "ai_conversion_memory" : "ai_conversion_failed"; break; }
                (void)jobs_->Update(work.job_id, JobState::kRunning, "committing", 80);
                media_id = NewId("ai"); const EpochMs now = static_cast<EpochMs>(esp_timer_get_time() / 1000);
                JsonDocument manifest; manifest["media_id"]=media_id; manifest["display_name"]="AI image"; manifest["category"]="ai"; manifest["created_at_ms"]=now; manifest["updated_at_ms"]=now; manifest["manifest_version"]=1; manifest["revision"]=1; manifest["prompt"]=work.prompt; manifest["model"]=config_->GetPublicConfig().model;
                JsonObject profile=manifest["display_profile"].to<JsonObject>(); profile["width"]=kDisplayWidth; profile["height"]=kDisplayHeight; profile["frame_bytes"]=kDisplayFrameBytes; profile["pixel_format"]="4bpp"; profile["palette"]="six_color_e6"; profile["orientation"]="landscape"; profile["rotation_degrees"]=0; profile["fit_mode"]="contain"; profile["converter_version"]="official-fs-v1";
                JsonObject files=manifest["files"].to<JsonObject>(); JsonObject frame=files["frame"].to<JsonObject>(); frame["present"]=true; frame["mime_type"]="application/octet-stream"; frame["bytes"]=kDisplayFrameBytes; frame["sha256"]=frame_hash;
                std::string manifest_text; serializeJson(manifest,manifest_text); result=storage_->AppendStagedFile("manifest.json",manifest_text.data(),manifest_text.size(),true); if(result==ESP_OK)result=storage_->FinalizeStagedFile("manifest.json",manifest_text.size()); if(result==ESP_OK)result=storage_->CommitTransaction("media/ai/"+media_id); transaction_started=false; if(result!=ESP_OK){failure="ai_commit_failed";break;} if(library_->RegisterCommitted(MediaCategory::kAi,media_id)!=ESP_OK){failure="ai_commit_failed";break;} (void)storage_->DeletePreview(work.preview_job_id); xSemaphoreTake(reinterpret_cast<SemaphoreHandle_t>(mutex_),portMAX_DELAY); preview_.ready=false; xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_)); (void)jobs_->CompleteSuccess(work.job_id,media_id); result=ESP_OK; break;
            }
            if (work.kind == WorkKind::kGeneratePreview) {
                if (!config_->GetSecret(&endpoint, &model, &key)) { failure = "ai_not_configured"; break; }
                (void)jobs_->Update(work.job_id, JobState::kRunning, "requesting", 10);
                ImageUrlRequestResult request_result;
                if (!RequestImageUrl(endpoint, model, key, work.prompt, &url, &request_result)) { failure = request_result.code; break; }
                (void)jobs_->Update(work.job_id, JobState::kRunning, "downloading_preview", 50);
                result = storage_->BeginWriteTransaction(NewId("ai-preview"), kRequiredBytes);
                if (result != ESP_OK) { failure = result == ESP_ERR_NO_MEM ? "storage_no_space" : "storage_busy"; break; }
                transaction_started = true;
                result = DownloadSource(storage_, url, &source_hash, &source_bytes);
                if (result != ESP_OK) { failure = result == ESP_ERR_INVALID_SIZE ? "ai_source_too_large" : "ai_download_failed"; break; }
                result = storage_->CommitTransaction(".ai_preview/" + std::string(work.job_id));
                transaction_started = false;
                if (result != ESP_OK) { failure = "ai_preview_commit_failed"; break; }
                xSemaphoreTake(reinterpret_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
                preview_.job_id=work.job_id; preview_.prompt=work.prompt; preview_.source_bytes=source_bytes; preview_.expires_at_ms=static_cast<std::uint64_t>(esp_timer_get_time()/1000)+30ULL*60ULL*1000ULL; preview_.ready=true;
                xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_));
                (void)jobs_->CompleteSuccess(work.job_id, "");
                result = ESP_OK;
                break;
            }
            if (!config_->GetSecret(&endpoint, &model, &key)) { failure = "ai_not_configured"; break; }
            (void)jobs_->Update(work.job_id, JobState::kRunning, "requesting", 5);
            ImageUrlRequestResult request_result;
            if (!RequestImageUrl(endpoint, model, key, work.prompt, &url, &request_result)) { failure = request_result.code; break; }
            (void)jobs_->Update(work.job_id, JobState::kRunning, "downloading", 25);
            result = storage_->BeginWriteTransaction(transaction, kRequiredBytes);
            if (result != ESP_OK) { failure = result == ESP_ERR_NO_MEM ? "storage_no_space" : "storage_busy"; break; }
            transaction_started = true;
            result = DownloadSource(storage_, url, &source_hash, &source_bytes);
            if (result != ESP_OK) { failure = result == ESP_ERR_INVALID_SIZE ? "ai_source_too_large" : "ai_download_failed"; break; }
            (void)jobs_->Update(work.job_id, JobState::kRunning, "converting", 55);
            result = ConvertToFrame(storage_, "/sdcard/.staging/" + transaction + "/source.jpg", &frame_hash);
            if (result != ESP_OK) { failure = result == ESP_ERR_NO_MEM ? "ai_conversion_memory" : "ai_conversion_failed"; break; }
            (void)jobs_->Update(work.job_id, JobState::kRunning, "committing", 85);
            media_id = NewId("ai");
            const EpochMs now = static_cast<EpochMs>(esp_timer_get_time() / 1000);
            JsonDocument manifest;
            manifest["media_id"] = media_id;
            manifest["display_name"] = "AI image";
            manifest["category"] = "ai";
            manifest["created_at_ms"] = now;
            manifest["updated_at_ms"] = now;
            manifest["manifest_version"] = 1;
            manifest["revision"] = 1;
            JsonObject profile = manifest["display_profile"].to<JsonObject>();
            profile["width"] = kDisplayWidth; profile["height"] = kDisplayHeight; profile["frame_bytes"] = kDisplayFrameBytes;
            profile["pixel_format"] = "4bpp"; profile["palette"] = "six_color_e6"; profile["orientation"] = "landscape";
            profile["rotation_degrees"] = 0; profile["fit_mode"] = "contain"; profile["converter_version"] = "official-fs-v1";
            JsonObject files = manifest["files"].to<JsonObject>();
            JsonObject source = files["source"].to<JsonObject>(); source["present"] = true; source["mime_type"] = "image/jpeg"; source["bytes"] = source_bytes; source["sha256"] = source_hash;
            JsonObject preview = files["preview"].to<JsonObject>(); preview["present"] = false; preview["mime_type"] = ""; preview["bytes"] = 0; preview["sha256"] = "";
            JsonObject frame = files["frame"].to<JsonObject>(); frame["present"] = true; frame["mime_type"] = "application/octet-stream"; frame["bytes"] = kDisplayFrameBytes; frame["sha256"] = frame_hash;
            std::string manifest_text; serializeJson(manifest, manifest_text);
            result = storage_->AppendStagedFile("manifest.json", manifest_text.data(), manifest_text.size(), true);
            if (result == ESP_OK) result = storage_->FinalizeStagedFile("manifest.json", manifest_text.size());
            if (result == ESP_OK) result = storage_->CommitTransaction("media/ai/" + media_id);
            transaction_started = false;
            if (result == ESP_OK) result = library_->RegisterCommitted(MediaCategory::kAi, media_id);
            if (result != ESP_OK) { failure = "ai_commit_failed"; break; }
            (void)jobs_->CompleteSuccess(work.job_id, media_id);
            result = ESP_OK;
        } while (false);
        if (result != ESP_OK) {
            if (transaction_started || storage_->HasActiveWriteTransaction()) (void)storage_->RollbackTransaction();
            (void)jobs_->Update(work.job_id, JobState::kFailed, "failed", 0, failure);
        }
        xSemaphoreTake(reinterpret_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
        active_ = false;
        xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_));
    }
}
}  // namespace photopainter::product
