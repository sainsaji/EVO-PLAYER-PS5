#include "evo/services/NetworkStorageService.hpp"
#include "evo_data_path.h"
#include "evo_smb_io.h"
#include "cJSON.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>

namespace evo {

static long long monotonicMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int NetworkStorageService::probeReachable(const std::string& host, int port, int timeoutMs) {
    struct addrinfo hints, *res = nullptr;
    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%d", port);
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) return -1;

    const long long t0 = monotonicMs();
    int result = -1;
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock >= 0) {
        /* O_NONBLOCK does not stop connect() blocking ~75 s on this console
         * (measured on a dead LAN host), so the bound is the send timeout. */
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int rc = connect(sock, res->ai_addr, res->ai_addrlen);
        bool up = (rc == 0);
        if (!up && errno == EINPROGRESS) {
            struct pollfd pfd;
            pfd.fd = sock;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, timeoutMs) > 0 && (pfd.revents & (POLLOUT | POLLERR | POLLHUP))) {
                int err = 0;
                socklen_t len = sizeof(err);
                up = (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0);
            }
        }
        if (up) result = static_cast<int>(std::max<long long>(1, monotonicMs() - t0));
        close(sock);
    }
    freeaddrinfo(res);
    return result;
}

NetworkStorageService& NetworkStorageService::getInstance() {
    static NetworkStorageService s_instance;
    return s_instance;
}

NetworkStorageService::NetworkStorageService()
    : m_ftpClient(std::make_unique<FtpClient>()),
      m_smbClient(std::make_unique<SmbClient>())
{
    loadConfig();
}

NetworkStorageService::~NetworkStorageService() {
    if (m_ftpClient) {
        m_ftpClient->disconnect();
    }
    if (m_smbClient) {
        m_smbClient->disconnect();
    }
}

const NetworkShare* NetworkStorageService::getShare(const std::string& id) const {
    for (const auto& s : m_shares) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

void NetworkStorageService::addShare(const NetworkShare& share) {
    for (auto& s : m_shares) {
        if (s.id == share.id) {
            s = share;
            saveConfig();
            return;
        }
    }
    m_shares.push_back(share);
    saveConfig();
}

void NetworkStorageService::removeShare(const std::string& id) {
    m_shares.erase(std::remove_if(m_shares.begin(), m_shares.end(),
        [&id](const NetworkShare& s) { return s.id == id; }), m_shares.end());
    saveConfig();
}

void NetworkStorageService::loadConfig() {
    m_shares.clear();

    const char* path = evo_data_path("evo_network_shares.json");
    FILE* f = std::fopen(path, "rb");
    if (f) {
        std::fseek(f, 0, SEEK_END);
        long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);

        if (len > 0 && len < 65536) {
            std::vector<char> buf(len + 1, 0);
            std::fread(buf.data(), 1, len, f);
            cJSON* root = cJSON_Parse(buf.data());
            if (root && cJSON_IsArray(root)) {
                int count = cJSON_GetArraySize(root);
                for (int i = 0; i < count; ++i) {
                    cJSON* item = cJSON_GetArrayItem(root, i);
                    if (!item) continue;

                    NetworkShare share;
                    cJSON* jId = cJSON_GetObjectItem(item, "id");
                    cJSON* jName = cJSON_GetObjectItem(item, "name");
                    cJSON* jProto = cJSON_GetObjectItem(item, "protocol");
                    cJSON* jHost = cJSON_GetObjectItem(item, "host");
                    cJSON* jPort = cJSON_GetObjectItem(item, "port");
                    cJSON* jUser = cJSON_GetObjectItem(item, "user");
                    cJSON* jPass = cJSON_GetObjectItem(item, "password");
                    cJSON* jDomain = cJSON_GetObjectItem(item, "domain");
                    cJSON* jPath = cJSON_GetObjectItem(item, "startPath");

                    if (jId && jId->valuestring) share.id = jId->valuestring;
                    if (jName && jName->valuestring) share.name = jName->valuestring;
                    if (jProto && jProto->valuestring) {
                        share.protocol = (std::strcmp(jProto->valuestring, "smb") == 0)
                            ? NetworkProtocol::SMB : NetworkProtocol::FTP;
                    }
                    if (jHost && jHost->valuestring) share.host = jHost->valuestring;
                    if (jPort && cJSON_IsNumber(jPort)) share.port = jPort->valueint;
                    if (jUser && jUser->valuestring) share.user = jUser->valuestring;
                    if (jPass && jPass->valuestring) share.password = jPass->valuestring;
                    if (jDomain && jDomain->valuestring) share.domain = jDomain->valuestring;
                    if (jPath && jPath->valuestring) share.startPath = jPath->valuestring;

                    if (!share.id.empty() && !share.host.empty()) {
                        m_shares.push_back(std::move(share));
                    }
                }
            }
            if (root) cJSON_Delete(root);
        }
        std::fclose(f);
    }

    // Default configuration if none loaded
    if (m_shares.empty()) {
        NetworkShare defaultPs5;
        defaultPs5.id = "ps5_local";
        defaultPs5.name = "PS5 Local FTP";
        defaultPs5.protocol = NetworkProtocol::FTP;
        defaultPs5.host = "127.0.0.1";
        defaultPs5.port = 2121;
        defaultPs5.user = "anonymous";
        defaultPs5.password = "anonymous";
        defaultPs5.startPath = "/";
        m_shares.push_back(defaultPs5);
        saveConfig();
    }
}

