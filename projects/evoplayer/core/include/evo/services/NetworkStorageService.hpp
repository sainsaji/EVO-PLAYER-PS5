#ifndef EVO_NETWORK_STORAGE_SERVICE_HPP
#define EVO_NETWORK_STORAGE_SERVICE_HPP

#include "evo/interfaces/IFileSystemBrowser.hpp"
#include "evo/services/FtpClient.hpp"
#include "evo/services/SmbClient.hpp"
#include <string>
#include <vector>
#include <memory>

namespace evo {

enum class NetworkProtocol {
    FTP = 0,
    SMB = 1
};

struct NetworkShare {
    std::string id;
    std::string name;
    NetworkProtocol protocol = NetworkProtocol::FTP;
    std::string host = "127.0.0.1";
    int port = 2121;
    std::string user = "anonymous";
    std::string password = "anonymous";
    std::string domain;
    std::string startPath = "/";
};

enum class NetworkTestStage { Ok, Address, Login, Share, Folder };

struct NetworkTestResult {
    NetworkTestStage stage = NetworkTestStage::Ok;
    std::string message;
    std::vector<std::string> shares;
};

class NetworkStorageService {
public:
    using NetworkTestStage = evo::NetworkTestStage;
    using NetworkTestResult = evo::NetworkTestResult;

    static NetworkStorageService& getInstance();

    NetworkStorageService();
    ~NetworkStorageService();

    void loadConfig();
    void saveConfig();

    const std::vector<NetworkShare>& getShares() const { return m_shares; }
    const NetworkShare* getShare(const std::string& id) const;
    void addShare(const NetworkShare& share);
    void removeShare(const std::string& id);

    bool listEntries(const std::string& shareId, const std::string& remotePath,
                     std::vector<BrowserEntry>& outEntries, std::string& outError);

    std::string buildMediaUrl(const std::string& shareId, const std::string& remotePath) const;
    std::string buildMediaUrl(const NetworkShare& share, const std::string& remotePath) const;

    /* Opens and closes a TCP connection to host:port. Touches no client state,
     * so it is safe off the UI thread. Returns the connect time in ms, or -1. */
    static int probeReachable(const std::string& host, int port, int timeoutMs);

    /* Tests a share's address, credentials, and target share/folder. Touches no
     * shared client state; safe to call on a worker thread. */
    static NetworkTestResult testConnection(const NetworkShare& share);

    static std::string percentEncode(const std::string& str);
    static std::string attachCredentials(const std::string& url);
    static std::string attachCredentials(const std::string& url, const std::vector<NetworkShare>& shares);
    static bool parseSmbUrl(const std::string& url, NetworkShare& outShare);
    static bool parseSmbUrl(const std::string& url,
                            std::string& outHost, int& outPort,
                            std::string& outUser, std::string& outPassword,
                            std::string& outStartPath,
                            std::string* outDomain = nullptr);
    static std::string buildEditUrl(const NetworkShare& share);

    FileCategory classifyFileName(const std::string& fileName) const;

private:
    std::vector<NetworkShare> m_shares;
    std::unique_ptr<FtpClient> m_ftpClient;
    std::unique_ptr<SmbClient> m_smbClient;
    std::string m_lastConnectedShareId;
};

} // namespace evo

#endif // EVO_NETWORK_STORAGE_SERVICE_HPP
