/* EVO Player - decoder_test
 *
 * PHASE 0 of the hardware-decode research plan: get the bytes off the console.
 *
 * WHY THIS SHAPE
 *   docs/hardware-decode-review.md argues the original plan's cost model is
 *   inverted. Guessing at sceAvPlayerInit's argument struct costs one console
 *   deploy per guess and teaches you an error code. But the media modules are
 *   already loaded, mapped and therefore DECRYPTED in this payload's own
 *   address space - so one deploy that copies those images to USB converts
 *   every remaining ABI question into an offline llvm-objdump question.
 *
 *   So this program is a dumper first and a symbol probe second. It makes NO
 *   media API calls: everything here is read-only reconnaissance.
 *
 * THREE THINGS MEASURED ON 12.70 THAT DICTATE THE DESIGN (2026-08-10)
 *
 *   1. sceKernelGetModuleList() reports exactly ONE module ("eboot.bin") in an
 *      elfldr payload, even after three SPRXes have loaded successfully. The
 *      userland module *list* is useless here.
 *
 *   2. sceKernelGetModuleInfo(modid) works perfectly, and the PS4
 *      SceKernelModuleInfo layout - 0x160 bytes: st_size, name[256], four
 *      {addr,size,prot} segments, count, fingerprint - parses correctly. So
 *      the exact segment table is available and nothing here guesses an
 *      extent. Since the list API cannot supply modids, this sweeps the handle
 *      space and keeps whatever GetModuleInfo answers for. That sweep is also
 *      the module enumeration the review asks for.
 *
 *   3. sceKernelVirtualQuery() FAILS in a payload, against a module known to
 *      be mapped. Measured with a control. Mapping-walk sizing is therefore
 *      not available and is not attempted.
 *
 * WHY NOTHING HERE SPECULATIVELY DEREFERENCES MODULE MEMORY
 *   libSceAvPlayer's first segment is mapped prot=4: EXECUTE-ONLY. A memcpy
 *   from it killed the payload outright on 2026-08-10 - and the SIGSEGV/SIGBUS
 *   sigaction handler installed below did NOT rescue it. Signal-guarded
 *   probing is not a usable safety net in an elfldr payload.
 *
 *   So the read method is chosen from the segment's declared protection plus a
 *   kernel-mediated probe that cannot fault this process at all
 *   (kernel_proc_copyout). The handler stays installed as a last resort, but
 *   nothing depends on it.
 *
 * WHAT IT PRODUCES ON /mnt/usb0
 *   evo_dump_log.txt              everything below, as text
 *   evo_dump_manifest.txt         one line per dumped file, machine-readable
 *   evo_dump_<module>_s<N>.bin    raw bytes of segment N of each module
 *   evo_dump_WATCHDOG.txt         only if the watchdog fired
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <setjmp.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>   /* nid_encode() - Sony symbol-name hashing */

#include "evo_ps5.h"
#include "nid_table.h" /* generated - see tools/re/gen_nid_table.py */

#define USB_DIR          "/mnt/usb0"
#define LOG_PATH         USB_DIR "/evo_dump_log.txt"
#define MANIFEST_PATH    USB_DIR "/evo_dump_manifest.txt"
#define EXPORTS_PATH     USB_DIR "/evo_dump_exports.txt"
#define WATCHDOG_PATH    USB_DIR "/evo_dump_WATCHDOG.txt"

/* Hard ceiling on the whole run. Nothing here holds VideoOut, so an overrun
 * is cheap - but it still ties up the elfldr socket, so bound it. */
#define WATCHDOG_SECONDS 120

#define DUMP_BUDGET      (64u * 1024u * 1024u)
#define MAX_MODID        0x100
#define MAX_MODULES      128
#define CHUNK            (64u * 1024u)

