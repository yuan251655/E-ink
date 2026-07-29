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
    esp_err_t ReadCommittedText(const std::string& relative_path,
                                std::size_t maximum_bytes,
                                std::string* output);
    esp_err_t GetCommittedFileSize(const std::string& relative_path,
                                   std::uint64_t* output_bytes);
    esp_err_t ListCommittedMediaIds(std::vector<MediaId>* output_media_ids);
    // Permanently removes one committed media directory. It never accepts a
    // caller-provided TF path and refuses while an upload transaction is live.
    esp_err_t DeleteCommittedMedia(const MediaId& media_id);

    bool IsReady() const;
    bool HasActiveWriteTransaction() const;

private:
    esp_err_t EnsureReadyLocked();
    esp_err_t RefreshSnapshotLocked();
    esp_err_t CreateTransactionDirectoryLocked();
    esp_err_t CleanupInterruptedTransactionsLocked();
    esp_err_t RemoveDirectoryTreeLocked(const std::string& absolute_path);
    esp_err_t EnsureDirectoryTreeLocked(const std::string& absolute_path);
    bool IsSafeRelativePath(const std::string& path) const;
    bool IsSafeTransactionId(const TransactionId& transaction_id) const;
    bool IsSafeMediaDirectory(const std::string& final_media_directory) const;
    std::string StagingDirectoryLocked() const;
    void SetErrorLocked(const char* code);

    CustomSDPort* sd_port_ = nullptr;
    std::string mount_point_;
    StorageSnapshot snapshot_;
    TransactionId active_transaction_id_;
    SemaphoreHandle_t mutex_ = nullptr;
};

}  // namespace photopainter::product
