#include "evo/services/NetworkStorageService.hpp"
#include "evo_data_path.h"
#include "cJSON.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace evo {

NetworkStorageService& NetworkStorageService::getInstance() {
    static NetworkStorageService s_instance;
    return s_instance;
}

NetworkStorageService::NetworkStorageService()
    : m_ftpClient(std::make_unique<FtpClient>())
{
    loadConfig();
}

NetworkStorageService::~NetworkStorageService() {
    if (m_ftpClient) {
        m_ftpClient->disconnect();
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

std::string NetworkStorageService::buildMediaUrl(const NetworkShare& share, const std::string& remotePath) const {
    std::string proto = (share.protocol == NetworkProtocol::SMB) ? "smb://" : "ftp://";
    std::string creds;
    if (!share.user.empty() && share.user != "anonymous") {
        creds = share.user;
        if (!share.password.empty()) {
            creds += ":" + share.password;
        }
        creds += "@";
    }

    std::string cleanPath = remotePath;
    if (cleanPath.empty() || cleanPath.front() != '/') {
        cleanPath = "/" + cleanPath;
    }

    return proto + creds + share.host + ":" + std::to_string(share.port) + cleanPath;
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
    } else {
        outError = "SMB support coming soon";
        return false;
    }
}

} // namespace evo
