#pragma once
/* Include matching hbn_isp_api.h and x5_capture.h first. Also compiled by the
 * host contract test with a fake SDK; that test does not validate hardware ABI. */
#include <math.h>
#include <stdio.h>
#include <string.h>

static int x5_exposure_request(x5_exposure_state *s, double us, double again, double dgain) {
    memset(s, 0, sizeof(*s));
    /* Preserve the existing positive-input contract. SDK gain encoding and
     * sensor limits are not specified by the supplied headers. No invented
     * dB/index conversion, clipping, or claim that 1 is the hardware minimum. */
    if (!isfinite(us) || us <= 0 || !isfinite(again) || again <= 0 ||
        !isfinite(dgain) || dgain <= 0 || !isfinite((float)(us/1e6)) ||
        (float)(us/1e6) <= 0 || !isfinite((float)again) || (float)again <= 0 ||
        !isfinite((float)dgain) || (float)dgain <= 0) {
        fprintf(stderr, "Exposure invalid request: us=%.17g again=%.17g dgain=%.17g; require positive finite float-representable values\n", us, again, dgain);
        return X5_CAPTURE_FATAL;
    }
    s->requested_us=us; s->requested_again=again; s->requested_dgain=dgain;
    s->submitted_seconds=(float)(us/1e6);
    s->submitted_again=(float)again; s->submitted_dgain=(float)dgain;
    return 0;
}
static int x5_exposure_error(const char *stage, int ret) {
    fprintf(stderr, "Exposure %s failed: ret=%d magnitude_hex=0x%llx; APS VC1; no fallback; sensor_actual=unknown\n",
            stage, ret, (unsigned long long)(ret<0?-(long long)ret:(long long)ret));
    return ret;
}
static int x5_apply_exposure(hbn_vnode_handle_t isp, x5_exposure_state *s) {
    hbn_isp_exposure_attr_t attr={0}, readback={0};
    int ret=hbn_isp_get_exposure_attr(isp, &attr);
    if (ret) return x5_exposure_error("get_before_set", ret);
    fprintf(stderr, "Exposure SDK before: version=%d seconds=%.9g again=%.9g dgain=%.9g ispgain=%.9g; getter_mode=unavailable\n",
            (int)attr.version, attr.manual_attr.exp_time, attr.manual_attr.again,
            attr.manual_attr.dgain, attr.manual_attr.ispgain);
    /* These are AE policy ranges, NOT certified manual sensor limits. */
    fprintf(stderr, "Exposure SDK auto_policy_ranges: seconds=[%.9g,%.9g] again=[%.9g,%.9g] dgain=[%.9g,%.9g]; manual_sensor_limits=unknown\n",
            attr.auto_attr.exp_time_range.min, attr.auto_attr.exp_time_range.max,
            attr.auto_attr.again_range.min, attr.auto_attr.again_range.max,
            attr.auto_attr.dgain_range.min, attr.auto_attr.dgain_range.max);
    attr.mode=HBN_ISP_MODE_MANUAL;
    attr.manual_attr.exp_time=s->submitted_seconds;
    attr.manual_attr.again=s->submitted_again;
    attr.manual_attr.dgain=s->submitted_dgain;
    s->submitted_ispgain=attr.manual_attr.ispgain;
    fprintf(stderr, "Exposure SUBMIT: requested_us=%.17g seconds=%.9g again=%.9g dgain=%.9g ispgain=%.9g mode=manual; conversion=us/1e6_then_float,gains_float_passthrough; clipping=none; ispgain=preserved\n",
            s->requested_us, s->submitted_seconds, s->submitted_again,
            s->submitted_dgain, s->submitted_ispgain);
    ret=hbn_isp_set_exposure_attr(isp, &attr);
    if (ret) return x5_exposure_error("hbn_isp_set_exposure_attr(manual)", ret);
    s->set_accepted=1;
    fprintf(stderr, "ISP AE API accepted request; sensor_actual=unknown/unverified\n");
    ret=hbn_isp_get_exposure_attr(isp, &readback);
    if (ret) return x5_exposure_error("get_after_set", ret);
    s->readback_seconds=readback.manual_attr.exp_time;
    s->readback_again=readback.manual_attr.again;
    s->readback_dgain=readback.manual_attr.dgain;
    s->readback_ispgain=readback.manual_attr.ispgain;
    if (!isfinite(s->readback_seconds) || s->readback_seconds<=0 ||
        !isfinite(s->readback_again) || !isfinite(s->readback_dgain) ||
        !isfinite(s->readback_ispgain))
        return x5_exposure_error("invalid_SDK_readback", X5_CAPTURE_FATAL);
    s->readback_available=1;
    fprintf(stderr, "ISP attribute readback (not sensor actual): seconds=%.9g again=%.9g dgain=%.9g ispgain=%.9g; mode=unknown; timing=after_set\n",
            s->readback_seconds, s->readback_again, s->readback_dgain, s->readback_ispgain);
    if (s->readback_seconds!=s->submitted_seconds || s->readback_again!=s->submitted_again || s->readback_dgain!=s->submitted_dgain)
        fprintf(stderr, "Exposure SDK readback differs from submission; cause=unknown (asynchronous application/quantization/limits require driver verification); not proof of requested sensor conditions\n");
    return 0;
}
