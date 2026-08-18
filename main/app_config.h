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

/* Capture buffers held by the V4L2 driver, and the depth of the queue that hands
 * their descriptors from the capture task to the encode task. One frame is always
 * in the encode task's hands, so the queue only needs to cover the rest. */
#define CAP_BUF_COUNT       2
#define VIDEO_QUEUE_LEN     (CAP_BUF_COUNT - 1)

/* More than one bitstream buffer so the encoder can start the next frame while the
 * previous one is still being packetised and sent. */
#define ENC_OUT_BUF_COUNT   2

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
 * So this must track the frame rate the pipeline actually sustains — measure it
 * from `enc=` in the 1 Hz stats line and keep the two in step. It is also what
 * libpeer stamps RTP timestamps with, hence the shared definition.
 */
#define H264_TARGET_FPS     CONFIG_CODEC_H264_FPS
#define H264_I_PERIOD       H264_TARGET_FPS
#define H264_BITRATE        1000000
#define H264_MIN_QP         22
#define H264_MAX_QP         38

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
 * Keep H264_TARGET_FPS in step with the `enc=` rate this produces.
 */
#define DET_FEED_EVERY_N    3

/* Inferences averaged per timing log line. At ~13 inferences/s this reports about
 * once a second, which is enough to compare feed resolutions or preprocessor
 * settings without flooding the log. */
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

/* Video may drop a frame rather than wait behind peer_connection_loop(); audio
 * cannot, because its I2S ring overruns if the task stops reading. */
#define WEBRTC_VIDEO_LOCK_MS 2
#define WEBRTC_AUDIO_LOCK_MS CONFIG_AUDIO_DURATION

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
 * Audio outranks video: it must call esp_codec_dev_read() every 20 ms or the I2S
 * RX ring overruns and samples are lost, whereas a late video frame only costs a
 * frame. The peer loop drives ICE/DTLS/RTP on a 1 ms cadence and outranks the
 * signaling loop, which only polls MQTT every 10 ms. The detector sits on core 1
 * by itself, so its priority only orders it against IDLE1.
 */
#define TASK_PRIO_DETECT        7   /* core 1 */
#define TASK_PRIO_AUDIO         6
#define TASK_PRIO_PEER          6
#define TASK_PRIO_VIDEO_CAP     5
#define TASK_PRIO_VIDEO_ENC     5
#define TASK_PRIO_VIDEO_INIT    5   /* lowered to TASK_PRIO_STATS once set up */
#define TASK_PRIO_WEBRTC        3
#define TASK_PRIO_STATS         1

#define TASK_CORE_VIDEO         0
#define TASK_CORE_AUDIO         0
#define TASK_CORE_NET           0
#define TASK_CORE_DETECT        1

#ifdef __cplusplus
}
#endif
