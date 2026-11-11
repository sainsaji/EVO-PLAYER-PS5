/*
 * tests/test_smb.cpp — Unit tests for SMB client and network storage integration.
 */

#include "evo/services/SmbClient.hpp"
#include "evo/services/NetworkStorageService.hpp"
#include "evo_data_path.h"
#include "evo_error.h"
#include "evo_smb_io.h"
#include "evo_recent.h"
#include "evo_favorites.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
    extern int g_tests_run;
    extern int g_tests_passed;
    extern int g_tests_failed;
}

#define TEST_START(name) do { \
    g_tests_run++; \
    printf("  RUN  %-50s", name); \
    fflush(stdout); \
} while(0)

#define TEST_PASS() do { \
    g_tests_passed++; \
    printf(" [ PASS ]\n"); \
} while(0)

#define TEST_FAIL(reason) do { \
    g_tests_failed++; \
    printf(" [ FAIL ] (%s at line %d)\n", reason, __LINE__); \
} while(0)

#define TEST_ASSERT(cond, reason) do { \
    if (!(cond)) { \
        TEST_FAIL(reason); \
        return; \
    } \
} while(0)

extern "C" void test_smb_split_path(void) {
    TEST_START("SMB: Path & Share Dissection");

    std::string share, sub;

    // Normal paths
    TEST_ASSERT(evo::SmbClient::splitShareAndPath("/Movies/Action", share, sub), "should split /Movies/Action");
    TEST_ASSERT(share == "Movies", "share should be Movies");
    TEST_ASSERT(sub == "Action", "subpath should be Action");

    TEST_ASSERT(evo::SmbClient::splitShareAndPath("/Shared", share, sub), "should split /Shared");
    TEST_ASSERT(share == "Shared", "share should be Shared");
    TEST_ASSERT(sub.empty(), "subpath should be empty");

    TEST_ASSERT(evo::SmbClient::splitShareAndPath("/Shared/", share, sub), "should split /Shared/");
    TEST_ASSERT(share == "Shared", "share should be Shared");
    TEST_ASSERT(sub.empty(), "subpath should be empty");

    TEST_ASSERT(evo::SmbClient::splitShareAndPath("Shared/Sub/Deep/file.mkv", share, sub), "should split without leading slash");
    TEST_ASSERT(share == "Shared", "share should be Shared");
    TEST_ASSERT(sub == "Sub/Deep/file.mkv", "subpath should be Sub/Deep/file.mkv");

    // Multiple slashes
    TEST_ASSERT(evo::SmbClient::splitShareAndPath("///Media///Movies///2026", share, sub), "should handle multiple slashes");
    TEST_ASSERT(share == "Media", "share should be Media");
    TEST_ASSERT(sub == "Movies///2026", "subpath should preserve inner path");

    // Invalid / empty / root
    TEST_ASSERT(!evo::SmbClient::splitShareAndPath("", share, sub), "empty should fail");
    TEST_ASSERT(!evo::SmbClient::splitShareAndPath("/", share, sub), "root slash should fail");
    TEST_ASSERT(!evo::SmbClient::splitShareAndPath("///", share, sub), "multiple root slashes should fail");

    TEST_PASS();
}

