#include "evo/services/NetworkStorageService.hpp"
#include "evo/services/NetArtSidecar.hpp"
#include "evo_data_path.h"
#include "evo_smb_io.h"
#include "evo_boot_log.h"
#include "evo_net.h"
#include "cJSON.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <pthread.h>
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

int NetworkStorageService::probeReachable(const std::string& host, int port, int timeoutMs, bool ftpGreeting) {
    struct addrinfo hints, *res = nullptr;
    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%d", port);
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) return -1;

    const long long t0 = monotonicMs();
    int result = -1;
    /* connect() ignores O_NONBLOCK and SO_SNDTIMEO on this console and waits
     * ~75 s on a dead LAN host; evo_net_connect_addr() stops waiting in time. */
    int sock = evo_net_connect_addr(res, timeoutMs, nullptr);
    if (sock >= 0) {
        bool up = true;
        if (ftpGreeting) {
            /* SO_RCVTIMEO bounds each recv (it does work for reads); the
             * greeting is one short line. */
            struct timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[128];
            size_t got = 0;
            while (got < 3) {
                ssize_t n = recv(sock, buf + got, sizeof(buf) - got, 0);
                if (n <= 0) break;
                got += static_cast<size_t>(n);
            }
            up = got >= 3 && std::strncmp(buf, "220", 3) == 0;
        }
        if (up) result = static_cast<int>(std::max<long long>(1, monotonicMs() - t0));
        close(sock);
    }
    freeaddrinfo(res);
    return result;
}

namespace {
struct UrlStatus {
    int state = -1;            // 1 online, 0 offline, -1 not known yet
    long long at = 0;          // monotonicMs() of the answer
    bool probing = false;
};
pthread_mutex_t g_urlStatusLock = PTHREAD_MUTEX_INITIALIZER;
std::map<std::string, UrlStatus> g_urlStatus;   // key "host:port"

struct UrlProbe {
    std::string key, host;
    int port = 0;
    bool ftp = false;
};

void* urlProbeEntry(void* arg) {
    UrlProbe* p = static_cast<UrlProbe*>(arg);
    const int ms = NetworkStorageService::probeReachable(p->host, p->port, 2500, p->ftp);
    pthread_mutex_lock(&g_urlStatusLock);
    g_urlStatus[p->key] = {ms >= 0 ? 1 : 0, monotonicMs(), false};
    pthread_mutex_unlock(&g_urlStatusLock);
    delete p;
    return nullptr;
}
}  // namespace

bool NetworkStorageService::parseServerUrl(const std::string& url, std::string& host, int& port, bool& ftp) {
    if (url.compare(0, 6, "smb://") == 0) { ftp = false; port = 445; }
    else if (url.compare(0, 6, "ftp://") == 0) { ftp = true; port = 21; }
    else return false;

    /* scheme://[user[:pass]@]host[:port]/... */
    const size_t slash = url.find('/', 6);
    std::string auth = url.substr(6, slash == std::string::npos ? std::string::npos : slash - 6);
    const size_t at = auth.rfind('@');
    if (at != std::string::npos) auth = auth.substr(at + 1);
    host = auth;
    const size_t colon = auth.rfind(':');
    if (colon != std::string::npos) {
        host = auth.substr(0, colon);
        port = std::atoi(auth.c_str() + colon + 1);
    }
    return !host.empty() && port > 0;
}

