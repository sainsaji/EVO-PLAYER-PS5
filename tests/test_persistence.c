/*
 * test_persistence.c - host tests for the data root (#46) and the recent /
 * favorites stores (#50).
 *
 * The app-module branch of resolve_root() (/data vs /download0, keyed on
 * evo_jailbreak_is_open()) is not covered: the host build compiles the
 * EVO_DATA_DIR_OVERRIDE branch instead, and testing the other one needs a
 * second object built with -DEVO_APP_MODULE and a stubbed sceKernelMkdir.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "evo_data_path.h"
#include "evo_recent.h"
#include "evo_favorites.h"

extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;

#define TEST_START(name) do { g_tests_run++; printf("  RUN  %-50s", name); fflush(stdout); } while (0)
#define TEST_PASS() do { g_tests_passed++; printf(" [ PASS ]\n"); } while (0)
#define TEST_FAIL(reason) do { g_tests_failed++; printf(" [ FAIL ] (%s at line %d)\n", reason, __LINE__); } while (0)
#define TEST_ASSERT(cond, reason) do { if (!(cond)) { TEST_FAIL(reason); goto done; } } while (0)

/* Point the data root at a fresh scratch dir; restore() puts the old one back. */
static char g_saved_root[512];

static int use_scratch_root(char *dir, size_t cap)
{
    snprintf(g_saved_root, sizeof g_saved_root, "%s", evo_data_dir());
    snprintf(dir, cap, "/tmp/evo_persist_XXXXXX");
    if (!mkdtemp(dir)) return -1;
    setenv("EVO_DATA_DIR_OVERRIDE", dir, 1);
    evo_data_path_rebind();
    return 0;
}

static void restore_root(const char *dir, const char *const *files)
{
    char p[600];
    for (; files && *files; files++) {
        snprintf(p, sizeof p, "%s/%s", dir, *files);
        unlink(p);
    }
    rmdir(dir);
    setenv("EVO_DATA_DIR_OVERRIDE", g_saved_root, 1);
    evo_data_path_rebind();
}

static int write_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fputs(text, fp);
    fclose(fp);
    return 0;
}

static void test_data_path_join(void)
{
    TEST_START("Data root: evo_data_path() join and overflow");
    char want[700];
    const char *root = evo_data_dir();

    snprintf(want, sizeof want, "%s/emby.conf", root);
    TEST_ASSERT(strcmp(evo_data_path("emby.conf"), want) == 0, "plain leaf join");
    TEST_ASSERT(strcmp(evo_data_path("//emby.conf"), want) == 0, "leading slashes not stripped");

    snprintf(want, sizeof want, "%s/", root);
    TEST_ASSERT(strcmp(evo_data_path(NULL), want) == 0, "NULL leaf is not the root plus '/'");
    TEST_ASSERT(strcmp(evo_data_path(""), want) == 0, "empty leaf is not the root plus '/'");

    {
        char longleaf[600];
        memset(longleaf, 'a', sizeof longleaf - 1);
        longleaf[sizeof longleaf - 1] = '\0';
        TEST_ASSERT(evo_data_path(longleaf) == evo_data_dir(), "overflow does not return evo_data_dir()");
    }
    TEST_PASS();
done:;
}

static void test_data_path_rebind(void)
{
    TEST_START("Data root: override + rebind re-resolves");
    char dir[64], want[600];
    const char *before = evo_data_dir();
    char saved[512];
    snprintf(saved, sizeof saved, "%s", before);

    TEST_ASSERT(strcmp(before, "/tmp/evo_test_data") == 0, "host root is not the override set in main()");
    TEST_ASSERT(use_scratch_root(dir, sizeof dir) == 0, "mkdtemp failed");
    if (strcmp(evo_data_dir(), dir) != 0) {
        restore_root(dir, NULL);
        TEST_FAIL("rebind did not pick up the new override");
        return;
    }
    snprintf(want, sizeof want, "%s/x.conf", dir);
    if (strcmp(evo_data_path("x.conf"), want) != 0) {
        restore_root(dir, NULL);
        TEST_FAIL("evo_data_path() still uses the old root");
        return;
    }
    restore_root(dir, NULL);
    TEST_ASSERT(strcmp(evo_data_dir(), saved) == 0, "root not restored");
    TEST_PASS();
done:;
}