extern "C" void test_smb_storage_service(void) {
    TEST_START("SMB: NetworkStorageService Configuration & URLs");

    auto& service = evo::NetworkStorageService::getInstance();

    // 1. Add SMB share
    evo::NetworkShare share1;
    share1.id = "smb_test_nas";
    share1.name = "TrueNAS SMB";
    share1.protocol = evo::NetworkProtocol::SMB;
    share1.host = "192.168.1.50";
    share1.port = 445;
    share1.user = "guest";
    share1.password = "";
    share1.startPath = "/Public";
    service.addShare(share1);

    const auto* found = service.getShare("smb_test_nas");
    TEST_ASSERT(found != nullptr, "share should be found");
    TEST_ASSERT(found->protocol == evo::NetworkProtocol::SMB, "protocol should be SMB");
    TEST_ASSERT(found->port == 445, "port should be 445");
    TEST_ASSERT(found->user == "guest", "user should be guest");

    // 2. buildMediaUrl for guest SMB (port 445 omitted, guest creds omitted)
    std::string url1 = service.buildMediaUrl(*found, "/Public/Videos/test.mkv");
    TEST_ASSERT(url1 == "smb://192.168.1.50/Public/Videos/test.mkv", "guest URL format mismatch");

    // 3. Authenticated SMB with custom port and percent-encoding
    evo::NetworkShare share2;
    share2.id = "smb_auth";
    share2.name = "Secure SMB";
    share2.protocol = evo::NetworkProtocol::SMB;
    share2.host = "10.0.0.5";
    share2.port = 4450;
    share2.user = "admin";
    share2.password = "P@ss123";
    share2.startPath = "/";
    service.addShare(share2);

    std::string url2 = service.buildMediaUrl(share2, "/SecretShare/doc.pdf");
    TEST_ASSERT(url2 == "smb://admin:P%40ss123@10.0.0.5:4450/SecretShare/doc.pdf", "auth URL format mismatch");

    std::string urlPercent = service.buildMediaUrl(share2, "/SecretShare/100%25.mkv");
    TEST_ASSERT(urlPercent == "smb://admin:P%40ss123@10.0.0.5:4450/SecretShare/100%2525.mkv", "path percent-escape mismatch");

    std::string urlSpace = service.buildMediaUrl(share2, "/SecretShare/My Movie.mkv");
    TEST_ASSERT(urlSpace == "smb://admin:P%40ss123@10.0.0.5:4450/SecretShare/My%20Movie.mkv", "path space-escape mismatch");

    // 4. FTP URLs remain unencoded
    evo::NetworkShare ftpShare;
    ftpShare.id = "ftp_test";
    ftpShare.name = "FTP Test";
    ftpShare.protocol = evo::NetworkProtocol::FTP;
    ftpShare.host = "10.0.0.5";
    ftpShare.port = 2121;
    ftpShare.user = "admin";
    ftpShare.password = "P@ss123";
    ftpShare.startPath = "/";
    std::string ftpUrl = service.buildMediaUrl(ftpShare, "/SecretShare/100%25.mkv");
    TEST_ASSERT(ftpUrl == "ftp://admin:P@ss123@10.0.0.5:2121/SecretShare/100%25.mkv", "FTP URL must remain unencoded");

    // 5. File classification
    TEST_ASSERT(service.classifyFileName("movie.mkv") == evo::FileCategory::Video, "mkv is video");
    TEST_ASSERT(service.classifyFileName("track.flac") == evo::FileCategory::Audio, "flac is audio");
    TEST_ASSERT(service.classifyFileName("subs.srt") == evo::FileCategory::Document, "srt is document");
    TEST_ASSERT(service.classifyFileName("poster.jpg") == evo::FileCategory::Image, "jpg is image");

    // 6. filterChosenShares helper tests
    {
        std::vector<std::string> offered = { "Videos", "Music", "Photos", "Backups" };

        // Empty chosen = all (offered returned unchanged)
        std::vector<std::string> emptyChosen;
        auto resAll = evo::NetworkStorageService::filterChosenShares(offered, emptyChosen);
        TEST_ASSERT(resAll == offered, "empty chosen must return offered unchanged");

        // Subset keeps server order
        std::vector<std::string> subsetChosen = { "Photos", "Videos" };
        auto resSubset = evo::NetworkStorageService::filterChosenShares(offered, subsetChosen);
        std::vector<std::string> expectedSubset = { "Videos", "Photos" };
        TEST_ASSERT(resSubset == expectedSubset, "subset must retain server order");

        // Case-insensitive (ASCII)
        std::vector<std::string> caseChosen = { "videos", "PHOTOS" };
        auto resCase = evo::NetworkStorageService::filterChosenShares(offered, caseChosen);
        std::vector<std::string> expectedCase = { "Videos", "Photos" };
        TEST_ASSERT(resCase == expectedCase, "case-insensitive matching must preserve offered server casing");

        // Chosen name the server no longer offers is ignored
        std::vector<std::string> staleChosen = { "Videos", "OldDeletedShare", "Backups", "NonExistent" };
        auto resStale = evo::NetworkStorageService::filterChosenShares(offered, staleChosen);
        std::vector<std::string> expectedStale = { "Videos", "Backups" };
        TEST_ASSERT(resStale == expectedStale, "chosen share no longer offered must be ignored");

        // Empty offered returns empty
        std::vector<std::string> emptyOffered;
        auto resEmptyOffered = evo::NetworkStorageService::filterChosenShares(emptyOffered, subsetChosen);
        TEST_ASSERT(resEmptyOffered.empty(), "empty offered must return empty result");
    }

    // 7. chosenShares persistence: saveConfig / loadConfig round-trip
    {
        evo::NetworkShare shareChosen;
        shareChosen.id = "smb_chosen_test";
        shareChosen.name = "Chosen SMB";
        shareChosen.protocol = evo::NetworkProtocol::SMB;
        shareChosen.host = "192.168.1.100";
        shareChosen.port = 445;
        shareChosen.user = "user";
        shareChosen.password = "pass";
        shareChosen.startPath = "/";
        shareChosen.chosenShares = { "Media", "Public", "Docs" };
        service.addShare(shareChosen);

        // Verify JSON file on disk contains "shares"
        const char* cfgPath = evo_data_path("evo_network_shares.json");
        FILE* fCheck = std::fopen(cfgPath, "rb");
        TEST_ASSERT(fCheck != nullptr, "evo_network_shares.json must exist after addShare");
        char jsonBuf[4096] = {0};
        size_t bytesRead = std::fread(jsonBuf, 1, sizeof(jsonBuf) - 1, fCheck);
        std::fclose(fCheck);
        jsonBuf[bytesRead] = '\0';
        TEST_ASSERT(std::strstr(jsonBuf, "\"shares\"") != nullptr, "JSON on disk must contain 'shares' key when chosenShares is non-empty");
        TEST_ASSERT(std::strstr(jsonBuf, "\"art\"") == nullptr, "JSON on disk must NOT contain 'art' key when art is true (default)");
        TEST_ASSERT(std::strstr(jsonBuf, "\"Media\"") != nullptr, "JSON must contain Media");
        TEST_ASSERT(std::strstr(jsonBuf, "\"Public\"") != nullptr, "JSON must contain Public");
        TEST_ASSERT(std::strstr(jsonBuf, "\"Docs\"") != nullptr, "JSON must contain Docs");

        // Reload config and verify chosenShares survived round-trip
        service.loadConfig();
        const auto* loadedShare = service.getShare("smb_chosen_test");
        TEST_ASSERT(loadedShare != nullptr, "smb_chosen_test should be loaded");
        std::vector<std::string> expectedChosen = { "Media", "Public", "Docs" };
        TEST_ASSERT(loadedShare->chosenShares == expectedChosen, "chosenShares must survive saveConfig/loadConfig round trip");
        TEST_ASSERT(loadedShare->art == true, "default art must remain true after round-trip");

        // Verify share with empty chosenShares does not write "shares" key
        evo::NetworkShare shareEmptyChosen;
        shareEmptyChosen.id = "smb_empty_chosen";
        shareEmptyChosen.name = "Empty Chosen SMB";
        shareEmptyChosen.protocol = evo::NetworkProtocol::SMB;
        shareEmptyChosen.host = "192.168.1.101";
        shareEmptyChosen.port = 445;
        shareEmptyChosen.user = "user";
        shareEmptyChosen.password = "pass";
        shareEmptyChosen.startPath = "/";
        shareEmptyChosen.chosenShares.clear();
        service.addShare(shareEmptyChosen);

        // Remove smb_chosen_test
        service.removeShare("smb_chosen_test");

        fCheck = std::fopen(cfgPath, "rb");
        TEST_ASSERT(fCheck != nullptr, "evo_network_shares.json must exist");
        std::memset(jsonBuf, 0, sizeof(jsonBuf));
        bytesRead = std::fread(jsonBuf, 1, sizeof(jsonBuf) - 1, fCheck);
        std::fclose(fCheck);
        jsonBuf[bytesRead] = '\0';
        TEST_ASSERT(std::strstr(jsonBuf, "\"shares\"") == nullptr, "JSON on disk must NOT contain 'shares' key when chosenShares is empty");

        service.loadConfig();
        const auto* loadedEmpty = service.getShare("smb_empty_chosen");
        TEST_ASSERT(loadedEmpty != nullptr, "smb_empty_chosen should be loaded");
        TEST_ASSERT(loadedEmpty->chosenShares.empty(), "chosenShares must remain empty");
        service.removeShare("smb_empty_chosen");

        // art=false survives round-trip and writes "art": false to JSON
        evo::NetworkShare shareNoArt;
        shareNoArt.id = "smb_no_art";
        shareNoArt.name = "No Art SMB";
        shareNoArt.protocol = evo::NetworkProtocol::SMB;
        shareNoArt.host = "192.168.1.102";
        shareNoArt.port = 445;
        shareNoArt.user = "user";
        shareNoArt.password = "pass";
        shareNoArt.startPath = "/";
        shareNoArt.art = false;
        service.addShare(shareNoArt);

        fCheck = std::fopen(cfgPath, "rb");
        TEST_ASSERT(fCheck != nullptr, "evo_network_shares.json must exist");
        std::memset(jsonBuf, 0, sizeof(jsonBuf));
        bytesRead = std::fread(jsonBuf, 1, sizeof(jsonBuf) - 1, fCheck);
        std::fclose(fCheck);
        jsonBuf[bytesRead] = '\0';
        TEST_ASSERT(std::strstr(jsonBuf, "\"art\"") != nullptr, "JSON on disk must contain 'art' key when art is false");

        service.loadConfig();
        const auto* loadedNoArt = service.getShare("smb_no_art");
        TEST_ASSERT(loadedNoArt != nullptr, "smb_no_art should be loaded");
        TEST_ASSERT(loadedNoArt->art == false, "art=false must survive saveConfig/loadConfig round trip");
        service.removeShare("smb_no_art");
    }

    // 8. Backward compatibility: loadConfig with old-format JSON without "shares"
    {
        const char* cfgPath = evo_data_path("evo_network_shares.json");
        FILE* fLegacy = std::fopen(cfgPath, "wb");
        TEST_ASSERT(fLegacy != nullptr, "must open config path for writing legacy test");
        const char* legacyJson = "[\n"
            "  {\n"
            "    \"id\": \"legacy_share\",\n"
            "    \"name\": \"Legacy Share\",\n"
            "    \"protocol\": \"smb\",\n"
            "    \"host\": \"192.168.1.200\",\n"
            "    \"port\": 445,\n"
            "    \"user\": \"guest\",\n"
            "    \"password\": \"\",\n"
            "    \"startPath\": \"/\"\n"
            "  }\n"
            "]";
        std::fwrite(legacyJson, 1, std::strlen(legacyJson), fLegacy);
        std::fclose(fLegacy);

        service.loadConfig();
        const auto* legacyShare = service.getShare("legacy_share");
        TEST_ASSERT(legacyShare != nullptr, "legacy share must load");
        TEST_ASSERT(legacyShare->id == "legacy_share", "legacy share id mismatch");
        TEST_ASSERT(legacyShare->host == "192.168.1.200", "legacy share host mismatch");
        TEST_ASSERT(legacyShare->chosenShares.empty(), "legacy share without 'shares' must have empty chosenShares");
        TEST_ASSERT(legacyShare->art == true, "legacy share without 'art' must default art to true");

        // 9. Load config with non-string entries in "shares" array (should skip non-strings)
        FILE* fMixed = std::fopen(cfgPath, "wb");
        TEST_ASSERT(fMixed != nullptr, "must open config path for writing mixed test");
        const char* mixedJson = "[\n"
            "  {\n"
            "    \"id\": \"mixed_share\",\n"
            "    \"name\": \"Mixed Share\",\n"
            "    \"protocol\": \"smb\",\n"
            "    \"host\": \"192.168.1.201\",\n"
            "    \"port\": 445,\n"
            "    \"user\": \"guest\",\n"
            "    \"password\": \"\",\n"
            "    \"startPath\": \"/\",\n"
            "    \"shares\": [123, \"ValidShare1\", null, true, \"ValidShare2\", {}]\n"
            "  }\n"
            "]";
        std::fwrite(mixedJson, 1, std::strlen(mixedJson), fMixed);
        std::fclose(fMixed);

        service.loadConfig();
        const auto* mixedShare = service.getShare("mixed_share");
        TEST_ASSERT(mixedShare != nullptr, "mixed share must load");
        std::vector<std::string> expectedMixed = { "ValidShare1", "ValidShare2" };
        TEST_ASSERT(mixedShare->chosenShares == expectedMixed, "non-string entries in 'shares' array must be skipped");

        service.removeShare("legacy_share");
        service.removeShare("mixed_share");
    }

    // 10. Cleanup
    service.removeShare("smb_test_nas");
    service.removeShare("smb_auth");
    TEST_ASSERT(service.getShare("smb_test_nas") == nullptr, "share should be removed");

    TEST_PASS();
}

