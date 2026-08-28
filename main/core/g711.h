#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ITU-T G.711 A-law, the codec libpeer negotiates as CODEC_PCMA: 8 kHz mono,
 * one byte per sample, no inter-sample state.
 *
 * Both directions live here because they are exact inverses and the two-way
 * audio path needs them in two different tasks -- task_audio encodes the
 * microphone for the uplink, task_speaker decodes the downlink for the DAC.
 */
uint8_t g711a_encode(int16_t sample);
int16_t g711a_decode(uint8_t code);

#ifdef __cplusplus
}
#endif
