#include "evo/services/PlaybackController.hpp"
#include "evo/interfaces/IMediaMetadataService.hpp"
#include "evo/Application.hpp"

#include "pp_playback.h"
#include "evo_playback.h"
#include "evo_demux.h"
#include "evo_audio_out.h"
#include "evo_audio_resample.h"
#include "evo_packet_queue.h"
#include "evo_vdec.h"
#include "evo_subtitle.h"
#include "evo_stream_io.h"
#include "evo_recent.h"
#include "evo_toast.h"
#include "evo_data_path.h"
#include "evo_boot_log.h"
#include "prospero_thumbnail.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutClose(int handle);
}

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

// Global playback instance
extern pp_playback g_pp_pb;
extern AVFormatContext *play_fmt;
extern AVCodecContext *audio_ctx;
extern evo_vdec *g_vdec;
extern int g_vdec_force_ffmpeg;
extern AVPacket *video_pending_pkt;
extern struct SwsContext *play_sws;
extern int screen;
extern int player_paused;
extern double media_duration_sec;
extern double resume_base_offset_seconds;
extern double requested_resume_seek_pos;
extern char current_media_path[768];
extern int evo_audio_channels;

namespace evo {

static uint64_t GetCurrentTimeMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec / 1000ULL);
}

PlaybackController::PlaybackController()
    : m_playbackFsm(PlaybackState::Stopped, "PlaybackFSM")
{
    initStateMachine();
}

PlaybackController::~PlaybackController() {
    stopPlayback();
}

void PlaybackController::initStateMachine() {
    m_playbackFsm
        .addState(PlaybackState::Stopped, "Stopped")
        .addState(PlaybackState::Opening, "Opening")
        .addState(PlaybackState::Playing, "Playing",
            []() {
                player_paused = 0;
                pp_playback_resume(&g_pp_pb);
            })
        .addState(PlaybackState::Paused, "Paused",
            []() {
                player_paused = 1;
                pp_playback_pause(&g_pp_pb);
            })
        .addState(PlaybackState::Scrubbing, "Scrubbing")
        .addState(PlaybackState::Seeking, "Seeking")
        .addState(PlaybackState::Finished, "Finished")
        .addState(PlaybackState::Error, "Error");

    // From Stopped
    m_playbackFsm
        .addTransition(PlaybackState::Stopped, PlaybackEvent::Open, PlaybackState::Opening)
        .addTransition(PlaybackState::Stopped, PlaybackEvent::Play, PlaybackState::Playing);

    // From Opening
    m_playbackFsm
        .addTransition(PlaybackState::Opening, PlaybackEvent::Play, PlaybackState::Playing)
        .addTransition(PlaybackState::Opening, PlaybackEvent::Fail, PlaybackState::Error)
        .addTransition(PlaybackState::Opening, PlaybackEvent::Stop, PlaybackState::Stopped);

    // From Playing
    m_playbackFsm
        .addTransition(PlaybackState::Playing, PlaybackEvent::Pause, PlaybackState::Paused)
        .addTransition(PlaybackState::Playing, PlaybackEvent::StartScrub, PlaybackState::Scrubbing)
        .addTransition(PlaybackState::Playing, PlaybackEvent::Seek, PlaybackState::Seeking)
        .addTransition(PlaybackState::Playing, PlaybackEvent::Finish, PlaybackState::Finished)
        .addTransition(PlaybackState::Playing, PlaybackEvent::Stop, PlaybackState::Stopped)
        .addTransition(PlaybackState::Playing, PlaybackEvent::Fail, PlaybackState::Error);

    // From Paused
    m_playbackFsm
        .addTransition(PlaybackState::Paused, PlaybackEvent::Resume, PlaybackState::Playing)
        .addTransition(PlaybackState::Paused, PlaybackEvent::Play, PlaybackState::Playing)
        .addTransition(PlaybackState::Paused, PlaybackEvent::StartScrub, PlaybackState::Scrubbing)
        .addTransition(PlaybackState::Paused, PlaybackEvent::Seek, PlaybackState::Seeking)
        .addTransition(PlaybackState::Paused, PlaybackEvent::Stop, PlaybackState::Stopped);

    // From Scrubbing
    m_playbackFsm
        .addTransition(PlaybackState::Scrubbing, PlaybackEvent::ConfirmScrub, PlaybackState::Seeking)
        .addTransition(PlaybackState::Scrubbing, PlaybackEvent::CancelScrub, PlaybackState::Playing)
        .addTransition(PlaybackState::Scrubbing, PlaybackEvent::Stop, PlaybackState::Stopped);

    // From Seeking
    m_playbackFsm
        .addTransition(PlaybackState::Seeking, PlaybackEvent::Play, PlaybackState::Playing)
        .addTransition(PlaybackState::Seeking, PlaybackEvent::Pause, PlaybackState::Paused)
        .addTransition(PlaybackState::Seeking, PlaybackEvent::Stop, PlaybackState::Stopped)
        .addTransition(PlaybackState::Seeking, PlaybackEvent::Fail, PlaybackState::Error);

    // From Finished / Error
    m_playbackFsm
        .addTransition(PlaybackState::Finished, PlaybackEvent::Open, PlaybackState::Opening)
        .addTransition(PlaybackState::Finished, PlaybackEvent::Stop, PlaybackState::Stopped)
        .addTransition(PlaybackState::Error, PlaybackEvent::Open, PlaybackState::Opening)
        .addTransition(PlaybackState::Error, PlaybackEvent::Stop, PlaybackState::Stopped);
}

