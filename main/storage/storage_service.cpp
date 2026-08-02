#include "storage_service.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdcard_bsp.h"

namespace photopainter::product {
namespace {

constexpr char kTag[] = "StorageService";
constexpr std::uint64_t kMinimumReserveBytes = 16ULL * 1024ULL * 1024ULL;

bool IsDirectory(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// FAT VFS does not reliably implement access(path, W_OK). Verify write access
// with a bounded, self-cleaning probe instead of reporting a writable card as
// read-only merely because of POSIX permission emulation.
bool CanWriteProbe(const std::string& mount_point) {
    const std::string probe_path = mount_point + "/.storage_health_probe";
    FILE* probe = fopen(probe_path.c_str(), "wb");
    if (probe == nullptr) return false;
    const std::uint8_t marker = 0x5A;
    const bool wrote = fwrite(&marker, 1, 1, probe) == 1 && fflush(probe) == 0;
    const bool closed = fclose(probe) == 0;
    const bool removed = unlink(probe_path.c_str()) == 0;
    return wrote && closed && removed;
}

}  // namespace

StorageService::StorageService(std::string mount_point) : mount_point_(std::move(mount_point)) {
    mutex_ = xSemaphoreCreateMutex();
}

StorageService::~StorageService() {
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
    }
}

esp_err_t StorageService::Initialize(CustomSDPort* sd_port) {
    if (mutex_ == nullptr || sd_port == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    sd_port_ = sd_port;
    snapshot_.state = StorageState::kMounting;

    esp_err_t result = EnsureReadyLocked();
    if (result == ESP_OK) {
        result = EnsureDirectoryTreeLocked(mount_point_ + "/media");
    }
    if (result == ESP_OK) {
        result = EnsureDirectoryTreeLocked(mount_point_ + "/media/local");
    }
    if (result == ESP_OK) {
        result = EnsureDirectoryTreeLocked(mount_point_ + "/media/ai");
    }
    if (result == ESP_OK) {
        result = EnsureDirectoryTreeLocked(mount_point_ + "/.staging");
    }
    if (result == ESP_OK) {
        result = EnsureDirectoryTreeLocked(mount_point_ + "/.ai_preview");
    }
    if (result == ESP_OK) {
        result = EnsureDirectoryTreeLocked(mount_point_ + "/state");
    }
    if (result == ESP_OK) {
        result = EnsureDirectoryTreeLocked(mount_point_ + "/system");
    }
    if (result == ESP_OK) {
        result = CleanupInterruptedTransactionsLocked();
    }
    if (result == ESP_OK) {
        // Fill the cached capacity once during controlled startup. HTTP status
        // handlers only read this cache and never initiate TF I/O.
        result = RefreshSnapshotLocked();
    }
    if (result == ESP_OK) {
        snapshot_.state = StorageState::kReady;
        snapshot_.revision++;
    }
    xSemaphoreGive(mutex_);
    return result;
}

StorageSnapshot StorageService::GetSnapshot() {
    if (mutex_ == nullptr) {
        return snapshot_;
    }
    // A status request must never wait indefinitely for TF/SDMMC I/O.  The
    // cached snapshot is refreshed by controlled storage operations instead.
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        StorageSnapshot unavailable;
        unavailable.state = StorageState::kDegraded;
        unavailable.last_error_code = "storage_busy";
        return unavailable;
    }
    StorageSnapshot snapshot = snapshot_;
    xSemaphoreGive(mutex_);
    return snapshot;
}

esp_err_t StorageService::Remount() {
    if (mutex_ == nullptr) return ESP_ERR_INVALID_STATE;
    // Re-detection is a maintenance operation. Do not queue it behind a long
    // upload/display read, and never disrupt an active writer.
    if (xSemaphoreTake(mutex_, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    if (!active_transaction_id_.empty()) {
        SetErrorLocked("storage_busy");
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = sd_port_ == nullptr ? ESP_ERR_INVALID_STATE : sd_port_->SDPort_Remount();
    if (result == ESP_OK) {
        result = EnsureDirectoryTreeLocked(mount_point_ + "/media");
        if (result == ESP_OK) result = EnsureDirectoryTreeLocked(mount_point_ + "/media/local");
        if (result == ESP_OK) result = EnsureDirectoryTreeLocked(mount_point_ + "/media/ai");
        if (result == ESP_OK) result = EnsureDirectoryTreeLocked(mount_point_ + "/.staging");
        if (result == ESP_OK) result = EnsureDirectoryTreeLocked(mount_point_ + "/state");
        if (result == ESP_OK) result = EnsureDirectoryTreeLocked(mount_point_ + "/system");
    }
    if (result == ESP_OK) result = RefreshSnapshotLocked();
    if (result == ESP_OK) {
        snapshot_.last_error_code.clear();
        snapshot_.last_remount_uptime_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
        ++snapshot_.revision;
    } else {
        snapshot_.state = StorageState::kDegraded;
        SetErrorLocked("storage_unavailable");
    }
    xSemaphoreGive(mutex_);
    return result;
}

esp_err_t StorageService::BeginWriteTransaction(const TransactionId& transaction_id,
                                                std::uint64_t required_bytes) {
    if (mutex_ == nullptr || !IsSafeTransactionId(transaction_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_err_t result = EnsureReadyLocked();
    if (result != ESP_OK) {
        xSemaphoreGive(mutex_);
        return result;
    }
    if (!active_transaction_id_.empty()) {
        SetErrorLocked("storage_busy");
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    RefreshSnapshotLocked();
    const std::uint64_t required_with_reserve = required_bytes + snapshot_.reserve_bytes;
    if (snapshot_.free_bytes < required_with_reserve) {
        SetErrorLocked("storage_no_space");
        xSemaphoreGive(mutex_);
        return ESP_ERR_NO_MEM;
    }
    active_transaction_id_ = transaction_id;
    snapshot_.active_transaction_id = transaction_id;
    result = CreateTransactionDirectoryLocked();
    if (result != ESP_OK) {
        active_transaction_id_.clear();
        snapshot_.active_transaction_id.clear();
    }
    xSemaphoreGive(mutex_);
    return result;
}

esp_err_t StorageService::AppendStagedFile(const std::string& relative_path,
                                           const void* data,
                                           std::size_t data_len,
                                           bool truncate) {
    if (mutex_ == nullptr || data == nullptr || data_len == 0 || !IsSafeRelativePath(relative_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_err_t result = EnsureReadyLocked();
    if (result != ESP_OK || active_transaction_id_.empty()) {
        if (result == ESP_OK) SetErrorLocked("storage_busy");
        xSemaphoreGive(mutex_);
        return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }

    const std::string path = StagingDirectoryLocked() + "/" + relative_path;
    const std::size_t separator = path.find_last_of('/');
    result = EnsureDirectoryTreeLocked(path.substr(0, separator));
    if (result == ESP_OK) {
        FILE* file = fopen(path.c_str(), truncate ? "wb" : "ab");
        if (file == nullptr) {
            SetErrorLocked("storage_write_failed");
            result = ESP_FAIL;
        } else {
            const std::size_t written = fwrite(data, 1, data_len, file);
            const int flush_result = fflush(file);
            fclose(file);
            // Staging files are intentionally not synced per network chunk.
            // FinalizeStagedFile performs the single durability sync before a
            // transaction is eligible for atomic rename into media/.
            if (written != data_len || flush_result != 0) {
                SetErrorLocked("storage_write_failed");
                result = ESP_FAIL;
            }
        }
    }
    xSemaphoreGive(mutex_);
    return result;
}

esp_err_t StorageService::FinalizeStagedFile(const std::string& relative_path,
                                             std::uint64_t expected_bytes) {
    if (mutex_ == nullptr || !IsSafeRelativePath(relative_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (active_transaction_id_.empty()) {
        SetErrorLocked("storage_busy");
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    const std::string path = StagingDirectoryLocked() + "/" + relative_path;
    struct stat info {};
    const bool valid = stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
                       static_cast<std::uint64_t>(info.st_size) == expected_bytes;
    if (!valid) {
        SetErrorLocked("media_incomplete");
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_SIZE;
    }
    // FatFs only guarantees a durable fsync for a writable file handle.  The
    // file is still in the private staging directory at this point, so opening
    // it read/write cannot expose or modify committed media.
    FILE* file = fopen(path.c_str(), "rb+");
    const int sync_result = file == nullptr ? -1 : fsync(fileno(file));
    if (file != nullptr) fclose(file);
    if (sync_result != 0) {
        SetErrorLocked("storage_write_failed");
        xSemaphoreGive(mutex_);
        return ESP_FAIL;
    }
    xSemaphoreGive(mutex_);
    return ESP_OK;
}

esp_err_t StorageService::CommitTransaction(const std::string& final_media_directory) {
    const bool preview_directory = IsSafePreviewDirectory(final_media_directory);
    if (mutex_ == nullptr || (!IsSafeMediaDirectory(final_media_directory) && !preview_directory)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (active_transaction_id_.empty()) {
        SetErrorLocked("storage_busy");
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    const MediaId media_id = preview_directory ? MediaId{} : MediaIdFromCategorizedDirectory(final_media_directory);
    if (!preview_directory && (media_id.empty() || CommittedMediaIdExistsLocked(media_id))) {
        ESP_LOGW(kTag, "rejecting globally duplicated media id: %s", media_id.c_str());
        SetErrorLocked("media_id_conflict");
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    const std::string destination = mount_point_ + "/" + final_media_directory;
    struct stat destination_info {};
    if (stat(destination.c_str(), &destination_info) == 0) {
        SetErrorLocked("request_id_conflict");
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    const std::size_t separator = destination.find_last_of('/');
    esp_err_t result = EnsureDirectoryTreeLocked(destination.substr(0, separator));
    if (result == ESP_OK && rename(StagingDirectoryLocked().c_str(), destination.c_str()) != 0) {
        ESP_LOGE(kTag, "atomic commit failed: %s", strerror(errno));
        SetErrorLocked("storage_write_failed");
        result = ESP_FAIL;
    }
    if (result == ESP_OK) {
        active_transaction_id_.clear();
        snapshot_.active_transaction_id.clear();
        snapshot_.last_error_code.clear();
        snapshot_.revision++;
        // The rename has already committed the media.  Refreshing the cache is
        // best-effort: reporting a later capacity-query failure as a failed
        // commit would cause callers to retry an already-visible media item.
        (void)RefreshSnapshotLocked();
    }
    xSemaphoreGive(mutex_);
    return result;
}

esp_err_t StorageService::RollbackTransaction() {
    if (mutex_ == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    if (!active_transaction_id_.empty()) {
        result = RemoveDirectoryTreeLocked(StagingDirectoryLocked());
        if (result == ESP_OK) {
            active_transaction_id_.clear();
            snapshot_.active_transaction_id.clear();
            (void)RefreshSnapshotLocked();
        }
    }
    xSemaphoreGive(mutex_);
    return result;
}

esp_err_t StorageService::CleanupInterruptedTransactions() {
    if (mutex_ == nullptr) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const esp_err_t result = CleanupInterruptedTransactionsLocked();
    xSemaphoreGive(mutex_);
    return result;
}

esp_err_t StorageService::CleanupInterruptedTransactionsLocked() {
    if (active_transaction_id_.empty()) {
        const std::string staging_root = mount_point_ + "/.staging";
        DIR* directory = opendir(staging_root.c_str());
        if (directory == nullptr) return ESP_FAIL;
        struct dirent* entry = nullptr;
        while ((entry = readdir(directory)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            const std::string candidate = staging_root + "/" + entry->d_name;
            if (IsDirectory(candidate)) RemoveDirectoryTreeLocked(candidate);
        }
        closedir(directory);
    }
    return ESP_OK;
}

esp_err_t StorageService::ReadCommittedFile(const std::string& relative_path,
                                            void* buffer,
                                            std::size_t expected_bytes) {
    if (mutex_ == nullptr || buffer == nullptr || expected_bytes == 0 ||
        !IsSafeRelativePath(relative_path) || relative_path.rfind("media/", 0) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_err_t result = EnsureReadyLocked();
    if (result != ESP_OK) {
        xSemaphoreGive(mutex_);
        return result;
    }
    const std::string path = mount_point_ + "/" + relative_path;
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        SetErrorLocked("storage_read_failed");
        xSemaphoreGive(mutex_);
        return ESP_ERR_NOT_FOUND;
    }
    const std::size_t read = fread(buffer, 1, expected_bytes, file);
    const int trailing = fgetc(file);
    fclose(file);
    if (read != expected_bytes || trailing != EOF) {
        SetErrorLocked("media_incomplete");
        xSemaphoreGive(mutex_);
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreGive(mutex_);
    return ESP_OK;
}

esp_err_t StorageService::StreamCommittedFile(
    const std::string& relative_path,
    std::uint64_t expected_bytes,
    const std::function<esp_err_t(const void*, std::size_t)>& consume) {
    if (mutex_ == nullptr || expected_bytes == 0 || !consume ||
        !IsSafeRelativePath(relative_path) || relative_path.rfind("media/", 0) != 0) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_err_t result = EnsureReadyLocked();
    if (result != ESP_OK) { xSemaphoreGive(mutex_); return result; }
    const std::string path = mount_point_ + "/" + relative_path;
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        SetErrorLocked("storage_read_failed");
        xSemaphoreGive(mutex_);
        return ESP_ERR_NOT_FOUND;
    }
    std::uint64_t size = 0;
    struct stat info {};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) result = ESP_ERR_INVALID_STATE;
    else size = static_cast<std::uint64_t>(info.st_size);
    if (result != ESP_OK || size != expected_bytes) {
        fclose(file);
        SetErrorLocked("media_incomplete");
        xSemaphoreGive(mutex_);
        return result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
    }
    char buffer[2048];
    std::uint64_t sent = 0;
    while (sent < expected_bytes) {
        const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(buffer), expected_bytes - sent));
        const std::size_t read = fread(buffer, 1, wanted, file);
        if (read != wanted) { result = ESP_FAIL; break; }
        result = consume(buffer, read);
        if (result != ESP_OK) break;
        sent += read;
    }
    fclose(file);
    if (result != ESP_OK) SetErrorLocked("storage_read_failed");
    xSemaphoreGive(mutex_);
    return result;
}

esp_err_t StorageService::StreamPreviewFile(
    const std::string& job_id,
    const std::function<esp_err_t(const void*, std::size_t)>& consume) {
    if (mutex_ == nullptr || !IsSafeTransactionId(job_id) || !consume) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_err_t result = EnsureReadyLocked();
    const std::string path = mount_point_ + "/.ai_preview/" + job_id + "/source.jpg";
    if (result == ESP_OK) {
        FILE* file = fopen(path.c_str(), "rb");
        if (file == nullptr) result = ESP_ERR_NOT_FOUND;
        std::uint8_t buffer[4096];
        while (result == ESP_OK && file != nullptr) {
            const std::size_t count = fread(buffer, 1, sizeof(buffer), file);
            if (count > 0) result = consume(buffer, count);
            if (count < sizeof(buffer)) { if (ferror(file)) result = ESP_FAIL; break; }
        }
        if (file != nullptr) fclose(file);
    }
    xSemaphoreGive(mutex_);
    return result;
}

esp_err_t StorageService::DeletePreview(const std::string& job_id) {
    if (mutex_ == nullptr || !IsSafeTransactionId(job_id)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_err_t result = EnsureReadyLocked();
    if (result == ESP_OK) result = RemoveDirectoryTreeLocked(mount_point_ + "/.ai_preview/" + job_id);
    xSemaphoreGive(mutex_);
    return result;
}

esp_err_t StorageService::ReadCommittedText(const std::string& relative_path,
                                            std::size_t maximum_bytes,
                                            std::string* output) {
    if (output == nullptr || maximum_bytes == 0) return ESP_ERR_INVALID_ARG;
    std::uint64_t bytes = 0;
    esp_err_t result = GetCommittedFileSize(relative_path, &bytes);
    if (result != ESP_OK || bytes > maximum_bytes) return result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
    output->assign(static_cast<std::size_t>(bytes), '\0');
    if (bytes == 0) return ESP_OK;
    return ReadCommittedFile(relative_path, output->data(), static_cast<std::size_t>(bytes));
}

esp_err_t StorageService::GetCommittedFileSize(const std::string& relative_path,
                                               std::uint64_t* output_bytes) {
    if (mutex_ == nullptr || output_bytes == nullptr || !IsSafeRelativePath(relative_path) ||
        relative_path.rfind("media/", 0) != 0) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_err_t result = EnsureReadyLocked();
    struct stat info {};
    if (result == ESP_OK && (stat((mount_point_ + "/" + relative_path).c_str(), &info) != 0 || !S_ISREG(info.st_mode))) {
        SetErrorLocked("storage_read_failed");
        result = ESP_ERR_NOT_FOUND;
    }
    if (result == ESP_OK) *output_bytes = static_cast<std::uint64_t>(info.st_size);
    xSemaphoreGive(mutex_);
    return result;
}

esp_err_t StorageService::ListCommittedMedia(std::vector<CommittedMediaLocation>* output_locations) {
    if (mutex_ == nullptr || output_locations == nullptr) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_err_t result = EnsureReadyLocked();
    if (result == ESP_OK) {
        output_locations->clear();
        const auto scan_category = [this, output_locations](MediaCategory category,
                                                            const char* name) -> esp_err_t {
            const std::string relative_root = std::string("media/") + name;
            DIR* directory = opendir((mount_point_ + "/" + relative_root).c_str());
            if (directory == nullptr) return ESP_FAIL;
            struct dirent* entry = nullptr;
            while ((entry = readdir(directory)) != nullptr) {
                const std::string id = entry->d_name;
                if (id == "." || id == ".." || !IsSafeTransactionId(id) ||
                    !IsDirectory(mount_point_ + "/" + relative_root + "/" + id)) continue;
                output_locations->push_back({category, id, relative_root + "/" + id});
            }
            closedir(directory);
            return ESP_OK;
        };
        result = scan_category(MediaCategory::kLocal, "local");
        if (result == ESP_OK) result = scan_category(MediaCategory::kAi, "ai");
        if (result == ESP_OK) {
            // Legacy product builds stored local items directly below media/.
            // Keep them readable in place; new writes never use this layout.
            DIR* directory = opendir((mount_point_ + "/media").c_str());
            if (directory == nullptr) {
                result = ESP_FAIL;
            } else {
                struct dirent* entry = nullptr;
                while ((entry = readdir(directory)) != nullptr) {
                    const std::string id = entry->d_name;
                    if (id == "." || id == ".." || id == "local" || id == "ai" ||
                        !IsSafeTransactionId(id) || !IsDirectory(mount_point_ + "/media/" + id)) continue;
                    output_locations->push_back({MediaCategory::kLocal, id, "media/" + id});
                }
                closedir(directory);
            }
        }
        if (result != ESP_OK) {
            SetErrorLocked("storage_read_failed");
        }
    }
    xSemaphoreGive(mutex_);
    return result;
}

DeleteCommittedMediaResult StorageService::DeleteCommittedMedia(const std::string& relative_directory) {
    DeleteCommittedMediaResult outcome;
    if (mutex_ == nullptr || !IsSafeCommittedMediaDirectory(relative_directory)) {
        outcome.error = ESP_ERR_INVALID_ARG;
        return outcome;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    esp_err_t result = EnsureReadyLocked();
    if (result != ESP_OK) {
        outcome.error = result;
        xSemaphoreGive(mutex_);
        return outcome;
    }
    if (!active_transaction_id_.empty()) {
        SetErrorLocked("storage_busy");
        outcome.error = ESP_ERR_INVALID_STATE;
        xSemaphoreGive(mutex_);
        return outcome;
    }
    const std::string path = mount_point_ + "/" + relative_directory;
    if (!IsDirectory(path)) {
        outcome.error = ESP_ERR_NOT_FOUND;
        xSemaphoreGive(mutex_);
        return outcome;
    }
    outcome.mutation = DeleteMutationState::kMayHaveMutated;
    result = RemoveDirectoryTreeLocked(path);
    if (result == ESP_OK) {
        outcome.mutation = DeleteMutationState::kRemoved;
        snapshot_.revision++;
        (void)RefreshSnapshotLocked();
    } else {
        SetErrorLocked("storage_delete_failed");
    }
    outcome.error = result;
    xSemaphoreGive(mutex_);
    return outcome;
}

bool StorageService::IsReady() const {
    return snapshot_.state == StorageState::kReady;
}

bool StorageService::HasActiveWriteTransaction() const {
    return !active_transaction_id_.empty();
}

esp_err_t StorageService::EnsureReadyLocked() {
    if (sd_port_ == nullptr || sd_port_->SDPort_GetSdcardInitOK() == 0 ||
        sd_port_->SDPort_GetSdMMCHost() == nullptr ||
        sdmmc_get_status(sd_port_->SDPort_GetSdMMCHost()) != ESP_OK) {
        snapshot_.state = StorageState::kMissing;
        snapshot_.mounted = false;
        snapshot_.readable = false;
        snapshot_.writable = false;
        snapshot_.checked_at_uptime_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
        SetErrorLocked("storage_unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    snapshot_.state = StorageState::kReady;
    snapshot_.mounted = true;
    return ESP_OK;
}

esp_err_t StorageService::RefreshSnapshotLocked() {
    esp_err_t result = EnsureReadyLocked();
    if (result != ESP_OK) return result;
    result = esp_vfs_fat_info(mount_point_.c_str(), &snapshot_.total_bytes, &snapshot_.free_bytes);
    if (result != ESP_OK) {
        snapshot_.mounted = false;
        snapshot_.readable = false;
        snapshot_.writable = false;
        SetErrorLocked("storage_unavailable");
        return result;
    }
    snapshot_.reserve_bytes = std::max(kMinimumReserveBytes, snapshot_.total_bytes / 20U);
    snapshot_.readable = access((mount_point_ + "/media").c_str(), R_OK) == 0;
    snapshot_.writable = CanWriteProbe(mount_point_);
    result = RefreshUsageLocked();
    snapshot_.checked_at_uptime_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
    if (result != ESP_OK) {
        snapshot_.state = StorageState::kDegraded;
        SetErrorLocked("storage_read_failed");
    }
    return result;
}

esp_err_t StorageService::MeasureDirectoryTreeLocked(const std::string& absolute_path,
                                                      std::uint64_t* output_bytes) const {
    if (output_bytes == nullptr) return ESP_ERR_INVALID_ARG;
    DIR* directory = opendir(absolute_path.c_str());
    if (directory == nullptr) return ESP_FAIL;
    std::uint64_t bytes = 0;
    struct dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        const std::string child = absolute_path + "/" + entry->d_name;
        struct stat info {};
        if (stat(child.c_str(), &info) != 0) { closedir(directory); return ESP_FAIL; }
        if (S_ISDIR(info.st_mode)) {
            std::uint64_t child_bytes = 0;
            if (MeasureDirectoryTreeLocked(child, &child_bytes) != ESP_OK) { closedir(directory); return ESP_FAIL; }
            bytes += child_bytes;
        } else if (S_ISREG(info.st_mode)) {
            bytes += static_cast<std::uint64_t>(info.st_size);
        }
    }
    closedir(directory);
    *output_bytes = bytes;
    return ESP_OK;
}

esp_err_t StorageService::RefreshUsageLocked() {
    snapshot_.local_media_count = 0;
    snapshot_.local_media_bytes = 0;
    snapshot_.ai_media_count = 0;
    snapshot_.ai_media_bytes = 0;
    snapshot_.staging_count = 0;
    snapshot_.staging_bytes = 0;
    const auto scan_media = [this](const std::string& root, MediaCategory category) -> esp_err_t {
        DIR* directory = opendir(root.c_str());
        if (directory == nullptr) return ESP_FAIL;
        struct dirent* entry = nullptr;
        while ((entry = readdir(directory)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            if (!IsSafeTransactionId(entry->d_name)) continue;
            const std::string child = root + "/" + entry->d_name;
            if (!IsDirectory(child)) continue;
            std::uint64_t bytes = 0;
            if (MeasureDirectoryTreeLocked(child, &bytes) != ESP_OK) { closedir(directory); return ESP_FAIL; }
            if (category == MediaCategory::kLocal) {
                ++snapshot_.local_media_count;
                snapshot_.local_media_bytes += bytes;
            } else {
                ++snapshot_.ai_media_count;
                snapshot_.ai_media_bytes += bytes;
            }
        }
        closedir(directory);
        return ESP_OK;
    };
    esp_err_t result = scan_media(mount_point_ + "/media/local", MediaCategory::kLocal);
    if (result == ESP_OK) result = scan_media(mount_point_ + "/media/ai", MediaCategory::kAi);
    if (result == ESP_OK) {
        // Count legacy flat directories as local without counting the new
        // category roots themselves.
        DIR* directory = opendir((mount_point_ + "/media").c_str());
        if (directory == nullptr) {
            result = ESP_FAIL;
        } else {
            struct dirent* entry = nullptr;
            while ((entry = readdir(directory)) != nullptr) {
                const std::string id = entry->d_name;
                if (id == "." || id == ".." || id == "local" || id == "ai" ||
                    !IsSafeTransactionId(id)) continue;
                const std::string child = mount_point_ + "/media/" + id;
                if (!IsDirectory(child)) continue;
                std::uint64_t bytes = 0;
                if (MeasureDirectoryTreeLocked(child, &bytes) != ESP_OK) { result = ESP_FAIL; break; }
                ++snapshot_.local_media_count;
                snapshot_.local_media_bytes += bytes;
            }
            closedir(directory);
        }
    }
    if (result == ESP_OK) {
        DIR* directory = opendir((mount_point_ + "/.staging").c_str());
        if (directory == nullptr) return ESP_FAIL;
        struct dirent* entry = nullptr;
        while ((entry = readdir(directory)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            const std::string child = mount_point_ + "/.staging/" + entry->d_name;
            if (!IsDirectory(child)) continue;
            std::uint64_t bytes = 0;
            if (MeasureDirectoryTreeLocked(child, &bytes) != ESP_OK) { result = ESP_FAIL; break; }
            ++snapshot_.staging_count;
            snapshot_.staging_bytes += bytes;
        }
        closedir(directory);
    }
    return result;
}

esp_err_t StorageService::CreateTransactionDirectoryLocked() {
    const std::string directory = StagingDirectoryLocked();
    if (IsDirectory(directory)) {
        SetErrorLocked("request_id_conflict");
        return ESP_ERR_INVALID_STATE;
    }
    return EnsureDirectoryTreeLocked(directory);
}

esp_err_t StorageService::RemoveDirectoryTreeLocked(const std::string& absolute_path) {
    DIR* directory = opendir(absolute_path.c_str());
    if (directory == nullptr) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    struct dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        const std::string child = absolute_path + "/" + entry->d_name;
        if (IsDirectory(child)) {
            if (RemoveDirectoryTreeLocked(child) != ESP_OK) {
                closedir(directory);
                return ESP_FAIL;
            }
        } else if (unlink(child.c_str()) != 0) {
            closedir(directory);
            return ESP_FAIL;
        }
    }
    closedir(directory);
    return rmdir(absolute_path.c_str()) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t StorageService::EnsureDirectoryTreeLocked(const std::string& absolute_path) {
    if (absolute_path.empty() || absolute_path.rfind(mount_point_, 0) != 0) return ESP_ERR_INVALID_ARG;
    std::string current;
    std::size_t start = 0;
    while (start < absolute_path.size()) {
        const std::size_t end = absolute_path.find('/', start + 1);
        current = absolute_path.substr(0, end == std::string::npos ? absolute_path.size() : end);
        if (!current.empty() && !IsDirectory(current) && mkdir(current.c_str(), 0775) != 0 && errno != EEXIST) {
            SetErrorLocked("storage_write_failed");
            return ESP_FAIL;
        }
        if (end == std::string::npos) break;
        start = end;
    }
    return ESP_OK;
}

bool StorageService::IsSafeRelativePath(const std::string& path) const {
    return !path.empty() && path.front() != '/' && path.find("..") == std::string::npos &&
           path.find('\\') == std::string::npos;
}

bool StorageService::IsSafeTransactionId(const TransactionId& transaction_id) const {
    if (transaction_id.empty() || transaction_id.size() > 64) return false;
    return std::all_of(transaction_id.begin(), transaction_id.end(), [](unsigned char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9') || value == '-' || value == '_';
    });
}

bool StorageService::IsSafeMediaDirectory(const std::string& final_media_directory) const {
    constexpr char kLocalPrefix[] = "media/local/";
    constexpr char kAiPrefix[] = "media/ai/";
    const char* prefix = final_media_directory.rfind(kLocalPrefix, 0) == 0 ? kLocalPrefix :
        (final_media_directory.rfind(kAiPrefix, 0) == 0 ? kAiPrefix : nullptr);
    if (prefix == nullptr) return false;
    return IsSafeTransactionId(final_media_directory.substr(std::strlen(prefix)));
}

bool StorageService::IsSafePreviewDirectory(const std::string& relative_directory) const {
    constexpr char kPrefix[] = ".ai_preview/";
    if (relative_directory.rfind(kPrefix, 0) != 0) return false;
    return IsSafeTransactionId(relative_directory.substr(sizeof(kPrefix) - 1));
}

bool StorageService::IsSafeCommittedMediaDirectory(const std::string& relative_directory) const {
    if (IsSafeMediaDirectory(relative_directory)) return true;
    constexpr char kLegacyPrefix[] = "media/";
    if (relative_directory.rfind(kLegacyPrefix, 0) != 0) return false;
    const std::string media_id = relative_directory.substr(sizeof(kLegacyPrefix) - 1);
    return media_id != "local" && media_id != "ai" && IsSafeTransactionId(media_id);
}

MediaId StorageService::MediaIdFromCategorizedDirectory(const std::string& relative_directory) const {
    constexpr char kLocalPrefix[] = "media/local/";
    constexpr char kAiPrefix[] = "media/ai/";
    if (relative_directory.rfind(kLocalPrefix, 0) == 0) {
        return relative_directory.substr(sizeof(kLocalPrefix) - 1U);
    }
    if (relative_directory.rfind(kAiPrefix, 0) == 0) {
        return relative_directory.substr(sizeof(kAiPrefix) - 1U);
    }
    return {};
}

bool StorageService::CommittedMediaIdExistsLocked(const MediaId& media_id) const {
    if (!IsSafeTransactionId(media_id)) return true;
    return IsDirectory(mount_point_ + "/media/local/" + media_id) ||
           IsDirectory(mount_point_ + "/media/ai/" + media_id) ||
           IsDirectory(mount_point_ + "/media/" + media_id);
}

std::string StorageService::StagingDirectoryLocked() const {
    return mount_point_ + "/.staging/" + active_transaction_id_;
}

void StorageService::SetErrorLocked(const char* code) {
    snapshot_.last_error_code = code;
}

}  // namespace photopainter::product
