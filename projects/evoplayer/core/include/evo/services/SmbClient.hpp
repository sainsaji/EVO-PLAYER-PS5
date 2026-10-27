#ifndef EVO_SMB_CLIENT_HPP
#define EVO_SMB_CLIENT_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <ctime>

struct smb2_context;

namespace evo {

struct SmbFileEntry {
    std::string name;
    bool isDirectory = false;
    uint64_t size = 0;
    time_t modifyTime = 0;
};

class SmbClient {
public:
    SmbClient();
    ~SmbClient();

    // Connects to a specific share on an SMB host
    bool connectShare(const std::string& host, int port,
                      const std::string& user,
                      const std::string& password,
                      const std::string& domain,
                      const std::string& shareName,
                      int timeoutSec = 6);

    void disconnect();
    bool isConnected() const { return m_ctx != nullptr; }

    // List folder contents within the currently connected share
    bool list(const std::string& subPath, std::vector<SmbFileEntry>& entries);

    // Enumerate disk shares available on the host (e.g. via IPC$)
    bool listShares(const std::string& host, int port,
                    const std::string& user,
                    const std::string& password,
                    const std::string& domain,
                    std::vector<std::string>& outShares,
                    int timeoutSec = 6);

    const std::string& getLastError() const { return m_lastError; }
    const std::string& getCurrentHost() const { return m_currentHost; }
    const std::string& getCurrentShare() const { return m_currentShare; }

    // Helper: parse a path like "/Movies/Action" into share="Movies", subpath="Action"
    static bool splitShareAndPath(const std::string& fullPath,
                                 std::string& outShare,
                                 std::string& outSubPath);

private:
    struct smb2_context* m_ctx = nullptr;
    std::string m_lastError;
    std::string m_currentHost;
    int m_currentPort = 0;
    std::string m_currentUser;
    std::string m_currentPassword;
    std::string m_currentDomain;
    std::string m_currentShare;
};

} // namespace evo

#endif // EVO_SMB_CLIENT_HPP