bool PlaybackController::isActive() const {
    return evo_pb_is_active() || (video_stream_index >= 0 && video_thread_running) || m_musicMode;
}

bool PlaybackController::isPaused() const {
    return m_playbackFsm.getCurrentState() == PlaybackState::Paused;
}

bool PlaybackController::isScrubbing() const {
    return m_playbackFsm.getCurrentState() == PlaybackState::Scrubbing;
}

double PlaybackController::getPositionSeconds() const {
    if (isScrubbing()) {
        return m_scrubTargetSeconds;
    }
    double pos = m_resumeBaseOffset + evo_pb_position_s();
    return (pos < 0.0) ? 0.0 : pos;
}

double PlaybackController::getPercentage() const {
    if (m_durationSeconds <= 0.1) return 0.0;
    double pos = getPositionSeconds();
    double pct = pos / m_durationSeconds;
    if (pct < 0.0) pct = 0.0;
    if (pct > 1.0) pct = 1.0;
    return pct;
}

void PlaybackController::togglePause() {
    setPaused(!isPaused());
}

void PlaybackController::setPaused(bool paused) {
    if (paused) {
        m_playbackFsm.postEvent(PlaybackEvent::Pause);
    } else {
        m_playbackFsm.postEvent(PlaybackEvent::Resume);
    }
}