extern "C" void test_smb_url_sanitizer(void) {
    TEST_START("SMB: URL Credential Sanitizer");

    char buf[512];

    // SMB URLs
    TEST_ASSERT(std::string(evo_url_sanitize("smb://admin:secret@192.168.1.50/Share/movie.mkv", buf, sizeof(buf)))
                == "smb://192.168.1.50/Share/movie.mkv", "SMB credentials must be stripped");

    TEST_ASSERT(std::string(evo_url_sanitize("smb://192.168.1.50/Share/movie.mkv", buf, sizeof(buf)))
                == "smb://192.168.1.50/Share/movie.mkv", "SMB without credentials must remain intact");

    TEST_ASSERT(std::string(evo_url_sanitize("smb://admin:P%40ss123@10.0.0.5:4450/SecretShare/doc.pdf", buf, sizeof(buf)))
                == "smb://10.0.0.5:4450/SecretShare/doc.pdf", "SMB with %40 in password must strip credentials correctly");

    // FTP URLs
    TEST_ASSERT(std::string(evo_url_sanitize("ftp://admin:P@ss123@10.0.0.5:2121/SecretShare/file.mkv", buf, sizeof(buf)))
                == "ftp://10.0.0.5:2121/SecretShare/file.mkv", "FTP with @ in password must strip credentials correctly");

    TEST_ASSERT(std::string(evo_url_sanitize("ftp://10.0.0.5:2121/Public/file.mkv", buf, sizeof(buf)))
                == "ftp://10.0.0.5:2121/Public/file.mkv", "FTP without credentials must remain intact");

    // HTTP / HTTPS URLs
    TEST_ASSERT(std::string(evo_url_sanitize("http://user:secret@srv.example.com:8080/live/1.ts", buf, sizeof(buf)))
                == "http://srv.example.com:8080/live/1.ts", "HTTP credentials must be stripped");

    TEST_ASSERT(std::string(evo_url_sanitize("http://srv.example.com:8080/live/1.ts", buf, sizeof(buf)))
                == "http://srv.example.com:8080/live/1.ts", "HTTP without credentials must remain intact");

    TEST_ASSERT(std::string(evo_url_sanitize("https://user:secret@srv.example.com/stream.m3u8", buf, sizeof(buf)))
                == "https://srv.example.com/stream.m3u8", "HTTPS credentials must be stripped");

    // Local path / non-network
    TEST_ASSERT(std::string(evo_url_sanitize("/mnt/usb0/video.mkv", buf, sizeof(buf)))
                == "/mnt/usb0/video.mkv", "Local filesystem path must remain intact");

    // Embedded in log message
    TEST_ASSERT(std::string(evo_url_sanitize("Opening 'smb://user:pass@192.168.1.50/share/file.mkv' failed", buf, sizeof(buf)))
                == "Opening 'smb://192.168.1.50/share/file.mkv' failed", "Embedded URL credentials must be stripped");

    // Edge cases
    TEST_ASSERT(std::string(evo_url_sanitize(nullptr, buf, sizeof(buf))).empty(), "Null URL returns empty string");
    TEST_ASSERT(std::string(evo_url_sanitize("", buf, sizeof(buf))).empty(), "Empty URL returns empty string");

    TEST_PASS();
}

