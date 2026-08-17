#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "frame_pool.hpp"

static const char *TAG = "frame_pool";

struct frame_pool {
    int elem_num;
    frame_pool_list_t free_list;   /*!< ready to be filled by the producer */
    frame_pool_list_t ready_list;  /*!< filled, waiting for the consumer */
    frame_pool_element_t *element;
    portMUX_TYPE lock;
    SemaphoreHandle_t ready_sem;
};

/* Move an element onto a list. Fails if it is not currently checked out. */
static esp_err_t list_push(struct frame_pool *pool, frame_pool_list_t *list,
                           frame_pool_element_t *element) {
    portENTER_CRITICAL_SAFE(&pool->lock);
    if (!element->checked_out) {
        portEXIT_CRITICAL_SAFE(&pool->lock);
        return ESP_ERR_INVALID_STATE;
    }
    element->checked_out = false;
    SLIST_INSERT_HEAD(list, element, node);
    portEXIT_CRITICAL_SAFE(&pool->lock);
    return ESP_OK;
}

/* Take the head off a list and check it out to the caller. NULL if empty. */
static frame_pool_element_t *list_pop(struct frame_pool *pool, frame_pool_list_t *list) {
    frame_pool_element_t *element = NULL;

    portENTER_CRITICAL_SAFE(&pool->lock);
    if (!SLIST_EMPTY(list)) {
        element = SLIST_FIRST(list);
        SLIST_REMOVE(list, element, frame_pool_element, node);
        element->checked_out = true;
    }
    portEXIT_CRITICAL_SAFE(&pool->lock);

    return element;
}

esp_err_t frame_pool_new(const frame_pool_cfg_t *cfg, pipeline_handle_t *ret_pool) {
    ESP_RETURN_ON_FALSE(cfg && cfg->elem_num > 0 && ret_pool, ESP_ERR_INVALID_ARG, TAG,
                        "elem_num must be greater than 0");

    struct frame_pool *pool = static_cast<struct frame_pool *>(
        heap_caps_calloc(1, sizeof(struct frame_pool), cfg->caps));
    ESP_RETURN_ON_FALSE(pool, ESP_ERR_NO_MEM, TAG, "pool alloc failed");

    pool->element = static_cast<frame_pool_element_t *>(
        heap_caps_calloc(cfg->elem_num, sizeof(frame_pool_element_t), cfg->caps));
    if (!pool->element) {
        free(pool);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "element array alloc failed");
    }

    SLIST_INIT(&pool->free_list);
    SLIST_INIT(&pool->ready_list);
    portMUX_INITIALIZE(&pool->lock);

    pool->ready_sem = xSemaphoreCreateCounting(cfg->elem_num, 0);
    if (!pool->ready_sem) {
        free(pool->element);
        free(pool);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "ready_sem alloc failed");
    }

    for (int i = 0; i < cfg->elem_num; i++) {
        frame_pool_element_t *element = &pool->element[i];

        element->buffer = static_cast<uint16_t *>(
            heap_caps_aligned_calloc(cfg->align_size, 1, cfg->buffer_size, cfg->caps));
        if (!element->buffer) {
            frame_pool_delete(pool);
            ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "buffer %d alloc failed", i);
        }
        element->owns_buffer = true;
        element->index = i;
        element->size = cfg->buffer_size;
        element->checked_out = true; /* so the push below is legal */

        pool->elem_num++;
        list_push(pool, &pool->free_list, element);
    }

    ESP_LOGI(TAG, "pool %p: %d x %u B", pool, pool->elem_num, (unsigned)cfg->buffer_size);
    *ret_pool = (pipeline_handle_t)pool;
    return ESP_OK;
}

esp_err_t frame_pool_delete(pipeline_handle_t handle) {
    struct frame_pool *pool = (struct frame_pool *)handle;
    ESP_RETURN_ON_FALSE(pool, ESP_ERR_INVALID_ARG, TAG, "invalid pool handle");

    for (int i = 0; i < pool->elem_num; i++) {
        if (pool->element[i].owns_buffer) {
            free(pool->element[i].buffer);
        }
    }
    if (pool->ready_sem) {
        vSemaphoreDelete(pool->ready_sem);
    }
    free(pool->element);
    free(pool);

    return ESP_OK;
}

frame_pool_element_t *frame_pool_acquire(pipeline_handle_t handle) {
    struct frame_pool *pool = (struct frame_pool *)handle;
    return pool ? list_pop(pool, &pool->free_list) : NULL;
}

esp_err_t IRAM_ATTR frame_pool_submit(pipeline_handle_t handle, frame_pool_element_t *element) {
    struct frame_pool *pool = (struct frame_pool *)handle;
    if (!pool || !element) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = list_push(pool, &pool->ready_list, element);
    if (ret != ESP_OK) {
        return ret;
    }

    if (xPortInIsrContext()) {
        BaseType_t wakeup = pdFALSE;
        xSemaphoreGiveFromISR(pool->ready_sem, &wakeup);
        if (wakeup == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    } else {
        xSemaphoreGive(pool->ready_sem);
    }
    return ESP_OK;
}

frame_pool_element_t *frame_pool_recv(pipeline_handle_t handle, uint32_t ticks) {
    struct frame_pool *pool = (struct frame_pool *)handle;
    if (!pool) {
        return NULL;
    }
    if (xSemaphoreTake(pool->ready_sem, (TickType_t)ticks) != pdTRUE) {
        return NULL;
    }
    return list_pop(pool, &pool->ready_list);
}

esp_err_t frame_pool_release(pipeline_handle_t handle, int index) {
    struct frame_pool *pool = (struct frame_pool *)handle;
    if (!pool || index < 0 || index >= pool->elem_num) {
        return ESP_ERR_INVALID_ARG;
    }
    return list_push(pool, &pool->free_list, &pool->element[index]);
}
