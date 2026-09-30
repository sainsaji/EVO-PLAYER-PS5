/*
 * tools/dns_host.c - host test for tools/native-app/stubs/evo_dns.c (#91).
 * Built and run by tools/dns_host.sh, once per scenario (argv[1]).
 *
 * evo_dns.c is compiled as-is, #included below with the getaddrinfo family
 * renamed so it cannot collide with the host libc (glibc's getnameinfo takes
 * socklen_t lengths where the PS5's FreeBSD header takes size_t, so the rename
 * has to apply to the definitions only, not to <netdb.h>'s declarations).
 * libSceNet is replaced by the mock below, which can answer, refuse, or fail to
 * create a pool or a resolver at all, and whose multi-record call can return
 * several addresses, an error, a layout that is not the documented one, or a
 * buffer overrun.
 *
 * The module remembers, for the rest of the process, that the multi-record call
 * is unusable. So each way it can fail is its own scenario and its own process:
 *
 *   main       the normal life of the resolver: lookups, several addresses,
 *              the cache, no-answer vs no-resolver, arguments, eight threads
 *   multifail  the multi-record call errors where the single lookup works
 *   garbage    the multi-record call returns a layout that is not the documented
 *   overrun    the multi-record call writes past the documented struct
 *   killswitch /mnt/usb0/evo_dns_single turns the multi-record call off, and is
 *              not sticky
 *
 * What it proves overall:
 *   - a lookup goes to the console resolver, and a literal / localhost / cached
 *     name does not
 *   - every address comes back, in the resolver's order, as a chain; a duplicate
 *     is dropped and an IPv6 record is skipped
 *   - the pool and the resolver are released on every path, including failure
 *   - sceNetInit runs once, and only when the first pool will not create
 *   - "no answer" and "no resolver" are told apart (EAI_NONAME / EAI_AGAIN)
 *   - an empty or bracket-only name is refused cleanly
 *   - the log line that says what resolved, to what
 */
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define EVO_DNS_SINGLE_FLAG "/tmp/evo_dns_single_test"

#define getaddrinfo  evo_getaddrinfo
#define freeaddrinfo evo_freeaddrinfo
#define getnameinfo  evo_getnameinfo
#define gai_strerror evo_gai_strerror
#include "native-app/stubs/evo_dns.c"

/* ---- the log the app would write to evo.log ---- */
static char            g_log[64][300];
static int             g_log_n;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void evo_boot_log(const char *fmt, ...)
{
    pthread_mutex_lock(&g_log_lock);
    if (g_log_n < 64) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(g_log[g_log_n++], sizeof g_log[0], fmt, ap);
        va_end(ap);
    }
    pthread_mutex_unlock(&g_log_lock);
}

static int log_has(const char *needle)
{
    int found = 0;
    pthread_mutex_lock(&g_log_lock);
    for (int i = 0; i < g_log_n; i++)
        if (strstr(g_log[i], needle)) found = 1;
    pthread_mutex_unlock(&g_log_lock);
    return found;
}

/* ---- libSceNet, mocked ---- */
static atomic_int m_init_calls, m_pool_create, m_pool_destroy, m_res_create,
                  m_res_destroy, m_ntoa, m_multi, m_open_pools, m_open_resolvers;
static atomic_int m_initialised;      /* set by sceNetInit                        */
static int        m_require_init;     /* pool creation fails until then           */
static int        m_pool_fail;        /* pool creation always fails               */
static int        m_resolver_fail;    /* resolver creation always fails           */

enum { MULTI_OK = 0, MULTI_ERROR, MULTI_GARBAGE, MULTI_OVERRUN };
static int m_multi_mode;

