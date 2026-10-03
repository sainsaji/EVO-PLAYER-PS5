#ifndef EVO_FTP_CLIENT_HPP
#define EVO_FTP_CLIENT_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <ctime>

namespace evo {

struct FtpFileEntry {
    std::string name;
    bool isDirectory = false;
    int64_t size = 0;
    time_t modifyTime = 0;
};

class FtpClient {
public:
    FtpClient();
    ~FtpClient();

    bool connectServer(const std::string& host, int port,
                       const std::string& user = "anonymous",
                       const std::string& pass = "anonymous",
                       int timeoutSec = 4);
    void disconnect();
    bool isConnected() const { return m_controlSock >= 0; }

    bool list(const std::string& path, std::vector<FtpFileEntry>& entries);

    /*
     * Streams localPath to remotePath with STOR. onProgress is called after
     * every chunk that reaches the socket; return false from it to cancel, which
     * aborts the transfer and deletes the half-written remote file.
     *
     * dataTimeoutSec overrides the short control-socket timeout for the data
     * socket - a multi-gigabyte body needs longer than 4 s of patience.
     */
    using StoreProgressFn = bool (*)(void* user, uint64_t totalSent, uint32_t chunkBytes);
    bool storeFile(const std::string& remotePath, const std::string& localPath,
                   StoreProgressFn onProgress = nullptr, void* user = nullptr,
                   int dataTimeoutSec = 30);

    bool removeFile(const std::string& remotePath);

    const std::string& getLastError() const { return m_lastError; }

private:
    int sendCommand(const std::string& cmd);
    int readResponse(std::string* outText = nullptr);
    int openDataConnection(int timeoutSecOverride = 0);
    bool parseUnixLine(const std::string& line, FtpFileEntry& entry);
    bool parseMlsdLine(const std::string& line, FtpFileEntry& entry);
    bool parseDosLine(const std::string& line, FtpFileEntry& entry);

    int m_controlSock = -1;
    std::string m_host;
    int m_port = 21;
    int m_timeoutSec = 4;
    std::string m_lastError;
    char m_recvBuf[2048];
    size_t m_recvLen = 0;
    size_t m_recvPos = 0;
};

} // namespace evo

#endif // EVO_FTP_CLIENT_HPP
