/*
 * net_bench.c - evo_net request latency and correctness against
 * tests/perf/latency_server.py (plain HTTP, the host build has no TLS).
 *
 *   net_bench PORT N
 *
 * Prints the mean time of N sequential GETs of /json, then checks the cases a
 * kept-open connection must get right: a chunked body with a trailer, a 204,
 * a "Connection: close" response, and a pooled connection the server closed
 * while idle. Ends with the number of connections the server accepted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "evo_net.h"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int get(int port, const char *path, size_t *len, int *status)
{
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d%s", port, path);
    char *body = NULL;
    *len = 0;
    *status = 0;
    int rc = evo_net_http_get_sync(url, NULL, 0, &body, len, status);
    free(body);
    return rc;
}

int main(int argc, char **argv)
{
    const int port = argc > 1 ? atoi(argv[1]) : 18080;
    const int n = argc > 2 ? atoi(argv[2]) : 40;
    size_t len, ref = 0;
    int st, fails = 0;

    double t0 = now_ms();
    for (int i = 0; i < n; i++) {
        if (get(port, "/json", &len, &st) != 0 || st != 200) fails++;
        if (i == 0) ref = len;
        else if (len != ref) fails++;
    }
    const double per = (now_ms() - t0) / n;
    printf("sequential GET /json: %d requests, %.1f ms each, %d failures (body %zu B)\n", n, per, fails, ref);

    int rc = get(port, "/chunked", &len, &st);
    printf("chunked + trailer: rc=%d status=%d len=%zu %s\n", rc, st, len, (rc == 0 && len == ref) ? "OK" : "FAIL");
    double t = now_ms();
    rc = get(port, "/empty", &len, &st);
    printf("204 no body: rc=%d status=%d %.0f ms %s\n", rc, st, now_ms() - t, (st == 204 && now_ms() - t < 1000) ? "OK" : "FAIL");
    rc = get(port, "/close", &len, &st);
    printf("Connection: close: rc=%d len=%zu %s\n", rc, len, (rc == 0 && len == ref) ? "OK" : "FAIL");
    rc = get(port, "/json", &len, &st);
    sleep(2);                          /* the server drops the idle connection after 1 s */
    t = now_ms();
    rc = get(port, "/json", &len, &st);
    printf("after server idle close: rc=%d len=%zu %.0f ms %s\n", rc, len, now_ms() - t, (rc == 0 && len == ref) ? "OK" : "FAIL");

    char *body = NULL;
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/stats", port);
    evo_net_http_get_sync(url, NULL, 0, &body, &len, &st);
    printf("connections accepted by the server: %s (for %d + 5 requests)\n", body ? body : "?", n);
    free(body);
    return 0;
}
