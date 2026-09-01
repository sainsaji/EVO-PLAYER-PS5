/*
 * evo_packet_queue.c — bounded AVPacket FIFO shared by the demux and decode
 * threads.
 *
 * Verbatim move of the PacketQueue helpers from main.c (Track A step A1 of
 * docs/modularisation-plan.md). No behaviour change: the demux still owns the
 * two queue instances until step A5.
 */
#include "evo_packet_queue.h"

void packet_queue_clear(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);

    for (int i = 0; i < PACKET_QUEUE_SIZE; i++) {
        if (q->packets[i]) {
            av_packet_free(&q->packets[i]);
            q->packets[i] = NULL;
        }
    }

    q->read = 0;
    q->write = 0;
    q->count = 0;

    pthread_mutex_unlock(&q->mutex);
}

int packet_queue_push(PacketQueue *q, AVPacket *pkt) {
    pthread_mutex_lock(&q->mutex);

    if (q->count >= PACKET_QUEUE_SIZE) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }

    q->packets[q->write] = av_packet_clone(pkt);
    q->write = (q->write + 1) % PACKET_QUEUE_SIZE;
    q->count++;

    pthread_mutex_unlock(&q->mutex);
    return 1;
}

AVPacket *packet_queue_pop(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);

    if (q->count <= 0) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }

    AVPacket *pkt = q->packets[q->read];
    q->packets[q->read] = NULL;
    q->read = (q->read + 1) % PACKET_QUEUE_SIZE;
    q->count--;

    pthread_mutex_unlock(&q->mutex);
    return pkt;
}

int packet_queue_count(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);
    int c = q->count;
    pthread_mutex_unlock(&q->mutex);
    return c;
}
