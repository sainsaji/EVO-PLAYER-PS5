/*
 * The browser engine seam (#237). The engine (ProsperoWeb now, the WPE GPU
 * engine later) runs outside EVO as separate payload processes; EVO only
 * starts and stops them, shows their frames and passes input in.
 *
 * Phase 0, spike S1 - lifetime only. EVO sends ProsperoWeb's broker ELF to
 * elfldr, connects to the broker on 127.0.0.1:17771 and keeps that one
 * connection for as long as the engine should live. The broker kills every
 * child and exits when the connection closes, so closing it is the stop, and
 * a killed EVO (PS button, `evo-remote.sh close`) stops it the same way.
 *
 * Dev trigger for the spike: /mnt/usb0/evo_browser_spike. Its first line may
 * name the broker ELF (default /mnt/usb0/prosperoweb-broker.elf).
 */
#ifndef EVO_BROWSER_ENGINE_H
#define EVO_BROWSER_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#define EVO_BROWSER_BROKER_PORT 17771

/* At boot: starts the engine in the background when the spike trigger file
 * exists. Never blocks. */
void evo_browser_engine_boot(void);

/* Starts the engine in the background (send the broker, connect, ping).
 * Never blocks. 0 when the start was begun, -1 when it already runs or the
 * ELF could not be read. */
int evo_browser_engine_start(const char *broker_elf_path);

/* 1 once the broker answered the ping and EVO holds its connection. */
int evo_browser_engine_running(void);

/* Soft close, first frame: drop the broker connection so it terminates its
 * children and exits. Never blocks. */
void evo_browser_engine_stop(void);

/* Soft close, before parking: wait up to timeout_ms for the start worker to
 * finish and for 17771 to stop answering. 0 when the broker is gone (or was
 * never started), -1 when it still answers. Only acts if EVO started one. */
int evo_browser_engine_wait_stopped(int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