extern "C" void test_smb_url_decoder(void) {
    TEST_START("SMB: URL Percent-Decoder");

    char out[256];

    // Standard percent-encoded spaces
    TEST_ASSERT(evo_url_decode("My%20Movie.mkv", out, sizeof(out)) == 0, "decode space should succeed");
    TEST_ASSERT(std::string(out) == "My Movie.mkv", "decoded space mismatch");

    // Percent-encoded '%' character (%25)
    TEST_ASSERT(evo_url_decode("100%2525.mkv", out, sizeof(out)) == 0, "decode %2525 should succeed");
    TEST_ASSERT(std::string(out) == "100%25.mkv", "decoded %2525 mismatch");

    TEST_ASSERT(evo_url_decode("100%25.mkv", out, sizeof(out)) == 0, "decode %25 should succeed");
    TEST_ASSERT(std::string(out) == "100%.mkv", "decoded %25 mismatch");

    // Hex digits case insensitivity
    TEST_ASSERT(evo_url_decode("%2f%2F", out, sizeof(out)) == 0, "decode %2f%2F should succeed");
    TEST_ASSERT(std::string(out) == "//", "decoded hex mismatch");

    // Punctuation and brackets
    TEST_ASSERT(evo_url_decode("Show%20S01E01%20%282026%29%20%5B1080p%5D.mkv", out, sizeof(out)) == 0, "decode brackets should succeed");
    TEST_ASSERT(std::string(out) == "Show S01E01 (2026) [1080p].mkv", "decoded brackets mismatch");

    // Unencoded strings
    TEST_ASSERT(evo_url_decode("plain_filename.mkv", out, sizeof(out)) == 0, "plain filename should succeed");
    TEST_ASSERT(std::string(out) == "plain_filename.mkv", "plain filename mismatch");

    // Empty and null
    TEST_ASSERT(evo_url_decode("", out, sizeof(out)) == 0, "empty string should succeed");
    TEST_ASSERT(std::string(out).empty(), "empty string should produce empty output");

    TEST_ASSERT(evo_url_decode(nullptr, out, sizeof(out)) == 0, "null input should succeed");
    TEST_ASSERT(std::string(out).empty(), "null input should produce empty output");

    // In-place decoding
    char inout[64] = "In%20Place%20Decode.mp4";
    TEST_ASSERT(evo_url_decode(inout, inout, sizeof(inout)) == 0, "in-place decode should succeed");
    TEST_ASSERT(std::string(inout) == "In Place Decode.mp4", "in-place decode mismatch");

    // Truncation detection
    char small[5];
    TEST_ASSERT(evo_url_decode("LongString", small, sizeof(small)) < 0, "overflow must return -1");

    // C++ std::string helper
    TEST_ASSERT(evo_url_decode_string("Another%20Movie%20%26%20Show.mkv") == "Another Movie & Show.mkv",
                "evo_url_decode_string mismatch");

    TEST_PASS();
}

