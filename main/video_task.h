#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Camera capture + H.264 encode task. Captures YUV420 from the MIPI-CSI/ISP path,
 * shares each post-ISP frame with the pedestrian detector (PPA-converted to RGB565),
 * overlays the latest detection boxes (OSD) onto the YUV420 frame, then H.264-encodes
 * it. Paces the encoded/processed output to ~15fps. */
void video_task(void *arg);

#ifdef __cplusplus
}
#endif
