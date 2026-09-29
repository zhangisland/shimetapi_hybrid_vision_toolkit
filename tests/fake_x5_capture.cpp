// Test-only native backend: padded NV12, independent arrivals and an EVS stall.
#include "../samples/cpp/live_record_display/x5_capture.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <thread>
#include <vector>
struct x5_capture {std::atomic<int> leases{0};int counts[2]={0,0};};
struct Storage {std::vector<uint8_t> y,uv;};
int x5_open(x5_capture** out,int profile,double us,double again,double dgain) {
    *out=nullptr;
    if(const char* expected=std::getenv("HVS_FAKE_EXPECT_CONTROLS")) {
        int p;double e,a,d;
        if(std::sscanf(expected,"%d,%lf,%lf,%lf",&p,&e,&a,&d)!=4 ||
           p!=profile || e!=us || a!=again || d!=dgain) return X5_CAPTURE_FATAL;
    }
    if(std::getenv("HVS_FAKE_OPEN_ERROR")) return -65545;
    *out=new x5_capture;return 0;
}
int x5_get(x5_capture* c,int aps,x5_image* im) {
    std::this_thread::sleep_for(std::chrono::milliseconds(aps?33:10));
    if(!aps && ++c->counts[0]>15) return -11;
    Storage* s=new Storage;
    s->y.resize(48*32,0xDD);s->uv.resize(48*16,0xDD);
    for(int y=0;y<32;++y) std::memset(s->y.data()+y*48,100,32);
    for(int y=0;y<16;++y) std::memset(s->uv.data()+y*48,128,32);
    *im={};im->plane[0]=s->y.data();im->plane[1]=s->uv.data();
    im->size[0]=s->y.size();im->size[1]=s->uv.size();
    im->width=im->height=32;im->stride=std::getenv("HVS_FAKE_BAD_STRIDE")?16:48;im->lease=s;++c->leases;return 0;
}
int x5_release(x5_capture* c,int,x5_image* im) {
    auto* s=static_cast<Storage*>(im->lease);std::fill(s->y.begin(),s->y.end(),0xEE);
    delete s;im->lease=nullptr;--c->leases;return 0;
}
void x5_close(x5_capture* c) {if(c->leases!=0) std::abort();delete c;}

int x5_get_exposure_state(const x5_capture*,x5_exposure_state* out) {*out={};return 0;}
