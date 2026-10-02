/*
 * evo_packet_queue.h — bounded AVPacket FIFO shared by the demux and decode
 * threads.
 *
 * A fixed-capacity ring of cloned AVPackets guarded by a single mutex. push
 * clones the packet in, pop hands ownership out, clear frees everything.
 * Pure leaf: no clocks, no threads of its own, FFmpeg the only dependency.
 */
#ifndef EVO_PACKET_QUEUE_H
#define EVO_PACKET_QUEUE_H

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/packet.h>

/*
 * Ring slots. A hard ceiling on the read-ahead, not a budget: a push into a
 * full ring is dropped, so the caps in Bridge.cpp must always bind first. 512
 * slots was 4 s of 24 fps video and under a second of TrueHD, too tight once
 * the read-ahead is sized in seconds. The array is pointers, so 2048 slots
 * cost 16 KB per queue.
 */
#define PACKET_QUEUE_SIZE 2048

typedef struct {
    AVPacket *packets[PACKET_QUEUE_SIZE];
    int read;
    int write;
    int count;
    /*
     * Queued payload. A packet count says nothing about memory when one source
     * is a 2 Mbit/s IPTV channel and the next a 54 Mbit/s UHD remux, and the
     * flexible pool is the scarce one on this platform - so the demux caps
     * bound bytes as well as packets, and this is what they read.
     */
    long long bytes;
    pthread_mutex_t mutex;
} PacketQueue;

/* Free every queued packet and reset the ring to empty. */
void packet_queue_clear(PacketQueue *q);

/* Clone `pkt` into the queue. Returns 1 on success, 0 if the queue is full. */
int packet_queue_push(PacketQueue *q, AVPacket *pkt);

/* Pop the oldest packet, transferring ownership to the caller. NULL if empty. */
AVPacket *packet_queue_pop(PacketQueue *q);

/* Current number of queued packets. */
int packet_queue_count(PacketQueue *q);

/* Total payload bytes currently queued. */
long long packet_queue_bytes(PacketQueue *q);

#ifdef __cplusplus
}
#endif

#endif /* EVO_PACKET_QUEUE_H */
