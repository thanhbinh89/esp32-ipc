#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Camera capture + H.264 encode. Opens the MIPI-CSI capture and hardware encoder
 * devices, starts the detector and its own capture/encode sub-tasks, then stays
 * on as a 1 Hz statistics logger.
 *
 * Each captured YUV420 frame is shared with the detector (PPA-downscaled to
 * RGB565), has the latest detection boxes burned in, is H.264-encoded and pushed
 * to the peer. Runs at whatever rate the sensor delivers; there is no pacing.
 *
 * Must be started after networking has an IP.
 */
void task_video(void *arg);

#ifdef __cplusplus
}
#endif
