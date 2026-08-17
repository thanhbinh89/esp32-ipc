#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/queue.h>
#include "esp_err.h"

/*
 * A fixed set of frame buffers passed between one producer and one consumer
 * without copying.
 *
 * Every element is in exactly one of three places: the free list, the ready list,
 * or checked out by a task. The producer takes one from the free list, fills it
 * and submits it to the ready list; the consumer blocks on the ready list and
 * returns the element to the free list when done. When nothing is free the
 * producer simply skips a frame, which is how a slow consumer drops work instead
 * of stalling the producer.
 */

typedef SLIST_ENTRY(frame_pool_element) frame_pool_node_t;
typedef SLIST_HEAD(frame_pool_list, frame_pool_element) frame_pool_list_t;

struct frame_pool_element {
    bool checked_out;          /*!< true while a task owns it, i.e. it is on neither list */
    bool owns_buffer;          /*!< true when the pool allocated `buffer` itself */
    frame_pool_node_t node;
    uint32_t index;            /*!< position in the element array */
    uint16_t *buffer;
    uint32_t size;             /*!< bytes in `buffer` */
};

typedef struct frame_pool_element frame_pool_element_t;

typedef struct {
    int elem_num;              /*!< number of buffers */
    uint32_t align_size;       /*!< buffer alignment, must cover the cache line for DMA */
    uint32_t caps;             /*!< heap_caps allocation flags, e.g. MALLOC_CAP_SPIRAM */
    uint32_t buffer_size;      /*!< bytes per buffer */
} frame_pool_cfg_t;

typedef void *pipeline_handle_t;

/* Allocate the pool and put every element on the free list. */
esp_err_t frame_pool_new(const frame_pool_cfg_t *cfg, pipeline_handle_t *ret_pool);

/* Free every buffer and the pool itself. No task may be using it. */
esp_err_t frame_pool_delete(pipeline_handle_t pool);

/* Producer: take a free element, or NULL when the consumer still holds them all. */
frame_pool_element_t *frame_pool_acquire(pipeline_handle_t pool);

/* Producer: publish a filled element to the consumer. ISR-safe; the IRAM_ATTR
 * lives on the definition, since repeating it here makes GCC emit conflicting
 * section attributes. */
esp_err_t frame_pool_submit(pipeline_handle_t pool, frame_pool_element_t *element);

/* Consumer: block up to `ticks` for a filled element. NULL on timeout. */
frame_pool_element_t *frame_pool_recv(pipeline_handle_t pool, uint32_t ticks);

/* Return an element to the free list, by index. Used by both sides. */
esp_err_t frame_pool_release(pipeline_handle_t pool, int index);
