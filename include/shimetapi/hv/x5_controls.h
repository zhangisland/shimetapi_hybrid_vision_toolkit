#pragma once
// Include the matching X5 SDK's common_utils.h and hbn_isp_api.h first.
// This adapter borrows live, same-process native handles. It owns no pipeline.
// Camera's private Impl is deliberately not inspected or ABI-patched.
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace Shimeta::hv::x5 {
inline void check(int code, const char* operation) {
    if (code != 0) throw std::runtime_error(std::string(operation) +
                                          " failed: " + std::to_string(code));
}
// VIN output throttling, not sensor exposure/frame-length programming.
// Native input FPS must come from the active sensor configuration.
inline void setOutputFrameRate(hbn_vnode_handle_t vin, uint32_t inputFps,
                               uint32_t outputFps) {
    if (!vin) throw std::invalid_argument("Missing APS VIN handle");
    if (!inputFps || outputFps > inputFps)
        throw std::invalid_argument("Require input FPS > 0 and output FPS <= input FPS");
    vin_attr_ex_t attr {};
    attr.ex_attr_type = VIN_DYNAMIC_FPS_CTRL;
    const bool skip = outputFps && outputFps < inputFps;
    attr.fps_ctrl.skip_mode = skip ? CIM_IN_RATIO_SKIP : CIM_DISABLE_SKIP;
    attr.fps_ctrl.in_fps = skip ? inputFps : 0;
    attr.fps_ctrl.out_fps = skip ? outputFps : 0;
    check(hbn_vnode_set_attr_ex(vin, &attr), "hbn_vnode_set_attr_ex(FPS)");
}
inline hbn_isp_exposure_attr_t getExposure(hbn_vnode_handle_t isp) {
    if (!isp) throw std::invalid_argument("Exposure needs an ISP handle; VIN bypass has none");
    hbn_isp_exposure_attr_t attr {};
    check(hbn_isp_get_exposure_attr(isp, &attr), "hbn_isp_get_exposure_attr");
    return attr; // SDK getter mode is documented as unavailable: do not infer AE mode.
}
// Preserve all vendor fields (version, gains, AE settings) with read-modify-write.
// Range checks and quantization are delegated to the matching sensor/ISP driver.
// Returned attributes are a separate readback, not proof of per-frame application.
inline hbn_isp_exposure_attr_t setManualExposureUs(hbn_vnode_handle_t isp,
                                                 double microseconds) {
    if (!std::isfinite(microseconds) || microseconds <= 0 || microseconds > 1e6)
        throw std::invalid_argument("Exposure must be finite and in (0, 1000000] us");
    auto attr = getExposure(isp);
    attr.mode = HBN_ISP_MODE_MANUAL;
    attr.manual_attr.exp_time = static_cast<float>(microseconds / 1e6);
    check(hbn_isp_set_exposure_attr(isp, &attr), "hbn_isp_set_exposure_attr(manual)");
    return getExposure(isp);
}
inline hbn_isp_exposure_attr_t setAutoExposure(hbn_vnode_handle_t isp) {
    auto attr = getExposure(isp);
    attr.mode = HBN_ISP_MODE_AUTO;
    check(hbn_isp_set_exposure_attr(isp, &attr), "hbn_isp_set_exposure_attr(auto)");
    return getExposure(isp);
}
} // namespace Shimeta::hv::x5
