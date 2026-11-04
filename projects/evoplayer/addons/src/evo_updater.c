/*
 * evo_updater.c - in-app self-update from GitHub releases (#120). See evo_updater.h.
 *
 * The release check uses evo_net; the download streams through FFmpeg's HTTP
 * client (evo_avio_stream, also used for the Xtream guide), which follows GitHub's
 * redirect to its release file host. The staging-then-rename handoff and the
 * "verify before anything is replaced" rule follow Nuvio Max'd's image updater.
 */
#include "evo_updater.h"
#include "evo_net.h"
#include "evo_boot_log.h"
#include "cJSON.h"

#include <openssl/evp.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "v0.13.5"
#endif

#define UPD_RELEASE_API  "https://api.github.com/repos/sainsaji/EVO-PLAYER-PS5/releases/latest"
#define UPD_DOWNLOAD_PFX "https://github.com/sainsaji/EVO-PLAYER-PS5/releases/download/"
#define UPD_TITLE_ID     "PPSA99039"
#define UPD_MIN_BYTES    (1u << 20)
#define UPD_MAX_BYTES    (512u << 20)

extern int evo_avio_stream(const char *url, int (*sink)(const char *, size_t, void *),
                           void *ud, int *out_status) __attribute__((weak));

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static evo_updater_status_t s_st;
static char s_url[EVO_NET_MAX_URL];
static char s_digest[65];
static char s_image[256];          /* the installed image */
static char s_stage[272];          /* the staging file beside it */
static int  s_cancel;
static int  s_new_flag;
static int  s_busy;                /* a worker thread is running */

static void set_state(evo_upd_state_t st, const char *err)
{
    pthread_mutex_lock(&s_mx);
    s_st.state = st;
    if (err) snprintf(s_st.error, sizeof s_st.error, "%s", err);
    else if (st != EVO_UPD_FAILED) s_st.error[0] = '\0';
    pthread_mutex_unlock(&s_mx);
}

static int parse_version(const char *s, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    if (*s == 'v' || *s == 'V') ++s;
    if (*s < '0' || *s > '9') return 0;
    for (int i = 0; i < 3; ++i) {
        v[i] = atoi(s);
        while (*s >= '0' && *s <= '9') ++s;
        if (*s != '.') break;
        ++s;
    }
    return 1;
}

int evo_updater_version_newer(const char *a, const char *b)
{
    int x[3], y[3];
    if (!parse_version(a, x) || !parse_version(b, y)) return 0;
    for (int i = 0; i < 3; ++i)
        if (x[i] != y[i]) return x[i] > y[i];
    return 0;
}

/* Where ShadowMount+ mounts the image from: the default folder, then the drives. */
static int find_installed_image(void)
{
    static const char *const dirs[] = {
        "/data/homebrew", "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew",
        "/mnt/usb3/homebrew", "/mnt/ext0/homebrew", "/mnt/ext1/homebrew", NULL
    };
    for (int i = 0; dirs[i]; ++i) {
        char p[256];
        struct stat sb;
        snprintf(p, sizeof p, "%s/%s.ffpfsc", dirs[i], UPD_TITLE_ID);
        if (stat(p, &sb) == 0 && S_ISREG(sb.st_mode)) {
            snprintf(s_image, sizeof s_image, "%s", p);
            snprintf(s_stage, sizeof s_stage, "%s.evo-update", p);
            return 1;
        }
    }
    return 0;
}

