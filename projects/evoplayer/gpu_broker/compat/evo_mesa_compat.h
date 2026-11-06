/*
 * evo_mesa_compat.h - the few Mesa util / C11 thread pieces the vendored
 * radv_ps5_platform.c uses, on plain pthreads, so EVO links the GPU broker
 * owner without any Mesa library (#237). Only what that file needs.
 * SPDX-License-Identifier: MIT
 */
#ifndef EVO_MESA_COMPAT_H
#define EVO_MESA_COMPAT_H

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* util/macros.h */
#ifndef MAX2
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN2
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif
#ifndef UNUSED
#define UNUSED __attribute__((unused))
#endif
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* util/u_math.h */
static inline uint64_t align64(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}
static inline uint32_t align(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}
static inline uint64_t util_next_power_of_two64(uint64_t x)
{
    if (x <= 1)
        return 1;
    return UINT64_C(1) << (64 - __builtin_clzll(x - 1));
}

/* util/u_atomic.h */
#define p_atomic_xchg(v, i) __atomic_exchange_n((v), (i), __ATOMIC_SEQ_CST)
#define p_atomic_add(v, i) ((void)__atomic_add_fetch((v), (i), __ATOMIC_SEQ_CST))

/* util/os_time.h */
static inline int64_t os_time_get_nano(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* c11/threads.h */
enum { mtx_plain = 0, mtx_try = 1, mtx_timed = 2, mtx_recursive = 4 };
enum { thrd_success = 0, thrd_timedout, thrd_error, thrd_busy, thrd_nomem };
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t cnd_t;
typedef pthread_t thrd_t;
typedef pthread_once_t once_flag;
typedef int (*thrd_start_t)(void *);
#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT

static inline int mtx_init(mtx_t *m, int type)
{
    (void)type;
    return pthread_mutex_init(m, NULL) == 0 ? thrd_success : thrd_error;
}
static inline int mtx_lock(mtx_t *m) { return pthread_mutex_lock(m) == 0 ? thrd_success : thrd_error; }
static inline int mtx_unlock(mtx_t *m) { return pthread_mutex_unlock(m) == 0 ? thrd_success : thrd_error; }
static inline void mtx_destroy(mtx_t *m) { pthread_mutex_destroy(m); }
static inline int cnd_init(cnd_t *c) { return pthread_cond_init(c, NULL) == 0 ? thrd_success : thrd_error; }
static inline int cnd_signal(cnd_t *c) { return pthread_cond_signal(c) == 0 ? thrd_success : thrd_error; }
static inline int cnd_broadcast(cnd_t *c) { return pthread_cond_broadcast(c) == 0 ? thrd_success : thrd_error; }
static inline int cnd_wait(cnd_t *c, mtx_t *m) { return pthread_cond_wait(c, m) == 0 ? thrd_success : thrd_error; }
static inline void call_once(once_flag *f, void (*fn)(void)) { pthread_once(f, fn); }

struct evo_thrd_pack {
    thrd_start_t fn;
    void *arg;
};
static void *evo_thrd_trampoline(void *p)
{
    struct evo_thrd_pack pack = *(struct evo_thrd_pack *)p;
    free(p);
    return (void *)(intptr_t)pack.fn(pack.arg);
}
static inline int thrd_create(thrd_t *t, thrd_start_t fn, void *arg)
{
    struct evo_thrd_pack *p = (struct evo_thrd_pack *)malloc(sizeof *p);
    if (!p)
        return thrd_nomem;
    p->fn = fn;
    p->arg = arg;
    if (pthread_create(t, NULL, evo_thrd_trampoline, p) != 0) {
        free(p);
        return thrd_error;
    }
    return thrd_success;
}
static inline int thrd_detach(thrd_t t) { return pthread_detach(t) == 0 ? thrd_success : thrd_error; }

/* util/simple_mtx.h */
typedef struct {
    pthread_mutex_t m;
} simple_mtx_t;
#define SIMPLE_MTX_INITIALIZER {PTHREAD_MUTEX_INITIALIZER}
static inline void simple_mtx_init(simple_mtx_t *s, int type)
{
    (void)type;
    pthread_mutex_init(&s->m, NULL);
}
static inline void simple_mtx_destroy(simple_mtx_t *s) { pthread_mutex_destroy(&s->m); }
static inline void simple_mtx_lock(simple_mtx_t *s) { pthread_mutex_lock(&s->m); }
static inline void simple_mtx_unlock(simple_mtx_t *s) { pthread_mutex_unlock(&s->m); }

#endif
