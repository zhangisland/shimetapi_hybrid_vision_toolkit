#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define X5_CAPTURE_FATAL (-200000001) /* adapter validation/allocation failure, not SDK timeout */
typedef struct x5_capture x5_capture;
typedef struct {
    double requested_us, requested_again, requested_dgain;
    float submitted_seconds, submitted_again, submitted_dgain, submitted_ispgain;
    float readback_seconds, readback_again, readback_dgain, readback_ispgain;
    int set_accepted, readback_available;
} x5_exposure_state;
/* Configuration snapshot only: never sensor actual/per-frame measurements. */
int x5_get_exposure_state(const x5_capture *, x5_exposure_state *);
typedef struct {
    const uint8_t *plane[2];
    size_t size[2];
    int width, height, stride, frame_id;
    void *lease;
    int format; /* actual hbn buffer format; raw recorder preserves all bytes */
    uint64_t wait_ns, cache_ns;
    uint64_t vin_timestamp, vin_tv_us;
} x5_image;
/* Configuration is initialization-only. AE auto is deliberately unavailable for
 * this HVS sensor_mode=2 profile (official sample reports nonconvergence). */
int x5_open(x5_capture **out, int profile, double exposure_us, double again, double dgain);
int x5_get(x5_capture *, int aps, x5_image *out); /* 100 ms SDK wait */
int x5_release(x5_capture *, int aps, x5_image *);
void x5_close(x5_capture *);
/* Raw-only build: configuration identity for ownership/control checks. */
int x5_raw_identity(x5_capture *, int *address, int *mode, int *config_index, int *bus);
#ifdef __cplusplus
}
#endif