void NetworkStorageService::saveConfig() {
    cJSON* root = cJSON_CreateArray();
    if (!root) return;

    for (const auto& s : m_shares) {
        cJSON* item = cJSON_CreateObject();
        if (!item) continue;

        cJSON_AddStringToObject(item, "id", s.id.c_str());
        cJSON_AddStringToObject(item, "name", s.name.c_str());
        cJSON_AddStringToObject(item, "protocol", (s.protocol == NetworkProtocol::SMB) ? "smb" : "ftp");
        cJSON_AddStringToObject(item, "host", s.host.c_str());
        cJSON_AddNumberToObject(item, "port", s.port);
        cJSON_AddStringToObject(item, "user", s.user.c_str());
        cJSON_AddStringToObject(item, "password", s.password.c_str());
        if (!s.domain.empty()) {
            cJSON_AddStringToObject(item, "domain", s.domain.c_str());
        }
        cJSON_AddStringToObject(item, "startPath", s.startPath.c_str());

        cJSON_AddItemToArray(root, item);
    }

    char* rendered = cJSON_PrintUnformatted(root);
    if (rendered) {
        const char* path = evo_data_path("evo_network_shares.json");
        FILE* f = std::fopen(path, "wb");
        if (f) {
            std::fwrite(rendered, 1, std::strlen(rendered), f);
            std::fclose(f);
        }
        std::free(rendered);
    }
    cJSON_Delete(root);
}

static bool isUnreservedChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

std::string NetworkStorageService::percentEncode(const std::string& str) {
    static const char hexChars[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(str.size() * 3);
    for (unsigned char c : str) {
        if (isUnreservedChar(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hexChars[(c >> 4) & 0x0F]);
            out.push_back(hexChars[c & 0x0F]);
        }
    }
    return out;
}

static std::string percentEncodePath(const std::string& path) {
    std::string out;
    out.reserve(path.size() * 3);
    size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '/') {
            out.push_back('/');
            i++;
        } else {
            size_t nextSlash = path.find('/', i);
            if (nextSlash == std::string::npos) {
                nextSlash = path.size();
            }
            std::string segment = path.substr(i, nextSlash - i);
            out += NetworkStorageService::percentEncode(segment);
            i = nextSlash;
        }
    }
    return out;
}