#define ZONE_IPS 10
static const struct { const char *host; const char *ips[ZONE_IPS]; } k_zone[] = {
    { "media.example",  { "10.1.2.3", "10.1.2.4", "10.1.2.5" } },
    { "iptv.example",   { "10.4.5.6" } },
    { "cdn.example",    { "10.7.8.9", "10.7.8.10" } },
    { "api.example",    { "10.9.9.9" } },
    { "dup.example",    { "10.5.5.5", "10.5.5.5", "10.5.5.6" } },
    { "mixed.example",  { "::", "10.6.6.6", "10.6.6.7" } },   /* first record is IPv6 */
    { "blocked.example", { "0.0.0.0" } },          /* DNS filtering: a blocked name answers 0.0.0.0 */
    { "v6only.example", { "::", "::" } },          /* AAAA records only */
    { "partial.example", { "0.0.0.0", "10.3.3.3" } }, /* one blocked, one real */
    { "many.example",   { "10.8.0.1", "10.8.0.2", "10.8.0.3", "10.8.0.4", "10.8.0.5",
                          "10.8.0.6", "10.8.0.7", "10.8.0.8", "10.8.0.9", "10.8.0.10" } },
};

static int zone_find(const char *host)
{
    for (size_t i = 0; i < sizeof k_zone / sizeof k_zone[0]; i++)
        if (!strcasecmp(host, k_zone[i].host)) return (int)i;
    return -1;
}

static int zone_count(int z)
{
    int n = 0;
    while (n < ZONE_IPS && k_zone[z].ips[n]) n++;
    return n;
}

int sceNetInit(void)
{
    atomic_fetch_add(&m_init_calls, 1);
    atomic_store(&m_initialised, 1);
    return 0;
}

int sceNetPoolCreate(const char *name, int size, int flags)
{
    (void)name; (void)size; (void)flags;
    atomic_fetch_add(&m_pool_create, 1);
    if (m_pool_fail || (m_require_init && !atomic_load(&m_initialised)))
        return (int)0x80410100;
    atomic_fetch_add(&m_open_pools, 1);
    return 7;
}

int sceNetPoolDestroy(int memid)
{
    (void)memid;
    atomic_fetch_add(&m_pool_destroy, 1);
    atomic_fetch_sub(&m_open_pools, 1);
    return 0;
}

int sceNetResolverCreate(const char *name, int memid, int flags)
{
    (void)name; (void)flags;
    atomic_fetch_add(&m_res_create, 1);
    if (m_resolver_fail || memid != 7) return (int)0x80410101;
    atomic_fetch_add(&m_open_resolvers, 1);
    return 3;
}

int sceNetResolverDestroy(int rid)
{
    (void)rid;
    atomic_fetch_add(&m_res_destroy, 1);
    atomic_fetch_sub(&m_open_resolvers, 1);
    return 0;
}

int sceNetResolverStartNtoa(int rid, const char *hostname, struct in_addr *addr,
                            int timeout, int retry, int flags)
{
    (void)rid; (void)timeout; (void)retry; (void)flags;
    atomic_fetch_add(&m_ntoa, 1);
    int z = zone_find(hostname);
    if (z >= 0) {
        /* The single call returns the first IPv4 record - and 0.0.0.0 counts:
         * that is how a blocked name comes back, as a success. */
        for (int i = 0; i < zone_count(z); i++) {
            if (strchr(k_zone[z].ips[i], ':')) continue;
            inet_pton(AF_INET, k_zone[z].ips[i], addr);
            return 0;
        }
    }
    return (int)0x804101a8;     /* no such name */
}

/* SceNetResolverInfoEx as evo_dns.c believes it to be: ten 32-byte records, then
 * the counts. The mock writes it, and can be told to write something else. */