static int ends_with(const char *s, const char *suffix)
{
    const size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static int g_prefer_pro;

static void *check_thread(void *arg)
{
    (void)arg;
    const char *headers[] = { "Accept: application/vnd.github+json" };
    char *body = NULL;
    size_t len = 0;
    int status = 0;
    const int rc = evo_net_http_get_sync(UPD_RELEASE_API, headers, 1, &body, &len, &status);
    if (rc != EVO_NET_OK || !body) {
        char why[160];
        evo_net_describe_failure(rc, status, UPD_RELEASE_API, why, sizeof why);
        evo_boot_log("updater: check failed: %s", why);
        set_state(EVO_UPD_FAILED, "Could not reach GitHub");
        free(body);
        goto done;
    }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    const cJSON *tag = root ? cJSON_GetObjectItemCaseSensitive(root, "tag_name") : NULL;
    if (!cJSON_IsString(tag) || !tag->valuestring) {
        cJSON_Delete(root);
        set_state(EVO_UPD_FAILED, "GitHub answered with something unexpected");
        goto done;
    }

    if (!evo_updater_version_newer(tag->valuestring, EVO_PLAYER_VERSION)) {
        evo_boot_log("updater: %s is the latest (running %s)", tag->valuestring, EVO_PLAYER_VERSION);
        cJSON_Delete(root);
        set_state(EVO_UPD_UP_TO_DATE, NULL);
        goto done;
    }

    const char *want = g_prefer_pro ? "-" UPD_TITLE_ID "-pro.ffpfsc" : "-" UPD_TITLE_ID ".ffpfsc";
    const cJSON *asset = NULL, *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    cJSON_ArrayForEach(asset, assets) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        if (cJSON_IsString(name) && name->valuestring && ends_with(name->valuestring, want)) break;
    }
    const cJSON *url = asset ? cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url") : NULL;
    const cJSON *size = asset ? cJSON_GetObjectItemCaseSensitive(asset, "size") : NULL;
    const cJSON *digest = asset ? cJSON_GetObjectItemCaseSensitive(asset, "digest") : NULL;
    const char *err = NULL;
    if (!asset) err = "The release has no image for this build";
    else if (!cJSON_IsString(url) || strncmp(url->valuestring, UPD_DOWNLOAD_PFX, strlen(UPD_DOWNLOAD_PFX)) != 0
             || strlen(url->valuestring) >= sizeof s_url) err = "The release address is not trusted";
    else if (!cJSON_IsNumber(size) || size->valuedouble < UPD_MIN_BYTES || size->valuedouble > UPD_MAX_BYTES)
        err = "The release image has an odd size";
    else if (!cJSON_IsString(digest) || strncmp(digest->valuestring, "sha256:", 7) != 0
             || strlen(digest->valuestring) != 7 + 64) err = "The release has no checksum";
    if (err) {
        evo_boot_log("updater: refused %s: %s", tag->valuestring, err);
        cJSON_Delete(root);
        set_state(EVO_UPD_FAILED, err);
        goto done;
    }

    const cJSON *notes = cJSON_GetObjectItemCaseSensitive(root, "body");
    pthread_mutex_lock(&s_mx);
    snprintf(s_url, sizeof s_url, "%s", url->valuestring);
    snprintf(s_digest, sizeof s_digest, "%s", digest->valuestring + 7);
    snprintf(s_st.latest, sizeof s_st.latest, "%s", tag->valuestring);
    snprintf(s_st.notes, sizeof s_st.notes, "%s",
             cJSON_IsString(notes) && notes->valuestring ? notes->valuestring : "");
    /* Release bodies are "<what changed>\n---\n<install instructions>": keep the first part. */
    char *rule = strstr(s_st.notes, "\n---");
    if (rule) *rule = '\0';
    s_st.total = (uint64_t)size->valuedouble;
    s_st.done = 0;
    s_st.state = EVO_UPD_AVAILABLE;
    s_st.error[0] = '\0';
    s_new_flag = 1;
    pthread_mutex_unlock(&s_mx);
    evo_boot_log("updater: %s available (%llu bytes)", tag->valuestring, (unsigned long long)s_st.total);
    cJSON_Delete(root);

done:
    pthread_mutex_lock(&s_mx);
    s_busy = 0;
    pthread_mutex_unlock(&s_mx);
    return NULL;
}

static int spawn(void *(*fn)(void *))
{
    pthread_t t;
    if (pthread_create(&t, NULL, fn, NULL) != 0) return 0;
    pthread_detach(t);
    return 1;
}