std::string NetworkStorageService::buildMediaUrl(const NetworkShare& share, const std::string& remotePath) const {
    std::string cleanPath = remotePath;
    if (cleanPath.empty() || cleanPath.front() != '/') {
        cleanPath = "/" + cleanPath;
    }

    if (share.protocol == NetworkProtocol::SMB) {
        std::string creds;
        if (!share.user.empty() && share.user != "anonymous" && share.user != "guest") {
            if (!share.domain.empty()) {
                creds = percentEncode(share.domain) + ";" + percentEncode(share.user);
            } else {
                creds = percentEncode(share.user);
            }
            if (!share.password.empty()) {
                creds += ":" + percentEncode(share.password);
            }
            creds += "@";
        }

        std::string portStr;
        if (share.port > 0 && share.port != 445) {
            portStr = ":" + std::to_string(share.port);
        }

        return "smb://" + creds + share.host + portStr + percentEncodePath(cleanPath);
    } else {
        // FTP: keep raw format as FTP consumers do not decode percent encoding
        std::string creds;
        if (!share.user.empty() && share.user != "anonymous" && share.user != "guest") {
            creds = share.user;
            if (!share.password.empty()) {
                creds += ":" + share.password;
            }
            creds += "@";
        }

        std::string portStr;
        if (share.port > 0) {
            portStr = ":" + std::to_string(share.port);
        }

        return "ftp://" + creds + share.host + portStr + cleanPath;
    }
}

static bool hostEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

std::string NetworkStorageService::attachCredentials(const std::string& url, const std::vector<NetworkShare>& shares) {
    if (url.empty()) return url;

    NetworkProtocol proto;
    int defaultPort = 0;
    size_t prefixLen = 0;

    if (url.rfind("smb://", 0) == 0) {
        proto = NetworkProtocol::SMB;
        defaultPort = 445;
        prefixLen = 6;
    } else if (url.rfind("ftp://", 0) == 0) {
        proto = NetworkProtocol::FTP;
        defaultPort = 21;
        prefixLen = 6;
    } else {
        return url;
    }

    size_t authEnd = url.find_first_of("/?#", prefixLen);
    std::string auth = (authEnd != std::string::npos)
                     ? url.substr(prefixLen, authEnd - prefixLen)
                     : url.substr(prefixLen);
    std::string pathPart = (authEnd != std::string::npos) ? url.substr(authEnd) : "";

    // If user info already present, return unchanged
    if (auth.find('@') != std::string::npos) {
        return url;
    }

    std::string host = auth;
    int port = defaultPort;
    size_t colon = auth.find(':');
    if (colon != std::string::npos) {
        host = auth.substr(0, colon);
        int parsedPort = std::atoi(auth.substr(colon + 1).c_str());
        if (parsedPort > 0 && parsedPort <= 65535) {
            port = parsedPort;
        }
    }

    if (host.empty()) {
        return url;
    }

    const NetworkShare* match = nullptr;
    for (auto it = shares.rbegin(); it != shares.rend(); ++it) {
        const auto& s = *it;
        if (s.protocol != proto) continue;
        int sPort = (s.port > 0) ? s.port : defaultPort;
        if (sPort != port) continue;
        if (!hostEquals(s.host, host)) continue;

        if (!s.startPath.empty() && s.startPath != "/") {
            std::string sharePrefix = s.startPath;
            if (sharePrefix.back() == '/') sharePrefix.pop_back();
            if (pathPart.rfind(sharePrefix, 0) == 0 &&
                (pathPart.size() == sharePrefix.size() || pathPart[sharePrefix.size()] == '/')) {
                match = &s;
                break;
            }
        }
    }

    if (!match) {
        for (auto it = shares.rbegin(); it != shares.rend(); ++it) {
            const auto& s = *it;
            if (s.protocol != proto) continue;
            int sPort = (s.port > 0) ? s.port : defaultPort;
            if (sPort != port) continue;
            if (hostEquals(s.host, host)) {
                match = &s;
                break;
            }
        }
    }

    if (!match) {
        return url;
    }

    std::string creds;
    if (!match->user.empty() && match->user != "anonymous" && match->user != "guest") {
        if (!match->domain.empty()) {
            creds = percentEncode(match->domain) + ";" + percentEncode(match->user);
        } else {
            creds = percentEncode(match->user);
        }
        if (!match->password.empty()) {
            creds += ":" + percentEncode(match->password);
        }
        creds += "@";
    }

    std::string portStr;
    if (match->port > 0 && match->port != defaultPort) {
        portStr = ":" + std::to_string(match->port);
    }

    std::string scheme = (proto == NetworkProtocol::SMB) ? "smb://" : "ftp://";
    return scheme + creds + match->host + portStr + pathPart;
}

