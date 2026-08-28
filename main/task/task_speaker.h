#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Downlink half of the two-way audio path: G.711 A-law arriving from the peer,
 * played out of the ES8311 DAC.
 *
 * The uplink (microphone -> peer) is task_audio; this is deliberately a second
 * task rather than a write bolted onto that loop. The two are clocked by
 * different things -- capture by the codec's own 8 kHz, playback by whenever
 * the network delivers -- and esp_codec_dev_read() has to come back round
 * within CONFIG_AUDIO_DURATION ms or the I2S RX ring overruns. Putting a write
 * that can stall on the TX DMA in front of that deadline would trade a
 * microphone dropout for a speaker dropout.
 *
 * The two are safe to run concurrently: esp_codec_dev takes no lock and its I2S
 * data interface reaches separate RX and TX channel handles.
 */

/*
 * Hand one G.711 A-law payload from an incoming RTP packet to the speaker.
 *
 * Called from the peer task, inside peer_connection_loop(). Never blocks:
 * returns false and drops the payload if the jitter buffer is full, or if the
 * speaker task is not running.
 */
bool speaker_submit_g711a(const uint8_t *payload, size_t len);

/* Plays the power-on tone, then drains the jitter buffer forever. */
void task_speaker(void *arg);

#ifdef __cplusplus
}
#endif