void PlaybackController::stopPlayback() {
    if (isActive()) {
        saveResumePosition();
    }

    m_playbackFsm.postEvent(PlaybackEvent::Stop);
    player_paused = 0;
    resetScrubHold();

    prospero_subtitle_clear();

    if (demux_thread_running) {
        demux_thread_running = 0;
        pthread_join(demux_thread, nullptr);
    }

    prospero_embedded_subtitle_close();

    if (video_thread_running) {
        video_thread_running = 0;
        pthread_join(video_thread, nullptr);
    }

    if (audio_decode_thread_running) {
        audio_decode_thread_running = 0;
        pthread_join(audio_decode_thread, nullptr);
    }

    if (audio_thread_running) {
        audio_thread_running = 0;
        pthread_join(audio_thread, nullptr);
    }

    packet_queue_clear(&video_packet_queue);
    packet_queue_clear(&audio_packet_queue);

    audio_queue_count = 0;
    audio_queue_read = 0;
    audio_queue_write = 0;
    audio_accum_pos = 0;

    if (video_pending_pkt) {
        av_packet_free(&video_pending_pkt);
        video_pending_pkt = nullptr;
    }

    /*
     * The decode loop in evo_playback.c holds its half-consumed AU here. Left
     * behind, it is the first thing fed to the *next* file's decoder - a
     * mid-GOP packet from the previous stream.
     */
    if (video_video_pending_pkt) {
        av_packet_free(&video_video_pending_pkt);
        video_video_pending_pkt = nullptr;
    }

    /* Scaler is sized to the closed file's geometry; the next file may differ. */
    if (play_sws) {
        sws_freeContext(play_sws);
        play_sws = nullptr;
    }

    if (audio_handle >= 1) {
        sceAudioOutClose(audio_handle);
        audio_handle = -1;
    }

    prospero_audio_resampler_destroy();

    if (audio_ctx) {
        avcodec_free_context(&audio_ctx);
        audio_ctx = nullptr;
    }

    if (g_vdec) {
        evo_vdec_close(g_vdec);
        g_vdec = nullptr;
    }

    if (play_fmt) {
        avformat_close_input(&play_fmt);
        play_fmt = nullptr;
    }

    video_stream_index = -1;
    audio_stream_index = -1;
    m_durationSeconds = 0.0;
    media_duration_sec = 0.0;
    m_resumeBaseOffset = 0.0;
    resume_base_offset_seconds = 0.0;
    m_musicMode = false;
    m_currentFilePath.clear();

    video_decode_ready = 0;
    video_decode_done = 0;
    video_frame_loaded = 0;

    /*
     * Reset the media clocks. These are session globals, not per-file state:
     * carried over, the next file's frames are compared against the previous
     * file's audio clock, so `audio_clock_seconds > video_rel + 0.50` holds
     * forever in audio_output_thread() - the audio queue fills, the decode
     * thread blocks on it, the demuxer blocks on the decode thread, and the
     * second file never plays at all.
     */
    audio_samples_played = 0;
    audio_samples_decoded = 0;
    audio_clock_seconds = 0.0;
    audio_pts_seconds = 0.0;
    video_clock_seconds = 0.0;
    first_audio_pts_seconds = -1.0;
    first_video_pts_seconds = -1.0;

    /* Don't let the next file's scrub previews reuse this file's demuxer. */
    prospero_thumbnail_close_context();

    pp_playback_on_file_close(&g_pp_pb);
    pp_playback_log_stats(&g_pp_pb);
}