std::string NetworkStorageService::attachCredentials(const std::string& url) {
    return attachCredentials(url, getInstance().getShares());
}

bool NetworkStorageService::parseSmbUrl(const std::string& url,
                                        std::string& outHost, int& outPort,
                                        std::string& outUser, std::string& outPassword,
                                        std::string& outStartPath,
                                        std::string* outDomain) {
    std::string trimmed = url;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    if (start != std::string::npos) {
        size_t end = trimmed.find_last_not_of(" \t\r\n");
        trimmed = trimmed.substr(start, end - start + 1);
    } else {
        return false;
    }
    if (trimmed.empty()) return false;

    if (trimmed.rfind("smb://", 0) == 0) {
        trimmed = trimmed.substr(6);
    }

    std::string domain;
    std::string user = "guest";
    std::string pass;

    size_t at = trimmed.find('@');
    if (at != std::string::npos) {
        std::string auth = trimmed.substr(0, at);
        trimmed = trimmed.substr(at + 1);

        size_t semi = auth.find(';');
        if (semi != std::string::npos) {
            domain = auth.substr(0, semi);
            auth = auth.substr(semi + 1);
        } else {
            size_t backslash = auth.find('\\');
            if (backslash != std::string::npos) {
                domain = auth.substr(0, backslash);
                auth = auth.substr(backslash + 1);
            }
        }

        size_t colon = auth.find(':');
        if (colon != std::string::npos) {
            user = auth.substr(0, colon);
            pass = auth.substr(colon + 1);
        } else {
            user = auth;
        }

        domain = evo_url_decode_string(domain);
        user = evo_url_decode_string(user);
        pass = evo_url_decode_string(pass);
    }

    std::string host = trimmed;
    std::string startPath = "/";
    size_t slash = trimmed.find('/');
    if (slash != std::string::npos) {
        host = trimmed.substr(0, slash);
        startPath = trimmed.substr(slash);
    }

    int port = 445;
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        port = std::atoi(host.substr(colon + 1).c_str());
        host = host.substr(0, colon);
        if (port <= 0 || port > 65535) port = 445;
    }

    if (host.empty()) return false;

    outHost = host;
    outPort = port;
    outUser = user;
    outPassword = pass;
    outStartPath = startPath;
    if (outDomain) {
        *outDomain = domain;
    }
    return true;
}

bool NetworkStorageService::parseSmbUrl(const std::string& url, NetworkShare& outShare) {
    std::string host, user, pass, startPath, domain;
    int port = 445;
    if (!parseSmbUrl(url, host, port, user, pass, startPath, &domain)) {
        return false;
    }
    outShare.protocol = NetworkProtocol::SMB;
    outShare.name = host + (startPath != "/" ? startPath : "");
    outShare.host = host;
    outShare.port = port;
    outShare.user = user;
    outShare.password = pass;
    outShare.domain = domain;
    outShare.startPath = startPath;
    return true;
}

std::string NetworkStorageService::buildEditUrl(const NetworkShare& share) {
    if (share.protocol == NetworkProtocol::SMB) {
        std::string creds;
        if (!share.user.empty() && share.user != "guest") {
            if (!share.domain.empty()) {
                creds = percentEncode(share.domain) + ";" + percentEncode(share.user);
            } else {
                creds = percentEncode(share.user);
            }
            if (!share.password.empty()) {
                creds += ":" + percentEncode(share.password);
            }
            creds += "@";
        }

        std::string portStr;
        if (share.port > 0 && share.port != 445) {
            portStr = ":" + std::to_string(share.port);
        }

        std::string pathStr;
        if (!share.startPath.empty() && share.startPath != "/") {
            if (share.startPath.front() != '/') pathStr = "/";
            pathStr += share.startPath;
        }

        return "smb://" + creds + share.host + portStr + pathStr;
    } else {
        std::string creds;
        if (!share.user.empty() && share.user != "anonymous") {
            creds = share.user;
            if (!share.password.empty() && share.password != "anonymous") {
                creds += ":" + share.password;
            }
            creds += "@";
        }

        std::string portStr;
        if (share.port > 0 && share.port != 21) {
            portStr = ":" + std::to_string(share.port);
        }

        std::string pathStr;
        if (!share.startPath.empty() && share.startPath != "/") {
            if (share.startPath.front() != '/') pathStr = "/";
            pathStr += share.startPath;
        }

        return creds + share.host + portStr + pathStr;
    }
}

