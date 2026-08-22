#pragma once

/*
 * Every tunable in the application lives here, so a change of resolution, bitrate
 * or scheduling can be reasoned about in one place instead of being hunted across
 * task headers.
 */

#include "libpeer_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- video ---- */

#define CAM_WIDTH           1920
#define CAM_HEIGHT          1080

/*
 * Capture buffers held by the V4L2 driver, and the depth of the queue that hands
 * their descriptors from the capture task to the encode task.
 *
 * The driver must always keep at least one buffer to DMA into. The application
 * can hold, at once: one in the capture task between DQBUF and xQueueSend,
 * VIDEO_QUEUE_LEN in the queue, and one in the encode task. So the queue has to
 * be CAP_BUF_COUNT - 2, not CAP_BUF_COUNT - 1 -- the latter lets the application
 * own every buffer at the same time, which is the steady state as soon as encode
 * is slower than the sensor.
 *
 * Getting this wrong does not fail loudly. With
 * CONFIG_ESP_VIDEO_DISABLE_MIPI_CSI_DRIVER_BACKUP_BUFFER=y,
 * csi_video_on_get_new_trans() answers an empty queued list by pointing the DMA
 * at the buffer it just filled, and csi_video_on_trans_finished() then skips the
 * done-list insert because the two pointers match. The frame is dropped inside
 * the driver with no counter and no log -- cap_drop stays 0 while cap falls.
 *
 * Cost is CAM_WIDTH * CAM_HEIGHT * 1.5 = 3,110,400 B of PSRAM per buffer.
 */
#define CAP_BUF_COUNT       3
#define VIDEO_QUEUE_LEN     (CAP_BUF_COUNT - 2)

/*
 * Bitstream buffers on the encoder's capture queue. One is enough, and a second
 * one buys nothing measurable.
 *
 * This driver is synchronous. VIDIOC_DQBUF is what triggers the encode at all
 * (esp_video.c notifies ESP_VIDEO_M2M_TRIGGER on the way in, and
 * esp_video_h264_device.c runs h264_video_m2m_process inline), and
 * esp_video_m2m_process() needs one queued element from each side only at that
 * instant. By then hal_h264_release() has already returned the previous buffer,
 * because the encode loop is straight-line code: encode, send, release, repeat.
 * So there is never a moment when the driver wants a buffer and the application
 * is holding the only one.
 *
 * It was 2 on the assumption that the encoder could start the next frame while
 * the previous one was being sent. It cannot, and the buffers are not cheap:
 * esp_video sizes an H.264 capture buffer as width * height * bpp / 8 with bpp =
 * 8 and no sizeimage set (esp_video.c:2164), i.e. 2,073,600 B each -- to hold
 * frames that actually measure 15-100 KB. Dropping the second one returned ~2 MB
 * of PSRAM.
 *
 * What 2 did cover was a leaked buffer, if hal_h264_release()'s QBUF ever failed;
 * at 1 the next encode fails with "no valid buffer" instead. Both paths log.
 */
#define ENC_OUT_BUF_COUNT   1

/*
 * How long VIDIOC_DQBUF may wait before the HAL treats the stage as stalled.
 *
 * esp_video defaults this to portMAX_DELAY, which turns any hardware stall into a
 * silent permanent stop: the capture task blocks holding nothing, the encode task
 * blocks holding a capture buffer, and with CAP_BUF_COUNT buffers the driver then
 * has none left to fill. Nothing crashes and nothing is logged -- the stats line
 * just prints zeros forever.
 *
 * A frame legitimately takes up to a few sensor periods when the encoder is the
 * slower stage, so this only has to be well clear of that; it is a stall detector,
 * not a deadline.
 */
#define VIDEO_DQBUF_TIMEOUT_MS 1000

/*
 * H264_I_PERIOD is not a free choice, for two reasons that both come from the
 * esp_video driver rather than from H.264:
 *
 * 1. The driver passes this one value as BOTH the GOP length and the assumed
 *    frame rate (`.gop = h264_video->gop, .fps = h264_video->gop`), so it also
 *    sets how the rate controller divides H264_BITRATE across a second. Setting
 *    it far above the rate we really deliver starves every frame of bits; far
 *    below, and frames overshoot the link.
 * 2. It is the *only* way to get an IDR. The driver implements no
 *    FORCE_KEY_FRAME control, and it reads `gop` only when it builds the encoder
 *    at STREAMON, so it cannot be nudged at runtime either. A joining browser
 *    therefore waits up to one period for its first decodable frame.
 *
 * It is also what libpeer stamps RTP timestamps with (90000 / CONFIG_CODEC_H264_FPS
 * per frame in rtp.c), hence the shared definition. So the rate is not something
 * to measure after the fact and track — it is a contract the capture side has to
 * meet, which is what VIDEO_FRAME_INTERVAL_US enforces.
 *
 * QP bigger -> compress higher
 */
