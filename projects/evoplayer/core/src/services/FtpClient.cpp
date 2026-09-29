#include "evo/services/FtpClient.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <sstream>
#include <algorithm>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>

namespace evo {

FtpClient::FtpClient() {
    std::memset(m_recvBuf, 0, sizeof(m_recvBuf));
}

FtpClient::~FtpClient() {
    disconnect();
}

void FtpClient::disconnect() {
    if (m_controlSock >= 0) {
        sendCommand("QUIT");
        close(m_controlSock);
        m_controlSock = -1;
    }
    m_recvLen = 0;
    m_recvPos = 0;
}

static int createConnectedSocket(const std::string& host, int port, int timeoutSec) {
    struct addrinfo hints, *res = nullptr, *rp = nullptr;
    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%d", port);

    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
        return -1;
    }

    int sock = -1;
    for (rp = res; rp != nullptr; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;

        struct timeval tv;
        tv.tv_sec = timeoutSec;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }

        close(sock);
        sock = -1;
    }

    freeaddrinfo(res);
    return sock;
}

int FtpClient::sendCommand(const std::string& cmd) {
    if (m_controlSock < 0) return -1;
    std::string line = cmd + "\r\n";
    ssize_t sent = send(m_controlSock, line.c_str(), line.size(), 0);
    return (sent == static_cast<ssize_t>(line.size())) ? 0 : -1;
}

int FtpClient::readResponse(std::string* outText) {
    if (m_controlSock < 0) return -1;

    std::string fullResponse;
    int replyCode = -1;

    while (true) {
        std::string currentLine;
        while (true) {
            if (m_recvPos >= m_recvLen) {
                ssize_t r = recv(m_controlSock, m_recvBuf, sizeof(m_recvBuf), 0);
                if (r <= 0) {
                    return (replyCode > 0) ? replyCode : -1;
                }
                m_recvLen = static_cast<size_t>(r);
                m_recvPos = 0;
            }

            char c = m_recvBuf[m_recvPos++];
            if (c == '\r') continue;
            if (c == '\n') break;
            currentLine += c;
        }

        fullResponse += currentLine + "\n";

        // Check if line starts with 3 digits
        if (currentLine.size() >= 3 &&
            std::isdigit(static_cast<unsigned char>(currentLine[0])) &&
            std::isdigit(static_cast<unsigned char>(currentLine[1])) &&
            std::isdigit(static_cast<unsigned char>(currentLine[2]))) {
            int code = std::atoi(currentLine.substr(0, 3).c_str());
            // If 4th char is '-' it is a multi-line continuation, otherwise it's the final line
            if (currentLine.size() == 3 || currentLine[3] == ' ') {
                replyCode = code;
                break;
            }
        }
    }

    if (outText) {
        *outText = fullResponse;
    }
    return replyCode;
}

bool FtpClient::connectServer(const std::string& host, int port,
                             const std::string& user, const std::string& pass,
                             int timeoutSec) {
    disconnect();
    m_host = host;
    m_port = port > 0 ? port : 21;
    m_timeoutSec = timeoutSec > 0 ? timeoutSec : 4;
    m_lastError.clear();

    m_controlSock = createConnectedSocket(m_host, m_port, m_timeoutSec);
    if (m_controlSock < 0) {
        m_lastError = "Failed to connect to " + m_host + ":" + std::to_string(m_port);
        return false;
    }

    int code = readResponse();
    if (code < 200 || code >= 300) {
        m_lastError = "Welcome greeting failed (code " + std::to_string(code) + ")";
        disconnect();
        return false;
    }

    // USER
    if (sendCommand("USER " + user) != 0) {
        m_lastError = "Failed to send USER";
        disconnect();
        return false;
    }
    code = readResponse();
    if (code == 331) {
        // PASS needed
        if (sendCommand("PASS " + pass) != 0) {
            m_lastError = "Failed to send PASS";
            disconnect();
            return false;
        }
        code = readResponse();
    }

    if (code < 200 || code >= 300) {
        m_lastError = "Authentication failed (code " + std::to_string(code) + ")";
        disconnect();
        return false;
    }

    // TYPE I (binary mode)
    sendCommand("TYPE I");
    readResponse();

    return true;
}

