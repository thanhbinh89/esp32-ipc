#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A detection, in the detector's own input coordinate space (DET_WIDTH x DET_HEIGHT). */
typedef struct {
    int x1;
    int y1;
    int x2;
    int y2;
    float score;
} det_box_t;

/*
 * A detector consumes downscaled RGB565 frames and produces boxes.
 *
 * It deliberately knows nothing about the pixel format the encoder wants, or
 * about drawing: the video task reads boxes back and renders them itself, so
 * swapping the model never touches pixel-layout code.
 *
 * Frames are handed over by borrowing one of the detector's own buffers rather
 * than by passing a pointer to copy from, so nothing is copied and how those
 * buffers are pooled stays an implementation detail:
 *
 *     det_input_t in;
 *     if (det->acquire_input(&in)) {
 *         if (fill(in.buffer) == ESP_OK) det->submit(&in);
 *         else                           det->discard(&in);
 *     }
 */

/* Size in bytes of the buffer acquire_input() lends out. */
#define DET_INPUT_BYTES (DET_WIDTH * DET_HEIGHT * 2)

typedef struct {
    void *token;   /*!< opaque to the caller; hand back to submit/discard */
    void *buffer;  /*!< DET_INPUT_BYTES of RGB565 to fill */
} det_input_t;

typedef struct {
    /* Bring up the model and its task. Returns ESP_OK when boxes will follow. */
    esp_err_t (*start)(void);

    /*
     * Borrow a free DET_WIDTH x DET_HEIGHT RGB565 buffer to fill. False when the
     * detector is still busy — the normal way inference running slower than
     * capture drops frames without holding video up.
     */
    bool (*acquire_input)(det_input_t *in);

    /* Hand a filled buffer over for inference. */
    void (*submit)(const det_input_t *in);

    /* Give a buffer back unused, e.g. when filling it failed. */
    void (*discard)(const det_input_t *in);

    /* Copy the most recent boxes into out. Returns how many were copied. */
    int (*get_boxes)(det_box_t *out, int max);
} detector_iface_t;

/* The pedestrian detector (esp-dl Pico s8 v1). */
extern const detector_iface_t detector_pedestrian;

#ifdef __cplusplus
}
#endif