int sceNetResolverStartNtoaMultipleRecordsEx(int rid, const char *hostname,
                                             void *info, int timeout, int retry, int flags)
{
    (void)rid; (void)timeout; (void)retry; (void)flags;
    atomic_fetch_add(&m_multi, 1);
    unsigned char *buf = (unsigned char *)info;

    if (m_multi_mode == MULTI_ERROR) return (int)0x80410107;

    int z = zone_find(hostname);
    if (z < 0) return (int)0x804101a8;           /* no such name */

    int n = zone_count(z), v4 = 0;
    for (int i = 0; i < n; i++) {
        unsigned char *rec = buf + i * 32;
        int32_t af = 2;
        if (strchr(k_zone[z].ips[i], ':')) {
            af = 28;
            memset(rec, 0x20, 16);              /* an IPv6 address */
        } else {
            inet_pton(AF_INET, k_zone[z].ips[i], rec);
            v4++;
        }
        memcpy(rec + 16, &af, 4);
    }
    int32_t records = n, dns4 = v4, dns6 = n - v4;
    memcpy(buf + 320, &records, 4);
    memcpy(buf + 324, &dns4, 4);
    memcpy(buf + 328, &dns6, 4);

    if (m_multi_mode == MULTI_GARBAGE) {
        int32_t junk = 999;
        memcpy(buf + 320, &junk, 4);
    } else if (m_multi_mode == MULTI_OVERRUN) {
        buf[500] = 0x11;                        /* past the documented 384 bytes */
    }
    return 0;
}

/* ---- checks ---- */
static int g_fail, g_pass;

#define CHECK(cond, ...)                                            \
    do {                                                            \
        if (cond) { g_pass++; }                                     \
        else {                                                      \
            g_fail++;                                               \
            fprintf(stderr, "  FAIL %s:%d  ", __FILE__, __LINE__);  \
            fprintf(stderr, __VA_ARGS__);                           \
            fputc('\n', stderr);                                    \
        }                                                           \
    } while (0)

static void reset_counts(void)
{
    atomic_store(&m_pool_create, 0);  atomic_store(&m_pool_destroy, 0);
    atomic_store(&m_res_create, 0);   atomic_store(&m_res_destroy, 0);
    atomic_store(&m_ntoa, 0);         atomic_store(&m_multi, 0);
}

/* Resolves `host`; the whole chain lands in ips[] (dotted, up to 12) and the
 * first entry's port in *port. Returns the EAI code. */
#define MAXCHAIN 12
static int lookup_chain(const char *host, const char *service, int flags,
                        char ips[MAXCHAIN][16], int *n, int *port)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = flags;
    if (n) *n = 0;

    int rc = getaddrinfo(host, service, &hints, &res);
    if (rc == 0 && res) {
        for (struct addrinfo *a = res; a && n && *n < MAXCHAIN; a = a->ai_next) {
            struct sockaddr_in *sin = (struct sockaddr_in *)a->ai_addr;
            inet_ntop(AF_INET, &sin->sin_addr, ips[*n], 16);
            if (*n == 0 && port) *port = ntohs(sin->sin_port);
            (*n)++;
        }
        freeaddrinfo(res);
    }
    return rc;
}

static int lookup(const char *host, const char *service, int flags, char *ip, int *port)
{
    char ips[MAXCHAIN][16];
    int n = 0;
    int rc = lookup_chain(host, service, flags, ips, &n, port);
    if (rc == 0 && n > 0 && ip) strcpy(ip, ips[0]);
    return rc;
}

static int pools_balanced(void)
{
    return atomic_load(&m_open_pools) == 0 && atomic_load(&m_open_resolvers) == 0 &&
           atomic_load(&m_pool_create) >= atomic_load(&m_pool_destroy) &&
           atomic_load(&m_res_create) >= atomic_load(&m_res_destroy);
}

/* ---- concurrency ---- */
static void *worker(void *arg)
{
    (void)arg;
    static const char *names[] = { "media.example", "iptv.example", "cdn.example", "nope.example" };
    static const char *first[] = { "10.1.2.3", "10.4.5.6", "10.7.8.9", NULL };
    for (int i = 0; i < 200; i++) {
        char ips[MAXCHAIN][16];
        int n = 0;
        int rc = lookup_chain(names[i % 4], "443", 0, ips, &n, NULL);
        if (i % 4 == 3) {
            if (rc != EAI_NONAME) g_fail++;
        } else if (rc != 0 || n < 1 || strcmp(ips[0], first[i % 4]) != 0) {
            g_fail++;
        }
    }
    return NULL;
}

