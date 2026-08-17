#pragma once

/*
 * The handful of libpeer settings the application and the RTP layer must agree
 * on, defined in exactly one place.
 *
 * libpeer keeps its own defaults in managed_components/sepfy__libpeer/src/config.h,
 * every one of them behind `#ifndef`. main/CMakeLists.txt force-includes this
 * header into the libpeer component, so these definitions win and libpeer's
 * fallbacks are skipped. app_config.h re-exports them under application names,
 * which is why there is no second copy of any of these numbers to drift.
 *
 * Editing libpeer's config.h instead would work but would have to live in
 * patches/, and managed_components/ is regenerated from the registry.
 */

/*
 * RTP timestamp step for video is 90000 / this, so it must be the rate frames
 * are really produced — not the sensor's rate, and not an aspiration. Too high
 * and timestamps advance slower than wall-clock, which shows up as a growing
 * jitter buffer and then stalling playback.
 */
#define CONFIG_CODEC_H264_FPS 12

/*
 * Milliseconds of audio per RTP packet. The G.711-A path samples at 8 kHz, so
 * libpeer steps the timestamp by CONFIG_AUDIO_DURATION * 8000 / 1000 per packet
 * and the application must hand it exactly that many samples each time — see
 * AUDIO_READ_BYTES in app_config.h, which is derived from this.
 */
#define CONFIG_AUDIO_DURATION 20

/**
 * CONFIG_MTU = 1300
 */
#define CONFIG_VIDEO_BUFFER_SIZE (1300 * 256)

/**
 * 
 */
#define CONFIG_AUDIO_BUFFER_SIZE (1300 * 256)

/**
 * SCTP_MTU = 1200
 */
#define CONFIG_DATA_BUFFER_SIZE (1200)
