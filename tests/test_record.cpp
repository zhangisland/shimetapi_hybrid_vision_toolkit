#include "../samples/cpp/live_record_display/live_widgets.h"
#include <atomic>
#include <iostream>
std::atomic<bool> g_running(true);
namespace fake { extern std::vector<uint8_t> aps,evs; extern unsigned aw,ah,ew,eh; extern bool paired,fail; }
#define CHECK(x) do {if(!(x)) throw std::runtime_error(#x);}while(0)
int main() { try {
 hv_live::RecordManager r; CHECK(r.start("fake_events","fake_images"));
 std::vector<uint8_t> raw(32*32,37),events{1,2,3}; auto original=raw;
 Shimeta::Frame f; f.width=f.height=32; f.format=Shimeta::PixelFormat::Gray8;
 f.aps={raw.data(),raw.size()}; f.evs={events.data(),events.size()};
 Shimeta::EvsTimestamp ts;ts.valid=true;ts.processed_timestamp=123;
 r.writeFrame(f,&ts);
 CHECK(fake::aw==32 && fake::ah==32 && fake::ew==768 && fake::eh==608);
 CHECK(fake::aps.size()==raw.size()*3/2); CHECK(std::equal(raw.begin(),raw.end(),fake::aps.begin()));
 CHECK(std::all_of(fake::aps.begin()+raw.size(),fake::aps.end(),[](uint8_t x){return x==128;}));
 CHECK(raw==original && fake::evs==events && fake::paired);
 f.format=Shimeta::PixelFormat::NV12;
 bool rejected=false;try{r.writeFrame(f,nullptr);}catch(const std::exception&){rejected=true;}
 CHECK(rejected && !r.isRecording());
 r.start("fake_events","fake_images"); f.format=Shimeta::PixelFormat::Gray8; fake::fail=true;
 rejected=false;try{r.writeFrame(f,nullptr);}catch(const std::exception&){rejected=true;}
 CHECK(rejected && !r.isRecording());
 std::cout<<"Fake writer: geometry, raw bytes, pairing, format switch and write failure passed\n";
 return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
