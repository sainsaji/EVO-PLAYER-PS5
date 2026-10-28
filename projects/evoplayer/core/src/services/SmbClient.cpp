#include "evo/services/SmbClient.hpp"
#include "evo_smb_io.h"

#include <cerrno>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#if defined(EVO_TARGET_PS5)
#define HAVE_LIBSMB2 1
extern "C" {
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-dcerpc-srvsvc.h>
}
#endif

namespace evo {

#if defined(HAVE_LIBSMB2)

struct ShareEnumCbData {
    int finished = 0;
    int status = 0;
    std::vector<std::string> shares;
};

static void shareEnumCallback(struct smb2_context *smb2, int status,
                              void *command_data, void *cb_data) {
    (void)smb2;
    auto *data = static_cast<ShareEnumCbData*>(cb_data);
    data->finished = 1;
    data->status = status;
    if (status == 0 && command_data) {
        auto *rep = static_cast<struct srvsvc_NetrShareEnum_rep*>(command_data);
        if (rep->ses.Level == 1) {
            auto &ctr1 = rep->ses.ShareInfo.Level1;
            if (ctr1.Buffer && ctr1.Buffer->share_info_1) {
                for (uint32_t i = 0; i < ctr1.EntriesRead; ++i) {
                    const auto &info = ctr1.Buffer->share_info_1[i];
                    if ((info.type & 3) == SHARE_TYPE_DISKTREE &&
                        !(info.type & SHARE_TYPE_HIDDEN) &&
                        info.netname.utf8) {
                        data->shares.push_back(info.netname.utf8);
                    }
                }
            }
        }
    }
}

static int pollUntilFinished(struct smb2_context *ctx, int *finished, int timeoutSec) {
    time_t start = time(nullptr);
    while (!(*finished)) {
        struct pollfd pfd;
        pfd.fd = smb2_get_fd(ctx);
        if (pfd.fd < 0) return -1;
        pfd.events = smb2_which_events(ctx);
        pfd.revents = 0;
        int r = poll(&pfd, 1, 100);
        if (r > 0) {
            if (smb2_service(ctx, pfd.revents) < 0) {
                return -1;
            }
        } else if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (time(nullptr) - start > timeoutSec) {
            return -ETIMEDOUT;
        }
    }
    return 0;
}

#endif // HAVE_LIBSMB2

SmbClient::SmbClient() = default;

SmbClient::~SmbClient() {
    disconnect();
}

bool SmbClient::connectShare(const std::string& host, int port,
                             const std::string& user,
                             const std::string& password,
                             const std::string& domain,
                             const std::string& shareName,
                             int timeoutSec) {
#if defined(HAVE_LIBSMB2)
    // Reuse existing connection only if host, port, user, password, domain and share ALL match
    if (m_ctx && m_currentHost == host && m_currentPort == port &&
        m_currentUser == user && m_currentPassword == password &&
        m_currentDomain == domain && m_currentShare == shareName) {
        return true;
    }

    disconnect();
    m_lastError.clear();

    if (evo_smb_tcp_precheck(host.c_str(), port, 4) != 0) {
        m_lastError = "Server not reachable";
        return false;
    }

    struct smb2_context* ctx = smb2_init_context();
    if (!ctx) {
        m_lastError = "Out of memory initializing SMB context";
        return false;
    }

    smb2_set_timeout(ctx, timeoutSec > 0 ? timeoutSec : 6);
    smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);

    std::string effectiveUser = user.empty() ? "guest" : user;
    smb2_set_user(ctx, effectiveUser.c_str());

    if (!password.empty()) {
        smb2_set_password(ctx, password.c_str());
    }
    if (!domain.empty()) {
        smb2_set_domain(ctx, domain.c_str());
    }

    // Connect to share
    // Format host[:port] if non-default port
    std::string serverWithPort = host;
    if (port > 0 && port != 445) {
        serverWithPort += ":" + std::to_string(port);
    }