/* Modules to load and dump.
 *
 * The first three are the review's phase-0 targets. libSceVideodec2 was added
 * once Phase 1 established that IT, not libSceVdecCore, exports the public
 * decode API - reading its prologues offline is what makes the first call into
 * it an informed one rather than a guess. Vdecwrap/Shevc are cheap and sit on
 * the same path.
 *
 * libSceAudiodec.sprx is deliberately NOT here. Loading it HANGS the payload
 * when it is loaded straight after the video modules - reproduced twice, on
 * 2026-08-10. It loads fine in the export-map pass, which reaches it only
 * after libSceGnmDriver and libSceAjm are already loaded, so the reading is
 * that its module init blocks on one of those. Audio is not on this critical
 * path; leave it to the pass that survives it. */
static const char *const kTargets[] = {
    "libSceAvPlayer.sprx",
    "libSceVdecCore.sprx",
    "libSceVideoDecoderArbitration.sprx",
    "libSceVideodec2.sprx",
    "libSceVideodec.sprx",
    "libSceVdecwrap.sprx",
    "libSceVdecShevc.sprx",
};

/* Symbols worth looking for. These names come from the public PS4
 * AvPlayer/Vdec ABI; on PS5 the AvPlayer six resolve and the Vdec five do
 * not, which is the naming problem Route B has to solve. */
static const char *const kAvPlayerSymbols[] = {
    "sceAvPlayerInit",
    "sceAvPlayerAddSource",
    "sceAvPlayerGetVideoData",
    "sceAvPlayerGetAudioData",
    "sceAvPlayerIsActive",
    "sceAvPlayerClose",
};

/* ------------------------------------------------------------------------ */
/* Logging                                                                   */
/* ------------------------------------------------------------------------ */

static FILE *g_log;
static FILE *g_manifest;
static char  g_stage[160] = "startup";
static unsigned long g_written;

static void
LOG(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        /* Flush every line: if this run dies on a fault, the log is still the
         * record of exactly where it died - which is how the execute-only
         * text mapping was diagnosed in the first place. */
        fflush(g_log);
    }
}

static void
stage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_stage, sizeof g_stage, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------ */
/* Watchdog                                                                  */
/* ------------------------------------------------------------------------ */

static void *
watchdog_main(void *arg)
{
    unsigned left = (unsigned)(uintptr_t)arg;

    while (left--)
        sleep(1);

    FILE *f = fopen(WATCHDOG_PATH, "w");
    if (f) {
        fprintf(f, "watchdog fired after %d seconds\nlast stage: %s\n"
                   "bytes written so far: %lu\n",
                WATCHDOG_SECONDS, g_stage, g_written);
        fclose(f);
    }
    if (g_log) {
        fprintf(g_log, "\n!!! WATCHDOG fired after %d s at stage: %s\n",
                WATCHDOG_SECONDS, g_stage);
        fclose(g_log);
    }
    if (g_manifest)
        fclose(g_manifest);

    _exit(2);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Reading module memory                                                     */
/* ------------------------------------------------------------------------ */

/* Installed but NOT relied upon - see the header comment. Keeping it costs
 * nothing and it would catch a fault in the readable segments. */
static sigjmp_buf g_fault_jmp;
static volatile sig_atomic_t g_fault_armed;

static void
fault_handler(int sig)
{
    if (g_fault_armed) {
        g_fault_armed = 0;
        siglongjmp(g_fault_jmp, sig);
    }
    _exit(3);
}

static void
install_fault_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
}

static int
guarded_memcpy(void *dst, const void *src, size_t len)
{
    if (sigsetjmp(g_fault_jmp, 1) != 0) {
        g_fault_armed = 0;
        return 0;
    }
    g_fault_armed = 1;
    memcpy(dst, src, len);
    g_fault_armed = 0;
    return 1;
}

enum read_method { RM_NONE = 0, RM_MEMCPY, RM_MPROTECT, RM_PROC_COPYOUT };

static const char *
read_method_name(enum read_method m)
{
    switch (m) {
    case RM_MEMCPY:       return "memcpy";
    case RM_MPROTECT:     return "kernel_mprotect+memcpy";
    case RM_PROC_COPYOUT: return "kernel_proc_copyout";
    default:              return "none";
    }
}