extern "C" void test_smb_url_parsing(void) {
    TEST_START("SMB: URL Parsing & Edit Round-Trip");

    auto& service = evo::NetworkStorageService::getInstance();

    // 1. Parse complex SMB URL with percent-encoded awkward password
    const std::string url = "smb://evotest:p%40ss%3Aw0rd%2F1@host:4450/Media";
    evo::NetworkShare share;
    TEST_ASSERT(evo::NetworkStorageService::parseSmbUrl(url, share), "parseSmbUrl should succeed");
    TEST_ASSERT(share.user == "evotest", "parsed user should be evotest");
    TEST_ASSERT(share.password == "p@ss:w0rd/1", "parsed password should be decoded raw 'p@ss:w0rd/1'");
    TEST_ASSERT(share.host == "host", "parsed host should be host");
    TEST_ASSERT(share.port == 4450, "parsed port should be 4450");
    TEST_ASSERT(share.startPath == "/Media", "parsed startPath should be /Media");

    // 2. buildMediaUrl round-trips back to the same encoded string
    std::string mediaUrl = service.buildMediaUrl(share, "/Media");
    TEST_ASSERT(mediaUrl == "smb://evotest:p%40ss%3Aw0rd%2F1@host:4450/Media",
                "buildMediaUrl round-trips back to the same encoded string");

    // 3. Edit prefill round-trip: Add -> Edit -> Save. The prefill never shows the saved
    //    password; the edit handler keeps it when the user is unchanged.
    std::string prefill = evo::NetworkStorageService::buildEditUrl(share);
    TEST_ASSERT(prefill == "smb://evotest@host:4450/Media",
                "edit prefill URL should hide the password");

    evo::NetworkShare reloaded;
    TEST_ASSERT(evo::NetworkStorageService::parseSmbUrl(prefill, reloaded),
                "re-parsing edit prefill should succeed");
    TEST_ASSERT(reloaded.password.empty(), "reloaded prefill carries no password");
    TEST_ASSERT(reloaded.user == "evotest", "reloaded user should match");
    TEST_ASSERT(reloaded.host == "host", "reloaded host should match");
    TEST_ASSERT(reloaded.port == 4450, "reloaded port should match");
    TEST_ASSERT(reloaded.startPath == "/Media", "reloaded startPath should match");
    TEST_ASSERT(evo::NetworkStorageService::buildEditUrl(reloaded) == prefill,
                "re-encoded prefill must match original prefill");

    // 4. URL parsing without smb:// prefix
    evo::NetworkShare noSchemeShare;
    TEST_ASSERT(evo::NetworkStorageService::parseSmbUrl("evotest:p%40ss%3Aw0rd%2F1@192.168.0.8:4450/Media", noSchemeShare),
                "parsing URL without smb:// should succeed");
    TEST_ASSERT(noSchemeShare.password == "p@ss:w0rd/1", "no-scheme password should be decoded");
    TEST_ASSERT(noSchemeShare.host == "192.168.0.8", "no-scheme host should match");
    TEST_ASSERT(noSchemeShare.port == 4450, "no-scheme port should match");

    // 5. URL parsing with domain
    evo::NetworkShare domainShare;
    TEST_ASSERT(evo::NetworkStorageService::parseSmbUrl("smb://CORP%3A1;user%40dom:secret@nas/Share", domainShare),
                "parsing URL with domain should succeed");
    TEST_ASSERT(domainShare.domain == "CORP:1", "domain should be decoded");
    TEST_ASSERT(domainShare.user == "user@dom", "user with @ should be decoded");
    TEST_ASSERT(domainShare.password == "secret", "password should match");

    // 6. TCP precheck failure cases
    TEST_ASSERT(evo_smb_tcp_precheck(nullptr, 445, 1) < 0, "null host precheck should return -1");
    TEST_ASSERT(evo_smb_tcp_precheck("", 445, 1) < 0, "empty host precheck should return -1");
    TEST_ASSERT(evo_smb_tcp_precheck("127.0.0.1", 1, 1) < 0, "closed port precheck should fail fast");

    TEST_PASS();
}