/* ========================= scenario: main ================================== */
static void scenario_main(void)
{
    char ip[16], ips[MAXCHAIN][16];
    int port = 0, n = 0;

    fprintf(stderr, "== names that need no lookup\n");
    reset_counts();
    CHECK(lookup("192.168.0.10", "80", 0, ip, &port) == 0 && !strcmp(ip, "192.168.0.10") && port == 80,
          "IPv4 literal (got %s:%d)", ip, port);
    CHECK(lookup("localhost", "8080", 0, ip, &port) == 0 && !strcmp(ip, "127.0.0.1") && port == 8080,
          "localhost");
    CHECK(lookup("[10.0.0.9]", "1", 0, ip, NULL) == 0 && !strcmp(ip, "10.0.0.9"), "bracketed literal");
    CHECK(atomic_load(&m_pool_create) == 0 && atomic_load(&m_ntoa) == 0 && atomic_load(&m_multi) == 0,
          "none of those touched the resolver");

    fprintf(stderr, "== a name goes to the console's resolver, and every address comes back\n");
    m_require_init = 1;          /* an app module: libSceNet not up until sceNetInit */
    CHECK(atomic_load(&m_init_calls) == 0, "sceNetInit has not run yet");
    CHECK(lookup_chain("media.example", "https", 0, ips, &n, &port) == 0, "resolves");
    CHECK(n == 3, "media.example has three addresses (%d)", n);
    CHECK(n == 3 && !strcmp(ips[0], "10.1.2.3") && !strcmp(ips[1], "10.1.2.4") && !strcmp(ips[2], "10.1.2.5"),
          "in the resolver's order: %s %s %s", ips[0], ips[1], ips[2]);
    CHECK(port == 443, "port %d", port);
    CHECK(atomic_load(&m_multi) == 1 && atomic_load(&m_ntoa) == 0,
          "one multi-record call and no single lookup (%d / %d)", atomic_load(&m_multi), atomic_load(&m_ntoa));
    CHECK(atomic_load(&m_init_calls) == 1, "sceNetInit ran exactly once, after the first pool failed (%d)",
          atomic_load(&m_init_calls));
    CHECK(atomic_load(&m_pool_create) == 2, "pool: failed once, then created (%d)", atomic_load(&m_pool_create));
    CHECK(pools_balanced() && atomic_load(&m_res_create) == atomic_load(&m_res_destroy),
          "pool and resolver released");
    CHECK(log_has("media.example -> 10.1.2.3, 10.1.2.4, 10.1.2.5") && log_has("3 addresses") &&
          log_has("console DNS"), "the log says what resolved, to what, and by which resolver");

    fprintf(stderr, "== the cache holds every address\n");
    reset_counts();
    CHECK(lookup_chain("MEDIA.Example", "80", 0, ips, &n, NULL) == 0 && n == 3 && !strcmp(ips[2], "10.1.2.5"),
          "cached, any case, all three (%d)", n);
    CHECK(atomic_load(&m_ntoa) == 0 && atomic_load(&m_multi) == 0 && atomic_load(&m_pool_create) == 0,
          "and it never went to the resolver");

    fprintf(stderr, "== duplicates dropped, IPv6 skipped, ten kept\n");
    CHECK(lookup_chain("dup.example", "80", 0, ips, &n, NULL) == 0 && n == 2 &&
          !strcmp(ips[0], "10.5.5.5") && !strcmp(ips[1], "10.5.5.6"), "duplicate dropped (%d)", n);
    CHECK(lookup_chain("mixed.example", "80", 0, ips, &n, NULL) == 0 && n == 2 &&
          !strcmp(ips[0], "10.6.6.6") && !strcmp(ips[1], "10.6.6.7"), "IPv6 record skipped (%d)", n);
    CHECK(lookup_chain("many.example", "80", 0, ips, &n, NULL) == 0 && n == 10 && !strcmp(ips[9], "10.8.0.10"),
          "ten addresses, the most a lookup returns (%d)", n);
    CHECK(pools_balanced(), "released");

    fprintf(stderr, "== a name the network's DNS blocks (0.0.0.0), and an IPv6-only name\n");
    reset_counts();
    CHECK(lookup("blocked.example", "443", 0, ip, NULL) == EAI_NONAME, "a blocked name is not found, not 0.0.0.0");
    CHECK(atomic_load(&m_ntoa) == 0, "the single lookup is not asked to second-guess an answer (%d)", atomic_load(&m_ntoa));
    CHECK(log_has("blocked.example: no answer") && log_has("blocked by the network's DNS"),
          "the log says the network's DNS blocked it");
    CHECK(lookup("v6only.example", "443", 0, ip, NULL) == EAI_NONAME, "an IPv6-only name has no IPv4 address");
    CHECK(pools_balanced() && atomic_load(&m_pool_create) == atomic_load(&m_pool_destroy), "released");
    reset_counts();
    CHECK(lookup("blocked.example", "443", 0, ip, NULL) == EAI_NONAME && atomic_load(&m_multi) == 1,
          "0.0.0.0 was not cached as an address: it is asked again (%d)", atomic_load(&m_multi));
    CHECK(lookup_chain("partial.example", "443", 0, ips, &n, NULL) == 0 && n == 1 && !strcmp(ips[0], "10.3.3.3"),
          "the blocked record is skipped and the real one kept (%d: %s)", n, n ? ips[0] : "");
    reset_counts();
    CHECK(lookup_chain("cdn.example", "443", 0, ips, &n, NULL) == 0 && n == 2 && atomic_load(&m_multi) == 1,
          "and none of that switched the multi-record call off (%d addresses, %d call)", n, atomic_load(&m_multi));
    CHECK(!log_has("single address from here on"), "nothing was marked broken");

    fprintf(stderr, "== a name with no answer\n");
    reset_counts();
    CHECK(lookup("missing.example", "80", 0, ip, NULL) == EAI_NONAME, "EAI_NONAME");
    CHECK(atomic_load(&m_multi) == 1 && atomic_load(&m_ntoa) == 1,
          "the multi-record call said no, so the single lookup was asked too (%d / %d)",
          atomic_load(&m_multi), atomic_load(&m_ntoa));
    CHECK(pools_balanced() && atomic_load(&m_res_create) == atomic_load(&m_res_destroy) &&
          atomic_load(&m_pool_create) == atomic_load(&m_pool_destroy),
          "resolver and pool released after a failed lookup");
    CHECK(lookup("missing.example", "80", 0, ip, NULL) == EAI_NONAME && atomic_load(&m_multi) == 2,
          "a failure is not cached");
    reset_counts();
    CHECK(lookup_chain("api.example", "80", 0, ips, &n, NULL) == 0 && atomic_load(&m_multi) == 1,
          "a name that does not exist does NOT make the multi-record call be given up on");
    CHECK(atomic_load(&m_init_calls) == 1, "and sceNetInit did not run again");
    CHECK(log_has("missing.example: no answer"), "the failure is logged");

    fprintf(stderr, "== no resolver to ask\n");
    reset_counts();
    m_resolver_fail = 1;
    CHECK(lookup("iptv.example", "80", 0, ip, NULL) == EAI_AGAIN, "resolver will not create: EAI_AGAIN");
    CHECK(atomic_load(&m_open_pools) == 0, "the pool is released when the resolver will not create");
    m_resolver_fail = 0;
    m_pool_fail = 1;
    CHECK(lookup("iptv.example", "80", 0, ip, NULL) == EAI_AGAIN, "pool will not create: EAI_AGAIN");
    CHECK(atomic_load(&m_open_pools) == 0 && atomic_load(&m_open_resolvers) == 0, "nothing leaked");
    CHECK(atomic_load(&m_init_calls) == 1, "sceNetInit still ran only once (%d)", atomic_load(&m_init_calls));
    CHECK(log_has("console resolver unavailable"), "and it says so");
    m_pool_fail = 0;
    CHECK(lookup("iptv.example", "80", 0, ip, NULL) == 0 && !strcmp(ip, "10.4.5.6"), "recovers when the resolver does");

    fprintf(stderr, "== arguments\n");
    reset_counts();
    char *empty = strdup("");                    /* exactly one byte, so ASan flags any read past it */
    CHECK(lookup(empty, "80", 0, ip, NULL) == EAI_NONAME, "empty name is refused");
    free(empty);
    CHECK(lookup("[", "80", 0, ip, NULL) == EAI_NONAME, "lone '['");
    CHECK(lookup("[]", "80", 0, ip, NULL) == EAI_NONAME, "'[]'");
    CHECK(lookup("[x", "80", 0, ip, NULL) == EAI_NONAME, "'[x'");
    CHECK(lookup("media.example", "80", AI_NUMERICHOST, ip, NULL) == EAI_NONAME, "AI_NUMERICHOST refuses a name");
    char longname[301];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    CHECK(lookup(longname, "80", 0, ip, NULL) == EAI_NONAME, "a name over 255 bytes is refused");
    CHECK(atomic_load(&m_multi) == 3,
          "only the three bracket-shaped names reached the resolver (%d)", atomic_load(&m_multi));
    {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET6;
        CHECK(getaddrinfo("media.example", "80", &hints, &res) == EAI_FAMILY, "IPv6 is refused");
        memset(&hints, 0, sizeof hints);
        hints.ai_flags = AI_PASSIVE;
        CHECK(getaddrinfo(NULL, "9000", &hints, &res) == 0 && res && !res->ai_next, "NULL node, passive: one entry");
        CHECK(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr == htonl(INADDR_ANY), "binds to any");
        freeaddrinfo(res);
        memset(&hints, 0, sizeof hints);
        hints.ai_flags = AI_CANONNAME;
        CHECK(getaddrinfo("media.example", "80", &hints, &res) == 0 && res && res->ai_canonname &&
              !strcmp(res->ai_canonname, "media.example") && res->ai_next && !res->ai_next->ai_canonname,
              "the canonical name is on the first entry only");
        freeaddrinfo(res);
        CHECK(getaddrinfo(NULL, NULL, NULL, &res) == EAI_NONAME, "no node and no service");
        CHECK(!strcmp(gai_strerror(EAI_AGAIN), "Temporary failure in name resolution"), "gai_strerror");
    }

    fprintf(stderr, "== eight threads at once\n");
    reset_counts();
    pthread_t th[8];
    int before_fail = g_fail;
    for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    CHECK(g_fail == before_fail, "every lookup answered correctly across threads");
    CHECK(atomic_load(&m_open_pools) == 0 && atomic_load(&m_open_resolvers) == 0, "no pool or resolver leaked");
    CHECK(atomic_load(&m_pool_create) == atomic_load(&m_pool_destroy) &&
          atomic_load(&m_res_create) == atomic_load(&m_res_destroy), "creates match destroys");
    CHECK(atomic_load(&m_init_calls) == 1, "sceNetInit stayed at one call (%d)", atomic_load(&m_init_calls));
}

