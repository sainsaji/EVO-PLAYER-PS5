/*
 * tests/test_smb.cpp — Unit tests for SMB client and network storage integration.
 */

#include "evo/services/SmbClient.hpp"
#include "evo/services/NetworkStorageService.hpp"
#include "evo_data_path.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>

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

    // 3. Authenticated SMB with custom port
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
    TEST_ASSERT(url2 == "smb://admin:P@ss123@10.0.0.5:4450/SecretShare/doc.pdf", "auth URL format mismatch");

    // 4. File classification
    TEST_ASSERT(service.classifyFileName("movie.mkv") == evo::FileCategory::Video, "mkv is video");
    TEST_ASSERT(service.classifyFileName("track.flac") == evo::FileCategory::Audio, "flac is audio");
    TEST_ASSERT(service.classifyFileName("subs.srt") == evo::FileCategory::Document, "srt is document");
    TEST_ASSERT(service.classifyFileName("poster.jpg") == evo::FileCategory::Image, "jpg is image");

    // 5. Cleanup
    service.removeShare("smb_test_nas");
    service.removeShare("smb_auth");
    TEST_ASSERT(service.getShare("smb_test_nas") == nullptr, "share should be removed");

    TEST_PASS();
}