extern "C" void test_smb_recent_and_favorites(void) {
    TEST_START("SMB: Recent & Favorites Credential Sanitization");

    // 1. Store with credentials in Recent -> file on disk has none
    recent_clear();
    const char *credUrl = "smb://evotest:p%40ss%3Aw0rd%2F1@192.168.0.8:4450/Media/Movie%20Name.mp4";
    recent_add_or_update(credUrl, "Movie Name", 12.5, 120.0);
    recent_save();

    FILE *rfp = fopen(RECENT_FILE_DB, "r");
    TEST_ASSERT(rfp != nullptr, "recent DB must be created");
    char rbuf[1024] = {0};
    size_t rread = fread(rbuf, 1, sizeof(rbuf) - 1, rfp);
    fclose(rfp);
    rbuf[rread] = '\0';

    TEST_ASSERT(strstr(rbuf, "192.168.0.8:4450/Media/Movie%20Name.mp4") != nullptr, "sanitized path must be on disk");
    TEST_ASSERT(strstr(rbuf, "evotest") == nullptr, "disk file must not contain username");
    TEST_ASSERT(strstr(rbuf, "p@ss") == nullptr && strstr(rbuf, "p%40ss") == nullptr, "disk file must not contain password");
    TEST_ASSERT(strstr(rbuf, "w0rd") == nullptr, "disk file must not contain password remainder");

    // Lookup with credentialed and clean path
    TEST_ASSERT(recent_lookup(credUrl) == 12.5, "recent_lookup with credentialed URL must succeed");
    TEST_ASSERT(recent_lookup("smb://192.168.0.8:4450/Media/Movie%20Name.mp4") == 12.5, "recent_lookup with clean URL must succeed");

    // 2. Load of an old file containing user:pass@ -> migrated
    rfp = fopen(RECENT_FILE_DB, "w");
    TEST_ASSERT(rfp != nullptr, "open recent DB for writing");
    fprintf(rfp, "smb://legacyuser:legacypass@10.0.0.5:445/Media/OldMovie.mp4\tOld Movie\t50.000\t200.000\t1700000000\n");
    fclose(rfp);

    recent_load();
    TEST_ASSERT(recent_file_count == 1, "recent_load must load migrated entry");
    TEST_ASSERT(std::string(recent_files[0].path) == "smb://10.0.0.5:445/Media/OldMovie.mp4",
                "loaded entry must be sanitized on load");
    TEST_ASSERT(recent_files[0].audio_stream == -1, "old 5-column entry must default audio_stream to -1");
    TEST_ASSERT(recent_lookup_audio("smb://10.0.0.5:445/Media/OldMovie.mp4") == -1, "recent_lookup_audio for old entry must return -1");

    // 2b. Audio track persistence on resume
    recent_add_or_update("smb://10.0.0.5:445/Media/MultiTrack.mkv", "Multi Track Movie", 300.0, 7200.0, 3);
    TEST_ASSERT(recent_lookup_audio("smb://10.0.0.5:445/Media/MultiTrack.mkv") == 3, "recent_lookup_audio must return saved audio stream");
    // Update position without passing audio stream -> must preserve existing audio stream 3
    recent_add_or_update("smb://10.0.0.5:445/Media/MultiTrack.mkv", "Multi Track Movie", 350.0, 7200.0);
    TEST_ASSERT(recent_lookup_audio("smb://10.0.0.5:445/Media/MultiTrack.mkv") == 3, "recent_lookup_audio must preserve audio stream on update");
    recent_save();

    recent_load();
    TEST_ASSERT(recent_lookup_audio("smb://10.0.0.5:445/Media/MultiTrack.mkv") == 3, "audio stream must persist across save and load");

    rfp = fopen(RECENT_FILE_DB, "r");
    memset(rbuf, 0, sizeof(rbuf));
    rread = fread(rbuf, 1, sizeof(rbuf) - 1, rfp);
    fclose(rfp);
    rbuf[rread] = '\0';
    TEST_ASSERT(strstr(rbuf, "legacyuser") == nullptr && strstr(rbuf, "legacypass") == nullptr,
                "re-saved recent file must not contain legacy credentials");

    // 3. Favorites: store with credentials -> file on disk has none
    favorites_clear();
    favorites_add(credUrl, "Movie Name", 120.0);
    favorites_save();

    FILE *ffp = fopen(FAVORITES_FILE_DB, "r");
    TEST_ASSERT(ffp != nullptr, "favorites DB must be created");
    char fbuf[1024] = {0};
    size_t fread_bytes = fread(fbuf, 1, sizeof(fbuf) - 1, ffp);
    fclose(ffp);
    fbuf[fread_bytes] = '\0';

    TEST_ASSERT(strstr(fbuf, "192.168.0.8:4450/Media/Movie%20Name.mp4") != nullptr, "favorites disk path must be sanitized");
    TEST_ASSERT(strstr(fbuf, "evotest") == nullptr, "favorites file must not contain username");
    TEST_ASSERT(strstr(fbuf, "p@ss") == nullptr && strstr(fbuf, "w0rd") == nullptr, "favorites file must not contain password");

    TEST_ASSERT(favorites_is_favorite(credUrl) == 1, "favorites_is_favorite with credentialed URL must match");
    TEST_ASSERT(favorites_is_favorite("smb://192.168.0.8:4450/Media/Movie%20Name.mp4") == 1, "favorites_is_favorite with clean URL must match");

    favorites_remove(credUrl);
    TEST_ASSERT(favorite_count == 0, "favorites_remove with credentialed URL must remove clean entry");

    // 4. Load of an old file containing user:pass@ in Favorites -> migrated
    ffp = fopen(FAVORITES_FILE_DB, "w");
    TEST_ASSERT(ffp != nullptr, "open favorites DB for writing");
    fprintf(ffp, "smb://legacyuser:legacypass@10.0.0.5/Media/OldFav.mp4\tOld Fav\t150.000\n");
    fclose(ffp);

    favorites_load();
    TEST_ASSERT(favorite_count == 1, "favorites_load must load migrated entry");
    TEST_ASSERT(std::string(favorite_files[0].path) == "smb://10.0.0.5/Media/OldFav.mp4",
                "loaded favorite must be sanitized on load");
    favorites_save();

    ffp = fopen(FAVORITES_FILE_DB, "r");
    memset(fbuf, 0, sizeof(fbuf));
    fread_bytes = fread(fbuf, 1, sizeof(fbuf) - 1, ffp);
    fclose(ffp);
    fbuf[fread_bytes] = '\0';
    TEST_ASSERT(strstr(fbuf, "legacyuser") == nullptr && strstr(fbuf, "legacypass") == nullptr,
                "re-saved favorites file must not contain legacy credentials");

    TEST_PASS();
}

