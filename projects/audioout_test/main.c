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

/* ---------------------------------------------------------------------------
 * Surround test.
 *
 * Opens an 8-channel port and plays a tone through one speaker at a time,
 * announcing each on stdout. Two things this establishes that nothing else
 * can, and that must be known before touching the player's audio path:
 *
 *   1. Whether sceAudioOutOpen accepts S16_8CH (format 2) at all from a
 *      homebrew app slot.
 *   2. The PS5's channel INTERLEAVE ORDER. FFmpeg's native 7.1 order is
 *      FL FR FC LFE BL BR SL SR. If the PS5 differs, feeding it FFmpeg's
 *      order puts dialogue (FC) into a surround speaker - audibly worse than
 *      plain stereo. The only way to know is to drive one index at a time and
 *      listen.
 *
 * No decoder is involved, so a failure here is unambiguously the output path.
 * ------------------------------------------------------------------------ */
#define SURROUND_CH  8

static int
surround_test(void)
{
    /* Index order assumed below is FFmpeg's. Whether the label matches the
     * speaker that actually sounds is exactly what we are measuring.
     *
     * Each channel gets a DISTINCT frequency. With one tone per speaker you
     * can only judge direction, which is hard for centre-vs-phantom-centre;
     * with distinct pitches you can also tell channels apart when two sound
     * at once, and spot a downmix immediately (two pitches from one speaker).
     *
     * On a 5.1 system indices 6 and 7 do not exist. Silence there is the
     * CORRECT result, not a failure. */
    static const struct {
        const char *name;
        double      hz;
        int         in_51;
    } chan[SURROUND_CH] = {
        { "front left    FL",  330.0, 1 },   /* E4  */
        { "front right   FR",  440.0, 1 },   /* A4  */
        { "centre        FC",  554.0, 1 },   /* C#5 */
        { "LFE / sub     LFE",  55.0, 1 },   /* A1  - subs cannot do 440 Hz */
        { "back left     BL",  659.0, 1 },   /* E5  */
        { "back right    BR",  880.0, 1 },   /* A5  */
        { "side left     SL", 1109.0, 0 },   /* absent on 5.1 */
        { "side right    SR", 1319.0, 0 },   /* absent on 5.1 */
    };

    int rc = sceAudioOutInit();
    if (rc < 0 && rc != (int)0x800f0002) {
        printf("sceAudioOutInit failed: 0x%08x\n", rc);
        return -1;
    }

    int32_t handle = sceAudioOutOpen(SCE_VIDEO_OUT_USER_ID_SYSTEM,
                                     SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0,
                                     GRAIN, SAMPLE_RATE,
                                     SCE_AUDIO_OUT_PARAM_FORMAT_S16_8CH);
    if (handle < 0) {
        printf("sceAudioOutOpen(S16_8CH) failed: 0x%08x\n", handle);
        printf("  8-channel output is not available from this context;\n");
        printf("  the player would have to stay on stereo.\n");
        return -1;
    }
    printf("8-channel port open, handle %d\n\n", handle);

    int32_t vol[SURROUND_CH];
    for (int i = 0; i < SURROUND_CH; i++)
        vol[i] = SCE_AUDIO_VOLUME_0DB;
    sceAudioOutSetVolume(handle, (1 << SURROUND_CH) - 1, vol);

    int16_t block[GRAIN * SURROUND_CH];
    /* 2 s per channel, then 1 s of silence. Long enough to walk to a speaker;
     * the gap makes the boundary between channels unambiguous. */
    const int blocks_per_ch = (SAMPLE_RATE * 2) / GRAIN;
    const int gap_blocks    = SAMPLE_RATE / GRAIN;
    double phase = 0.0;

    /* Emit one channel for `blocks` blocks. mask selects the channels. */
    #define EMIT(mask, hz, blocks)                                            \
        do {                                                                  \
            double step = 2.0 * M_PI * (hz) / (double)SAMPLE_RATE;            \
            int total = (blocks) * GRAIN;                                     \
            int fade = SAMPLE_RATE / 20;                                      \
            for (int b = 0; b < (blocks); b++) {                              \
                for (int f = 0; f < GRAIN; f++) {                             \
                    int idx = b * GRAIN + f;                                  \
                    double env = 1.0;                                         \
                    if (idx < fade)              env = (double)idx / fade;    \
                    else if (idx > total - fade) env = (double)(total-idx)/fade; \
                    int16_t s = (int16_t)(sin(phase) * AMPLITUDE * env * 32767.0); \
                    phase += step;                                            \
                    if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;             \
                    for (int c = 0; c < SURROUND_CH; c++)                     \
                        block[f * SURROUND_CH + c] = ((mask) & (1u << c)) ? s : 0; \
                }                                                             \
                if (sceAudioOutOutput(handle, block) < 0) {                   \
                    printf("    sceAudioOutOutput failed\n");                 \
                    break;                                                    \
                }                                                             \
            }                                                                 \
            memset(block, 0, sizeof block);                                   \
            for (int g = 0; g < gap_blocks; g++)                              \
                sceAudioOutOutput(handle, block);                             \
        } while (0)

    /* -- reference: both fronts together ---------------------------------
     * Establishes what "correct" sounds like before anything is in doubt.
     * If this is not clearly stereo across the two front speakers, the
     * console itself is downmixing and nothing below can be trusted. */
    printf("REFERENCE: front left + front right together\n");
    fflush(stdout);
    evo_notify("EVO surround: REFERENCE - both fronts");
    EMIT((1u << 0) | (1u << 1), 440.0, blocks_per_ch);

    /* -- one speaker at a time -------------------------------------------- */
    for (int ch = 0; ch < SURROUND_CH; ch++) {
        if (!chan[ch].in_51) {
            printf("  skipping %d: %s  (%.0f Hz) - not present on 5.1\n",
                   ch, chan[ch].name, chan[ch].hz);
            continue;
        }
        printf("  playing  %d: %s  (%.0f Hz)\n", ch, chan[ch].name, chan[ch].hz);
        fflush(stdout);
        evo_notify("EVO surround %d: %s", ch, chan[ch].name);
        EMIT(1u << ch, chan[ch].hz, blocks_per_ch);
    }

    /* -- rotation: FL -> FC -> FR -> BR -> BL -----------------------------
     * A clockwise circle. Direction of travel is much easier to judge than
     * any single static tone, so this catches a swapped pair that the
     * one-at-a-time pass might not. */
    printf("\nROTATION (clockwise): FL -> centre -> FR -> BR -> BL\n");
    fflush(stdout);
    evo_notify("EVO surround: rotation, should travel clockwise");
    static const int circle[] = { 0, 2, 1, 5, 4 };
    for (size_t i = 0; i < sizeof circle / sizeof *circle; i++)
        EMIT(1u << circle[i], 440.0, SAMPLE_RATE / GRAIN);   /* 1 s each */

    #undef EMIT

    sceAudioOutOutput(handle, NULL);
    sceAudioOutClose(handle);
    printf("\nsurround test done\n");
    return 0;
}

int
main(int argc, char **argv)
{
    int rc;
    int32_t handle;

    if (argc > 1 && strcmp(argv[1], "surround") == 0) {
        printf("=== EVO Player audioout_test (7.1 surround) ===\n");
        printf("format: %d Hz, %d ch, S16, grain %d frames\n\n",
               SAMPLE_RATE, SURROUND_CH, GRAIN);
        int r = surround_test();
        evo_notify("EVO surround test %s", r == 0 ? "finished" : "FAILED");
        return r == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /* Same story as videoout_test: a payload has no user session, so
     * sceUserServiceGetInitialUser returns 0x80940004 (measured on 12.70).
     * Pass the system user id instead. */
    const int32_t userId = SCE_VIDEO_OUT_USER_ID_SYSTEM;   /* 0xff */

    printf("=== EVO Player audioout_test ===\n");
    printf("format: %d Hz, %d ch, S16, grain %d frames\n",
           SAMPLE_RATE, CHANNELS, GRAIN);

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
