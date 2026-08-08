/* EVO Player - audioout_test
 *
 * Milestone 4. Exercises the audio path:
 *
 *     sceAudioOutInit -> sceAudioOutOpen (48 kHz, stereo, S16)
 *                     -> sceAudioOutOutput in grain-sized blocks
 *
 * Plays a generated sine wave. This matters more than it looks: AudioOut is
 * the sink for every codec EVO Player cares about, and a lot of the reported
 * ProsperoPlayer bugs ("E-AC3 silent", "FLAC varies") are ultimately about
 * getting decoded PCM into this call correctly and on time.
 *
 * AudioOut is fixed at 48 kHz. Anything a decoder produces at another rate
 * has to be resampled (libswresample) before it reaches here - which is why
 * the FFmpeg build must always include swresample.
 *
 * NOT YET RUN ON HARDWARE by the author of this scaffold. Every symbol used
 * is present in sce_stubs/libSceAudioOut.c so it links; record the first
 * console result in docs/validation.md.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_ps5.h"

/* AudioOut delivers audio in fixed-size "grains". 256 frames at 48 kHz is
 * 5.33 ms per block - the standard low-latency choice. */
#define GRAIN        SCE_AUDIO_OUT_GRAIN_DEFAULT
#define SAMPLE_RATE  SCE_AUDIO_OUT_SAMPLE_RATE
#define CHANNELS     2
#define TONE_HZ      440.0     /* concert A */
#define SECONDS      3
#define AMPLITUDE    0.25      /* headroom; full scale is unpleasant on TVs */

int
main(void)
{
    int rc;
    int32_t userId = 0;
    int32_t handle;

    printf("=== EVO Player audioout_test ===\n");
    printf("format: %d Hz, %d ch, S16, grain %d frames\n",
           SAMPLE_RATE, CHANNELS, GRAIN);

    rc = sceUserServiceInitialize(NULL);
    if (rc < 0)
        printf("sceUserServiceInitialize -> 0x%08x (continuing)\n", rc);

    rc = sceUserServiceGetInitialUser(&userId);
    if (rc < 0) {
        printf("sceUserServiceGetInitialUser failed: 0x%08x\n", rc);
        evo_notify("EVO audioout_test: no logged-in user");
        return EXIT_FAILURE;
    }

    rc = sceAudioOutInit();
    /* Returns "already initialised" if a previous payload got here first.
     * That is fine; only a genuine error should stop us. */
    if (rc < 0 && rc != (int)0x800f0002) {
        printf("sceAudioOutInit failed: 0x%08x\n", rc);
        evo_notify("EVO audioout_test: sceAudioOutInit failed 0x%08x", rc);
        return EXIT_FAILURE;
    }

    handle = sceAudioOutOpen(userId,
                             SCE_AUDIO_OUT_PORT_TYPE_MAIN,
                             0,
                             GRAIN,
                             SAMPLE_RATE,
                             SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (handle < 0) {
        printf("sceAudioOutOpen failed: 0x%08x\n", handle);
        evo_notify("EVO audioout_test: sceAudioOutOpen failed 0x%08x", handle);
        return EXIT_FAILURE;
    }
    printf("audio handle: %d\n", handle);

    /* Volume is per-channel; 32768 is 0 dB. */
    int32_t vol[CHANNELS];
    for (int i = 0; i < CHANNELS; i++)
        vol[i] = SCE_AUDIO_VOLUME_0DB;
    sceAudioOutSetVolume(handle, (1 << CHANNELS) - 1, vol);

    /* -- generate and stream ---------------------------------------------- */
    int16_t block[GRAIN * CHANNELS];
    const int total_blocks = (SAMPLE_RATE * SECONDS) / GRAIN;
    double phase = 0.0;
    const double phase_step = 2.0 * M_PI * TONE_HZ / (double)SAMPLE_RATE;

    printf("playing %d Hz for %d s (%d blocks)...\n",
           (int)TONE_HZ, SECONDS, total_blocks);
    evo_notify("EVO audioout_test: playing %d Hz sine for %d s",
               (int)TONE_HZ, SECONDS);

    for (int b = 0; b < total_blocks; b++) {
        for (int f = 0; f < GRAIN; f++) {
            /* A short fade in/out avoids the click that an abrupt start or
             * stop produces - easy to mistake for a decoder bug later. */
            double env = 1.0;
            int frame_index = b * GRAIN + f;
            int total_frames = total_blocks * GRAIN;
            int fade = SAMPLE_RATE / 20;         /* 50 ms */
            if (frame_index < fade)
                env = (double)frame_index / fade;
            else if (frame_index > total_frames - fade)
                env = (double)(total_frames - frame_index) / fade;

            int16_t s = (int16_t)(sin(phase) * AMPLITUDE * env * 32767.0);
            block[f * CHANNELS + 0] = s;      /* left  */
            block[f * CHANNELS + 1] = s;      /* right */

            phase += phase_step;
            if (phase >= 2.0 * M_PI)
                phase -= 2.0 * M_PI;
        }

        /* Blocks until the port can accept another grain - this call is the
         * clock that paces playback. */
        rc = sceAudioOutOutput(handle, block);
        if (rc < 0) {
            printf("sceAudioOutOutput failed at block %d: 0x%08x\n", b, rc);
            break;
        }
    }

    /* Draining: one silent grain, then a final NULL output, so the port does
     * not cut off the tail of the last block. */
    memset(block, 0, sizeof block);
    sceAudioOutOutput(handle, block);
    sceAudioOutOutput(handle, NULL);

    sceAudioOutClose(handle);

    printf("audioout_test done\n");
    evo_notify("EVO audioout_test: finished cleanly");
    return EXIT_SUCCESS;
}
