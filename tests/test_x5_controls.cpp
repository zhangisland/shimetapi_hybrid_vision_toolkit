// Contract tests only: SDK ABI and hardware behavior require an X5 build.
#include <cstdint>
#include <stdexcept>
#include <cmath>
using hbn_vnode_handle_t = int;
enum { VIN_DYNAMIC_FPS_CTRL=2, CIM_DISABLE_SKIP=0, CIM_IN_RATIO_SKIP=1,
       HBN_ISP_MODE_MANUAL=1, HBN_ISP_MODE_AUTO=0 };
struct vin_attr_ex_t { int ex_attr_type; struct { int skip_mode; uint32_t in_fps,out_fps; } fps_ctrl; };
struct hbn_isp_exposure_attr_t { int mode; struct { float exp_time,again; } manual_attr; };
static vin_attr_ex_t fps;
static hbn_isp_exposure_attr_t exposure {0,{0.01f,2.0f}};
static int error=0, calls=0;
int hbn_vnode_set_attr_ex(int, vin_attr_ex_t* a) { ++calls; fps=*a; return error; }
int hbn_isp_get_exposure_attr(int, hbn_isp_exposure_attr_t* a) { ++calls; *a=exposure; return error; }
int hbn_isp_set_exposure_attr(int, hbn_isp_exposure_attr_t* a) { ++calls; exposure=*a; return error; }
#include <shimetapi/hv/x5_controls.h>
void require(bool ok) { if (!ok) throw std::runtime_error("contract assertion failed"); }
int main() {
    using namespace Shimeta::hv::x5;
    setOutputFrameRate(1,30,10); require(fps.fps_ctrl.out_fps==10 && fps.fps_ctrl.skip_mode==1);
    setOutputFrameRate(1,30,0); require(fps.fps_ctrl.skip_mode==0);
    setOutputFrameRate(1,30,30); require(fps.fps_ctrl.skip_mode==0);
    int before=calls;
    try { setOutputFrameRate(1,30,31); return 1; } catch(const std::invalid_argument&) {}
    try { setManualExposureUs(0,5000); return 2; } catch(const std::invalid_argument&) {}
    try { setManualExposureUs(1,NAN); return 3; } catch(const std::invalid_argument&) {}
    require(calls==before);
    auto value=setManualExposureUs(2,5000);
    require(std::abs(value.manual_attr.exp_time-0.005f)<1e-8 && value.manual_attr.again==2);
    setAutoExposure(2); require(exposure.mode==HBN_ISP_MODE_AUTO);
    error=-35;
    try { setOutputFrameRate(1,30,10); return 4; } catch(const std::runtime_error&) {}
    try { setManualExposureUs(2,5000); return 5; } catch(const std::runtime_error&) {}
}
