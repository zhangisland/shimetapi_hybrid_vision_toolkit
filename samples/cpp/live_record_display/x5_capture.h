#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define X5_CAPTURE_FATAL (-200000001) /* adapter validation/allocation failure, not SDK timeout */
typedef struct x5_capture x5_capture;
typedef struct {
    const uint8_t *plane[2];
    size_t size[2];
    int width, height, stride, frame_id;
    void *lease;
} x5_image;
/* Configuration is initialization-only. AE auto is deliberately unavailable for
 * this HVS sensor_mode=2 profile (official sample reports nonconvergence). */
int x5_open(x5_capture **out, int profile, double exposure_us, double again, double dgain);
int x5_get(x5_capture *, int aps, x5_image *out); /* 100 ms SDK wait */
int x5_release(x5_capture *, int aps, x5_image *);
void x5_close(x5_capture *);
#ifdef __cplusplus
}
#endif
