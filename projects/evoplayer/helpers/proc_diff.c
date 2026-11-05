/*
 * proc_diff.c - find where the kernel keeps a process's direct-memory budget
 * (#237, browser GPU plan: GLSlim needs direct memory, and a payload process
 * has none: sceKernelGetDirectMemorySize() == 0, while EVO, an app, has 12 GB).
 *
 * Read-only: walks allproc, logs every process (pid, authid, the first name
 * string in its struct proc), then diffs this process's struct proc and
 * ucred against the process whose name contains /data/proc_diff_name
 * (default "eboot"), printing the 8-byte words that differ and are not
 * kernel pointers. Logs to /data/proc_diff.log. No GPU, no writes to the
 * kernel. Built by tools/build_glslim_probe.sh.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define PROC_BYTES 0x1400
#define UCRED_BYTES 0x200
#define VMSPACE_BYTES 0x600

static FILE *g_log;

static void name_of(const unsigned char *b, size_t n, char *out, size_t outn)
{
    /* The first run of 4+ printable bytes after the list/pid fields. */
    out[0] = 0;
    for (size_t i = 0x100; i + 4 < n; ++i) {
        size_t k = 0;
        while (i + k < n && b[i + k] >= 0x20 && b[i + k] < 0x7f && k < outn - 1)
            k++;
        if (k >= 4 && (i + k >= n || b[i + k] == 0)) {
            memcpy(out, b + i, k);
            out[k] = 0;
            return;
        }
        if (k)
            i += k;
    }
}

static int is_kptr(uint64_t v) { return (v >> 48) == 0xffff; }

static void diff(const char *what, const unsigned char *a, const unsigned char *b, size_t n)
{
    for (size_t i = 0; i < n; i += 8) {
        uint64_t x, y;
        memcpy(&x, a + i, 8);
        memcpy(&y, b + i, 8);
        if (x != y && !is_kptr(x) && !is_kptr(y))
            fprintf(g_log, "  %s +%#05zx: ours %#018llx  theirs %#018llx\n", what, i,
                    (unsigned long long)x, (unsigned long long)y);
    }
}

/* 4-byte words that differ where both sides are small (indices, counts,
 * flags): a pool or container number hides in these. */
static void diff4(const char *what, const unsigned char *a, const unsigned char *b, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t x, y;
        memcpy(&x, a + i, 4);
        memcpy(&y, b + i, 4);
        if (x != y && x < 0x10000 && y < 0x10000)
            fprintf(g_log, "  %s small +%#05zx: ours %#x  theirs %#x\n", what, i, x, y);
    }
}

static void findv(const char *what, const unsigned char *a, const unsigned char *b, size_t n, uint32_t v)
{
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t x, y;
        memcpy(&x, a + i, 4);
        memcpy(&y, b + i, 4);
        if (y == v)
            fprintf(g_log, "  %s +%#05zx: theirs %#x, ours %#x\n", what, i, y, x);
    }
}

int main(void)
{
    g_log = fopen("/data/proc_diff.log", "w");
    if (!g_log)
        return 1;
    char want[64] = "eboot";
    FILE *f = fopen("/data/proc_diff_name", "r");
    if (f) {
        if (fscanf(f, "%63s", want) != 1)
            strcpy(want, "eboot");
        fclose(f);
    }
    pid_t me = getpid();
    intptr_t mine = kernel_get_proc(me), theirs = 0;
    pid_t their_pid = 0;
    fprintf(g_log, "me pid %d proc %#lx, looking for '%s'\n", (int)me, (long)mine, want);

    intptr_t p = 0;
    kernel_copyout(KERNEL_ADDRESS_ALLPROC, &p, sizeof p);
    static unsigned char buf[PROC_BYTES];
    for (int guard = 0; p && guard < 2000; ++guard) {
        if (kernel_copyout(p, buf, sizeof buf) != 0)
            break;
        int pid = 0;
        memcpy(&pid, buf + KERNEL_OFFSET_PROC_P_PID, 4);
        char name[48];
        name_of(buf, sizeof buf, name, sizeof name);
        fprintf(g_log, "pid %5d authid %#018llx proc %#lx name '%s'\n", pid,
                (unsigned long long)kernel_get_ucred_authid(pid), (long)p, name);
        /* A number picks that pid; anything else matches the name. */
        int match = (want[0] >= '0' && want[0] <= '9') ? pid == atoi(want) : strstr(name, want) != NULL;
        if (!theirs && pid != me && match) {
            theirs = p;
            their_pid = pid;
        }
        intptr_t next = 0;
        memcpy(&next, buf, sizeof next);
        p = next;
    }
    fflush(g_log);
    if (theirs) {
        static unsigned char a[PROC_BYTES], b[PROC_BYTES];
        kernel_copyout(mine, a, sizeof a);
        kernel_copyout(theirs, b, sizeof b);
        fprintf(g_log, "== struct proc: me %d vs %d\n", (int)me, (int)their_pid);
        diff("proc", a, b, sizeof a);
        static unsigned char ua[UCRED_BYTES], ub[UCRED_BYTES];
        kernel_copyout(kernel_get_proc_ucred(me), ua, sizeof ua);
        kernel_copyout(kernel_get_proc_ucred(their_pid), ub, sizeof ub);
        fprintf(g_log, "== ucred\n");
        diff("ucred", ua, ub, sizeof ua);
        /* The direct-memory pool may hang off the vmspace instead (vm_map
         * header first; the PS5 additions follow it). */
        static unsigned char va[VMSPACE_BYTES], vb[VMSPACE_BYTES];
        intptr_t vma = 0, vmb = 0;
        kernel_copyout(mine + KERNEL_OFFSET_PROC_P_VMSPACE, &vma, sizeof vma);
        kernel_copyout(theirs + KERNEL_OFFSET_PROC_P_VMSPACE, &vmb, sizeof vmb);
        fprintf(g_log, "== vmspace: ours %#lx theirs %#lx\n", (long)vma, (long)vmb);
        if (vma && vmb && kernel_copyout(vma, va, sizeof va) == 0 && kernel_copyout(vmb, vb, sizeof vb) == 0) {
            diff("vmspace", va, vb, sizeof va);
            diff4("vmspace", va, vb, sizeof va);
        }
        diff4("proc", a, b, sizeof a);
        /* /data/proc_diff_find "<hex>": where that 4-byte value (e.g. klog
         * EXEC's dmem# for their process) sits in their proc and vmspace,
         * and what we hold at the same offsets. */
        unsigned find = 0;
        FILE *ff = fopen("/data/proc_diff_find", "r");
        if (ff) {
            if (fscanf(ff, "%x", &find) != 1)
                find = 0;
            fclose(ff);
        }
        if (find) {
            fprintf(g_log, "== value %#x\n", find);
            findv("proc", a, b, sizeof a, find);
            findv("vmspace", va, vb, sizeof va, find);
        }
    } else {
        fprintf(g_log, "no process named like '%s'\n", want);
    }
    fprintf(g_log, "end\n");
    fclose(g_log);
    return 0;
}
