// Executable contract tests, NOT SDK ABI or sensor verification.
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
#include "../samples/cpp/live_record_display/x5_capture.h"
using hbn_vnode_handle_t=int;
enum { HBN_ISP_MODE_MANUAL=1 };
struct Range {float min,max;};
struct hbn_isp_exposure_attr_t {
    int version,mode;
    struct {Range exp_time_range,again_range,dgain_range;} auto_attr;
    struct {float exp_time,again,dgain,ispgain;} manual_attr;
};
static hbn_isp_exposure_attr_t initial,submitted,returned;
static std::vector<int> calls;
static int failGet=0,failSet=0;
int hbn_isp_get_exposure_attr(int,hbn_isp_exposure_attr_t* out) {
    calls.push_back(1);
    if(failGet && int(calls.size())==failGet) return -77;
    *out=calls.size()==1?initial:returned;return 0;
}
int hbn_isp_set_exposure_attr(int,hbn_isp_exposure_attr_t* in) {
    calls.push_back(2);submitted=*in;return failSet;
}
#include "../samples/cpp/live_record_display/x5_exposure_control.h"
#define CHECK(x) do {if(!(x)) throw std::runtime_error(#x);} while(0)
int main() {
    x5_exposure_state s{};
    initial.version=2;initial.mode=0;
    initial.manual_attr={0.01f,4,3,2.5f};
    initial.auto_attr={{0.001f,0.03f},{1,8},{1,4}};
    returned=initial;returned.manual_attr={0.000048f,1.5f,1.25f,2.5f};
    CHECK(x5_exposure_request(&s,50,1.5,1.25)==0);
    CHECK(std::abs(s.submitted_seconds-0.00005)<1e-11);
    CHECK(x5_apply_exposure(3,&s)==0);
    CHECK((calls==std::vector<int>{1,2,1}));
    CHECK(submitted.version==2 && submitted.mode==HBN_ISP_MODE_MANUAL);
    CHECK(submitted.manual_attr.ispgain==2.5f);
    CHECK(submitted.auto_attr.exp_time_range.min==0.001f); // not a manual limit
    CHECK(submitted.manual_attr.again==1.5f && submitted.manual_attr.dgain==1.25f);
    CHECK(s.requested_us==50 && s.readback_seconds==0.000048f && s.readback_available);
    CHECK(s.readback_seconds!=s.submitted_seconds); // must not replace getter data
    for(double us:{50.,500.,5000.}) {
        CHECK(x5_exposure_request(&s,us,1,1)==0);
        CHECK(std::abs(s.submitted_seconds*1e6-us)<0.001);
    }
    for(double invalid:{0.,-1.,std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN(),1e-300,1e300}) {
        CHECK(x5_exposure_request(&s,invalid,1,1)!=0);
        CHECK(x5_exposure_request(&s,500,invalid,1)!=0);
        CHECK(x5_exposure_request(&s,500,1,invalid)!=0);
    }
    calls.clear();failGet=1;x5_exposure_request(&s,500,1,1);
    CHECK(x5_apply_exposure(3,&s)==-77 && calls.size()==1 && !s.set_accepted);
    calls.clear();failGet=0;failSet=-65545;x5_exposure_request(&s,500,1,1);
    CHECK(x5_apply_exposure(3,&s)==-65545 && calls.size()==2 && !s.set_accepted);
    calls.clear();failSet=0;failGet=3;x5_exposure_request(&s,500,1,1);
    CHECK(x5_apply_exposure(3,&s)==-77 && s.set_accepted && !s.readback_available);
    calls.clear();failGet=0;returned.manual_attr.exp_time=NAN;
    x5_exposure_request(&s,500,1,1);
    CHECK(x5_apply_exposure(3,&s)==X5_CAPTURE_FATAL && !s.readback_available);
}
