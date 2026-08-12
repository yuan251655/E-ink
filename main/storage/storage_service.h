#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "product_types.h"

class CustomSDPort;

namespace photopainter::product {

enum class DeleteMutationState : std::uint8_t {
    kNotStarted,
    kMayHaveMutated,
    kRemoved,
};

struct DeleteCommittedMediaResult {
    esp_err_t error = ESP_FAIL;
    DeleteMutationState mutation = DeleteMutationState::kNotStarted;
};

// Product-layer TF adapter. It borrows the already-mounted official SDMMC BSP;
// it never owns the card lifecycle or changes board pin configuration.
class StorageService {
public:
    explicit StorageService(std::string mount_point = "/sdcard");
    ~StorageService();

    StorageService(const StorageService&) = delete;
    StorageService& operator=(const StorageService&) = delete;

    esp_err_t Initialize(CustomSDPort* sd_port);
    StorageSnapshot GetSnapshot();
    // Safely remounts using the official board configuration. It refuses while
    // a storage transaction or another operation owns the service lock.
    esp_err_t Remount();

    // A transaction serializes one product-layer writer. Callers stream files
    // into .staging/<transaction_id>/ and commit only after higher layers have
    // validated the media manifest and the six-colour frame.
    esp_err_t BeginWriteTransaction(const TransactionId& transaction_id,
                                    std::uint64_t required_bytes);
    esp_err_t AppendStagedFile(const std::string& relative_path,
                               const void* data,
                               std::size_t data_len,
                               bool truncate);
    esp_err_t FinalizeStagedFile(const std::string& relative_path,
                                 std::uint64_t expected_bytes);
    esp_err_t CommitTransaction(const std::string& final_media_directory);
    esp_err_t RollbackTransaction();
    esp_err_t CleanupInterruptedTransactions();

    // Reads an already committed file while holding the product-layer TF lock.
    // DisplayService uses this to load a frame, then releases TF before the
    // approximately 25-second physical e-paper refresh begins.
    esp_err_t ReadCommittedFile(const std::string& relative_path,
                                void* buffer,
                                std::size_t expected_bytes);
    // Streams an already committed file under the TF lock using a bounded
    // buffer. The callback must not retain the supplied memory.
    esp_err_t StreamCommittedFile(const std::string& relative_path,
                                  std::uint64_t expected_bytes,
                                  const std::function<esp_err_t(const void*, std::size_t)>& consume);
    // AI previews are deliberately outside media/. They may be streamed only
    // by their internally-generated job id and are removed after confirmation
    // or expiry.
    esp_err_t StreamPreviewFile(const std::string& job_id,
                                const std::function<esp_err_t(const void*, std::size_t)>& consume);
    esp_err_t GetPreviewFileSize(const std::string& job_id, std::uint64_t* output_bytes);
    bool PreviewFileExists(const std::string& job_id);
    esp_err_t DeletePreview(const std::string& job_id);
    esp_err_t ReadCommittedText(const std::string& relative_path,
                                std::size_t maximum_bytes,
                                std::string* output);
    // Small device-state documents live under /state. Names are validated and
    // writes use a same-volume temporary file followed by atomic rename.
    esp_err_t ReadStateText(const std::string& name, std::size_t maximum_bytes,
                            std::string* output);
    esp_err_t WriteStateTextAtomic(const std::string& name, const std::string& content);
    esp_err_t WriteStateBlobAtomic(const std::string& name, const void* data, std::size_t bytes);
    esp_err_t GetCommittedFileSize(const std::string& relative_path,
                                   std::uint64_t* output_bytes);
    // Enumerates categorized media plus legacy flat media/<id> entries. New
    // writes use media/local/<id> or media/ai/<id>; legacy entries are exposed
    // as local without moving or rewriting user data.
    esp_err_t ListCommittedMedia(std::vector<CommittedMediaLocation>* output_locations);
    // Permanently removes one committed media directory. It never accepts a
    // caller-provided TF path and refuses while an upload transaction is live.
    DeleteCommittedMediaResult DeleteCommittedMedia(const std::string& relative_directory);

    bool IsReady() const;
    bool HasActiveWriteTransaction() const;

private:
    esp_err_t EnsureReadyLocked();
    esp_err_t RefreshSnapshotLocked();
    esp_err_t RefreshUsageLocked();
    esp_err_t MeasureDirectoryTreeLocked(const std::string& absolute_path,
                                         std::uint64_t* output_bytes) const;
    esp_err_t CreateTransactionDirectoryLocked();
    esp_err_t CleanupInterruptedTransactionsLocked();
    // Device previews are RAM-owned and expire on reboot. Remove abandoned
    // directories so rebooted job ids can never collide with stale previews.
    esp_err_t CleanupAiPreviewsLocked();
    esp_err_t RemoveDirectoryTreeLocked(const std::string& absolute_path);
    esp_err_t EnsureDirectoryTreeLocked(const std::string& absolute_path);
    bool IsSafeRelativePath(const std::string& path) const;
    bool IsSafeStateName(const std::string& name) const;
    bool IsSafeTransactionId(const TransactionId& transaction_id) const;
    bool IsSafeMediaDirectory(const std::string& final_media_directory) const;
    bool IsSafePreviewDirectory(const std::string& relative_directory) const;
    bool IsSafeCommittedMediaDirectory(const std::string& relative_directory) const;
    bool CommittedMediaIdExistsLocked(const MediaId& media_id) const;
    MediaId MediaIdFromCategorizedDirectory(const std::string& relative_directory) const;
    std::string StagingDirectoryLocked() const;
    void SetErrorLocked(const char* code);

    CustomSDPort* sd_port_ = nullptr;
    std::string mount_point_;
    StorageSnapshot snapshot_;
    TransactionId active_transaction_id_;
    SemaphoreHandle_t mutex_ = nullptr;
};

}  // namespace photopainter::product