std::string NetworkStorageService::buildMediaUrl(const std::string& shareId, const std::string& remotePath) const {
    const NetworkShare* share = getShare(shareId);
    if (!share) return "";
    return buildMediaUrl(*share, remotePath);
}

FileCategory NetworkStorageService::classifyFileName(const std::string& fileName) const {
    size_t dot = fileName.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= fileName.size()) {
        return FileCategory::Unknown;
    }

    std::string ext = fileName.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Video
    if (ext == "mkv"  || ext == "mp4"  || ext == "mov"  || ext == "m4v"  ||
        ext == "avi"  || ext == "webm" || ext == "ts"   || ext == "m2ts" ||
        ext == "mpg"  || ext == "mpeg" || ext == "wmv"  || ext == "flv"  ||
        ext == "asf"  || ext == "wm"   || ext == "rm"   || ext == "rmvb" ||
        ext == "ogv"  || ext == "dv"   || ext == "mxf"  || ext == "mts"  ||
        ext == "vob"  || ext == "m2v"  || ext == "3gp"  || ext == "3g2"  ||
        ext == "mp2"  || ext == "f4v"  || ext == "ivf"  || ext == "obu") {
        return FileCategory::Video;
    }

    // Audio
    if (ext == "mp3"  || ext == "flac" || ext == "wav"  || ext == "aac"  ||
        ext == "m4a"  || ext == "ogg"  || ext == "opus" || ext == "oga"  ||
        ext == "mka"  || ext == "m4b"  || ext == "wma"  || ext == "ac3"  ||
        ext == "eac3" || ext == "dts"  || ext == "dtshd"|| ext == "thd"  ||
        ext == "mlp"  || ext == "mp2"  || ext == "mpa"  || ext == "ape"  ||
        ext == "wv"   || ext == "tta"  || ext == "tak"  || ext == "shn"  ||
        ext == "aif"  || ext == "aiff" || ext == "au"   || ext == "caf"  ||
        ext == "w64"  || ext == "dsf"  || ext == "dff"  || ext == "amr"  ||
        ext == "awb"  || ext == "mpc"  || ext == "voc"  || ext == "ra"   ||
        ext == "oma"  || ext == "aa3"  || ext == "at9"  || ext == "gsm"  ||
        ext == "qoa") {
        return FileCategory::Audio;
    }

    // Image
    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp" || ext == "webp") {
        return FileCategory::Image;
    }

    // Document
    if (ext == "srt" || ext == "ass" || ext == "ssa" || ext == "vtt" ||
        ext == "sub" || ext == "smi" || ext == "sami" || ext == "mpl" ||
        ext == "jss" || ext == "rt"  || ext == "stl"  ||
        ext == "txt" || ext == "log" || ext == "md"  || ext == "nfo" ||
        ext == "ini" || ext == "cfg" || ext == "json" || ext == "csv") {
        return FileCategory::Document;
    }

    if (ext == "iso" || ext == "img") return FileCategory::DiscImage;
    if (ext == "elf" || ext == "bin" || ext == "sprx") return FileCategory::Homebrew;

    return FileCategory::Unknown;
}

