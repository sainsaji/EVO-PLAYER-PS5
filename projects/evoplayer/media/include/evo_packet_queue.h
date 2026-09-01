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

#include <libavcodec/packet.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PACKET_QUEUE_SIZE 512

typedef struct {
    AVPacket *packets[PACKET_QUEUE_SIZE];
    int read;
    int write;
    int count;
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

#ifdef __cplusplus
}
#endif

#endif /* EVO_PACKET_QUEUE_H */