static void test_data_mkdir(void)
{
    TEST_START("Data root: evo_mkdir() new, existing, blocked");
    char dir[64] = "/tmp/evo_mkdir_XXXXXX", sub[128], file[128], bad[160];
    TEST_ASSERT(mkdtemp(dir) != NULL, "mkdtemp failed");
    snprintf(sub, sizeof sub, "%s/sub", dir);
    snprintf(file, sizeof file, "%s/file.txt", dir);
    snprintf(bad, sizeof bad, "%s/file.txt/sub", dir);
    write_file(file, "x");

    int fresh = evo_mkdir(sub);
    int again = evo_mkdir(sub);
    int under_file = evo_mkdir(bad);
    struct stat st;
    int is_dir = stat(sub, &st) == 0 && S_ISDIR(st.st_mode);

    rmdir(sub);
    unlink(file);
    rmdir(dir);
    TEST_ASSERT(fresh == 0 && is_dir, "new directory not created");
    TEST_ASSERT(again == 0, "an existing directory is not success (EEXIST)");
    TEST_ASSERT(under_file != 0, "mkdir under a regular file reported success");
    TEST_PASS();
done:;
}

static const char *const k_recent_files[] = { "evo_recent.txt", NULL };
static const char *const k_fav_files[] = { "evo_favorites.txt", NULL };

static void test_recent_parse(void)
{
    TEST_START("Recent: tolerant parse, reload, lookup, cap");
    char dir[64];
    int ok = 0;
    const char *why = "";
    if (use_scratch_root(dir, sizeof dir) != 0) { TEST_FAIL("mkdtemp failed"); return; }

    do {
        /* good, path only (skipped), blank (skipped), good with CRLF */
        write_file(RECENT_FILE_DB,
                   "/mnt/usb0/a.mkv\tA\t12.500\t100.000\t1700000000\t2\n"
                   "/mnt/usb0/only_path.mkv\n"
                   "\n"
                   "/mnt/usb0/b.mp4\tB\t3.000\t50.000\t1700000001\t-1\r\n");
        recent_load();
        if (recent_file_count != 2) { why = "bad lines not skipped"; break; }
        if (strcmp(recent_files[1].path, "/mnt/usb0/b.mp4") != 0) { why = "CRLF line path wrong"; break; }
        if (recent_files[1].audio_stream != -1) { why = "CRLF line audio stream wrong"; break; }
        if (recent_files[0].last_played != 1700000000LL) { why = "last_played not parsed"; break; }

        recent_load();
        if (recent_file_count != 2) { why = "a second load appended instead of replacing"; break; }

        if (recent_lookup("/mnt/usb0/a.mkv") != 12.5) { why = "lookup hit returns the wrong position"; break; }
        if (recent_lookup("/mnt/usb0/missing.mkv") != 0.0) { why = "lookup miss is not 0"; break; }
        if (recent_lookup_audio("/mnt/usb0/a.mkv") != 2) { why = "audio lookup wrong"; break; }

        {
            FILE *fp = fopen(RECENT_FILE_DB, "w");
            if (!fp) { why = "cannot write recent file"; break; }
            for (int i = 0; i < MAX_RECENT_FILES + 5; i++)
                fprintf(fp, "/mnt/usb0/f%d.mkv\tF%d\t1.000\t2.000\t%d\t-1\n", i, i, i);
            fclose(fp);
        }
        recent_load();
        if (recent_file_count != MAX_RECENT_FILES) { why = "MAX_RECENT_FILES cap not enforced on load"; break; }
        ok = 1;
    } while (0);

    recent_clear();
    restore_root(dir, k_recent_files);
    if (ok) TEST_PASS(); else TEST_FAIL(why);
}

static void test_recent_roundtrip(void)
{
    TEST_START("Recent: add -> save -> load round-trip");
    char dir[64];
    int ok = 0;
    const char *why = "";
    if (use_scratch_root(dir, sizeof dir) != 0) { TEST_FAIL("mkdtemp failed"); return; }

    do {
        recent_clear();
        recent_add_or_update("/mnt/usb0/movie.mkv", "Movie", 42.0, 6000.0, 1);
        recent_add_or_update("/mnt/usb0/show.mkv", "Show", 7.5, 1500.0, -1);
        int n = recent_file_count;
        recent_save();
        recent_clear();
        if (recent_file_count != 0) { why = "recent_clear() left entries"; break; }
        recent_load();
        if (recent_file_count != n || n != 2) { why = "entry count changed across save/load"; break; }
        if (recent_lookup("/mnt/usb0/movie.mkv") != 42.0) { why = "movie position lost"; break; }
        if (recent_lookup("/mnt/usb0/show.mkv") != 7.5) { why = "show position lost"; break; }
        if (recent_lookup_audio("/mnt/usb0/movie.mkv") != 1) { why = "audio stream lost"; break; }
        ok = 1;
    } while (0);

    recent_clear();
    restore_root(dir, k_recent_files);
    if (ok) TEST_PASS(); else TEST_FAIL(why);
}