void evo_updater_check(int prefer_pro)
{
    pthread_mutex_lock(&s_mx);
    const int ok = !s_busy && s_st.state != EVO_UPD_DOWNLOADING && s_st.state != EVO_UPD_READY
                   && s_st.state != EVO_UPD_APPLIED;
    if (ok) {
        s_busy = 1;
        g_prefer_pro = prefer_pro;
        s_st.state = EVO_UPD_CHECKING;
        s_st.error[0] = '\0';
    }
    pthread_mutex_unlock(&s_mx);
    if (ok && !spawn(check_thread)) {
        pthread_mutex_lock(&s_mx);
        s_busy = 0;
        pthread_mutex_unlock(&s_mx);
        set_state(EVO_UPD_FAILED, "Could not start the check");
    }
}

typedef struct {
    EVP_MD_CTX *md;
    FILE       *fp;
    int         failed;
} dl_t;

static int dl_sink(const char *data, size_t n, void *ud)
{
    dl_t *d = (dl_t *)ud;
    if (s_cancel) return 1;
    pthread_mutex_lock(&s_mx);
    const int over = s_st.done + n > s_st.total;
    pthread_mutex_unlock(&s_mx);
    if (over || fwrite(data, 1, n, d->fp) != n || EVP_DigestUpdate(d->md, data, n) != 1) {
        d->failed = 1;
        return 1;
    }
    pthread_mutex_lock(&s_mx);
    s_st.done += n;
    pthread_mutex_unlock(&s_mx);
    return 0;
}

static void *download_thread(void *arg)
{
    (void)arg;
    const char *err = NULL;
    dl_t d = { EVP_MD_CTX_new(), NULL, 0 };
    char hex[65] = "";

    if (!evo_avio_stream) err = "Downloads are not available in this build";
    else if (!d.md || EVP_DigestInit_ex(d.md, EVP_sha256(), NULL) != 1) err = "Could not start the checksum";
    else if (!(d.fp = fopen(s_stage, "wb"))) err = "Could not write beside the installed image";
    if (err) goto fail;

    int status = 0;
    const int rc = evo_avio_stream(s_url, dl_sink, &d, &status);
    if (fclose(d.fp) != 0 && rc == EVO_NET_OK) d.failed = 1;
    d.fp = NULL;

    if (s_cancel) { err = "Cancelled"; goto fail; }
    if (rc != EVO_NET_OK || d.failed) { err = "The download did not finish"; goto fail; }

    pthread_mutex_lock(&s_mx);
    const uint64_t done = s_st.done, total = s_st.total;
    pthread_mutex_unlock(&s_mx);
    if (done != total) { err = "The download was cut short"; goto fail; }

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    if (EVP_DigestFinal_ex(d.md, md, &mdlen) != 1 || mdlen != 32) { err = "Checksum failed"; goto fail; }
    for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", md[i]);
    if (strcasecmp(hex, s_digest) != 0) { err = "The download does not match GitHub's checksum"; goto fail; }

    EVP_MD_CTX_free(d.md);
    evo_boot_log("updater: %s downloaded and verified", s_st.latest);
    set_state(EVO_UPD_READY, NULL);
    goto out;

fail:
    if (d.fp) fclose(d.fp);
    if (d.md) EVP_MD_CTX_free(d.md);
    remove(s_stage);          /* never leave an unverified image behind */
    evo_boot_log("updater: download failed: %s", err);
    if (s_cancel) {
        pthread_mutex_lock(&s_mx);
        s_st.state = EVO_UPD_AVAILABLE;
        s_st.done = 0;
        pthread_mutex_unlock(&s_mx);
    } else {
        set_state(EVO_UPD_FAILED, err);
    }
out:
    pthread_mutex_lock(&s_mx);
    s_busy = 0;
    pthread_mutex_unlock(&s_mx);
    return NULL;
}

