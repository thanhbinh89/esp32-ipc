#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/ppa.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The PPA holds its scale factor as 8.4 fixed point and computes
 *     out = in * (int_part + frag/16)
 * truncating anything that is not a multiple of 1/16. Asking for a ratio it
 * cannot express does not fail — it silently produces a smaller image in the
 * corner of the destination buffer, leaving the rest stale. Any resolution pair
 * used with this HAL must therefore be exactly representable.
 */
#define HAL_PPA_SCALE_IS_EXACT(src, dst) (((dst) * 16) % (src) == 0)

/* Downscale a packed YUV420 frame into an RGB565 buffer. Blocking.
 * dst_w/dst_h must satisfy HAL_PPA_SCALE_IS_EXACT against src_w/src_h. */
esp_err_t hal_ppa_yuv420_to_rgb565(ppa_client_handle_t ppa,
                                   const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                   void *dst, size_t dst_size, uint32_t dst_w, uint32_t dst_h);

#ifdef __cplusplus
}
#endif
