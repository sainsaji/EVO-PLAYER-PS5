/*
 * Loopback sockets and the ELF loader (elfldr on 127.0.0.1:9021).
 *
 * EVO starts helper payloads (the NTFS disk helper, the browser engine's
 * broker) by writing the raw ELF to the loader. Every call here is bounded:
 * connect to loopback answers at once (refused or accepted), and sends and
 * receives carry SO_SNDTIMEO / SO_RCVTIMEO.
 */
#ifndef EVO_ELFLDR_H
#define EVO_ELFLDR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVO_ELFLDR_PORT 9021

/* A TCP socket to 127.0.0.1:port with send/receive timeouts of timeout_ms,
 * or -1 when nothing listens. */
int evo_loopback_connect(int port, int timeout_ms);

/* 1 if something accepts on 127.0.0.1:port right now, else 0. The
 * connection is closed straight away. */
int evo_loopback_port_open(int port);

/* Write all of buf to s; 0 on success, -1 on error or timeout. */
int evo_sock_send_all(int s, const void *buf, size_t n);

/* Read exactly n bytes; 0 on success, -1 on error, timeout or EOF. */
int evo_sock_recv_all(int s, void *buf, size_t n);

/* Send a raw ELF image to the loader. 0 when the whole image was written,
 * -1 when no loader listens or the send failed. The payload's own result is
 * not reported: callers check for whatever the payload serves. */
int evo_elfldr_send(const void *elf, size_t len, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
