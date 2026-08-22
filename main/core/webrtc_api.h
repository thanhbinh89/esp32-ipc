#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The only way for media producers to reach the PeerConnection.
 *
 * The connection handle and its ICE state are private to task_webrtc.cpp.
 * Senders used to reach them directly, which meant every call site had to
 * remember to re-check the state for itself.
 */

/* True when the peer connection is up and media will actually go somewhere. */
bool webrtc_is_streaming(void);

/*
 * Hand one encoded frame to libpeer. Returns the number of bytes accepted,
 * 0 when there is no peer to send to, or negative when the stream's ring buffer
 * has no room for it. Never blocks: the frame is queued here and transmitted by
 * the peer task.
 */
int webrtc_send_video(const uint8_t *buf, size_t len);
int webrtc_send_audio(const uint8_t *buf, size_t len);

/* Read-and-clear the browser's RTCP PLI request for a keyframe. */
bool webrtc_take_keyframe_request(void);

/* Creates the PeerConnection, connects signaling, then polls the PC + signaling
 * loops forever. Must be started after networking has an IP. */
void task_webrtc(void *arg);

#ifdef __cplusplus
}
#endif