int NetworkStorageService::serverStatusForUrl(const std::string& url) {
    std::string host;
    int port = 0;
    bool ftp = false;
    if (!parseServerUrl(url, host, port, ftp)) return -1;
    const std::string key = host + ":" + std::to_string(port);

    const long long now = monotonicMs();
    pthread_mutex_lock(&g_urlStatusLock);
    UrlStatus& st = g_urlStatus[key];
    const int previous = st.state;
    if (st.probing || (st.state >= 0 && now - st.at < 30000)) {
        pthread_mutex_unlock(&g_urlStatusLock);
        return previous;
    }
    st.probing = true;
    pthread_mutex_unlock(&g_urlStatusLock);

    UrlProbe* p = new UrlProbe{key, host, port, ftp};
    pthread_attr_t attr;
    pthread_t th;
    int rc = pthread_attr_init(&attr);
    if (rc == 0) {
        pthread_attr_setstacksize(&attr, 1u << 18);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        rc = pthread_create(&th, &attr, urlProbeEntry, p);
        pthread_attr_destroy(&attr);
    }
    if (rc != 0) {
        EVO_LOGE("netstore", "server probe thread not started (%d)", rc);
        delete p;
        pthread_mutex_lock(&g_urlStatusLock);
        g_urlStatus[key].probing = false;
        pthread_mutex_unlock(&g_urlStatusLock);
    }
    return previous;   // the last answer stands while the recheck runs
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
                    cJSON* jShares = cJSON_GetObjectItem(item, "shares");
                    cJSON* jArt = cJSON_GetObjectItem(item, "art");

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

                    if (jShares && cJSON_IsArray(jShares)) {
                        int sharesCount = cJSON_GetArraySize(jShares);
                        for (int sIdx = 0; sIdx < sharesCount; ++sIdx) {
                            cJSON* sItem = cJSON_GetArrayItem(jShares, sIdx);
                            if (sItem && cJSON_IsString(sItem) && sItem->valuestring) {
                                share.chosenShares.push_back(sItem->valuestring);
                            }
                        }
                    }

                    if (jArt && cJSON_IsBool(jArt)) {
                        share.art = cJSON_IsTrue(jArt);
                    }

                    if (!share.id.empty() && !share.host.empty()) {
                        m_shares.push_back(std::move(share));
                    }
                }
            } else {
                /* The defaults below then replace the saved shares. */
                EVO_LOGW("netstore", "%s is not a JSON array (%ld bytes) - shares reset", path, len);
            }
            if (root) cJSON_Delete(root);
        } else {
            EVO_LOGW("netstore", "%s is %ld bytes - ignored", path, len);
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

        if (!s.chosenShares.empty()) {
            cJSON* sharesArr = cJSON_CreateArray();
            if (sharesArr) {
                for (const auto& shareName : s.chosenShares) {
                    cJSON_AddItemToArray(sharesArr, cJSON_CreateString(shareName.c_str()));
                }
                cJSON_AddItemToObject(item, "shares", sharesArr);
            }
        }

        if (!s.art) {
            cJSON_AddItemToObject(item, "art", cJSON_CreateFalse());
        }

        cJSON_AddItemToArray(root, item);
    }

    char* rendered = cJSON_PrintUnformatted(root);
    if (rendered) {
        const char* path = evo_data_path("evo_network_shares.json");
        FILE* f = std::fopen(path, "wb");
        if (f) {
            const size_t n = std::strlen(rendered);
            const size_t put = std::fwrite(rendered, 1, n, f);
            if (std::fclose(f) != 0 || put != n)
                EVO_LOGE("netstore", "short write saving %s (%zu of %zu bytes)", path, put, n);
        } else {
            EVO_LOGE("netstore", "cannot save %s (errno %d)", path, errno);
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
            creds += "@";   // the saved password is never shown; the edit handler keeps it
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
            creds = share.user + "@";   // the saved password is never shown; the edit handler keeps it
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

static bool asciiEqualsCi(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + ('a' - 'A')) : a[i];
        char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + ('a' - 'A')) : b[i];
        if (ca != cb) return false;
    }
    return true;
}