    int rc = smb2_connect_share(ctx, serverWithPort.c_str(), shareName.c_str(), effectiveUser.c_str());
    if (rc < 0) {
        const char* err = smb2_get_error(ctx);
        m_lastError = (err && err[0]) ? err : "Failed to connect to SMB share";
        smb2_destroy_context(ctx);
        return false;
    }

    m_ctx = ctx;
    m_currentHost = host;
    m_currentPort = port;
    m_currentUser = user;
    m_currentPassword = password;
    m_currentDomain = domain;
    m_currentShare = shareName;
    return true;
#else
    (void)host; (void)port; (void)user; (void)password; (void)domain; (void)shareName; (void)timeoutSec;
    m_lastError = "SMB2 not supported on this platform";
    return false;
#endif
}

void SmbClient::disconnect() {
#if defined(HAVE_LIBSMB2)
    if (m_ctx) {
        smb2_disconnect_share(m_ctx);
        smb2_destroy_context(m_ctx);
        m_ctx = nullptr;
    }
#endif
    m_currentHost.clear();
    m_currentPort = 0;
    m_currentUser.clear();
    m_currentPassword.clear();
    m_currentDomain.clear();
    m_currentShare.clear();
}

bool SmbClient::list(const std::string& subPath, std::vector<SmbFileEntry>& entries) {
    entries.clear();
#if defined(HAVE_LIBSMB2)
    if (!m_ctx) {
        m_lastError = "Not connected to SMB share";
        return false;
    }

    const char* pathStr = subPath.c_str();
    while (*pathStr == '/') pathStr++; // strip leading slashes

    struct smb2dir *dir = smb2_opendir(m_ctx, pathStr);
    if (!dir) {
        const char* err = smb2_get_error(m_ctx);
        m_lastError = (err && err[0]) ? err : "Can't open SMB directory";
        return false;
    }

    struct smb2dirent *ent = nullptr;
    while ((ent = smb2_readdir(m_ctx, dir)) != nullptr) {
        if (!ent->name || strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) {
            continue;
        }
        SmbFileEntry fe;
        fe.name = ent->name;
        fe.isDirectory = (ent->st.smb2_type == SMB2_TYPE_DIRECTORY);
        fe.size = ent->st.smb2_size;
        fe.modifyTime = static_cast<time_t>(ent->st.smb2_mtime);
        entries.push_back(std::move(fe));
    }

    smb2_closedir(m_ctx, dir);
    return true;
#else
    (void)subPath;
    m_lastError = "SMB2 not supported";
    return false;
#endif
}

bool SmbClient::readFile(const std::string& subPath, size_t maxBytes, std::vector<uint8_t>& out) {
    out.clear();
    m_lastError.clear();
#if defined(HAVE_LIBSMB2)
    if (!m_ctx) {
        m_lastError = "Not connected to SMB share";
        return false;
    }

    const char* pathStr = subPath.c_str();
    while (*pathStr == '/') pathStr++; // strip leading slashes

    struct smb2fh* fh = smb2_open(m_ctx, pathStr, O_RDONLY);
    if (!fh) {
        const char* err = smb2_get_error(m_ctx);
        m_lastError = (err && err[0]) ? err : "Can't open SMB file";
        return false;
    }

    struct smb2_stat_64 st;
    if (smb2_fstat(m_ctx, fh, &st) < 0) {
        const char* err = smb2_get_error(m_ctx);
        m_lastError = (err && err[0]) ? err : "Can't stat SMB file";
        smb2_close(m_ctx, fh);
        return false;
    }

    if (st.smb2_size > maxBytes) {
        m_lastError = "File exceeds maximum size";
        smb2_close(m_ctx, fh);
        return false;
    }

    size_t fileSize = static_cast<size_t>(st.smb2_size);
    if (fileSize == 0) {
        smb2_close(m_ctx, fh);
        return true;
    }

    out.resize(fileSize);
    size_t totalRead = 0;
    while (totalRead < fileSize) {
        uint32_t toRead = static_cast<uint32_t>(std::min<size_t>(fileSize - totalRead, 64 * 1024));
        int rc = smb2_pread(m_ctx, fh, out.data() + totalRead, toRead, totalRead);
        if (rc < 0) {
            const char* err = smb2_get_error(m_ctx);
            m_lastError = (err && err[0]) ? err : "Read error from SMB file";
            smb2_close(m_ctx, fh);
            out.clear();
            return false;
        }
        if (rc == 0) {
            break;
        }
        totalRead += static_cast<size_t>(rc);
    }

    smb2_close(m_ctx, fh);
    if (totalRead < fileSize) {
        out.resize(totalRead);
    }
    return true;
#else
    (void)subPath;
    (void)maxBytes;
    (void)out;
    m_lastError = "SMB2 not supported";
    return false;
#endif
}