bool PlaybackController::startPlayback(const std::string& filePath, double resumeOffset) {
    if (filePath.empty()) {
        return false;
    }

    stopPlayback();

    m_playbackFsm.postEvent(PlaybackEvent::Open);
    m_currentFilePath = filePath;
    std::snprintf(current_media_path, sizeof(current_media_path), "%s", filePath.c_str());

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "probesize", "4194304", 0);
    av_dict_set(&opts, "analyzeduration", "4000000", 0);

    if (avformat_open_input(&play_fmt, filePath.c_str(), nullptr, &opts) < 0) {
        av_dict_free(&opts);
        m_playbackFsm.postEvent(PlaybackEvent::Fail);
        toast("OPEN FAIL", "Could not open media");
        return false;
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(play_fmt, nullptr) < 0) {
        avformat_close_input(&play_fmt);
        play_fmt = nullptr;
        m_playbackFsm.postEvent(PlaybackEvent::Fail);
        toast("STREAM FAIL", "Could not find streams");
        return false;
    }

    if (play_fmt->duration > 0) {
        m_durationSeconds = static_cast<double>(play_fmt->duration) / static_cast<double>(AV_TIME_BASE);
        media_duration_sec = m_durationSeconds;
    }

    // Identify primary video and audio streams
    video_stream_index = -1;
    audio_stream_index = -1;

    for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
        AVStream* st = play_fmt->streams[i];
        if (!st || !st->codecpar) continue;

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_index < 0) {
            video_stream_index = static_cast<int>(i);
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_index < 0) {
            audio_stream_index = static_cast<int>(i);
        }
    }

    /*
     * A track switch asks for a specific stream. Honour it only if it really is
     * a decodable audio stream in this file, so a stale index from a previous
     * file can never leave the session with no audio at all.
     */
    if (m_requestedAudioStream >= 0 &&
        m_requestedAudioStream < static_cast<int>(play_fmt->nb_streams)) {
        AVStream* want = play_fmt->streams[m_requestedAudioStream];
        if (want && want->codecpar &&
            want->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            avcodec_find_decoder(want->codecpar->codec_id)) {
            audio_stream_index = m_requestedAudioStream;
        }
    }
    m_requestedAudioStream = -1;

    m_musicMode = (video_stream_index < 0 && audio_stream_index >= 0);

    // Initialize Video Decoder if video present
    if (video_stream_index >= 0) {
        AVStream* vStream = play_fmt->streams[video_stream_index];

        AVRational fr = av_guess_frame_rate(play_fmt, vStream, nullptr);
        if (fr.num && fr.den)
            video_fps = (double)fr.num / (double)fr.den;
        else
            video_fps = 60.0;

        evo_vdec_open_params vp;
        std::memset(&vp, 0, sizeof(vp));
        vp.backend = g_vdec_force_ffmpeg
                         ? EVO_VDEC_BACKEND_FFMPEG
                         : evo_vdec_pref_resolve(EVO_VDEC_PREF_AUTO, vStream->codecpar->codec_id);
        vp.codec_id = vStream->codecpar->codec_id;
        vp.width = vStream->codecpar->width;
        vp.height = vStream->codecpar->height;
        vp.avctx_params = vStream->codecpar;
        vp.thread_count = 4;
        /* Frame threading for HEVC; slice threading for H.264 to prevent frame worker stack exhaustion */
        vp.thread_type = (vStream->codecpar->codec_id == AV_CODEC_ID_HEVC)
                            ? FF_THREAD_FRAME
                            : FF_THREAD_SLICE;
        vp.flag2_fast = (vStream->codecpar->codec_id == AV_CODEC_ID_HEVC);
        vp.skip_loop_filter = EVO_VDEC_KEEP;
        vp.skip_frame = EVO_VDEC_KEEP;
        vp.skip_idct = EVO_VDEC_KEEP;

        evo_vdec_backend chosen = EVO_VDEC_BACKEND_FFMPEG;
        g_vdec = evo_vdec_open(&vp, &chosen);
        if (!g_vdec) {
            evo_boot_log("PlaybackController: failed to open video decoder");
        } else {
            evo_boot_log("PlaybackController: video decoder opened (backend=%s, codec=%d, %dx%d @ %.2f fps)",
                         (chosen == EVO_VDEC_BACKEND_NATIVE) ? "NATIVE (sceVideodec2)" : "FFmpeg",
                         vStream->codecpar->codec_id,
                         vp.width, vp.height, video_fps);
        }
    }

    // Initialize Audio Decoder if audio present
    if (audio_stream_index >= 0) {
        AVStream* aStream = play_fmt->streams[audio_stream_index];
        const AVCodec* aDec = avcodec_find_decoder(aStream->codecpar->codec_id);
        if (aDec) {
            audio_ctx = avcodec_alloc_context3(aDec);
            if (audio_ctx && avcodec_parameters_to_context(audio_ctx, aStream->codecpar) >= 0) {
                if (avcodec_open2(audio_ctx, aDec, nullptr) >= 0) {
                    sceAudioOutInit();

                    /*
                     * The grain passed to sceAudioOutOpen is how many frames
                     * sceAudioOutOutput() consumes per call, and the output
                     * thread hands it whole AUDIO_BLOCK_SAMPLES blocks. Open
                     * the port with any other grain and the tail of every
                     * block is silently discarded while audio_samples_played
                     * still advances by a full block - the audio clock then
                     * runs fast by exactly that ratio and drags the
                     * audio-mastered video pacing along with it.
                     */
                    int chCount = audio_ctx->ch_layout.nb_channels;
                    int handle = -1;

                    if (chCount > 2) {
                        handle = sceAudioOutOpen(0xFF, 0, 0,
                                                 AUDIO_BLOCK_SAMPLES, 48000,
                                                 2 /* S16_8CH */);
                        if (handle >= 1) {
                            evo_audio_channels = 8;
                        } else {
                            toast("AUDIO",
                                  "surround port refused, falling back to stereo");
                        }
                    }

                    /* Surround refused (or a genuinely stereo source): stereo. */
                    if (handle < 1) {
                        evo_audio_channels = 2;
                        handle = sceAudioOutOpen(0xFF, 0, 0,
                                                 AUDIO_BLOCK_SAMPLES, 48000,
                                                 1 /* S16_STEREO */);
                    }

                    audio_handle = handle;

                    if (audio_handle < 1) {
                        toast("AUDIO OUTPUT ERROR",
                              "PS5 audio output could not start");
                    }

                    detected_audio_rate = (audio_ctx->sample_rate > 0)
                                              ? audio_ctx->sample_rate
                                              : 48000;

                    audio_queue_read = 0;
                    audio_queue_write = 0;
                    audio_queue_count = 0;
                    audio_accum_pos = 0;

                    prospero_audio_resampler_reset();
                }
            }
        }
    }

    // Subtitle setup
    prospero_subtitle_clear();
    prospero_subtitle_load_for_media(filePath.c_str());
    prospero_embedded_subtitle_open(play_fmt);

    /*
     * Apply the resume position. This has to be a real seek on play_fmt before
     * the demux thread starts: m_resumeBaseOffset is added to the decode clock
     * for every position readout (OSD, progress bar, subtitle timing), so
     * setting it without moving the file makes the whole UI report a time the
     * picture is not at - the OSD showed 5:15 while frame one was still 0:00,
     * and every subtitle cue was out by the same amount. requested_resume_seek_pos
     * used to drive this in start_video_playback(); after the C++ migration it
     * had no reader at all.
     */
    m_resumeBaseOffset = 0.0;
    resume_base_offset_seconds = 0.0;
    requested_resume_seek_pos = 0.0;

    if (resumeOffset > 0.0 && resumeOffset < m_durationSeconds &&
        (video_stream_index >= 0 || audio_stream_index >= 0)) {
        double seekPos = resumeOffset;
        int seekStream = (video_stream_index >= 0) ? video_stream_index
                                                   : audio_stream_index;
        /* Land slightly early so the first decoded frame is at or before the
         * resume point rather than just past it. */
        if (video_stream_index >= 0 && seekPos > 3.0)
            seekPos -= 1.0;

        int64_t seekTs = static_cast<int64_t>(
            seekPos / av_q2d(play_fmt->streams[seekStream]->time_base));

        if (av_seek_frame(play_fmt, seekStream, seekTs, AVSEEK_FLAG_BACKWARD) >= 0) {
            evo_vdec_flush(g_vdec);
            if (audio_ctx) {
                avcodec_flush_buffers(audio_ctx);
                prospero_audio_resampler_reset();
            }
            prospero_embedded_subtitle_reset();
            packet_queue_clear(&video_packet_queue);
            packet_queue_clear(&audio_packet_queue);

            m_resumeBaseOffset = resumeOffset;
            resume_base_offset_seconds = resumeOffset;
        } else {
            evo_boot_log("PlaybackController: resume seek to %.2fs failed; starting at 0",
                         resumeOffset);
        }
    }

    // Load chapters
    if (auto metaService = Application::getInstance().getMediaMetadataService()) {
        metaService->loadChapters(play_fmt);
    }

    // Add to recent files database
    std::string mediaTitle, mediaCat;
    if (auto metaService = Application::getInstance().getMediaMetadataService()) {
        metaService->cleanMediaTitle(filePath.c_str(), mediaTitle, mediaCat);
    }
    recent_add_or_update(filePath.c_str(), mediaTitle.c_str(), m_resumeBaseOffset, m_durationSeconds);
    recent_save();
    player_paused = 0;
    video_decode_ready = (video_stream_index >= 0) ? 1 : 0;
    video_decode_done = 0;
    evo_pb_reset_decode_fatal();
    pp_playback_on_file_open(&g_pp_pb);

    applyViewMode();

    // Start background worker threads
    demux_thread_running = 1;
    video_thread_running = (video_stream_index >= 0) ? 1 : 0;
    audio_decode_thread_running = (audio_ctx != nullptr) ? 1 : 0;
    audio_thread_running = (audio_handle >= 1) ? 1 : 0;

    pthread_create(&demux_thread, nullptr, demux_thread_func, nullptr);

    if (video_thread_running) {
        pthread_create(&video_thread, nullptr, video_decode_thread_func, nullptr);
    }
    if (audio_decode_thread_running) {
        pthread_create(&audio_decode_thread, nullptr, audio_decode_thread_func, nullptr);
    }
    if (audio_thread_running) {
        pthread_create(&audio_thread, nullptr, audio_output_thread, nullptr);
    }

    m_playbackFsm.postEvent(PlaybackEvent::Play);
    return true;
}

