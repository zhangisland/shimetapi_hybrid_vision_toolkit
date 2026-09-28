// Test-only SDK; never linked into a production sample. No device access.
#include <shimetapi/hv/camera.h>
#include <shimetapi/io/hybrid_writer.h>
#include <shimetapi/codec/mipi_raw8_codec.h>
#include <atomic>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <map>
#include <thread>
#include <chrono>
extern std::atomic<bool> g_running;
namespace fake { std::vector<uint8_t> aps, evs; unsigned aw=0,ah=0,ew=0,eh=0; bool paired=false, fail=false, files=false; int delayMs=0; std::map<const void*,std::string> paths; }
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
bool HybridWriter::open(const std::string&,const std::string& path,uint32_t w,uint32_t h,RawFormat,double) {fake::aw=w;fake::ah=h;aps_frames_=0;if(fake::files)fake::paths[this]=path;return true;}
void HybridWriter::close() {
    auto it=fake::paths.find(this);if(it==fake::paths.end())return;
    auto word=[](std::string& s,uint32_t n){for(int i=0;i<4;++i)s+=char(n>>(8*i));};
    auto chunk=[&](const std::string& tag,const std::string& data){std::string s=tag;word(s,uint32_t(data.size()));s+=data;if(data.size()%2)s+='\0';return s;};
    std::string avih(56,0),strh(56,0);strh.replace(0,4,"vids");
    for(int i=0;i<4;++i)strh[32+i]=char(aps_frames_>>(8*i));
    const auto content=chunk("RIFF","AVI "+chunk("LIST","hdrl"+chunk("avih",avih)+chunk("LIST","strl"+chunk("strh",strh))));
    std::ofstream out(it->second,std::ios::binary);out.write(content.data(),content.size());fake::paths.erase(it);
}
bool HybridWriter::writeFrame(const Frame& f,const EvsTimestamp* ts) {
    if(fake::delayMs) std::this_thread::sleep_for(std::chrono::milliseconds(fake::delayMs));
    if(fake::fail) return false;
    if(f.aps.size) {fake::aps.assign(f.aps.data,f.aps.data+f.aps.size); ++aps_frames_;}
    fake::paired=ts && ts->valid; return true;
}
}
