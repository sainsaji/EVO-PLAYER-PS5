/*
 * evo_agc_probe.c - boot-time sceAgc reachability recon (app module only).
 * See evo_agc_probe.h. Compiled only under -DEVO_AGC_PROBE.
 *
 * The app module has NO kernel R/W, so the payload approach (agc_probe project:
 * kernel_dynlib_resolve) is out. Instead: sceKernelLoadStartModule the .sprx,
 * then sceKernelDlsym each symbol by its Sony NID (computed here from OpenSSL
 * SHA1 - libcrypto is already linked). Plain-name dlsym is tried as a fallback.
 */
#ifdef EVO_AGC_PROBE

#include "evo_agc_probe.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <openssl/sha.h>

/* --- app-module module loader + dlsym (userland; no stub header in EVO) --- */
extern int sceKernelLoadStartModule(const char *name, unsigned long argc,
                                    const void *argv, unsigned int flags,
                                    void *opt, int *res);
extern int sceKernelDlsym(int handle, const char *symbol, void **addr);

struct agc_note { char pad[45]; char msg[3075]; };
extern int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
extern int sceKernelDebugOutText(int, const char *);

static void note(const char *fmt, ...)
{
    struct agc_note n;
    char line[512];
    va_list ap;
    memset(&n, 0, sizeof n);
    va_start(ap, fmt);
    vsnprintf(n.msg, sizeof n.msg, fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
    snprintf(line, sizeof line, "%s\n", n.msg);
    sceKernelDebugOutText(0, line);
}

/*
 * Sony NID: SHA1(name + salt); take the first 8 bytes, reverse them, append 4
 * zero bytes, then base64 the 12 bytes MSB-first with the custom alphabet and
 * keep 11 chars. Verified byte-for-byte against the SDK's prospero-nid tool
 * (puts -> YQ0navp+YIc, sceAgcInit -> kW3GLb7QfPg). The byte reversal is what
 * makes this match the identifiers on-device modules actually publish - see
 * third_party/SharpProspero/tools/SharpProspero.Link/NidEncoder.cs.
 */
static void nid_of(const char *sym, char out[12])
{
    static const uint8_t salt[16] = {
        0x51,0x8D,0x64,0xA6,0x35,0xDE,0xD8,0xC1,
        0xE6,0xB0,0x39,0xB1,0xC3,0xE5,0x52,0x30};
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

    uint8_t buf[256], hash[SHA_DIGEST_LENGTH], d[9] = {0};
    size_t n = strlen(sym);
    if (n > sizeof(buf) - sizeof(salt)) n = sizeof(buf) - sizeof(salt);
    memcpy(buf, sym, n);
    memcpy(buf + n, salt, sizeof(salt));
    SHA1(buf, n + sizeof(salt), hash);

    for (int i = 0; i < 8; i++) d[i] = hash[7 - i];  /* d[8] stays zero */

    char tmp[12];
    int p = 0;
    for (int i = 0; i < 9; i += 3) {
        int abc = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        tmp[p++] = b64[(abc >> 18) & 0x3f];
        tmp[p++] = b64[(abc >> 12) & 0x3f];
        tmp[p++] = b64[(abc >> 6) & 0x3f];
        tmp[p++] = b64[abc & 0x3f];
    }
    memcpy(out, tmp, 11);
    out[11] = '\0';
}

static const char *const kSyms[] = {
    "sceAgcInit", "sceAgcGetRegisterDefaults", "sceAgcCreateShader",
    "sceAgcLinkShaders", "sceAgcDcbSetShRegistersIndirect",
    "sceAgcCbSetShRegisterRangeDirect", "sceAgcDcbDrawIndexAuto",
    "sceAgcDcbSetFlip", "sceAgcDriverSubmitDcb",
    "sceAgcDriverWaitUntilSafeForRendering",
};

static int load_module(const char *basename)
{
    static const char *const dirs[] = {
        "/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/",
    };
    char path[256];
    for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        int res = 0;
        snprintf(path, sizeof path, "%s%s", dirs[i], basename);
        int id = sceKernelLoadStartModule(path, 0, 0, 0, 0, &res);
        if (id >= 0)
            return id;
    }
    return -1;
}

void evo_agc_probe(void)
{
    int agc = load_module("libSceAgc.sprx");
    if (agc < 0) {
        note("EVO agc: libSceAgc.sprx load FAILED - Step 2 blocked "
             "(#27: switch to the PRX import stub, like libSceAvPlayer)");
        return;
    }

    int found = 0, by_nid = 0;
    const int total = (int)(sizeof kSyms / sizeof *kSyms);
    for (int i = 0; i < total; i++) {
        void *addr = 0;
        char nid[12];
        nid_of(kSyms[i], nid);
        if (sceKernelDlsym(agc, nid, &addr) == 0 && addr) { found++; by_nid++; continue; }
        if (sceKernelDlsym(agc, kSyms[i], &addr) == 0 && addr) { found++; continue; }
    }

    int agcd = load_module("libSceAgcDriver.sprx");

    note("EVO agc: libSceAgc modid=0x%x  %d/%d syms (nid=%d)  driver=%s  -> Step 2 %s",
         agc, found, total, by_nid, agcd >= 0 ? "loaded" : "folded",
         found >= total - 2 ? "VIABLE" : (found ? "partial" : "NOT AVAILABLE"));
}

#endif /* EVO_AGC_PROBE */