double PlaybackController::clampScrubTarget(double target) const {
    if (target < 0.0) return 0.0;
    if (m_durationSeconds > 1.0 && target > m_durationSeconds) {
        return m_durationSeconds;
    }
    return target;
}

void PlaybackController::beginScrub() {
    if (!isActive()) return;

    m_playbackFsm.postEvent(PlaybackEvent::StartScrub);
    m_scrubTargetSeconds = getPositionSeconds();
    pp_playback_pause(&g_pp_pb);
    resetScrubHold();
}

void PlaybackController::moveScrub(double deltaSeconds) {
    if (!isScrubbing()) {
        beginScrub();
    }
    m_scrubTargetSeconds = clampScrubTarget(m_scrubTargetSeconds + deltaSeconds);
    m_scrubAutoCommitDeadlineMs = GetCurrentTimeMs() + 600ULL; // auto-commit after 600ms idle
}

bool PlaybackController::confirmScrub() {
    if (!isScrubbing()) return false;

    double target = m_scrubTargetSeconds;
    m_playbackFsm.postEvent(PlaybackEvent::ConfirmScrub);
    resetScrubHold();

    seekTo(target);

    m_playbackFsm.postEvent(PlaybackEvent::Play);
    return true;
}

void PlaybackController::cancelScrub() {
    if (!isScrubbing()) return;

    m_playbackFsm.postEvent(PlaybackEvent::CancelScrub);
    resetScrubHold();
    pp_playback_resume(&g_pp_pb);
}