bool SmbClient::listShares(const std::string& host, int port,
                           const std::string& user,
                           const std::string& password,
                           const std::string& domain,
                           std::vector<std::string>& outShares,
                           int timeoutSec) {
    outShares.clear();
#if defined(HAVE_LIBSMB2)
    m_lastError.clear();

    if (evo_smb_tcp_precheck(host.c_str(), port, 4) != 0) {
        m_lastError = "Server not reachable";
        return false;
    }

    struct smb2_context* ctx = smb2_init_context();
    if (!ctx) {
        m_lastError = "Out of memory initializing SMB context";
        return false;
    }

    smb2_set_timeout(ctx, timeoutSec > 0 ? timeoutSec : 6);
    smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);

    std::string effectiveUser = user.empty() ? "guest" : user;
    smb2_set_user(ctx, effectiveUser.c_str());

    if (!password.empty()) {
        smb2_set_password(ctx, password.c_str());
    }
    if (!domain.empty()) {
        smb2_set_domain(ctx, domain.c_str());
    }

    std::string serverWithPort = host;
    if (port > 0 && port != 445) {
        serverWithPort += ":" + std::to_string(port);
    }

    int rc = smb2_connect_share(ctx, serverWithPort.c_str(), "IPC$", effectiveUser.c_str());
    if (rc < 0) {
        const char* err = smb2_get_error(ctx);
        m_lastError = (err && err[0]) ? err : "Failed to connect to IPC$";
        smb2_destroy_context(ctx);
        return false;
    }

    ShareEnumCbData cbData;
    int enumRc = smb2_share_enum_async(ctx, SHARE_INFO_1, shareEnumCallback, &cbData);
    if (enumRc < 0) {
        const char* err = smb2_get_error(ctx);
        m_lastError = (err && err[0]) ? err : "Failed to initiate share enumeration";
        smb2_disconnect_share(ctx);
        smb2_destroy_context(ctx);
        return false;
    }

    if (pollUntilFinished(ctx, &cbData.finished, timeoutSec > 0 ? timeoutSec : 6) < 0 ||
        cbData.status < 0) {
        const char* err = smb2_get_error(ctx);
        m_lastError = (err && err[0]) ? err : "Share enumeration timed out or failed";
        smb2_disconnect_share(ctx);
        smb2_destroy_context(ctx);
        return false;
    }

    outShares = std::move(cbData.shares);
    smb2_disconnect_share(ctx);
    smb2_destroy_context(ctx);
    return true;
#else
    (void)host; (void)port; (void)user; (void)password; (void)domain; (void)timeoutSec;
    m_lastError = "SMB2 not supported";
    return false;
#endif
}

bool SmbClient::splitShareAndPath(const std::string& fullPath,
                                  std::string& outShare,
                                  std::string& outSubPath) {
    outShare.clear();
    outSubPath.clear();

    if (fullPath.empty()) return false;

    size_t start = 0;
    while (start < fullPath.size() && fullPath[start] == '/') {
        start++;
    }
    if (start >= fullPath.size()) return false;

    size_t slash = fullPath.find('/', start);
    if (slash == std::string::npos) {
        outShare = fullPath.substr(start);
        outSubPath = "";
    } else {
        outShare = fullPath.substr(start, slash - start);
        size_t subStart = slash;
        while (subStart < fullPath.size() && fullPath[subStart] == '/') {
            subStart++;
        }
        outSubPath = (subStart < fullPath.size()) ? fullPath.substr(subStart) : "";
    }
    return !outShare.empty();
}

} // namespace evo
