#pragma once

/*
 * Who may touch play_fmt, the open file's AVFormatContext, while it changes.
 *
 * playback opens, probes and closes play_fmt on worker threads too (provider
 * auto-next and episode start, the web/cast handoff), while screens on the UI
 * thread walk its streams: the subtitle picker, the OSD badges, the perf HUD,
 * Media Info. A walk that met a close read freed memory (#279).
 *
 * Writers hold the lock across the open + probe and across each close.
 * Readers never wait for it: PlayFmtRead try-locks, and if a writer has it
 * (an open or close can block on the network for seconds) the reader sees no
 * file for that frame. Hold a PlayFmtRead only across a memory-only walk.
 *
 *     evo::PlayFmtRead fmt;            // fmt.get() is null while busy
 *     if (AVFormatContext* f = fmt.get()) { ... }
 */

extern "C" {
#include <libavformat/avformat.h>

void evo_play_fmt_lock(void);
void evo_play_fmt_unlock(void);
/* 1 and the lock held, or 0 when a writer holds it. */
int  evo_play_fmt_trylock(void);
/* play_fmt itself; only meaningful while the lock is held. */
AVFormatContext* evo_play_fmt_get(void);
}

namespace evo {

class PlayFmtRead {
public:
    PlayFmtRead() : m_locked(evo_play_fmt_trylock() != 0) {}
    ~PlayFmtRead() { if (m_locked) evo_play_fmt_unlock(); }
    PlayFmtRead(const PlayFmtRead&) = delete;
    PlayFmtRead& operator=(const PlayFmtRead&) = delete;

    /* The open file, or null when there is none or a writer is busy. */
    AVFormatContext* get() const { return m_locked ? evo_play_fmt_get() : nullptr; }
    /* Done with the walk before the scope ends; get() is null afterwards. */
    void release() { if (m_locked) { m_locked = false; evo_play_fmt_unlock(); } }

private:
    bool m_locked;
};

/* Writer side: lock for this scope (blocking). */
class PlayFmtWrite {
public:
    PlayFmtWrite() { evo_play_fmt_lock(); }
    ~PlayFmtWrite() { evo_play_fmt_unlock(); }
    PlayFmtWrite(const PlayFmtWrite&) = delete;
    PlayFmtWrite& operator=(const PlayFmtWrite&) = delete;
};

}  // namespace evo