/* Choose how to read a segment WITHOUT ever risking a fault.
 *
 * kernel_proc_copyout goes through the kernel, so it can read a page this
 * process is not permitted to read and it cannot kill us if it is wrong. It
 * is the probe and the fallback. A direct memcpy is only used where the
 * segment's own declared protection says the CPU may read it, and only after
 * a 16-byte kernel-side read has already confirmed the address is live. */
static enum read_method
pick_read_method(intptr_t addr, size_t len, int prot)
{
    uint8_t probe[16];
    size_t  n = len < sizeof probe ? len : sizeof probe;

    int kernel_ok = (kernel_proc_copyout(getpid(), addr, probe, n) == 0);

    if ((prot & PROT_READ) && kernel_ok && guarded_memcpy(probe, (const void *)addr, n))
        return RM_MEMCPY;

    /* Execute-only text. Ask the kernel to add read permission - it is our own
     * process, and if it works the bulk copy is far faster. Only then is a
     * memcpy attempted, and only if a kernel-side read of the same bytes has
     * already succeeded. */
    if (!(prot & PROT_READ)) {
        if (kernel_mprotect(getpid(), addr, len, prot | PROT_READ) == 0 &&
            kernel_proc_copyout(getpid(), addr, probe, n) == 0 &&
            guarded_memcpy(probe, (const void *)addr, n))
            return RM_MPROTECT;
    }

    return kernel_ok ? RM_PROC_COPYOUT : RM_NONE;
}

