#ifndef EVO_I_FILE_TRANSFER_SERVICE_HPP
#define EVO_I_FILE_TRANSFER_SERVICE_HPP

#include <string>
#include <cstdint>

namespace evo {

enum class FileOpType {
    None,
    Copy,
    Move,
    Delete,
    Rename,
    CreateFolder
};

enum class TransferStatus {
    Idle,
    Scanning,
    Transferring,
    Completed,
    Failed,
    Cancelled
};

enum class CollisionAction {
    Overwrite,
    Skip,
    AutoRename
};

struct TransferProgress {
    FileOpType opType = FileOpType::None;
    TransferStatus status = TransferStatus::Idle;
    std::string currentItemName;
    std::string currentSourcePath;
    std::string currentDestPath;
    uint64_t bytesTransferred = 0;
    uint64_t totalBytes = 0;
    uint32_t filesTransferred = 0;
    uint32_t totalFiles = 0;
    double currentSpeedBps = 0.0;
    double progressPercent = 0.0;
    int estimatedSecondsRemaining = 0;
    std::string errorMessage;
};

enum class ClipboardAction {
    None,
    Copy,
    Cut
};

struct ClipboardItem {
    std::string path;
    std::string name;
    bool isDirectory = false;
    ClipboardAction action = ClipboardAction::None;
};

class IFileTransferService {
public:
    virtual ~IFileTransferService() = default;

    // Clipboard management
    virtual void copyToClipboard(const std::string& path, const std::string& name, bool isDir) = 0;
    virtual void cutToClipboard(const std::string& path, const std::string& name, bool isDir) = 0;
    virtual const ClipboardItem& getClipboard() const = 0;
    virtual bool hasClipboardItem() const = 0;
    virtual void clearClipboard() = 0;

    // Asynchronous transfer operations (runs on background worker thread)
    virtual bool startCopy(const std::string& srcPath, const std::string& dstDir) = 0;
    virtual bool startMove(const std::string& srcPath, const std::string& dstDir) = 0;
    virtual bool startDelete(const std::string& path, bool isDir) = 0;
    virtual bool startPaste(const std::string& targetDir) = 0;

    // Synchronous immediate operations
    virtual bool renameItem(const std::string& oldPath, const std::string& newName) = 0;
    virtual bool createDirectory(const std::string& parentDir, const std::string& folderName) = 0;

    // Progress & Control
    virtual bool isBusy() const = 0;
    virtual TransferProgress getProgress() const = 0;
    virtual void cancelTransfer() = 0;
    virtual void resetStatus() = 0;

    // Configuration / Policy
    virtual void setCollisionAction(CollisionAction action) = 0;
    virtual CollisionAction getCollisionAction() const = 0;
};

} // namespace evo

#endif // EVO_I_FILE_TRANSFER_SERVICE_HPP
