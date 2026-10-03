#ifndef EVO_FILE_TRANSFER_SERVICE_HPP
#define EVO_FILE_TRANSFER_SERVICE_HPP

#include "evo/interfaces/IFileTransferService.hpp"
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <pthread.h>

namespace evo {

class FileTransferService : public IFileTransferService {
public:
    FileTransferService();
    ~FileTransferService() override;

    // Clipboard
    void copyToClipboard(const std::string& path, const std::string& name, bool isDir) override;
    void cutToClipboard(const std::string& path, const std::string& name, bool isDir) override;
    const ClipboardItem& getClipboard() const override { return m_clipboard; }
    bool hasClipboardItem() const override { return m_clipboard.action != ClipboardAction::None && !m_clipboard.path.empty(); }
    void clearClipboard() override;

    // Asynchronous transfer triggers
    bool startCopy(const std::string& srcPath, const std::string& dstDir) override;
    bool startMove(const std::string& srcPath, const std::string& dstDir) override;
    bool startDelete(const std::string& path, bool isDir) override;
    bool startPaste(const std::string& targetDir) override;

    // Synchronous immediate operations
    bool renameItem(const std::string& oldPath, const std::string& newName) override;
    bool createDirectory(const std::string& parentDir, const std::string& folderName) override;

    // Progress & Control
    bool isBusy() const override;
    TransferProgress getProgress() const override;
    void cancelTransfer() override;
    void resetStatus() override;

    void setCollisionAction(CollisionAction action) override { m_collisionAction = action; }
    CollisionAction getCollisionAction() const override { return m_collisionAction; }

    /* FtpClient progress hook - public only so the C-style callback can reach
     * it. Returns false to cancel the transfer in progress. */
    bool onFtpChunk(uint32_t chunkBytes);

private:
    struct FileEntry {
        std::string relPath;
        uint64_t size = 0;
        bool isDir = false;
        uint32_t mode = 0;
        time_t mtime = 0;
        time_t atime = 0;
    };

    static void* WorkerThreadEntry(void* arg);
    void runTransfer();

    // Internal operations
    bool executeCopy(const std::string& src, const std::string& dst);
    bool executeMove(const std::string& src, const std::string& dst);
    bool executeDelete(const std::string& path, bool isDir);

    // Fast copy implementation (ported from Ultracopier essentials)
    bool copySingleFile(const std::string& src, const std::string& dst, uint64_t fileSize, uint32_t mode, time_t mtime, time_t atime);
    bool copyInProcess(const std::string& src, const std::string& finalDst, uint64_t fileSize, uint32_t mode);
    bool copyViaLoopbackFtp(const std::string& src, const std::string& finalDst);
    bool loopbackFtpUsable();
    bool shouldDelegateToFtp(uint64_t fileSize) const;
    void accountBytes(uint64_t bytes);
    bool scanTree(const std::string& rootPath, std::vector<FileEntry>& outEntries, uint64_t& outTotalBytes);
    bool ensureDirectory(const std::string& dirPath);
    bool recursiveDelete(const std::string& path);
    std::string resolveDestinationCollision(const std::string& dstPath, bool isDir);

    ClipboardItem m_clipboard;
    CollisionAction m_collisionAction = CollisionAction::AutoRename;

    // Worker thread management
    pthread_t m_workerThread = 0;
    std::atomic<bool> m_isBusy{false};
    std::atomic<bool> m_cancelRequested{false};

    // Queued operation parameters
    FileOpType m_activeOp = FileOpType::None;
    std::string m_activeSrc;
    std::string m_activeDst;
    bool m_activeIsDir = false;

    // Progress tracking
    mutable std::mutex m_progressMutex;
    TransferProgress m_progress;

    // Speed estimation
    uint64_t m_lastSampleTimeMs = 0;
    uint64_t m_lastSampleBytes = 0;
    double m_smoothedSpeed = 0.0;

    // Directory creation cache (Ultracopier MkPath optimization)
    std::unordered_set<std::string> m_createdDirs;
    std::string m_currentDestinationPartialFile;

    /*
     * The app process is write-throttled after ~1152 MB (measured, see the
     * comment on kProcessWriteBudget in the .cpp), so big copies are handed to
     * the console's own FTP daemon instead. These track how much we have
     * written ourselves since launch and whether the daemon answered.
     */
    uint64_t m_processBytesWritten = 0;
    int      m_ftpProbe = 0;   /* 0 untried, 1 usable, -1 unavailable */

    std::vector<char> m_copyBuf;   /* one in-process copy buffer, not one per file */
};

} // namespace evo

#endif // EVO_FILE_TRANSFER_SERVICE_HPP