#define H264_TARGET_FPS     CONFIG_CODEC_H264_FPS
#define H264_I_PERIOD       H264_TARGET_FPS
#define H264_BITRATE        2000000
/* Back to the values chosen for picture quality. They were raised to 35/40 while
 * the capture side was un-paced and the encoder was being fed ~24 fps against a
 * 12 fps budget, i.e. to hold the bitrate down by force; VIDEO_FRAME_INTERVAL_US
 * fixes that at the source. MIN_QP is the real quality knob here -- H264_BITRATE
 * barely moves the picture by comparison. Raise them again only if the measured
 * bitrate overshoots with pacing in place. */
#define H264_MIN_QP         22
#define H264_MAX_QP         51

/*
 * Size of one bitstream buffer, i.e. the largest encoded frame the driver may
 * hand back.
 *
 * esp_video only computes a size when V4L2 sizeimage is left at 0, and its guess
 * is width * height * bpp / 8 with bpp = 8 for H.264 (esp_video.c:2164) --
 * 2,073,600 B for 1080p, to hold frames that measure 15-100 KB and an IDR that
 * measures at most ~375 KB. Setting sizeimage explicitly returns the difference
 * to PSRAM.
 *
 * The margin over 375 KB is deliberate and should not be trimmed to fit: an IDR
 * that overruns comes back with V4L2_BUF_FLAG_ERROR and is dropped, and dropping
 * an IDR costs the peer a whole GOP -- H264_I_PERIOD frames, about a second at
 * this frame rate -- of undecodable video. H264_MIN_QP also bounds this from the
 * other side: lowering it lets the rate controller spend more bits per frame, so
 * a smaller QP floor means larger peaks. 375 KB was measured at QP 35; this is
 * now 22.
 *
 * hal_h264_encode() already logs a bad frame, so an undersized buffer shows up as
 * "encoder reported a bad frame" rather than as silent corruption.
 */
#define H264_MAX_FRAME_BYTES (640 * 1024)

/*
 * Sensor period the capture task resamples to, so the pipeline delivers exactly
 * H264_TARGET_FPS.
 *
 * Nothing else paces it. The sensor has one 1080p mode and it is 30 fps
 * (CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS), and the capture rate that
 * reaches the encoder is otherwise set by how fast a buffer comes back round —
 * which moves with load, not with any target. It sat near 15 fps only because the
 * encode loop happened to take ~66 ms; when the detector falls behind and
 * feed_detector() starts skipping, the PPA stage disappears, the loop drops to
 * ~36 ms and the rate doubles to ~28 fps on its own.
 *
 * Both consumers of H264_TARGET_FPS are then wrong by that same ratio:
 *   - the encoder's rate controller budgets H264_BITRATE across H264_TARGET_FPS
 *     frames, so twice the frames is twice the bitrate (measured: 2.4-6.3 Mbps
 *     against a 1.5 Mbps target),
 *   - libpeer advances the RTP timestamp by one fixed step per frame, so the
 *     browser's timeline runs at half real speed. Its jitter buffer never drains
 *     and it asks for a keyframe several times a second, forever.
 */
#define VIDEO_FRAME_INTERVAL_US (1000000 / H264_TARGET_FPS)

/* ------------------------------------------------------------- detector ---- */

/*
 * RGB565 resolution the camera task downscales each frame to before detection.
 *
 * The PPA stores its scale factor as 8.4 fixed point, so only multiples of 1/16
 * are reachable: it computes out = in * (int_part + frag/16) and silently
 * truncates anything else. 1/3 is NOT representable — it becomes 5/16 = 0.3125,
 * which turns a requested 640x360 into an actual 600x337 image sitting in the
 * corner of the buffer, with detections then mapped back by the wrong factor.
 * 480x270 is exactly 4/16 of 1920x1080: 16:9 preserved, no border, and box
 * coordinates scale back by an exact x4.
 */
#define DET_WIDTH           480
#define DET_HEIGHT          270
#define DET_MAX_BOX         5

