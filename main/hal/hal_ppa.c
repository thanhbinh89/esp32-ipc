#include <string.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_log.h"

#include "hal_ppa.h"

static const char *TAG = "hal_ppa";

esp_err_t hal_ppa_yuv420_to_rgb565(ppa_client_handle_t ppa,
                                   const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                   void *dst, size_t dst_size, uint32_t dst_w, uint32_t dst_h) {
    ppa_srm_oper_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.in.buffer = src;
    cfg.in.pic_w = src_w;
    cfg.in.pic_h = src_h;
    cfg.in.block_w = src_w;
    cfg.in.block_h = src_h;
    cfg.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
    cfg.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    cfg.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;

    cfg.out.buffer = dst;
    cfg.out.buffer_size = dst_size;
    cfg.out.pic_w = dst_w;
    cfg.out.pic_h = dst_h;
    cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    cfg.scale_x = (float)dst_w / src_w;
    cfg.scale_y = (float)dst_h / src_h;
    cfg.byte_swap = false;
    cfg.mode = PPA_TRANS_MODE_BLOCKING;

    ESP_RETURN_ON_ERROR(ppa_do_scale_rotate_mirror(ppa, &cfg), TAG, "SRM failed");

    /* The PPA wrote this buffer by DMA; invalidate so the CPU reads it fresh. */
    ESP_RETURN_ON_ERROR(esp_cache_msync(dst, dst_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C),
                        TAG, "cache invalidate failed");
    return ESP_OK;
}
