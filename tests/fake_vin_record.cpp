// No hardware claims. Full-size padded RAW16 buffers, poisoned on release.
#include "../samples/cpp/live_record_display/x5_capture.h"
#include <shimetapi/io/event_writer.h>
#include <shimetapi/io/hybrid_writer.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <vector>
#include <stdexcept>
struct x5_capture {int count[2]={};std::atomic<int> leases{0};};
static bool captureClosed=false;
int x5_open(x5_capture** out,int,double,double,double) {captureClosed=false;*out=new x5_capture;return 0;}
int x5_raw_identity(x5_capture*,int* a,int* m,int* i,int* bus) {*a=0x3c;*m=2;*i=240;*bus=6;return 0;}
int x5_get(x5_capture* c,int aps,x5_image* im) {
 std::this_thread::sleep_for(std::chrono::milliseconds(aps?33:10));
 if(std::getenv("HVS_FAKE_GET_FATAL")) return X5_CAPTURE_FATAL;
 *im={};im->width=aps?1632:4096;im->height=aps?1224:256;
 im->stride=aps?3280:4096;im->size[0]=size_t(im->stride)*im->height;
 auto* data=new uint8_t[im->size[0]];
 const uint8_t value=uint8_t(++c->count[aps]);
 if(!aps&&c->count[0]==20&&std::getenv("HVS_FAKE_SIGINT")) std::raise(SIGINT);
 std::memset(data,value,im->size[0]);
 if(aps) for(int y=0;y<im->height;++y) for(int x=0;x<im->width;++x) data[y*im->stride+2*x+1]=0;
 im->frame_id=c->count[aps];im->format=aps?0x2b:0x2a;
 im->plane[0]=data;im->lease=data;++c->leases;
 if(std::getenv("HVS_FAKE_BAD_RAW_STRIDE")) im->stride=1;
 return 0;
}
int x5_release(x5_capture* c,int,x5_image* im) {
 auto* p=static_cast<uint8_t*>(im->lease);std::memset(p,0xee,im->size[0]);delete[] p;im->lease=nullptr;--c->leases;return 0;
}
void x5_close(x5_capture* c) {if(c->leases!=0) std::abort();delete c;captureClosed=true;}
namespace Shimeta::io {
static std::map<const void*,std::ofstream> files;
static bool injected=false;
EventWriter::EventWriter()=default;EventWriter::~EventWriter()=default;
bool EventWriter::open(const std::string& path,uint32_t,uint32_t,RawFormat,uint64_t) {
 if(!captureClosed) throw std::runtime_error("Payload IO during capture!");
 file_.open(path,std::ios::binary|std::ios::trunc);file_<<"FAKE-EVENT-HEADER\n";return bool(file_);
}
void EventWriter::close() {if(file_.is_open()) file_.close();}
size_t EventWriter::writeRaw(const uint8_t* p,size_t n) {file_.write(reinterpret_cast<const char*>(p),n);return file_?n:0;}
HybridWriter::~HybridWriter() {close();}
static void u32(std::string& s,uint32_t v) {for(int i=0;i<4;++i)s+=char(v>>(i*8));}
static std::string chunk(const std::string& tag,const std::string& data) {std::string s=tag;u32(s,uint32_t(data.size()));s+=data;if(data.size()%2)s+='\0';return s;}
bool HybridWriter::open(const std::string&,const std::string& path,uint32_t,uint32_t,RawFormat,double) {
 if(!captureClosed) throw std::runtime_error("AVI opened during capture!");
 if(std::getenv("HVS_FAKE_SAVE_FAILURE")&&!injected) {injected=true;return false;}
 auto& out=files[this];out.open(path,std::ios::binary|std::ios::trunc);
 std::string avih(56,0),strh(56,0);strh.replace(0,4,"vids");
 auto header=chunk("RIFF","AVI "+chunk("LIST","hdrl"+chunk("avih",avih)+chunk("LIST","strl"+chunk("strh",strh))));
 out.write(header.data(),header.size());aps_frames_=0;return bool(out);
}
void HybridWriter::close() {auto it=files.find(this);if(it!=files.end()) {
 auto& out=it->second;const auto size=out.tellp();std::string n;u32(n,uint32_t(size)-8);
 out.seekp(4);out.write(n.data(),4);out.close();files.erase(it);
}}
bool HybridWriter::writeFrame(const Frame& f,const EvsTimestamp*) {
 auto& out=files[this];std::string header="00db";u32(header,uint32_t(f.aps.size));out.write(header.data(),header.size());
 out.write(reinterpret_cast<const char*>(f.aps.data),f.aps.size);++aps_frames_;return bool(out);
}
}