static void test_favorites_parse(void)
{
    TEST_START("Favorites: tolerant parse, reload, find, cap");
    char dir[64];
    int ok = 0;
    const char *why = "";
    if (use_scratch_root(dir, sizeof dir) != 0) { TEST_FAIL("mkdtemp failed"); return; }

    do {
        write_file(FAVORITES_FILE_DB,
                   "/mnt/usb0/a.mkv\tA\t100.000\n"
                   "/mnt/usb0/only_path.mkv\n"
                   "\n"
                   "/mnt/usb0/b.mp4\tB\t50.000\r\n");
        favorites_load();
        if (favorite_count != 2) { why = "bad lines not skipped"; break; }
        if (favorite_files[1].duration != 50.0) { why = "CRLF line duration wrong"; break; }

        favorites_load();
        if (favorite_count != 2) { why = "a second load appended instead of replacing"; break; }

        if (favorites_find("/mnt/usb0/b.mp4") != 1) { why = "find hit returns the wrong index"; break; }
        if (favorites_find("/mnt/usb0/missing.mkv") >= 0) { why = "find miss is not negative"; break; }
        if (!favorites_is_favorite("/mnt/usb0/a.mkv")) { why = "is_favorite hit is false"; break; }
        if (favorites_is_favorite("/mnt/usb0/missing.mkv")) { why = "is_favorite miss is true"; break; }

        {
            FILE *fp = fopen(FAVORITES_FILE_DB, "w");
            if (!fp) { why = "cannot write favorites file"; break; }
            for (int i = 0; i < MAX_FAVORITES + 5; i++)
                fprintf(fp, "/mnt/usb0/f%d.mkv\tF%d\t1.000\n", i, i);
            fclose(fp);
        }
        favorites_load();
        if (favorite_count != MAX_FAVORITES) { why = "MAX_FAVORITES cap not enforced on load"; break; }
        ok = 1;
    } while (0);

    favorites_clear();
    restore_root(dir, k_fav_files);
    if (ok) TEST_PASS(); else TEST_FAIL(why);
}

static void test_favorites_roundtrip(void)
{
    TEST_START("Favorites: add -> save -> load round-trip");
    char dir[64];
    int ok = 0;
    const char *why = "";
    if (use_scratch_root(dir, sizeof dir) != 0) { TEST_FAIL("mkdtemp failed"); return; }

    do {
        favorites_clear();
        favorites_add("/mnt/usb0/movie.mkv", "Movie", 6000.0);
        favorites_add("/mnt/usb0/show.mkv", "Show", 1500.0);
        favorites_save();
        favorites_clear();
        if (favorite_count != 0) { why = "favorites_clear() left entries"; break; }
        favorites_load();
        if (favorite_count != 2) { why = "entry count changed across save/load"; break; }
        int i = favorites_find("/mnt/usb0/movie.mkv");
        if (i < 0) { why = "movie lost"; break; }
        if (strcmp(favorite_files[i].title, "Movie") != 0) { why = "title lost"; break; }
        if (favorite_files[i].duration != 6000.0) { why = "duration lost"; break; }
        favorites_remove("/mnt/usb0/movie.mkv");
        if (favorites_is_favorite("/mnt/usb0/movie.mkv") || favorite_count != 1) { why = "remove failed"; break; }
        ok = 1;
    } while (0);

    favorites_clear();
    restore_root(dir, k_fav_files);
    if (ok) TEST_PASS(); else TEST_FAIL(why);
}

void test_persistence_all(void)
{
    test_data_path_join();
    test_data_path_rebind();
    test_data_mkdir();
    test_recent_parse();
    test_recent_roundtrip();
    test_favorites_parse();
    test_favorites_roundtrip();
}