bool NetworkStorageService::listEntries(const std::string& shareId, const std::string& remotePath,
                                       std::vector<BrowserEntry>& outEntries, std::string& outError) {
    outEntries.clear();
    outError.clear();

    const NetworkShare* share = getShare(shareId);
    if (!share) {
        outError = "Network share not found";
        return false;
    }

    if (share->protocol == NetworkProtocol::FTP) {
        if (!m_ftpClient->isConnected() || m_lastConnectedShareId != shareId) {
            m_ftpClient->disconnect();
            if (!m_ftpClient->connectServer(share->host, share->port, share->user, share->password, 3)) {
                outError = m_ftpClient->getLastError();
                return false;
            }
            m_lastConnectedShareId = shareId;
        }

        std::vector<FtpFileEntry> ftpEntries;
        std::string targetPath = remotePath.empty() ? share->startPath : remotePath;
        if (!m_ftpClient->list(targetPath, ftpEntries)) {
            outError = m_ftpClient->getLastError();
            // Retry once with reconnect in case connection timed out
            m_ftpClient->disconnect();
            if (m_ftpClient->connectServer(share->host, share->port, share->user, share->password, 3) &&
                m_ftpClient->list(targetPath, ftpEntries)) {
                outError.clear();
            } else {
                return false;
            }
        }

        for (const auto& fe : ftpEntries) {
            BrowserEntry be;
            be.name = fe.name;
            be.relativePath = fe.name;

            std::string fullItemPath = targetPath;
            if (fullItemPath.empty() || fullItemPath.back() != '/') {
                fullItemPath += "/";
            }
            fullItemPath += fe.name;

            be.fullPath = buildMediaUrl(*share, fullItemPath);
            if (fe.isDirectory) {
                be.category = FileCategory::Folder;
                be.directoryType = 4; // DT_DIR
            } else {
                be.category = classifyFileName(fe.name);
                be.directoryType = 8; // DT_REG
            }
            outEntries.push_back(std::move(be));
        }
        return true;
    } else if (share->protocol == NetworkProtocol::SMB) {
        std::string targetPath = remotePath.empty() ? share->startPath : remotePath;
        if (targetPath.empty()) targetPath = "/";

        std::string smbShare;
        std::string subPath;
        bool hasShare = SmbClient::splitShareAndPath(targetPath, smbShare, subPath);

        if (!hasShare) {
            // Root of SMB server: enumerate available disk shares
            std::vector<std::string> diskShares;
            if (m_smbClient->listShares(share->host, share->port, share->user, share->password, share->domain, diskShares, 6)) {
                for (const auto& ds : diskShares) {
                    BrowserEntry be;
                    be.name = ds;
                    be.relativePath = ds;
                    be.fullPath = buildMediaUrl(*share, "/" + ds);
                    be.category = FileCategory::Folder;
                    be.directoryType = 4; // DT_DIR
                    outEntries.push_back(std::move(be));
                }
                return true;
            } else {
                outError = m_smbClient->getLastError();
                return false;
            }
        } else {
            // Inside a share: connect to share and list subpath
            if (!m_smbClient->connectShare(share->host, share->port, share->user, share->password, share->domain, smbShare, 6)) {
                outError = m_smbClient->getLastError();
                return false;
            }

            std::vector<SmbFileEntry> smbEntries;
            if (!m_smbClient->list(subPath, smbEntries)) {
                outError = m_smbClient->getLastError();
                // Retry once with reconnect in case connection timed out
                m_smbClient->disconnect();
                if (m_smbClient->connectShare(share->host, share->port, share->user, share->password, share->domain, smbShare, 6) &&
                    m_smbClient->list(subPath, smbEntries)) {
                    outError.clear();
                } else {
                    return false;
                }
            }

            for (const auto& se : smbEntries) {
                BrowserEntry be;
                be.name = se.name;
                be.relativePath = se.name;

                std::string fullItemPath = "/" + smbShare;
                if (!subPath.empty()) {
                    if (subPath.front() != '/') fullItemPath += "/";
                    fullItemPath += subPath;
                }
                if (fullItemPath.back() != '/') fullItemPath += "/";
                fullItemPath += se.name;

                be.fullPath = buildMediaUrl(*share, fullItemPath);
                if (se.isDirectory) {
                    be.category = FileCategory::Folder;
                    be.directoryType = 4; // DT_DIR
                } else {
                    be.category = classifyFileName(se.name);
                    be.directoryType = 8; // DT_REG
                }
                outEntries.push_back(std::move(be));
            }
            return true;
        }
    } else {
        outError = "Unknown protocol";
        return false;
    }
}

} // namespace evo