void PlaybackController::resetScrubHold() {
    m_scrubHoldDirection = 0;
    m_scrubHoldStartMs = 0;
    m_scrubHoldLastStepMs = 0;
    m_scrubAutoCommitDeadlineMs = 0;
}

void PlaybackController::updateScrubHold(uint32_t heldButtons) {
    if (!isScrubbing()) return;

    int dir = 0;
    if (heldButtons & PadButtons::Right) dir = 1;
    else if (heldButtons & PadButtons::Left) dir = -1;

    if (dir == 0) {
        m_scrubHoldDirection = 0;
        m_scrubHoldStartMs = 0;
        m_scrubHoldLastStepMs = 0;
        return;
    }

    uint64_t now = GetCurrentTimeMs();
    if (m_scrubHoldDirection != dir) {
        m_scrubHoldDirection = dir;
        m_scrubHoldStartMs = now;
        m_scrubHoldLastStepMs = now;
        return;
    }

    // Accelerate scrub hold rate
    uint64_t holdDuration = now - m_scrubHoldStartMs;
    uint64_t stepInterval = 100ULL; // step every 100ms
    if (now - m_scrubHoldLastStepMs >= stepInterval) {
        double speed = 1.0;
        if (holdDuration > 3000) speed = 30.0;
        else if (holdDuration > 1500) speed = 10.0;
        else if (holdDuration > 500) speed = 3.0;

        moveScrub(static_cast<double>(dir) * speed);
        m_scrubHoldLastStepMs = now;
    }
}