int evo_updater_download(void)
{
    pthread_mutex_lock(&s_mx);
    int ok = !s_busy && s_st.state == EVO_UPD_AVAILABLE;
    pthread_mutex_unlock(&s_mx);
    if (!ok) return 0;
    if (!find_installed_image()) {
        set_state(EVO_UPD_FAILED, "EVO's image was not found in a homebrew folder");
        return 0;
    }
    pthread_mutex_lock(&s_mx);
    s_busy = 1;
    s_cancel = 0;
    s_st.state = EVO_UPD_DOWNLOADING;
    s_st.done = 0;
    pthread_mutex_unlock(&s_mx);
    if (!spawn(download_thread)) {
        pthread_mutex_lock(&s_mx);
        s_busy = 0;
        pthread_mutex_unlock(&s_mx);
        set_state(EVO_UPD_FAILED, "Could not start the download");
        return 0;
    }
    return 1;
}

void evo_updater_cancel(void)
{
    s_cancel = 1;
}

int evo_updater_apply(void)
{
    pthread_mutex_lock(&s_mx);
    const int ok = s_st.state == EVO_UPD_READY;
    pthread_mutex_unlock(&s_mx);
    if (!ok) return 0;

    /* One rename is the whole swap. No backup: link() is missing from the app
     * module's libc (null import, SIGSEGV at 0 on hardware), and the previous
     * release is still on GitHub. */
    if (rename(s_stage, s_image) != 0) {
        evo_boot_log("updater: rename failed");
        remove(s_stage);
        set_state(EVO_UPD_FAILED, "Could not replace the installed image");
        return 0;
    }
    evo_boot_log("updater: %s installed over %s", s_st.latest, s_image);
    set_state(EVO_UPD_APPLIED, NULL);
    return 1;
}

void evo_updater_get(evo_updater_status_t *out)
{
    pthread_mutex_lock(&s_mx);
    *out = s_st;
    pthread_mutex_unlock(&s_mx);
}

void evo_updater_plain_notes(const char *notes, char *out, unsigned cap)
{
    unsigned n = 0;
    int prose_done = 0;
    if (!cap) return;
    out[0] = '\0';
    while (notes && *notes && n + 1 < cap) {
        const char *eol = strchr(notes, '\n');
        const size_t len = eol ? (size_t)(eol - notes) : strlen(notes);
        char line[512];
        unsigned m = 0;
        const char *p = notes, *end = notes + len;
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        const int bullet = (end - p > 2 && (*p == '-' || *p == '+') && p[1] == ' ');
        const int heading = (p < end && *p == '#');
        if (bullet) p += 2;
        /* Markup out: images go whole, a link keeps its text, tags and emphasis marks go. */
        for (; p < end && m + 1 < sizeof line; ++p) {
            if (p[0] == '!' && p + 1 < end && p[1] == '[') {
                while (p < end && *p != ')') ++p;
                continue;
            }
            if (*p == '[') continue;
            if (*p == ']' && p + 1 < end && p[1] == '(') {
                while (p < end && *p != ')') ++p;
                continue;
            }
            if (*p == '<') { while (p < end && *p != '>') ++p; continue; }
            if (*p == '*' || *p == '`' || *p == '#' || *p == '\r') continue;
            if (*p == ' ' && (m == 0 || line[m - 1] == ' ')) continue;
            line[m++] = *p;
        }
        while (m && line[m - 1] == ' ') --m;
        line[m] = '\0';
        notes = eol ? eol + 1 : notes + len;

        if (!m || heading) continue;
        if (bullet) {
            /* The bold lead of a bullet is its headline: stop at the first full stop. */
            char *dot = strstr(line, ". ");
            if (dot) dot[1] = '\0';
        } else if (prose_done || strncmp(line, "Download", 8) == 0 || strncmp(line, "Questions", 9) == 0) {
            continue;
        } else {
            char *dot = strstr(line, ". ");
            if (dot) dot[1] = '\0';
            prose_done = 1;
        }
        const int w = snprintf(out + n, cap - n, "%s%s", n ? " - " : "", line);
        if (w < 0) break;
        n += (unsigned)w;
        if (n + 1 > cap) { n = cap - 1; break; }
    }
    out[n < cap ? n : cap - 1] = '\0';
}

int evo_updater_take_new_flag(void)
{
    pthread_mutex_lock(&s_mx);
    const int f = s_new_flag;
    s_new_flag = 0;
    pthread_mutex_unlock(&s_mx);
    return f;
}
