/*
 * evo_controller_speaker.h — DualSense controller speaker routing for in-movie phone audio.
 */
#ifndef EVO_CONTROLLER_SPEAKER_H
#define EVO_CONTROLLER_SPEAKER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize controller speaker with active user id */
void evo_controller_speaker_init(int user_id);

/* Enable / disable controller phone audio feature */
void evo_controller_speaker_set_enabled(int enabled);
int  evo_controller_speaker_is_enabled(void);

/* Start controller speaker thread / audio port when playback begins */
void evo_controller_speaker_start(int enabled);

/* Feed one audio block from decoded PCM (called from audio output thread) */
void evo_controller_speaker_feed(const int16_t *samples, size_t frames, int channels, double clock_seconds);

/* Stop / close controller speaker audio port */
void evo_controller_speaker_stop(void);

/* Flush any pending controller audio grains (on seek) */
void evo_controller_speaker_flush(void);

/* Check if phone audio cue is active right now */
int  evo_controller_speaker_is_phone_active(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_CONTROLLER_SPEAKER_H */
