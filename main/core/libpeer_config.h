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

/*
 * Outgoing ring buffers, in bytes.
 *
 * These hold whole payloads as handed to peer_connection_send_*(), not RTP
 * packets — fragmentation to CONFIG_MTU happens later, when peer_connection_loop()
 * drains them. So MTU does not enter the sizing; the largest single payload does.
 *
 * peer_connection_loop() runs every 1 ms and empties the video and audio rings
 * completely each pass, while the producers add one entry every ~83 ms and 20 ms
 * respectively. Steady-state occupancy is therefore one entry, and the real
 * requirement is "holds the largest entry, with slack for a wrap" — an entry that
 * will not fit contiguously at the tail restarts at offset 0 and wastes the
 * remainder. Each entry also carries a 4-byte length prefix, 4-byte aligned.
 *
 * Note the two #undefs: libpeer's own CMakeLists puts -DCONFIG_AUDIO_BUFFER_SIZE
 * and -DCONFIG_DATA_BUFFER_SIZE on the command line, which bypasses the #ifndef in
 * its config.h. Redefining them without #undef still wins, but warns on every
 * translation unit. CONFIG_VIDEO_BUFFER_SIZE is not passed that way and needs none.
 */

/* 1 frame per ~83 ms; must fit one IDR whole. */
#define CONFIG_VIDEO_BUFFER_SIZE (150 * 1024)

/* 164 B per 20 ms -> 5 entries is ~100 ms of slack against a stalled peer task. */
#undef CONFIG_AUDIO_BUFFER_SIZE
#define CONFIG_AUDIO_BUFFER_SIZE (164 * 5)

/* Outbound datachannel only, and the app never sends on it. Receiving is
 * unaffected: on_dc_message() does not go through this ring. */
#undef CONFIG_DATA_BUFFER_SIZE
#define CONFIG_DATA_BUFFER_SIZE (1024)
