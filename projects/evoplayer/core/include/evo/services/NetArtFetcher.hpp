#ifndef EVO_NET_ART_FETCHER_HPP
#define EVO_NET_ART_FETCHER_HPP

#include "evo/services/FtpClient.hpp"
#include "evo/services/NetworkStorageService.hpp"
#include "evo/services/SmbClient.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace evo {

/* One sidecar download (#230). The share is a copy, so the worker never reads the
 * live share list while the UI thread edits it. */
struct NetArtRequest {
    NetworkShare share;
    std::string path;   /* sidecar path on the server (SMB: "/Share/dir/poster.jpg") */
    std::string key;    /* disk-cache key: server + path + size + mtime */
};

/*
 * Downloads small cover images over connections of its own, so the folder
 * listing (which uses NetworkStorageService's clients) is never blocked or
 * disturbed by art. One thread only: the CoverArtService worker.
 */
class NetArtFetcher {
public:
    static constexpr size_t MaxImageBytes = 3u * 1024u * 1024u;

    bool fetch(const NetArtRequest& req, std::vector<uint8_t>& out);
    void disconnect();

private:
    SmbClient m_smb;
    FtpClient m_ftp;
    std::string m_ftpFor;   /* host:port:user the FTP control connection belongs to */
};

} // namespace evo

#endif // EVO_NET_ART_FETCHER_HPP