void PlaybackController::tickScrubAutoCommit() {
    if (isScrubbing() && m_scrubAutoCommitDeadlineMs > 0) {
        if (GetCurrentTimeMs() >= m_scrubAutoCommitDeadlineMs) {
            confirmScrub();
        }
    }
}

void PlaybackController::seekTo(double targetSeconds) {
    if (!isActive() || targetSeconds < 0.0) return;

    m_resumeBaseOffset = targetSeconds;
    resume_base_offset_seconds = targetSeconds;

    int64_t targetUs = static_cast<int64_t>(targetSeconds * 1000000.0);
    prospero_request_inplace_seek(targetSeconds, 0);
    pp_playback_notify_seek_begin(&g_pp_pb, targetUs);
}

void PlaybackController::jumpChapter(int direction) {
    auto metaService = Application::getInstance().getMediaMetadataService();
    if (!metaService || metaService->getChapterCount() == 0) {
        // Default relative jump by 30 seconds
        double target = getPositionSeconds() + (direction > 0 ? 30.0 : -30.0);
        seekTo(clampScrubTarget(target));
        return;
    }

    int currentChapter = metaService->getChapterIndexAtPosition(getPositionSeconds());
    int targetChapter = currentChapter + direction;

    if (targetChapter >= 0 && targetChapter < static_cast<int>(metaService->getChapterCount())) {
        const ChapterInfo* ch = metaService->getChapter(targetChapter);
        if (ch) {
            seekTo(ch->startTimeSeconds);
            toast("CHAPTER", ch->title.c_str());
        }
    }
}

void PlaybackController::cycleViewMode() {
    int nextMode = (static_cast<int>(m_viewMode) + 1) % 3;
    setViewMode(static_cast<ViewMode>(nextMode));
}

void PlaybackController::setViewMode(ViewMode mode) {
    m_viewMode = mode;
    applyViewMode();
}

void PlaybackController::applyViewMode() {
    pp_aspect_mode aspect = PP_ASPECT_FIT;
    if (m_viewMode == ViewMode::Fill) aspect = PP_ASPECT_FILL;
    else if (m_viewMode == ViewMode::Stretch) aspect = PP_ASPECT_STRETCH;

    g_pp_pb.aspect = aspect;
    g_pp_pb.stats.aspect = static_cast<int>(aspect);
    pp_playback_set_output(&g_pp_pb, DisplayWidth, DisplayHeight, aspect);
}

void PlaybackController::saveResumePosition() {
    if (m_currentFilePath.empty()) return;

    double pos = getPositionSeconds();
    if (pos < 5.0 || (m_durationSeconds > 0.0 && pos >= m_durationSeconds - 10.0)) {
        pos = 0.0; // Clear near start or finish
    }

    const char* resumeFile = evo_data_path("ps5_media_resume.txt");
    FILE* fp = std::fopen(resumeFile, "w");
    if (fp) {
        std::fprintf(fp, "%s\n%.2f\n", m_currentFilePath.c_str(), pos);
        std::fclose(fp);
    }

    /*
     * The resume file above is only read by the browser. Home, Recent and
     * Favorites all start playback from recent_files[i].last_pos instead, and
     * the only writer of that field is startPlayback(), which stores the offset
     * playback *began* at. Without this second half the stored position never
     * advances past the point a file was last opened from, so every launch
     * replays from the same frozen timestamp. (Pre-refactor this was
     * recent_update_current_position(); it has no caller any more.)
     */
    std::string title, category;
    if (auto metaService = Application::getInstance().getMediaMetadataService()) {
        metaService->cleanMediaTitle(m_currentFilePath, title, category);
    }
    recent_add_or_update(m_currentFilePath.c_str(), title.c_str(),
                         pos, m_durationSeconds);
    recent_save();
}