/* ==================== scenarios: the multi-record call misbehaves ========== */

/* Common: after a failure the module falls back to the single address, says so,
 * and does not try the multi-record call again. */
static void expect_single_from_now_on(const char *what)
{
    char ips[MAXCHAIN][16];
    int n = 0;

    reset_counts();
    CHECK(lookup_chain("media.example", "443", 0, ips, &n, NULL) == 0, "%s: still resolves", what);
    CHECK(n == 1 && !strcmp(ips[0], "10.1.2.3"), "%s: the single address answers (%d: %s)", what, n, ips[0]);
    CHECK(atomic_load(&m_ntoa) == 1, "%s: via the single lookup (%d)", what, atomic_load(&m_ntoa));
    CHECK(pools_balanced() && atomic_load(&m_pool_create) == atomic_load(&m_pool_destroy),
          "%s: released", what);

    reset_counts();
    CHECK(lookup_chain("cdn.example", "443", 0, ips, &n, NULL) == 0 && n == 1 && !strcmp(ips[0], "10.7.8.9"),
          "%s: a cold host is single too", what);
    CHECK(atomic_load(&m_multi) == 0, "%s: the multi-record call is not tried again (%d)", what,
          atomic_load(&m_multi));
    CHECK(atomic_load(&m_ntoa) == 1, "%s: one single lookup, no double query", what);
}