/* Feed buffers the camera task fills and the detector drains. Two lets the camera
 * be at most one frame ahead; when both are held, detection is skipped for that
 * frame and video is unaffected. */
#define DET_FEED_ELEMENTS   2

/*
 * Feed the detector only every Nth captured frame.
 *
 * Measured on target, one PPA downscale costs ~85 ms against ~47 ms for
 * everything else in the encode loop (OSD + H.264 + send), so feeding on every
 * frame caps the stream at ~7.5 fps. Most of that 85 ms is not the scaling: the
 * PPA driver unconditionally cache-write-backs its whole input window before
 * starting the DMA — 1920*1080*12/8 = 3,110,400 bytes per call — and that is not
 * avoidable through its public API.
 *
 * Skipping the call therefore buys back both the scaling and the flush. Trading
 * detection rate for frame rate is the right way round here: pedestrians do not
 * need 7 detections a second, and the detector itself only sustains ~10/s.
 *
 * This was briefly 5, to stop the detector starving IDLE1 on a core it shared
 * with the peer loop. It does not share a core with anything: TASK_CORE_PEER is 0
 * and TASK_CORE_DETECT is 1. At 3 the detector is ~44% of core 1 (4 inferences a
 * second at 12 fps, ~110 ms each), which leaves the idle task plenty, and
 * task_detect.cpp yields a tick per inference besides.
 *
 * What 3 does cost is core 0: the PPA stage averages ~28 ms per frame instead of
 * ~17, taking busy from ~56 ms to ~70 ms against the 83 ms frame interval. Still
 * inside budget, but the margin to watch is wait= in the stats line -- if it
 * approaches zero the encode loop has stopped keeping up and this is the first
 * knob to turn back.
 */
#define DET_FEED_EVERY_N    3

/* Inferences averaged per timing log line. At DET_FEED_EVERY_N=3 and 12 fps that
 * is 4 inferences a second, so a line every ~4 s: enough to compare feed
 * resolutions or preprocessor settings without flooding the log. */
#define DET_TIMING_WINDOW   16

/* OSD marks: colour in YUV (red), stroke thickness and corner-arm length in luma
 * pixels. Detections are drawn as four corner brackets rather than a closed
 * rectangle, because a closed rectangle's side edges dirty every row of the box
 * and force a full-height cache write-back to publish a few bytes per row — on a
 * tall box that measured ~20 ms per frame. Corners dirty two thin bands instead. */
#define OSD_Y               76
#define OSD_U               84
#define OSD_V               255
#define OSD_THICKNESS       4
#define OSD_CORNER_LEN      48

/* ---------------------------------------------------------------- audio ---- */

/* One RTP packet worth of microphone PCM: CONFIG_AUDIO_DURATION ms of 8 kHz mono
 * int16. At the default 20 ms that is 160 samples -> 320 bytes in, 160 G.711-A
 * bytes out. Derived rather than hardcoded because libpeer advances the RTP
 * timestamp by exactly this much per packet. */
#define AUDIO_SAMPLE_RATE   8000
#define AUDIO_SAMPLES_PER_PACKET (CONFIG_AUDIO_DURATION * AUDIO_SAMPLE_RATE / 1000)
#define AUDIO_READ_BYTES    (AUDIO_SAMPLES_PER_PACKET * 2)

/* --------------------------------------------------------------- webrtc ---- */

/*
 * How long peer_connection_task() sleeps between iterations, before and after
 * the connection is up.
 *
 * These cannot be one number, because libpeer paces ICE by *iterations* of that
 * loop rather than by time: agent.c retransmits a STUN binding request every
 * AGENT_CONNCHECK_PERIOD (100) passes and abandons a candidate pair after
 * AGENT_CONNCHECK_MAX (1000). Slowing the loop rescales every ICE timeout with
 * it -- measured on target, a uniform 5 ms stretched the retransmit interval
 * from ~200 ms to ~600 ms and turned an 18 s connectivity check into a 54 s
 * failure ("select candidate pair failed local=3 remote=2 pairs=4"). So ICE
 * keeps the cadence it was written against.
 *
 * Once COMPLETED, no iteration counter is running any more and the loop is only
 * draining the media rings, where the deadlines are the 83 ms frame interval and
 * the 20 ms audio packet -- so a pass every 5 ms is still far more often than
 * anything needs.
 *
 * The original reason for backing off was that the loop held a lock across its
 * 1 ms select() and was timing out four video sends in five. That lock is gone
 * (webrtc_send() is lock-free now, see task_webrtc.cpp), so what is left is only
 * the wakeup cost. 1 ms has not been re-measured since; if this ever needs to be
 * tight for RTP pacing, it is safe to try, and send_fail= is no longer the signal
 * to watch because there is nothing left to fail.
 */
