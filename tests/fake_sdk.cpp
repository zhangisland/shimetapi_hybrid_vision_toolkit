// Test-only SDK; never linked into a production sample. No device access.
#include <shimetapi/hv/camera.h>
#include <shimetapi/io/hybrid_writer.h>
#include <shimetapi/codec/mipi_raw8_codec.h>
#include <atomic>
#include <stdexcept>
#include <iostream>
extern std::atomic<bool> g_running;
namespace fake { std::vector<uint8_t> aps, evs; unsigned aw=0,ah=0,ew=0,eh=0; bool paired=false, fail=false; }
namespace Shimeta::hv {
struct Camera::Impl { int count=0; bool started=false; Camera::FrameCallback callback; };
Camera::Camera():impl_(new Impl){}
Camera::~Camera()=default;
bool Camera::Init(const DeviceConfig&) { return true; }
void Camera::SetFrameCallback(FrameCallback callback) {impl_->callback=callback;}
bool Camera::StartStream() { impl_->started=true; return true; }
void Camera::StopStream() { impl_->started=false; }
void Camera::Destroy() { if(impl_->started) throw std::runtime_error("Destroy before Stop"); }
bool Camera::GetFrame(Frame& f,int) {
    if(!impl_->started) throw std::runtime_error("GetFrame before Start");
    if(++impl_->count>6) { g_running=false; return false; }
    f.width=f.height=impl_->count<4?32:64;
    auto n=size_t(f.width)*f.height;
    f.aps_owner.reset(new uint8_t[n]); std::fill_n(f.aps_owner.get(),n,uint8_t(100));
    f.aps={f.aps_owner.get(),n}; f.format=PixelFormat::Gray8;
    if(impl_->callback) impl_->callback(f);
    return true;
}
}
namespace Shimeta::codec {
size_t MipiRaw8Decoder::Decode(const uint8_t*,size_t,std::vector<EventCD>&,int) {return 0;}
}
namespace Shimeta::io {
EventWriter::EventWriter()=default;
EventWriter::~EventWriter()=default;
bool EventWriter::open(const std::string&,uint32_t w,uint32_t h,RawFormat,uint64_t) {fake::ew=w;fake::eh=h;return true;}
void EventWriter::close() {}
size_t EventWriter::writeRaw(const uint8_t* p,size_t n) {fake::evs.assign(p,p+n);return n;}
HybridWriter::~HybridWriter()=default;
bool HybridWriter::open(const std::string&,const std::string&,uint32_t w,uint32_t h,RawFormat,double) {fake::aw=w;fake::ah=h;aps_frames_=0;return true;}
void HybridWriter::close() {}
bool HybridWriter::writeFrame(const Frame& f,const EvsTimestamp* ts) {
    if(fake::fail) return false;
    if(f.aps.size) {fake::aps.assign(f.aps.data,f.aps.data+f.aps.size); ++aps_frames_;}
    fake::paired=ts && ts->valid; return true;
}
}
