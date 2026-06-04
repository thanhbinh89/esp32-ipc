#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "peer_connection.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared WebRTC state, owned by webrtc_task and read by video_task/audio_task.
 * video_task pushes H.264 via peer_connection_send_video(); audio_task pushes
 * G.711-A via peer_connection_send_audio(). Both gate on eState and serialise
 * against peer_connection_loop() using g_pc_lock. */
extern PeerConnection *g_pc;
extern PeerConnectionState eState;
extern SemaphoreHandle_t g_pc_lock;

bool webrtc_take_keyframe_request(void);

/* Creates the PeerConnection, connects signaling, then polls the PC + signaling
 * loops forever. Must be started after networking has an IP. */
void webrtc_task(void *arg);

#ifdef __cplusplus
}
#endif