int FtpClient::openDataConnection() {
    if (sendCommand("PASV") != 0) {
        m_lastError = "Failed to send PASV";
        return -1;
    }

    std::string resp;
    int code = readResponse(&resp);
    if (code != 227) {
        m_lastError = "PASV rejected (code " + std::to_string(code) + ")";
        return -1;
    }

    // Parse (h1,h2,h3,h4,p1,p2)
    size_t start = resp.find('(');
    size_t end = resp.find(')', start);
    if (start == std::string::npos || end == std::string::npos || end <= start + 1) {
        m_lastError = "Malformed PASV response";
        return -1;
    }

    std::string numbers = resp.substr(start + 1, end - start - 1);
    int h1, h2, h3, h4, p1, p2;
    if (std::sscanf(numbers.c_str(), "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        m_lastError = "Failed to parse PASV port";
        return -1;
    }

    int dataPort = (p1 << 8) | p2;

    // Determine target host for data connection
    // If control host is localhost or 127.0.0.1, stick with control host to avoid NAT rewrite bugs
    std::string targetHost = m_host;
    if (m_host != "127.0.0.1" && m_host != "localhost") {
        char parsedIp[32];
        std::snprintf(parsedIp, sizeof(parsedIp), "%d.%d.%d.%d", h1, h2, h3, h4);
        targetHost = parsedIp;
    }

    int dataSock = createConnectedSocket(targetHost, dataPort, m_timeoutSec);
    if (dataSock < 0) {
        m_lastError = "Failed to open data socket to " + targetHost + ":" + std::to_string(dataPort);
    }
    return dataSock;
}

bool FtpClient::parseUnixLine(const std::string& line, FtpFileEntry& entry) {
    if (line.size() < 10) return false;
    char typeChar = line[0];
    if (typeChar != 'd' && typeChar != '-' && typeChar != 'l') {
        return false;
    }

    entry.isDirectory = (typeChar == 'd');

    // Tokenize line by spaces
    std::istringstream iss(line);
    std::string token;
    std::vector<std::string> tokens;
    while (iss >> token) {
        tokens.push_back(token);
    }

    // Standard Unix:
    // 0: perms (-rw-r--r--)
    // 1: links (1)
    // 2: owner (user)
    // 3: group (group)
    // 4: size (12345)
    // 5: month (Jan)
    // 6: day (1)
    // 7: time/year (12:00 or 2026)
    // 8+: filename
    if (tokens.size() < 8) return false;

    // Find the size token (usually 4th, or 3rd if owner/group combined)
    int64_t sizeVal = 0;
    size_t nameTokenStart = 8;

    if (tokens.size() >= 9) {
        sizeVal = std::strtoll(tokens[4].c_str(), nullptr, 10);
        nameTokenStart = 8;
    } else {
        // Shorter format, e.g. without group
        sizeVal = std::strtoll(tokens[3].c_str(), nullptr, 10);
        nameTokenStart = 7;
    }

    entry.size = (sizeVal > 0) ? sizeVal : 0;

    // Reconstruct full filename from nameTokenStart
    // Find where token[nameTokenStart] starts in the original line
    size_t searchPos = 0;
    for (size_t i = 0; i < nameTokenStart && i < tokens.size(); ++i) {
        searchPos = line.find(tokens[i], searchPos);
        if (searchPos != std::string::npos) {
            searchPos += tokens[i].size();
        }
    }

    while (searchPos < line.size() && (line[searchPos] == ' ' || line[searchPos] == '\t')) {
        searchPos++;
    }

    if (searchPos < line.size()) {
        entry.name = line.substr(searchPos);
        // Trim trailing carriage return
        while (!entry.name.empty() && (entry.name.back() == '\r' || entry.name.back() == '\n')) {
            entry.name.pop_back();
        }
    } else {
        entry.name = tokens.back();
    }

    // If symlink, strip target "link -> target"
    if (typeChar == 'l') {
        size_t arrow = entry.name.find(" -> ");
        if (arrow != std::string::npos) {
            entry.name = entry.name.substr(0, arrow);
        }
        // Often on PS5 /mnt/usb0 or similar can be a symlink to storage
        entry.isDirectory = true;
    }

    return !entry.name.empty();
}

bool FtpClient::parseDosLine(const std::string& line, FtpFileEntry& entry) {
    // Format: 01-01-20  12:00PM       <DIR>          FolderName
    //         01-01-20  12:00PM             12345678 FileName.mp4
    if (line.size() < 39) return false;
    if (!std::isdigit(static_cast<unsigned char>(line[0]))) return false;

    if (line.find("<DIR>") != std::string::npos) {
        entry.isDirectory = true;
        entry.size = 0;
        size_t dirPos = line.find("<DIR>");
        size_t namePos = dirPos + 5;
        while (namePos < line.size() && line[namePos] == ' ') namePos++;
        entry.name = line.substr(namePos);
    } else {
        std::istringstream iss(line);
        std::string date, time, sizeStr;
        if (iss >> date >> time >> sizeStr) {
            entry.isDirectory = false;
            entry.size = std::strtoll(sizeStr.c_str(), nullptr, 10);
            std::string rest;
            std::getline(iss, rest);
            while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
            entry.name = rest;
        } else {
            return false;
        }
    }

    while (!entry.name.empty() && (entry.name.back() == '\r' || entry.name.back() == '\n')) {
        entry.name.pop_back();
    }
    return !entry.name.empty();
}

bool FtpClient::parseMlsdLine(const std::string& line, FtpFileEntry& entry) {
    // Format: type=file;size=1234;modify=2026...; filename
    size_t lastSemi = line.rfind(';');
    if (lastSemi == std::string::npos || lastSemi + 1 >= line.size()) {
        return false;
    }

    std::string name = line.substr(lastSemi + 1);
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    while (!name.empty() && (name.back() == '\r' || name.back() == '\n')) name.pop_back();
    if (name.empty()) return false;

    entry.name = name;
    entry.isDirectory = (line.find("type=dir") != std::string::npos ||
                         line.find("type=pdir") != std::string::npos ||
                         line.find("type=cdir") != std::string::npos);

    size_t sizePos = line.find("size=");
    if (sizePos != std::string::npos) {
        entry.size = std::strtoll(line.c_str() + sizePos + 5, nullptr, 10);
    } else {
        entry.size = 0;
    }

    return true;
}

bool FtpClient::list(const std::string& path, std::vector<FtpFileEntry>& entries) {
    entries.clear();
    if (m_controlSock < 0) {
        m_lastError = "Not connected";
        return false;
    }

    int dataSock = openDataConnection();
    if (dataSock < 0) {
        return false;
    }

    std::string targetPath = path.empty() ? "/" : path;
    if (sendCommand("LIST " + targetPath) != 0) {
        m_lastError = "Failed to send LIST";
        close(dataSock);
        return false;
    }

    int code = readResponse();
    // 150 Opening ASCII/BINARY mode data connection
    // 125 Data connection already open
    if (code != 150 && code != 125) {
        m_lastError = "LIST failed (code " + std::to_string(code) + ")";
        close(dataSock);
        return false;
    }

    std::string dataBuffer;
    char chunk[4096];
    while (true) {
        ssize_t n = recv(dataSock, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        dataBuffer.append(chunk, static_cast<size_t>(n));
    }
    close(dataSock);

    // Read control response after transfer (226 Transfer complete)
    readResponse();

    // Parse lines
    std::istringstream stream(dataBuffer);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.empty()) continue;

        FtpFileEntry entry;
        bool ok = false;
        if (line[0] == 'd' || line[0] == '-' || line[0] == 'l') {
            ok = parseUnixLine(line, entry);
        } else if (line.find("type=") != std::string::npos) {
            ok = parseMlsdLine(line, entry);
        } else if (std::isdigit(static_cast<unsigned char>(line[0]))) {
            ok = parseDosLine(line, entry);
        }

        if (ok) {
            if (entry.name == "." || entry.name == ".." || entry.name == "$RECYCLE.BIN" || entry.name == "System Volume Information") {
                continue;
            }
            entries.push_back(std::move(entry));
        }
    }

    return true;
}

} // namespace evo
