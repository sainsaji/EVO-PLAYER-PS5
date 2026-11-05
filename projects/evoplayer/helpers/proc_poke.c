/*
 * proc_poke.c - test which struct proc field gates a process's direct memory
 * (#237 browser GPU plan; see proc_diff.c). Reads "<hex offset> <hex value>
 * <bytes>" lines from /data/proc_poke, writes each into THIS process's struct
 * proc (its own, nothing else), then logs sceKernelGetDirectMemorySize and
 * the available direct memory before and after. Logs to /data/proc_poke.log.
 * Built by tools/build_glslim_probe.sh.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>

int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t *out_start,
                                       size_t *out_size);
int sceKernelMapFlexibleMemory(void **addr, size_t len, int prot, int flags);
int sceKernelMunmap(void *addr, size_t len);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type, int64_t *phys);
int sceKernelReleaseDirectMemory(int64_t phys, size_t len);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t phys, size_t align);
int sceKernelReserveVirtualRange(void **addr, size_t len, int flags, size_t align);

/* libSceComposite's batch entry (mapInternal, 0x7ba0 in the 13.60 .sprx). */
typedef struct {
    void *start;
    int64_t phys;
    size_t len;
    uint8_t prot;
    uint8_t type;
    int16_t reserved;
    int32_t op; /* 0 = MAP_DIRECT */
} batch_entry;
int sceKernelBatchMap(batch_entry *entries, int n, int *done);

/* The compositor's grant fails at sceKernelBatchMap (EPERM). Same call on
 * direct memory this process allocates itself: if the allocation fails, the
 * process has no direct-memory pool; if it works and the map fails, the
 * block is not the problem, the process is. 2 MiB, released again. */
static void direct_map_test(FILE *log, const char *when)
{
    const size_t sz = 2u << 20;
    int64_t phys = 0;
    int r = sceKernelAllocateDirectMemory(0, (int64_t)1 << 36, sz, sz, 0, &phys);
    fprintf(log, "%s: direct alloc 2 MiB type 0 = %#x (phys %#llx)\n", when, (unsigned)r, (unsigned long long)phys);
    fflush(log);
    if (r != 0)
        return;
    void *addr = NULL;
    r = sceKernelMapDirectMemory(&addr, sz, 0x03, 0, phys, sz);
    fprintf(log, "%s: MapDirectMemory prot 3 = %#x (%p)\n", when, (unsigned)r, addr);
    if (r == 0)
        sceKernelMunmap(addr, sz);
    void *va = NULL;
    r = sceKernelReserveVirtualRange(&va, sz, 0, sz);
    fprintf(log, "%s: reserve 2 MiB = %#x (%p)\n", when, (unsigned)r, va);
    if (r == 0) {
        batch_entry e;
        memset(&e, 0, sizeof e);
        e.start = va;
        e.phys = phys;
        e.len = sz;
        e.prot = 0x03;
        int done = 0;
        r = sceKernelBatchMap(&e, 1, &done);
        fprintf(log, "%s: BatchMap MAP_DIRECT prot 3 = %#x (done %d)\n", when, (unsigned)r, done);
        sceKernelMunmap(va, sz);
    }
    r = sceKernelReleaseDirectMemory(phys, sz);
    fprintf(log, "%s: release = %#x\n", when, (unsigned)r);
    fflush(log);
}

/* Can this process map memory the GPU may use? The compositor's grant fails
 * at this step for a payload (sceKernelBatchMap EPERM). Flexible memory with
 * CPU+GPU RW (0x33) vs CPU only (0x03), 2 MiB each, unmapped again. */
static void gpu_map_test(FILE *log, const char *when)
{
    static const int prots[] = {0x03, 0x33, 0x13, 0x23};
    for (unsigned i = 0; i < sizeof prots / sizeof prots[0]; ++i) {
        void *addr = NULL;
        int r = sceKernelMapFlexibleMemory(&addr, 2u << 20, prots[i], 0);
        fprintf(log, "%s: flexible map prot %#x = %#x (%p)\n", when, prots[i], (unsigned)r, addr);
        if (r == 0)
            sceKernelMunmap(addr, 2u << 20);
    }
    fflush(log);
}

static void report(FILE *log, const char *when)
{
    gpu_map_test(log, when);
    int64_t total = sceKernelGetDirectMemorySize(), start = 0;
    size_t avail = 0;
    int r = sceKernelAvailableDirectMemorySize(0, total > 0 ? total : (int64_t)1 << 36, 2u << 20, &start, &avail);
    fprintf(log, "%s: direct total %lld MB, available %zu MB (rc %#x)\n", when, (long long)(total >> 20),
            avail >> 20, (unsigned)r);
    fflush(log);
    direct_map_test(log, when);
}

int main(void)
{
    FILE *log = fopen("/data/proc_poke.log", "w");
    if (!log)
        return 1;
    pid_t me = getpid();
    intptr_t proc = kernel_get_proc(me);
    fprintf(log, "pid %d proc %#lx\n", (int)me, (long)proc);
    report(log, "before");
    FILE *f = fopen("/data/proc_poke", "r");
    unsigned long long off, val;
    unsigned n;
    while (f && fscanf(f, "%llx %llx %u", &off, &val, &n) == 3) {
        if (off >= 0x1400 || (n != 1 && n != 2 && n != 4 && n != 8))
            continue;
        unsigned char old[8] = {0};
        kernel_copyout(proc + (intptr_t)off, old, n);
        int r = kernel_copyin(&val, proc + (intptr_t)off, n);
        uint64_t o = 0;
        memcpy(&o, old, n);
        fprintf(log, "poke +%#llx (%u bytes): %#llx -> %#llx (rc %d)\n", off, n, (unsigned long long)o, val, r);
        fflush(log);
    }
    if (f)
        fclose(f);
    /* /data/proc_poke_authid (hex): run the "after" tests under that authid,
     * as glslim_probe does before GLSlim's init. */
    unsigned long long want = 0;
    FILE *af = fopen("/data/proc_poke_authid", "r");
    if (af) {
        if (fscanf(af, "%llx", &want) != 1)
            want = 0;
        fclose(af);
    }
    if (want) {
        unsigned long long was = (unsigned long long)kernel_get_ucred_authid(me);
        int r = kernel_set_ucred_authid(me, want);
        fprintf(log, "authid %#llx -> %#llx (rc %d), now %#llx\n", was, want, r,
                (unsigned long long)kernel_get_ucred_authid(me));
        fflush(log);
    }
    report(log, "after");
    fclose(log);
    return 0;
}
