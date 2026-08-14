#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define CAM_WIDTH       1920
#define CAM_HEIGHT      1080
#define CAP_BUF_COUNT   3
#define VIDEO_QUEUE_LEN 2

#define H264_I_PERIOD   5
#define H264_BITRATE    1000000
#define H264_MIN_QP     35
#define H264_MAX_QP     45

/* Camera capture + H.264 encode task. Captures YUV420 from the MIPI-CSI/ISP path,
 * shares each post-ISP frame with the pedestrian detector (PPA-converted to RGB565),
 * overlays the latest detection boxes (OSD) onto the YUV420 frame, then H.264-encodes
 * it. Paces the encoded/processed output to ~15fps. */
void video_task(void *arg);

#ifdef __cplusplus
}
#endif