extern "C" void test_smb_attach_credentials(void) {
    TEST_START("SMB: attachCredentials Resolution & Edge Cases");

    std::vector<evo::NetworkShare> testShares;

    evo::NetworkShare s1;
    s1.id = "s1";
    s1.name = "MediaShare";
    s1.protocol = evo::NetworkProtocol::SMB;
    s1.host = "192.168.0.8";
    s1.port = 4450;
    s1.user = "evotest";
    s1.password = "p@ss:w0rd/1";
    testShares.push_back(s1);

    evo::NetworkShare s2;
    s2.id = "s2";
    s2.name = "DefaultNas";
    s2.protocol = evo::NetworkProtocol::SMB;
    s2.host = "nas.lan";
    s2.port = 0; // Default 445
    s2.user = "admin";
    s2.password = "secret";
    testShares.push_back(s2);

    evo::NetworkShare s3;
    s3.id = "s3";
    s3.name = "FtpCustom";
    s3.protocol = evo::NetworkProtocol::FTP;
    s3.host = "ftp.example.com";
    s3.port = 2121;
    s3.user = "ftpuser";
    s3.password = "ftppass";
    testShares.push_back(s3);

    evo::NetworkShare s4;
    s4.id = "s4";
    s4.name = "FtpDefault";
    s4.protocol = evo::NetworkProtocol::FTP;
    s4.host = "ftp.default.com";
    s4.port = 21;
    s4.user = "user";
    s4.password = "pass";
    testShares.push_back(s4);

    // 1. Matching SMB with custom port and awkward password
    std::string in1 = "smb://192.168.0.8:4450/Media/Movie%20Name.mp4";
    std::string out1 = evo::NetworkStorageService::attachCredentials(in1, testShares);
    TEST_ASSERT(out1 == "smb://evotest:p%40ss%3Aw0rd%2F1@192.168.0.8:4450/Media/Movie%20Name.mp4",
                "attachCredentials must attach percent-encoded credentials and retain encoded path");

    // 2. Matching SMB with default port (treated as 445)
    std::string in2 = "smb://nas.lan/Media/Movie.mp4";
    std::string out2 = evo::NetworkStorageService::attachCredentials(in2, testShares);
    TEST_ASSERT(out2 == "smb://admin:secret@nas.lan/Media/Movie.mp4",
                "attachCredentials must match share with default port 445");

    std::string in2_explicit = "smb://nas.lan:445/Media/Movie.mp4";
    std::string out2_explicit = evo::NetworkStorageService::attachCredentials(in2_explicit, testShares);
    TEST_ASSERT(out2_explicit == "smb://admin:secret@nas.lan/Media/Movie.mp4",
                "explicit :445 must match default port and omit default port in output");

    // 3. Non-matching share -> return unchanged
    std::string in3 = "smb://unknown.server:4450/Share/File.mp4";
    std::string out3 = evo::NetworkStorageService::attachCredentials(in3, testShares);
    TEST_ASSERT(out3 == in3, "non-matching share must return URL unchanged");

    // 4. URL that already has credentials -> return unchanged
    std::string in4 = "smb://custom:pass@192.168.0.8:4450/Media/Movie.mp4";
    std::string out4 = evo::NetworkStorageService::attachCredentials(in4, testShares);
    TEST_ASSERT(out4 == in4, "URL with credentials must return unchanged");

    // 5. FTP with custom port
    std::string in5 = "ftp://ftp.example.com:2121/pub/file.txt";
    std::string out5 = evo::NetworkStorageService::attachCredentials(in5, testShares);
    TEST_ASSERT(out5 == "ftp://ftpuser:ftppass@ftp.example.com:2121/pub/file.txt",
                "FTP with custom port must attach credentials");

    // 6. FTP with default port
    std::string in6 = "ftp://ftp.default.com/pub/file.txt";
    std::string out6 = evo::NetworkStorageService::attachCredentials(in6, testShares);
    TEST_ASSERT(out6 == "ftp://user:pass@ftp.default.com/pub/file.txt",
                "FTP with default port must attach credentials");

    // 7. Non-network paths -> return unchanged
    std::string in7 = "/mnt/usb0/video.mp4";
    std::string out7 = evo::NetworkStorageService::attachCredentials(in7, testShares);
    TEST_ASSERT(out7 == in7, "local path must return unchanged");

    TEST_PASS();
}