static void scenario_multifail(void)
{
    fprintf(stderr, "== the multi-record call errors where the single lookup works\n");
    m_multi_mode = MULTI_ERROR;
    expect_single_from_now_on("multi errors");
    CHECK(log_has("multi-record lookup failed") && log_has("single address from here on"),
          "the log says the multi-record call was given up on");
}

static void scenario_garbage(void)
{
    fprintf(stderr, "== the multi-record call returns a layout that is not the documented one\n");
    m_multi_mode = MULTI_GARBAGE;
    expect_single_from_now_on("garbage layout");
    CHECK(log_has("unusable layout"), "the log says so");
}

static void scenario_overrun(void)
{
    fprintf(stderr, "== the multi-record call writes past the documented struct\n");
    m_multi_mode = MULTI_OVERRUN;
    expect_single_from_now_on("overrun");
    CHECK(log_has("unusable layout"), "the log says so");
}

static void scenario_killswitch(void)
{
    char ips[MAXCHAIN][16];
    int n = 0;

    fprintf(stderr, "== the kill switch\n");
    FILE *f = fopen(EVO_DNS_SINGLE_FLAG, "w");
    if (f) fclose(f);
    reset_counts();
    CHECK(lookup_chain("media.example", "443", 0, ips, &n, NULL) == 0 && n == 1 && !strcmp(ips[0], "10.1.2.3"),
          "with the file present: one address (%d)", n);
    CHECK(atomic_load(&m_multi) == 0 && atomic_load(&m_ntoa) == 1,
          "the multi-record call is not made (%d / %d)", atomic_load(&m_multi), atomic_load(&m_ntoa));

    remove(EVO_DNS_SINGLE_FLAG);
    reset_counts();
    CHECK(lookup_chain("cdn.example", "443", 0, ips, &n, NULL) == 0 && n == 2, "file removed: all addresses (%d)", n);
    CHECK(atomic_load(&m_multi) == 1, "the switch is not sticky - the multi-record call is back (%d)",
          atomic_load(&m_multi));
    CHECK(!log_has("single address from here on"), "and nothing was marked broken");
}

int main(int argc, char **argv)
{
    const char *scenario = argc > 1 ? argv[1] : "main";

    if (!strcmp(scenario, "main"))            scenario_main();
    else if (!strcmp(scenario, "multifail"))  scenario_multifail();
    else if (!strcmp(scenario, "garbage"))    scenario_garbage();
    else if (!strcmp(scenario, "overrun"))    scenario_overrun();
    else if (!strcmp(scenario, "killswitch")) scenario_killswitch();
    else { fprintf(stderr, "unknown scenario '%s'\n", scenario); return 2; }

    fprintf(stderr, "\n[%s] %d checks passed, %d failed\n", scenario, g_pass, g_fail);
    return g_fail ? 1 : 0;
}