static int
read_chunk(enum read_method m, void *dst, intptr_t addr, size_t len)
{
    switch (m) {
    case RM_MEMCPY:
    case RM_MPROTECT:
        return guarded_memcpy(dst, (const void *)addr, len);
    case RM_PROC_COPYOUT:
        return kernel_proc_copyout(getpid(), addr, dst, len) == 0;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static void
sanitise(const char *in, char *out, size_t outlen)
{
    size_t j = 0;

    for (size_t i = 0; in[i] && j + 1 < outlen; i++) {
        char c = in[i];
        if (c == '.' && strcmp(in + i, ".sprx") == 0)
            break;
        out[j++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9')) ? c : '_';
    }
    out[j] = '\0';
}

static void
hexdump(const void *p, size_t len, const char *indent)
{
    const uint8_t *b = p;

    for (size_t i = 0; i < len; i += 16) {
        char line[160];
        int  n = snprintf(line, sizeof line, "%s%04zx  ", indent, i);
        for (size_t j = 0; j < 16 && i + j < len; j++)
            n += snprintf(line + n, sizeof line - (size_t)n, "%02x ", b[i + j]);
        n += snprintf(line + n, sizeof line - (size_t)n, " |");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = b[i + j];
            n += snprintf(line + n, sizeof line - (size_t)n, "%c",
                          (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        LOG("%s|\n", line);
    }
}

static const char *
prot_str(int prot, char buf[8])
{
    buf[0] = (prot & PROT_READ)  ? 'r' : '-';
    buf[1] = (prot & PROT_WRITE) ? 'w' : '-';
    buf[2] = (prot & PROT_EXEC)  ? 'x' : '-';
    buf[3] = '\0';
    return buf;
}

/* ------------------------------------------------------------------------ */
/* Module enumeration by modid sweep                                         */
/*                                                                           */
/* sceKernelGetModuleList does not work here, but sceKernelGetModuleInfo does */
/* - so ask it about every plausible handle and keep the answers. Sweeping    */
/* also catches modids that were allocated by a dependency load and that no   */
/* other API mentions: the 2026-08-10 run showed AvPlayer taking modid 0x33   */
/* and VdecCore 0x35, with 0x34 unaccounted for.                             */
/* ------------------------------------------------------------------------ */

typedef struct {
    int      modid;
    char     name[256];
    unsigned nseg;
    struct { intptr_t addr; size_t size; int prot; } seg[4];
} modinfo_t;

static int
get_module_info(int modid, modinfo_t *out, uint8_t raw[0x160])
{
    /* Oversized and zeroed: if the PS5 struct is larger than the PS4 one, a
     * callee writing past 0x160 lands in slack rather than on the stack. */
    static uint8_t big[0x800];
    evo_module_info_t *info = (evo_module_info_t *)big;

    memset(big, 0, sizeof big);
    info->st_size = sizeof(evo_module_info_t);

    if (sceKernelGetModuleInfo(modid, info) != 0)
        return 0;
    if (info->name[0] == '\0')
        return 0;

    if (raw)
        memcpy(raw, big, 0x160);

    out->modid = modid;
    snprintf(out->name, sizeof out->name, "%s", info->name);
    out->nseg = info->segment_count > 4 ? 4 : info->segment_count;
    for (unsigned i = 0; i < out->nseg; i++) {
        out->seg[i].addr = (intptr_t)info->segments[i].address;
        out->seg[i].size = info->segments[i].size;
        out->seg[i].prot = info->segments[i].prot;
    }
    return 1;
}

static size_t
sweep_modules(modinfo_t *out, size_t max)
{
    size_t n = 0;

    for (int id = 0; id < MAX_MODID && n < max; id++)
        if (get_module_info(id, &out[n], NULL))
            n++;
    return n;
}

static void
print_module_table(const char *title, const modinfo_t *m, size_t n)
{
    char pbuf[8];

    LOG("\n%s (%zu modules):\n", title, n);
    for (size_t i = 0; i < n; i++) {
        LOG("  modid=0x%-3x %-44s segs=%u\n", m[i].modid, m[i].name, m[i].nseg);
        for (unsigned s = 0; s < m[i].nseg; s++)
            LOG("        s%u 0x%012lx size=0x%-8zx prot=%s(%d)\n", s,
                (unsigned long)m[i].seg[s].addr, m[i].seg[s].size,
                prot_str(m[i].seg[s].prot, pbuf), m[i].seg[s].prot);
    }
}

/* ------------------------------------------------------------------------ */
/* The dump itself                                                           */
/* ------------------------------------------------------------------------ */

static size_t
dump_segment(const char *modname, int idx, intptr_t addr, size_t len, int prot,
             void *buf)
{
    char shortname[128];
    char path[256];
    char pbuf[8];

    sanitise(modname, shortname, sizeof shortname);
    snprintf(path, sizeof path, USB_DIR "/evo_dump_%s_s%d.bin", shortname, idx);

    stage("dump %s s%d (%zu bytes)", shortname, idx, len);

    enum read_method m = pick_read_method(addr, len, prot);
    LOG("    s%d 0x%lx size=0x%zx prot=%s(%d) read=%s\n", idx,
        (unsigned long)addr, len, prot_str(prot, pbuf), prot,
        read_method_name(m));

    if (m == RM_NONE) {
        LOG("    s%d UNREADABLE by any method - skipped\n", idx);
        return 0;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        LOG("    s%d cannot open %s for writing\n", idx, path);
        return 0;
    }

    size_t done = 0, holes = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > CHUNK)
            n = CHUNK;

        if (!read_chunk(m, buf, addr + (intptr_t)done, n)) {
            /* One bad page must not cost the rest of the segment. Zero-fill so
             * every file offset still maps to the right virtual address - an
             * offline disassembler depends on that. */
            memset(buf, 0, n);
            holes++;
        }
        if (fwrite(buf, 1, n, f) != n) {
            LOG("    s%d short write at 0x%zx\n", idx, done);
            break;
        }
        done += n;
    }
    fclose(f);
    g_written += done;

    LOG("    s%d wrote %zu bytes to %s%s\n", idx, done, path,
        holes ? " (WITH HOLES)" : "");

    if (g_manifest) {
        fprintf(g_manifest,
                "%s\tseg%d\tvaddr=0x%lx\tsize=0x%zx\tprot=%d\tmethod=%s\t"
                "holes=%zu\tfile=evo_dump_%s_s%d.bin\n",
                modname, idx, (unsigned long)addr, done, prot,
                read_method_name(m), holes, shortname, idx);
        fflush(g_manifest);
    }
    return done;
}

static size_t
dump_module(const modinfo_t *m, const char *why)
{
    uint32_t dynh = 0;
    uint8_t  raw[0x160];
    modinfo_t again;

    LOG("\n  MODULE %s  (modid=0x%x, %s)\n", m->name, m->modid, why);

    /* Log the raw struct once per module so the layout stays confirmable
     * rather than assumed. */
    if (get_module_info(m->modid, &again, raw)) {
        LOG("    raw SceKernelModuleInfo:\n");
        hexdump(raw, 0x160, "      ");
    }

    if (kernel_dynlib_handle(getpid(), m->name, &dynh) == 0) {
        intptr_t base  = kernel_dynlib_mapbase_addr(getpid(), dynh);
        intptr_t entry = kernel_dynlib_entry_addr(getpid(), dynh);
        intptr_t init  = kernel_dynlib_init_addr(getpid(), dynh);
        intptr_t fini  = kernel_dynlib_fini_addr(getpid(), dynh);

        LOG("    dynlib=0x%x mapbase=0x%lx entry=+0x%lx init=+0x%lx "
            "fini=+0x%lx\n", dynh, (unsigned long)base,
            (unsigned long)(entry ? entry - base : 0),
            (unsigned long)(init ? init - base : 0),
            (unsigned long)(fini ? fini - base : 0));
    } else {
        LOG("    no kernel dynlib handle for this module\n");
    }

    void *buf = malloc(CHUNK);
    if (!buf) {
        LOG("    out of memory for the copy buffer\n");
        return 0;
    }

    size_t total = 0;
    for (unsigned i = 0; i < m->nseg; i++) {
        if (!m->seg[i].addr || !m->seg[i].size) {
            LOG("    s%u empty - skipped\n", i);
            continue;
        }
        if (g_written + m->seg[i].size > DUMP_BUDGET) {
            LOG("    s%u would exceed the %u MB budget - stopping\n", i,
                DUMP_BUDGET / (1024 * 1024));
            break;
        }
        total += dump_segment(m->name, (int)i, m->seg[i].addr, m->seg[i].size,
                              m->seg[i].prot, buf);
    }
    free(buf);

    LOG("    %s: %zu bytes across %u segment(s)\n", m->name, total, m->nseg);
    return total;
}

/* ------------------------------------------------------------------------ */
/* Symbol probe                                                              */
/* ------------------------------------------------------------------------ */

static int
resolve_symbols(const char *basename, const char *const *syms, size_t nsym)
{
    uint32_t dynh = 0;
    intptr_t base = 0;

    if (kernel_dynlib_handle(getpid(), basename, &dynh) != 0) {
        LOG("    no dynlib handle for %s - cannot resolve\n", basename);
        return 0;
    }
    base = kernel_dynlib_mapbase_addr(getpid(), dynh);
    LOG("    dynlib=0x%x mapbase=0x%lx\n", dynh, (unsigned long)base);

    int found = 0;
    for (size_t i = 0; i < nsym; i++) {
        char nid[12] = {0};

        /* Sony modules export NIDs - a hash of the name - not names, which is
         * why plain sceKernelDlsym returns 0x80020003 (ESRCH) for all of
         * these. nid_encode() applies that hash. */
        nid_encode(syms[i], nid);

        intptr_t addr = kernel_dynlib_resolve(getpid(), dynh, nid);
        if (addr) {
            /* The offset, not the address, is what matters offline: addresses
             * move between boots, offsets index straight into the dump. */
            LOG("      + %-38s %-12s 0x%lx  (+0x%lx)\n",
                syms[i], nid, (unsigned long)addr,
                (unsigned long)(addr - base));
            if (g_manifest) {
                fprintf(g_manifest, "%s\tsymbol\t%s\tnid=%s\taddr=0x%lx\t"
                                    "offset=0x%lx\n",
                        basename, syms[i], nid, (unsigned long)addr,
                        (unsigned long)(addr - base));
                fflush(g_manifest);
            }
            found++;
        } else {
            LOG("      - %-38s nid=%-11s unresolved\n", syms[i], nid);
        }
    }
    return found;
}

/* ------------------------------------------------------------------------ */
/* Export map - which module exports which of the recovered names            */
/*                                                                           */
/* Phase 1 recovered the real PS5 API names from strings inside the dumped    */
/* images (sceVideodec2*, sceVdecCore*, sceAudiodec*, sceMp4*) and aerolib    */
/* supplied their NIDs. This asks the console which module actually exports   */
/* each one. It RESOLVES ONLY - no function here is called - so it stays on   */
/* the safe side of the go/no-go before sceAvPlayerInit.                      */
/* ------------------------------------------------------------------------ */

static void
export_map(FILE *out)
{
    const size_t nmod = sizeof kCandidateModules / sizeof *kCandidateModules;
    const size_t nnid = sizeof kNidTable / sizeof *kNidTable;

    LOG("\n=== Export map: %zu symbols against %zu modules ===\n", nnid, nmod);

    for (size_t i = 0; i < nmod; i++) {
        const char *mod = kCandidateModules[i];
        char path[256];
        int  res = 0;
        uint32_t dynh = 0;

        stage("export map: %s", mod);

        /* A breadcrumb, flushed before the load. Loading libSceAudiodec.sprx
         * hung the payload once with no watchdog output, so "which module was
         * being loaded when it stopped" has to survive the process dying
         * without any further cooperation from it. */
        if (out) {
            fprintf(out, "#loading\t%s\n", mod);
            fflush(out);
        }

        /* Already loaded modules return their existing modid rather than
         * loading twice, so this is safe to call unconditionally. */
        snprintf(path, sizeof path, "/system/common/lib/%s", mod);
        int modid = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);

        if (modid < 0) {
            LOG("\n  %-38s LOAD FAILED 0x%08x\n", mod, modid);
            if (out)
                fprintf(out, "%s\tloadfail\t0x%08x\n", mod, modid);
            continue;
        }
        if (kernel_dynlib_handle(getpid(), mod, &dynh) != 0) {
            LOG("\n  %-38s loaded (modid=0x%x) but no dynlib handle\n",
                mod, modid);
            continue;
        }

        intptr_t base = kernel_dynlib_mapbase_addr(getpid(), dynh);
        int hits = 0;

        LOG("\n  %-38s modid=0x%-3x base=0x%lx\n", mod, modid,
            (unsigned long)base);

        for (size_t j = 0; j < nnid; j++) {
            intptr_t a = kernel_dynlib_resolve(getpid(), dynh,
                                               kNidTable[j].nid);
            if (!a)
                continue;
            hits++;
            LOG("      + %-44s +0x%lx\n", kNidTable[j].name,
                (unsigned long)(a - base));
            if (out) {
                fprintf(out, "%s\texport\t%s\t%s\t0x%lx\t+0x%lx\n", mod,
                        kNidTable[j].name, kNidTable[j].nid,
                        (unsigned long)a, (unsigned long)(a - base));
                fflush(out);
            }
        }
        LOG("      %d export(s) matched\n", hits);
        if (out) {
            fprintf(out, "%s\tsummary\tmodid=0x%x\tbase=0x%lx\texports=%d\n",
                    mod, modid, (unsigned long)base, hits);
            fflush(out);
        }
    }
}

/* ------------------------------------------------------------------------ */

int
main(void)
{
    pthread_t wd;
    static modinfo_t before[MAX_MODULES];
    static modinfo_t after[MAX_MODULES];

    g_log = fopen(LOG_PATH, "w");
    g_manifest = fopen(MANIFEST_PATH, "w");

    install_fault_handler();
    pthread_create(&wd, NULL, watchdog_main,
                   (void *)(uintptr_t)WATCHDOG_SECONDS);
    pthread_detach(wd);

    LOG("=== EVO Player decoder_test - Phase 0 module image dump ===\n");
    LOG("firmware : 0x%08x\n", kernel_get_fw_version());
    LOG("pid      : %d\n", getpid());
    LOG("watchdog : %d s\n", WATCHDOG_SECONDS);
    if (!g_log)
        printf("WARNING: could not open %s - is the USB stick mounted?\n",
               LOG_PATH);

    /* -- control ---------------------------------------------------------
     * This payload links -lSceVideoOut and -lSceAudioOut, so those must show
     * as mapped. If they do not, the dynlib probe itself is broken and every
     * other result below is meaningless. */
    stage("control check");
    LOG("\nControl (modules this payload links against):\n");
    int control = 0;
    static const char *const kControl[] = {
        "libSceVideoOut.sprx", "libSceAudioOut.sprx", "libSceSysmodule.sprx",
    };
    for (size_t i = 0; i < sizeof kControl / sizeof *kControl; i++) {
        uint32_t h = 0;
        if (kernel_dynlib_handle(getpid(), kControl[i], &h) == 0) {
            LOG("  [x] %-32s handle=0x%x base=0x%lx\n", kControl[i], h,
                (unsigned long)kernel_dynlib_mapbase_addr(getpid(), h));
            control++;
        } else {
            LOG("  [ ] %-32s NOT MAPPED - probe is unreliable!\n", kControl[i]);
        }
    }
    LOG("  control %d/3\n", control);

    /* -- control for the sizing method -----------------------------------
     * Everything below depends on sceKernelGetModuleInfo answering. Prove it
     * against something already loaded before trusting it on a media module.
     * (sceKernelVirtualQuery was tried here on 2026-08-10 and FAILED, which is
     * why it is no longer used at all.) */
    stage("module-info control");
    {
        modinfo_t ctl;
        LOG("\nGetModuleInfo control (modid 0, the payload's own image):\n");
        if (get_module_info(0, &ctl, NULL))
            LOG("  ok  name='%s' segments=%u\n", ctl.name, ctl.nseg);
        else
            LOG("  FAILED - sizing is unavailable, dump results below are "
                "meaningless\n");
    }

    /* -- user session ----------------------------------------------------
     * videoout_test established a payload has none (0x80940004). Re-record it
     * here so this run's log is self-contained, per the review. */
    stage("user session");
    int32_t user_id = -1;
    int uinit = sceUserServiceInitialize(NULL);
    int urc   = sceUserServiceGetInitialUser(&user_id);
    LOG("\nUser session: sceUserServiceInitialize=0x%08x "
        "sceUserServiceGetInitialUser=0x%08x user_id=%d\n",
        uinit, urc, user_id);

    /* -- what is loaded before -------------------------------------------- */
    stage("sweep modules (before)");
    size_t nbefore = sweep_modules(before, MAX_MODULES);
    print_module_table("Modules BEFORE loading any media module",
                       before, nbefore);

    /* -- load the targets ------------------------------------------------- */
    LOG("\nLoading media modules from /system/common/lib/:\n");
    for (size_t i = 0; i < sizeof kTargets / sizeof *kTargets; i++) {
        char path[256];
        int  res = 0;

        snprintf(path, sizeof path, "/system/common/lib/%s", kTargets[i]);
        stage("load %s", kTargets[i]);
        int modid = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);
        LOG("  %-40s modid=0x%x res=0x%x\n", kTargets[i], modid, res);
    }

    /* -- and after: the delta IS the run-time dependency list -------------- */
    stage("sweep modules (after)");
    size_t nafter = sweep_modules(after, MAX_MODULES);
    print_module_table("Modules AFTER", after, nafter);

    LOG("\nDelta (pulled in by loading the three media modules):\n");
    for (size_t i = 0; i < nafter; i++) {
        int seen = 0;
        for (size_t j = 0; j < nbefore; j++)
            if (after[i].modid == before[j].modid) { seen = 1; break; }
        if (!seen)
            LOG("  + modid=0x%-3x %s\n", after[i].modid, after[i].name);
    }

    /* -- symbol resolution, for the offsets ------------------------------- */
    LOG("\nSymbol resolution (offsets are what matter offline):\n");
    LOG("  libSceAvPlayer.sprx:\n");
    stage("resolve AvPlayer symbols");
    int hits = resolve_symbols("libSceAvPlayer.sprx", kAvPlayerSymbols,
                               sizeof kAvPlayerSymbols / sizeof *kAvPlayerSymbols);
    /* The PS4-era sceVideoDecoder* names used to be probed here against
     * VdecCore and Arbitration. They are gone: Phase 1 established those names
     * do not exist on PS5 at all, and the export map below covers the real
     * ones (sceVdecCore*, sceVideodec2*) against every module. */

    /* -- THE DUMP --------------------------------------------------------- */
    LOG("\n=== Dumping mapped module images ===\n");

    int dumped = 0;

    /* The three targets first, so a budget or watchdog cut-off costs the
     * least important files rather than the ones the plan asked for. */
    for (size_t i = 0; i < sizeof kTargets / sizeof *kTargets; i++) {
        int found = 0;
        for (size_t j = 0; j < nafter; j++) {
            if (strcmp(after[j].name, kTargets[i]) != 0)
                continue;
            found = 1;
            if (dump_module(&after[j], "phase-0 target"))
                dumped++;
            break;
        }
        if (!found)
            LOG("\n  %s: not in the module table - cannot dump\n", kTargets[i]);
    }

    /* Then everything the load pulled in. Free information: these are the
     * modules the import graph will point at. */
    for (size_t i = 0; i < nafter; i++) {
        int seen = 0;
        for (size_t j = 0; j < nbefore; j++)
            if (after[i].modid == before[j].modid) { seen = 1; break; }
        for (size_t j = 0; j < sizeof kTargets / sizeof *kTargets; j++)
            if (strcmp(after[i].name, kTargets[j]) == 0) { seen = 1; break; }
        if (seen)
            continue;
        if (g_written >= DUMP_BUDGET) {
            LOG("\n  budget exhausted before %s\n", after[i].name);
            break;
        }
        if (dump_module(&after[i], "pulled in by the load"))
            dumped++;
    }

    /* -- PASSIVE EXPORT MAP ----------------------------------------------
     * Last, because it loads further modules and so changes the process. The
     * dump above is taken from the pristine state. */
    {
        FILE *ex = fopen(EXPORTS_PATH, "w");
        export_map(ex);
        if (ex)
            fclose(ex);
    }

    stage("finishing");
    LOG("\nRESULT: control %d/3, symbols resolved %d, modules dumped %d, "
        "bytes written %lu\n", control, hits, dumped, g_written);
    LOG("Files are on %s - pull them with:\n"
        "  curl -s -o local.bin http://$PS5_HOST:8080/fs/mnt/usb0/<name>\n",
        USB_DIR);

    if (g_manifest) {
        fprintf(g_manifest, "#end\tcontrol=%d\tsymbols=%d\tmodules=%d\t"
                            "bytes=%lu\n", control, hits, dumped, g_written);
        fclose(g_manifest);
        g_manifest = NULL;
    }
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }

    evo_notify("EVO decoder_test: control %d/3, dumped %d modules, %lu KB",
               control, dumped, g_written / 1024);

    /* _exit rather than return: the watchdog thread is detached and there is
     * nothing worth running atexit for. This also guarantees the elfldr socket
     * closes promptly. */
    _exit(EXIT_SUCCESS);
}