std::vector<std::string> NetworkStorageService::filterChosenShares(
    const std::vector<std::string>& offered,
    const std::vector<std::string>& chosen)
{
    if (chosen.empty()) {
        return offered;
    }

    std::vector<std::string> result;
    for (const auto& off : offered) {
        for (const auto& ch : chosen) {
            if (asciiEqualsCi(off, ch)) {
                result.push_back(off);
                break;
            }
        }
    }
    return result;
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

/*
 * Cover art from the listing already in hand (#230): for each file, pick the best
 * sidecar image among the same folder's entries and remember where it lives. No
 * network call happens here; the image is fetched later on the art worker.
 * Only image names are matched, so a folder of 5000 videos stays cheap.
 */
static bool isSidecarImageName(const std::string& name) {
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext;
    for (size_t i = dot + 1; i < name.size(); ++i)
        ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(name[i]))));
    return ext == "jpg" || ext == "jpeg" || ext == "png";
}

static void attachSidecars(const NetworkShare& share, std::vector<BrowserEntry>& entries,
                           const std::vector<std::string>& remote,
                           const std::vector<int64_t>& sizes, const std::vector<int64_t>& mtimes) {
    if (!share.art || entries.size() != remote.size()) return;

    std::vector<SidecarEntry> images;
    std::vector<size_t> imageAt;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].category == FileCategory::Folder || !isSidecarImageName(entries[i].name)) continue;
        images.push_back({entries[i].name, remote[i], false});
        imageAt.push_back(i);
    }
    if (images.empty()) return;

    const std::string server = std::string(share.protocol == NetworkProtocol::SMB ? "smb://" : "ftp://") +
                               share.host + ":" + std::to_string(share.port);
    for (size_t i = 0; i < entries.size(); ++i) {
        const FileCategory cat = entries[i].category;
        if (cat == FileCategory::Folder || cat == FileCategory::Image) continue;
        const int hit = pickSidecar(entries[i].name, images);
        if (hit < 0) continue;
        const size_t at = imageAt[static_cast<size_t>(hit)];
        entries[i].artPath = remote[at];
        entries[i].artKey = server + remote[at] + "|" + std::to_string(sizes[at]) + "|" + std::to_string(mtimes[at]);
    }
}

bool NetworkStorageService::listEntries(const std::string& shareId, const std::string& remotePath,
                                       std::vector<BrowserEntry>& outEntries, std::string& outError) {
    outEntries.clear();
    outError.clear();

    const NetworkShare* share = getShare(shareId);
    if (!share) {
        outError = "Network share not found";
        EVO_LOGE("netstore", "list: no share with id '%s'", shareId.c_str());
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

        std::vector<std::string> remote;
        std::vector<int64_t> sizes, mtimes;
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
            remote.push_back(fullItemPath);
            sizes.push_back(fe.size);
            mtimes.push_back(static_cast<int64_t>(fe.modifyTime));
            if (fe.isDirectory) {
                be.category = FileCategory::Folder;
                be.directoryType = 4; // DT_DIR
            } else {
                be.category = classifyFileName(fe.name);
                be.directoryType = 8; // DT_REG
            }
            outEntries.push_back(std::move(be));
        }
        attachSidecars(*share, outEntries, remote, sizes, mtimes);
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
                diskShares = filterChosenShares(diskShares, share->chosenShares);
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
                EVO_LOGE("netstore", "SMB share list on %s:%d failed: %s", share->host.c_str(),
                         share->port, outError.c_str());
                return false;
            }
        } else {
            // Inside a share: connect to share and list subpath
            if (!m_smbClient->connectShare(share->host, share->port, share->user, share->password, share->domain, smbShare, 6)) {
                outError = m_smbClient->getLastError();
                EVO_LOGE("netstore", "SMB connect %s:%d share '%s' failed: %s", share->host.c_str(),
                         share->port, smbShare.c_str(), outError.c_str());
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
                    EVO_LOGE("netstore", "SMB list '%s' in '%s' on %s failed (after a reconnect): %s",
                             subPath.c_str(), smbShare.c_str(), share->host.c_str(), outError.c_str());
                    return false;
                }
            }

            std::vector<std::string> remote;
            std::vector<int64_t> sizes, mtimes;
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
                remote.push_back(fullItemPath);
                sizes.push_back(static_cast<int64_t>(se.size));
                mtimes.push_back(static_cast<int64_t>(se.modifyTime));
                if (se.isDirectory) {
                    be.category = FileCategory::Folder;
                    be.directoryType = 4; // DT_DIR
                } else {
                    be.category = classifyFileName(se.name);
                    be.directoryType = 8; // DT_REG
                }
                outEntries.push_back(std::move(be));
            }
            attachSidecars(*share, outEntries, remote, sizes, mtimes);
            return true;
        }
    } else {
        outError = "Unknown protocol";
        return false;
    }
}

