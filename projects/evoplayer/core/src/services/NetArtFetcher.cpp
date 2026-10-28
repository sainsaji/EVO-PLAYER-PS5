#include "evo/services/NetArtFetcher.hpp"

namespace evo {

void NetArtFetcher::disconnect() {
    m_smb.disconnect();
    m_ftp.disconnect();
    m_ftpFor.clear();
}

bool NetArtFetcher::fetch(const NetArtRequest& req, std::vector<uint8_t>& out) {
    out.clear();
    const NetworkShare& s = req.share;

    if (s.protocol == NetworkProtocol::SMB) {
        std::string shareName, subPath;
        if (!SmbClient::splitShareAndPath(req.path, shareName, subPath)) return false;

        /* A new share, or a dropped connection, needs a fresh connect. */
        const bool same = m_smb.isConnected() && m_smb.getCurrentHost() == s.host &&
                          m_smb.getCurrentShare() == shareName;
        if (!same) {
            m_smb.disconnect();
            if (!m_smb.connectShare(s.host, s.port, s.user, s.password, s.domain, shareName, 6))
                return false;
        }
        if (!m_smb.readFile(subPath, MaxImageBytes, out)) {
            m_smb.disconnect();   /* next job reconnects instead of reusing a bad link */
            return false;
        }
        return true;
    }

    const std::string who = s.host + ":" + std::to_string(s.port) + ":" + s.user;
    if (!m_ftp.isConnected() || m_ftpFor != who) {
        m_ftp.disconnect();
        if (!m_ftp.connectServer(s.host, s.port, s.user, s.password, 3)) return false;
        m_ftpFor = who;
    }
    if (!m_ftp.retrieveFile(req.path, MaxImageBytes, out)) {
        m_ftp.disconnect();
        m_ftpFor.clear();
        return false;
    }
    return true;
}

} // namespace evo