double PlaybackController::loadResumePosition(const std::string& filePath) const {
    if (filePath.empty()) return 0.0;

    const char* resumeFile = evo_data_path("ps5_media_resume.txt");
    FILE* fp = std::fopen(resumeFile, "r");
    if (!fp) return 0.0;

    char pathBuf[512] = {0};
    double pos = 0.0;
    if (std::fgets(pathBuf, sizeof(pathBuf), fp)) {
        size_t len = std::strlen(pathBuf);
        while (len > 0 && (pathBuf[len - 1] == '\n' || pathBuf[len - 1] == '\r')) {
            pathBuf[len - 1] = '\0';
            len--;
        }
        if (std::fscanf(fp, "%lf", &pos) == 1 && std::strcmp(pathBuf, filePath.c_str()) == 0) {
            std::fclose(fp);
            return pos;
        }
    }

    std::fclose(fp);
    return 0.0;
}

bool PlaybackController::playNextVideo() {
    // Attempt auto-advancing to next video entry in current directory
    auto browser = Application::getInstance().getFileSystemBrowser();
    if (!browser) return false;

    std::string currentFile = m_currentFilePath;
    size_t lastSlash = currentFile.find_last_of('/');
    std::string filename = (lastSlash != std::string::npos) ? currentFile.substr(lastSlash + 1) : currentFile;

    const auto& entries = browser->getEntries();
    int currentIndex = -1;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == filename) {
            currentIndex = static_cast<int>(i);
            break;
        }
    }

    if (currentIndex >= 0) {
        for (size_t i = currentIndex + 1; i < entries.size(); ++i) {
            if (entries[i].category == FileCategory::Video) {
                std::string nextPath = browser->getFullPath(i);
                return startPlayback(nextPath, 0.0);
            }
        }
    }

    return false;
}

std::vector<PlaybackController::AudioTrackInfo>
PlaybackController::getAudioTracks() const {
    std::vector<AudioTrackInfo> out;
    if (!play_fmt) return out;

    for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
        AVStream* st = play_fmt->streams[i];
        if (!st || !st->codecpar ||
            st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        /* Listing a track EVO cannot decode is an invitation to silence. */
        if (!avcodec_find_decoder(st->codecpar->codec_id)) {
            continue;
        }

        AudioTrackInfo t;
        t.streamIndex = static_cast<int>(i);
        t.channels    = st->codecpar->ch_layout.nb_channels;
        t.sampleRate  = st->codecpar->sample_rate;
        const char* cn = avcodec_get_name(st->codecpar->codec_id);
        t.codecName = cn ? cn : "unknown";

        const AVDictionaryEntry* lang =
            av_dict_get(st->metadata, "language", nullptr, 0);
        t.language = (lang && lang->value && lang->value[0]) ? lang->value : "UND";

        const AVDictionaryEntry* title =
            av_dict_get(st->metadata, "title", nullptr, 0);
        if (title && title->value && title->value[0]) {
            t.title = title->value;
        }

        out.push_back(std::move(t));
    }
    return out;
}

int PlaybackController::getActiveAudioStream() const {
    return audio_stream_index;
}

bool PlaybackController::switchAudioTrack(int streamIndex) {
    if (m_currentFilePath.empty() || streamIndex < 0) return false;
    if (streamIndex == audio_stream_index) return true;

    /*
     * Resume where the picture is, not where the file started: getPositionSeconds()
     * already folds in the resume base, and startPlayback() seeks to whatever it
     * is handed. Copy the path first - stopPlayback() clears m_currentFilePath.
     */
    std::string path = m_currentFilePath;
    double resumeAt = getPositionSeconds();
    bool wasPaused = isPaused();

    m_requestedAudioStream = streamIndex;
    if (!startPlayback(path, resumeAt)) {
        m_requestedAudioStream = -1;
        return false;
    }

    if (wasPaused) setPaused(true);
    return true;
}

bool PlaybackController::replay() {
    if (m_currentFilePath.empty()) return false;
    return startPlayback(m_currentFilePath, 0.0);
}

} // namespace evo