NetworkTestResult NetworkStorageService::testConnection(const NetworkShare& share) {
    const int effectivePort = share.port > 0 ? share.port : (share.protocol == NetworkProtocol::SMB ? 445 : 21);

    if (probeReachable(share.host, effectivePort, 2500) < 0) {
        EVO_LOGE("netstore", "test: %s:%d unreachable within 2.5 s", share.host.c_str(), effectivePort);
        NetworkTestResult result;
        result.stage = NetworkTestStage::Address;
        result.message = "Can't reach " + share.host + ". Check the address.";
        return result;
    }

    if (share.protocol == NetworkProtocol::SMB) {
        std::string smbShare;
        std::string subPath;
        const bool hasShare = SmbClient::splitShareAndPath(share.startPath, smbShare, subPath);

        SmbClient smb;
        const int timeoutSec = 3;

        if (!hasShare) {
            std::vector<std::string> shares;
            if (!smb.listShares(share.host, effectivePort, share.user, share.password, share.domain, shares, timeoutSec)) {
                EVO_LOGE("netstore", "test: SMB share list on %s:%d failed: %s", share.host.c_str(),
                         effectivePort, smb.getLastError().c_str());
                NetworkTestResult result;
                result.stage = NetworkTestStage::Login;
                result.message = "Login refused. Check user and password.";
                return result;
            }
            NetworkTestResult result;
            result.stage = NetworkTestStage::Ok;
            result.shares = std::move(shares);
            return result;
        } else {
            if (!smb.connectShare(share.host, effectivePort, share.user, share.password, share.domain, smbShare, timeoutSec)) {
                NetworkTestResult result;
                const std::string& err = smb.getLastError();
                EVO_LOGE("netstore", "test: SMB connect %s:%d share '%s' failed: %s", share.host.c_str(),
                         effectivePort, smbShare.c_str(), err.c_str());
                if (err.find("STATUS_LOGON_FAILURE") != std::string::npos ||
                    err.find("STATUS_ACCESS_DENIED") != std::string::npos ||
                    err.find("LOGON") != std::string::npos ||
                    err.find("ACCESS_DENIED") != std::string::npos) {
                    result.stage = NetworkTestStage::Login;
                    result.message = "Login refused. Check user and password.";
                } else {
                    result.stage = NetworkTestStage::Share;
                    result.message = "Share " + smbShare + " not found on " + share.host + ".";
                }
                return result;
            }
            NetworkTestResult result;
            result.stage = NetworkTestStage::Ok;
            return result;
        }
    } else if (share.protocol == NetworkProtocol::FTP) {
        FtpClient ftp;
        const int timeoutSec = 3;

        if (!ftp.connectServer(share.host, effectivePort, share.user, share.password, timeoutSec)) {
            NetworkTestResult result;
            result.stage = NetworkTestStage::Login;
            result.message = "Login refused. Check user and password.";
            return result;
        }

        const std::string path = share.startPath.empty() ? "/" : share.startPath;
        std::vector<FtpFileEntry> entries;
        if (!ftp.list(path, entries)) {
            NetworkTestResult result;
            result.stage = NetworkTestStage::Folder;
            result.message = "Start folder " + path + " not found.";
            return result;
        }

        NetworkTestResult result;
        result.stage = NetworkTestStage::Ok;
        return result;
    } else {
        NetworkTestResult result;
        result.stage = NetworkTestStage::Address;
        result.message = "Unknown protocol";
        return result;
    }
}

} // namespace evo