#define WEBRTC_PEER_ICE_LOOP_MS 1
#define WEBRTC_PEER_LOOP_MS     5

/* ------------------------------------------------------------ scheduling ---- */

#define TASK_AUDIO_STACK_SIZE   4096
#define TASK_VIDEO_STACK_SIZE   4096
#define TASK_WEBRTC_STACK_SIZE  4096
#define TASK_CAPTURE_STACK_SIZE 4096
#define TASK_ENCODE_STACK_SIZE  4096
#define TASK_PEER_STACK_SIZE    8192
#define TASK_DETECT_STACK_SIZE  8192

/*
 * Priorities, ordered by how hard the deadline is.
 *
 * The peer loop is highest: it drives ICE, the DTLS handshake and the RTP drain,
 * and a late RTP burst shows up at the far end as jitter. It outranks the
 * signaling loop, which only polls MQTT every 10 ms and has no deadline at all.
 *
 * Audio, capture and encode share one level. Audio has the hardest deadline of
 * the three -- esp_codec_dev_read() every 20 ms or the I2S RX ring overruns --
 * so on paper it belongs above video, and it used to be. It sits level for now
 * because no audio dropout has ever been observed and because task_audio.cpp
 * still discards webrtc_send_audio()'s return value, which means a dropout would
 * be silent anyway. Fix the reporting before re-raising the priority, otherwise
 * there is no way to tell whether the change did anything.
 *
 * The detector is below video on purpose: one inference is ~110 ms of solid CPU
 * against a ~83 ms frame interval, so at an equal level it would displace whole
 * frames. Detection is best-effort by construction -- feed_detector() skips when
 * no buffer is free -- so what it loses is detections, never frames.
 */
#define TASK_PRIO_AUDIO         5
#define TASK_PRIO_PEER          6   /* peer loop */
#define TASK_PRIO_VIDEO_CAP     5
#define TASK_PRIO_VIDEO_ENC     5
#define TASK_PRIO_VIDEO_INIT    4   /* lowered to TASK_PRIO_STATS once set up */
#define TASK_PRIO_DETECT        4   /* core 1, below video */
#define TASK_PRIO_WEBRTC        3   /* signaling loop */
#define TASK_PRIO_STATS         1

/*
 * Core 0 runs the whole media path plus the network; core 1 runs only the
 * detector.
 *
 * The peer loop stays on core 0 with video above it in nothing but priority, and
 * that is deliberate. Attaching a browser over Wi-Fi used to add ~70 ms per frame
 * to the encode loop, which looked exactly like the peer task preempting video
 * and cost 15 fps -> 5-7. It was not. The same firmware over Ethernet, with the
 * peer task in this same place, shows no regression at all: enc stays 34-42 ms
 * with a browser attached, busy 50-64 ms, send_fail=0, full frame rate. The cost
 * was esp_hosted -- four SDIO tasks at priority 23, above everything here, and
 * ~128 KB of internal RAM which forced the PeerConnection struct and every RTP
 * buffer into PSRAM to contend with the H.264 encoder.
 *
 * So do not re-tune these against a Wi-Fi measurement. Whatever a Wi-Fi trace
 * says about scheduling is being told by the transport, not by these numbers.
 *
 * Both cores have headroom as configured: core 0 runs ~70 ms busy against the
 * 83 ms frame interval, core 1 about 44 % duty (one ~110 ms inference per
 * DET_FEED_EVERY_N frames at 12 fps). Pinning is kept rather than leaving tasks
 * unpinned because it is what makes the per-stage timings in the 1 Hz stats line
 * comparable between runs -- every diagnosis in this file was reached that way.
 */
#define TASK_CORE_VIDEO         0   /* capture, OSD, encode */
#define TASK_CORE_AUDIO         0
#define TASK_CORE_WEBRTC        0   /* signaling only: prio 3, polls MQTT at 10 ms */
#define TASK_CORE_PEER          0   /* peer loop */
#define TASK_CORE_DETECT        1

#ifdef __cplusplus
}
#endif
