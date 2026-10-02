#include "memory_arena.h"
#include "apx_manual.h"
#include "dual_stream_writer.h"
#include "avi_timing.h"
#include "recording_storage.h"
#include "../live_record_display/x5_capture.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#ifdef __linux__
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <unistd.h>
#endif
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
static uint64_t ns() {return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
static volatile std::sig_atomic_t interrupted=0;
static void signalStop(int) {interrupted=1;}
struct Options {
 fs::path output;
 double seconds=0,warmup=1,timeout=10,stall=2,us=0,lineUs=0,gain=-1;
 size_t maxBytes=1024ULL<<20;
 int lines=0,bus=-1,address=0x3c,profile=1;
 std::string diagnostic="record";
};
static Options parse(int argc,char** argv) {
 Options o;
 for(int i=1;i<argc;++i) {
   std::string k=argv[i];
   if(k=="--help") {std::cout<<"Native dual VIN recorder v1: --output NEW --seconds 5 --max-mib 1024 [--warmup 1] [--diagnostic record|receive|copy] [--aps-exposure-lines 100 | --aps-exposure-us 1000 --aps-line-time-us VERIFIED] [--aps-gain-db 0] --i2c-bus VERIFIED\n"; std::exit(0);}
   if(i+1==argc) throw std::runtime_error("Missing value: "+k);
   std::string v=argv[++i];
   if(k=="--output") {o.output=v;continue;}
   if(k=="--storage") {if(v!="memory") throw std::runtime_error("native VIN requires --storage memory; legacy SDK provides disk baseline");continue;}
   if(k=="--ram-dir") continue; // deprecated compatibility; no tmpfs is used
   if(k=="--diagnostic") {o.diagnostic=v;continue;}
   size_t used=0; double n=std::stod(v,&used);
   if(used!=v.size()||!std::isfinite(n)||n<0) throw std::runtime_error("Invalid number: "+v);
   auto integer=[&](int lo,int hi) {if(n<lo||n>hi||n!=std::floor(n)) throw std::runtime_error("Invalid integer: "+k); return int(n);};
   if(k=="--seconds") {if(n>86400) throw std::runtime_error("seconds must be <=86400");o.seconds=n;}
   else if(k=="--warmup") {if(n>300) throw std::runtime_error("warmup must be <=300");o.warmup=n;}
   else if(k=="--timeout") o.timeout=n;
   else if(k=="--aps-stall-timeout") o.stall=n;
   else if(k=="--max-mib") {if(n<64||n>2048) throw std::runtime_error("native --max-mib must be 64..2048 including 32 MiB workspace");o.maxBytes=size_t(n*1048576);}
   else if(k=="--aps-exposure-lines") o.lines=integer(1,1162);
   else if(k=="--aps-exposure-us") {if(n==0) throw std::runtime_error("exposure must be positive");o.us=n;}
   else if(k=="--aps-line-time-us") {if(n==0) throw std::runtime_error("line time must be positive");o.lineUs=n;}
   else if(k=="--aps-gain-db") {apx::gainIndex(n);o.gain=n;}
   else if(k=="--i2c-bus") o.bus=integer(0,255);
   else if(k=="--i2c-address") o.address=integer(0x08,0x77);
   else if(k=="--profile") o.profile=integer(1,1); // only audited config_index=240
   else if(k=="--aps-width") {if(n!=1632) throw std::runtime_error("profile 1 APS width is 1632");}
   else if(k=="--aps-height") {if(n!=1224) throw std::runtime_error("profile 1 APS height is 1224");}
   else if(k=="--evs-width") {if(n!=768) throw std::runtime_error("event geometry is 768x608");}
   else if(k=="--evs-height") {if(n!=608) throw std::runtime_error("event geometry is 768x608");}
   else throw std::runtime_error("Unknown option: "+k);
 }
 if(o.output.empty()||o.timeout<=0||o.stall<=0) throw std::runtime_error("output and positive timeouts required");
 if(o.lines&&o.us) throw std::runtime_error("exposure lines and microseconds are mutually exclusive");
 if(o.us) o.lines=apx::exposureLines(o.us,o.lineUs);
 if((o.lines||o.gain>=0)&&o.bus<0) throw std::runtime_error("Manual control requires explicit --i2c-bus from board configuration (no scanning)");
 if(o.diagnostic!="record"&&o.diagnostic!="receive"&&o.diagnostic!="copy") throw std::runtime_error("Unknown diagnostic mode");
 return o;
}

class SensorIO {
 int fd=-1,lockFd=-1;
 int address;
public:
 SensorIO(int bus,int addr):address(addr) {
#ifdef __linux__
   const std::string lock="/tmp/hvs-apx-i2c-"+std::to_string(bus)+"-"+std::to_string(addr)+".lock";
   lockFd=::open(lock.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
   if(lockFd<0||flock(lockFd,LOCK_EX|LOCK_NB)) {if(lockFd>=0) ::close(lockFd);throw std::runtime_error("Sensor control lock busy/unavailable");}
   fd=::open(("/dev/i2c-"+std::to_string(bus)).c_str(),O_RDWR|O_CLOEXEC);
   // Deliberately NOT I2C_SLAVE_FORCE. Kernel-owned devices fail closed.
   if(fd<0||ioctl(fd,I2C_SLAVE,addr)<0) {
     if(fd>=0) ::close(fd); ::close(lockFd);
     throw std::runtime_error("I2C open/ownership check failed; use a supported driver control, never -f");
   }
#else
   throw std::runtime_error("Manual sensor I2C requires Linux");
#endif
 }
 ~SensorIO() {
#ifdef __linux__
   if(fd>=0) ::close(fd); if(lockFd>=0) ::close(lockFd);
#endif
 }
 void readBytes(uint16_t reg,uint8_t* values,uint16_t count) {
#ifdef __linux__
   uint8_t bytes[]={uint8_t(reg>>8),uint8_t(reg)};
   i2c_msg msgs[]={{uint16_t(address),0,2,bytes},{uint16_t(address),I2C_M_RD,count,values}};
   i2c_rdwr_ioctl_data req{msgs,2};
   if(ioctl(fd,I2C_RDWR,&req)!=2) throw std::runtime_error("I2C read failed at "+std::to_string(reg));
#else
   throw std::runtime_error("No I2C");
#endif
 }
 uint8_t read(uint16_t reg) {uint8_t value=0;readBytes(reg,&value,1);return value;}
 void write(uint16_t reg,uint8_t value) {
#ifdef __linux__
   uint8_t bytes[]={uint8_t(reg>>8),uint8_t(reg),value};
   if(::write(fd,bytes,3)!=3) throw std::runtime_error("I2C write failed at "+std::to_string(reg));
#endif
 }
};
struct Stats {
 std::atomic<uint64_t> frames{0},last{0};
 uint64_t first=0,bytes=0,warmup=0,errors=0,gaps=0,duplicates=0,resets=0,overflow=0;
 uint64_t getNs=0,waitNs=0,cacheNs=0,copyNs=0,releaseNs=0,workMax=0,intervalMax=0;
 uint32_t previous=0;
 int lastError=0;
 void received(const x5_image& im,uint64_t now) {
   const auto count=frames.load();
   if(!count) first=now;
   else {
     intervalMax=std::max(intervalMax,now-last.load());
     const uint32_t delta=uint32_t(im.frame_id)-previous;
     if(delta==0) ++duplicates;
     else if(delta<0x80000000U) gaps+=delta-1;
     else ++resets;
   }
   previous=uint32_t(im.frame_id);last=now;bytes+=im.size[0];++frames;
 }
 double fps() const {return frames>1&&last>first?double(frames-1)*1e9/double(last-first):0;}
};
static void statsOut(std::ostream& out,const char* prefix,const Stats& s,double seconds) {
 out<<prefix<<"_received="<<s.frames<<'\n'<<prefix<<"_bytes="<<s.bytes<<'\n'
    <<prefix<<"_receive_fps="<<s.fps()<<'\n'<<prefix<<"_window_rate="<<(seconds>0?s.frames/seconds:0)<<'\n'
    <<prefix<<"_bytes_per_second="<<(seconds>0?s.bytes/seconds:0)<<'\n'
    <<prefix<<"_warmup_received="<<s.warmup<<'\n'<<prefix<<"_get_errors_or_timeouts="<<s.errors<<'\n'
    <<prefix<<"_last_get_error="<<s.lastError<<'\n'
    <<prefix<<"_frame_id_gaps="<<s.gaps<<'\n'<<prefix<<"_duplicate_ids="<<s.duplicates<<'\n'
    <<prefix<<"_id_resets="<<s.resets<<'\n'<<prefix<<"_capacity_rejected="<<s.overflow<<'\n'
    <<prefix<<"_get_including_wait_cache_ns="<<s.getNs<<'\n'<<prefix<<"_copy_reservation_ns="<<s.copyNs<<'\n'
    <<prefix<<"_VIN_wait_ns="<<s.waitNs<<'\n'<<prefix<<"_cache_invalidation_ns="<<s.cacheNs<<'\n'
    <<prefix<<"_release_ns="<<s.releaseNs<<'\n'<<prefix<<"_max_hold_ns="<<s.workMax<<'\n'
    <<prefix<<"_max_receive_interval_ns="<<s.intervalMax<<'\n';
}

static void checkedClose(std::ofstream& f) {f.flush(); f.close(); if(!f) throw std::runtime_error("Save stream flush/close failed");}
static uint64_t hashBytes(const uint8_t* p,size_t n) {
 uint64_t h=14695981039346656037ULL;for(size_t i=0;i<n;++i) {h^=p[i];h*=1099511628211ULL;}return h;
}
// After capture only. aps.vin.bin retains every original byte including padding.
// AVI is an 8-bit preview using the bundled SDK's audited RAW10 (word >> 2)
// conversion, never the sole copy of the RAW10 input.
static void save(const Options& o,const MemoryArena& arena,double fps,const std::string& report) {
 const auto begin=ns();
 fs::path stage=o.output/"saving.partial";
 fs::create_directories(stage);
 if(fs::space(stage).available<arena.used()*2+(32ULL<<20))
   throw std::runtime_error("Insufficient free space for RAW + AVI; free space then create retry.request");
 DualStreamWriter writer;
 if(!writer.open((stage/"events.raw").string(),(stage/"aps.avi").string(),768,608,1632,1224)) throw std::runtime_error("Cannot open compatibility writer");
 std::ofstream raw(stage/"aps.vin.bin",std::ios::binary|std::ios::trunc),
   evsRaw(stage/"evs.vin.bin",std::ios::binary|std::ios::trunc),meta(stage/"vin.frames.jsonl",std::ios::trunc);
 if(!raw||!evsRaw||!meta) throw std::runtime_error("Cannot open RAW files");
 std::vector<uint8_t> preview(size_t(1632)*1224*3/2,128);
 std::vector<uint8_t> packedEvents(size_t(4096)*256);
 uint64_t aps=0,evs=0,offset=0,evsOffset=0,evsPackedBytes=0,done=0,invalid=0,convertNs=0,writeNs=0,verifyNs=0;
 int progress=-1;
 try {
 arena.each([&](const MemoryArena::Header& h,const uint8_t* bytes) {
   const auto start=ns();
   Shimeta::Frame f{};f.width=h.width;f.height=h.height;f.frame_id=int(h.frameId);
   if(h.stream) {
     raw.write(reinterpret_cast<const char*>(bytes),h.bytes);
     writeNs+=ns()-start;
     const auto convert=ns();
     if(h.width!=1632||h.height!=1224||h.stride<h.width*2||h.bytes<uint64_t(h.stride)*h.height) throw std::runtime_error("Unexpected APS RAW layout");
     for(size_t y=0;y<h.height;++y) for(size_t x=0;x<h.width;++x) {
       const auto* p=bytes+y*h.stride+x*2;
       const uint16_t word=uint16_t(p[0])|(uint16_t(p[1])<<8);
       if(word>1023) ++invalid;
       preview[y*h.width+x]=uint8_t(std::min<unsigned>(word>>2,255));
     }
     convertNs+=ns()-convert;
     f.aps={preview.data(),preview.size()};f.format=Shimeta::PixelFormat::NV12;++aps;
   } else {
     evsRaw.write(reinterpret_cast<const char*>(bytes),h.bytes);
     if(h.width!=4096||h.height!=256||h.stride<h.width||h.bytes<uint64_t(h.stride)*h.height) throw std::runtime_error("Unexpected EVS RAW layout");
     for(size_t y=0;y<h.height;++y) std::memcpy(packedEvents.data()+y*h.width,bytes+y*h.stride,h.width);
     f.evs={packedEvents.data(),packedEvents.size()};evsPackedBytes+=packedEvents.size();++evs;
   }
   const auto write=ns();
   if(!writer.writeFrame(f,nullptr)) throw std::runtime_error("Compatibility write failed");
   const auto verify=ns(); const auto hash=hashBytes(bytes,h.bytes);const auto hashNs=ns()-verify;verifyNs+=hashNs;
   meta<<"{\"stream\":\""<<(h.stream?"aps":"evs")<<"\",\"offset\":"<<(h.stream?offset:evsOffset)
       <<",\"bytes\":"<<h.bytes<<",\"host_ns\":"<<h.hostNs<<",\"frame_id\":"<<h.frameId
       <<",\"width\":"<<h.width<<",\"height\":"<<h.height<<",\"stride\":"<<h.stride
       <<",\"format\":"<<h.format<<",\"fnv1a64\":\""<<std::hex<<hash<<std::dec
       <<"\",\"exposure_per_frame\":null,\"sensor_timestamp\":null}\n";
   if(h.stream) offset+=h.bytes;else evsOffset+=h.bytes;
   writeNs+=ns()-write-hashNs;
   done+=MemoryArena::extent(h.bytes);
   int percent=int(done*100/std::max<size_t>(arena.used(),1));
   if(percent/10!=progress) {progress=percent/10;std::cout<<"Saving "<<percent<<"% APS="<<aps<<" EVS packets="<<evs<<std::endl;}
 });
 if(writer.apsFrameCount()!=aps) throw std::runtime_error("AVI writer frame count mismatch");
 writer.close();checkedClose(raw);checkedClose(evsRaw);checkedClose(meta);
 } catch(...) {writer.close();throw;}
 if(aps&&fps>0) setAviFrameRate(stage/"aps.avi",fps);
 if(fs::file_size(stage/"aps.vin.bin")!=offset || fs::file_size(stage/"evs.vin.bin")!=evsOffset || fs::file_size(stage/"events.raw")<evsPackedBytes ||
    (aps&&fs::file_size(stage/"aps.avi")<aps*preview.size())) throw std::runtime_error("Save size validation failed");
 // Verify the unique full-resolution raw copy byte-for-byte before committing.
 const auto verify=ns();
 std::ifstream rawCheck(stage/"aps.vin.bin",std::ios::binary),evsCheck(stage/"evs.vin.bin",std::ios::binary);
 std::vector<uint8_t> check(1<<20);
 arena.each([&](const MemoryArena::Header& h,const uint8_t* bytes) {
   auto& in=h.stream?rawCheck:evsCheck;
   for(size_t at=0;at<h.bytes;) {
     const auto n=std::min<size_t>(check.size(),h.bytes-at);in.read(reinterpret_cast<char*>(check.data()),n);
     if(!in||std::memcmp(bytes+at,check.data(),n)) throw std::runtime_error("Saved RAW byte verification failed");at+=n;
   }
 });
 verifyNs+=ns()-verify;rawCheck.close();evsCheck.close();
 // Also verify the packed compatibility EVS payload, independently of its
 // variable-length SDK header. Every packet retains its VIN boundaries in JSONL.
 const auto packedVerify=ns();
 std::ifstream eventCheck(stage/"events.raw",std::ios::binary);
 eventCheck.seekg(std::streamoff(fs::file_size(stage/"events.raw")-evsPackedBytes));
 arena.each([&](const MemoryArena::Header& h,const uint8_t* bytes) {
   if(h.stream) return;
   for(size_t y=0;y<h.height;++y) {
     eventCheck.read(reinterpret_cast<char*>(check.data()),h.width);
     if(!eventCheck||std::memcmp(check.data(),bytes+y*h.stride,h.width)) throw std::runtime_error("Compatibility EVS verification failed");
   }
 });
 eventCheck.close();verifyNs+=ns()-packedVerify;
 for(auto name:{"aps.vin.bin","evs.vin.bin","vin.frames.jsonl","events.raw","aps.avi"}) RecordingStorage::syncFile(stage/name);
 std::ofstream summary(stage/"summary.txt",std::ios::trunc);
 summary<<report<<"aps_frames="<<aps<<"\nevs_packets="<<evs<<"\naps_observed_fps="<<fps
   <<"\naps_width=1632\naps_height=1224\nevs_width=768\nevs_height=608\ngray8_frames="<<aps
   <<"\naps_paired_timestamps=0\naps_storage_format=RAW10_16LE_with_stride_and_NV12_preview"
   <<"\nraw10_words_outside_10bits="<<invalid<<"\npreview_layout_status="<<(invalid?"UNVERIFIED_do_not_use_preview_for_photometry":"matches_bundled_SDK_word_shift_2")
   <<"\nconvert_ns="<<convertNs<<"\nwrite_ns="<<writeNs<<"\nverify_ns="<<verifyNs
   <<"\nsave_seconds="<<(ns()-begin)/1e9<<"\nsave_payload_MiB_per_second="<<(arena.used()/1048576.0)/((ns()-begin)/1e9)<<'\n';
 checkedClose(summary);RecordingStorage::syncFile(stage/"summary.txt");
 // summary is the last rename and the sole completion marker. A retry can
 // replace this session's own partly-published files; output was NEW at start.
 for(auto name:{"aps.vin.bin","evs.vin.bin","vin.frames.jsonl","events.raw","aps.avi"}) {
   if(fs::exists(o.output/name)) fs::remove(o.output/name);
   fs::rename(stage/name,o.output/name);
 }
 RecordingStorage::syncFile(o.output);
 if(fs::exists(o.output/"summary.txt")) fs::remove(o.output/"summary.txt");
 fs::rename(stage/"summary.txt",o.output/"summary.txt");
 try {RecordingStorage::syncFile(o.output);} catch(...) {
   fs::rename(o.output/"summary.txt",stage/"summary.txt");throw;
 }
 fs::remove(o.output/"recording.storage");
 std::cout<<"Saved and byte-verified in "<<(ns()-begin)/1e9<<" seconds\n";
}

static int run(const Options& o) {
 const size_t capacity=MemoryArena::payloadCapacity(o.maxBytes,RecordingStorage::availableMemory());
 if(!fs::create_directory(o.output)) throw std::runtime_error("Output must be a new directory");
 std::ofstream(o.output/"recording.storage")<<"process-memory: do not kill; on save failure create retry.request\n";
 std::signal(SIGINT,signalStop);std::signal(SIGTERM,signalStop);
 MemoryArena arena(o.diagnostic=="receive"?64:capacity);
 bool locked=false;
#ifdef __linux__
 int captureLock=::open("/tmp/hvs-apx-capture.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
 if(captureLock<0||flock(captureLock,LOCK_EX|LOCK_NB)) {if(captureLock>=0)::close(captureLock);throw std::runtime_error("Another native capture process owns this sensor");}
 // Process exit releases this lock. It is not a substitute for external SDK ownership.
 locked=mlock(arena.data(),arena.capacity())==0;
 std::ifstream swaps("/proc/swaps");std::string line;std::getline(swaps,line);
 const bool swapActive=bool(std::getline(swaps,line));
 if(swapActive&&!locked) throw std::runtime_error("Swap active and mlock failed: grant sufficient memlock limit; no global swap settings changed");
#endif
 std::cout<<"Native VIN recorder v1, preallocated="<<arena.capacity()<<" bytes locked="<<locked<<"; no payload files until stop\n";
 x5_capture* camera=nullptr;
 if(x5_open(&camera,o.profile,0,1,1)) throw std::runtime_error("Native VIN open failed");
 std::unique_ptr<x5_capture,decltype(&x5_close)> owner(camera,x5_close);
#ifdef __linux__
 // Small provenance file only; records actual mappings, not ldd predictions.
 {std::ifstream maps("/proc/self/maps");std::ofstream loaded(o.output/"runtime.maps");loaded<<maps.rdbuf();}
#endif
 int addr=0,mode=0,index=0,configuredBus=-1;
 if(x5_raw_identity(camera,&addr,&mode,&index,&configuredBus)||addr!=o.address||mode!=2||index!=240) throw std::runtime_error("Unexpected sensor identity/config; manual control and RAW layout not audited for this mode");
 std::unique_ptr<SensorIO> sensor;
 apx::Registers regs;
 int vts=-1;
 if(o.lines||o.gain>=0) {
   if(configuredBus<0||o.bus!=configuredBus) throw std::runtime_error("Requested I2C bus differs from selected VIN vcon device-tree bus, or bus is unavailable");
   sensor=std::make_unique<SensorIO>(o.bus,o.address);
   // The exact pair's vp_sensors chip_id is 0x0808 at 0x3428.
   const auto chipId=apx::verifyIdentity(*sensor,o.bus,o.address);
   std::cout<<"APX003CC identity reg16/data16: 0x"<<std::hex<<chipId<<std::dec<<std::endl;
   vts=(int(sensor->read(0x0160))<<8)|sensor->read(0x0161);
   regs=apx::apply(*sensor,o.lines,o.gain);
   std::cout<<"APS control after both streams started: lines="<<o.lines<<" gain_dB="<<(o.gain>=0?apx::gainIndex(o.gain)*0.375:-1)<<" VTS="<<vts<<" registers verified; NOT per-frame exposure\n";
 }
 std::atomic<bool> stop{false},full{false};
 Stats stats[2];std::string errors[2];
 const uint64_t start=ns(),begin=start+uint64_t(o.warmup*1e9);
 const uint64_t end=o.seconds>0?begin+uint64_t(o.seconds*1e9):UINT64_MAX;
 auto receive=[&](int stream) {
   auto& s=stats[stream];
   while(!stop.load()) {
     x5_image im{};const auto getBegin=ns();
     const int result=x5_get(camera,stream,&im);const auto received=ns();
     if(result) {++s.errors;s.lastError=result;if(result==X5_CAPTURE_FATAL) {errors[stream]="fatal VIN get/validation failure";stop=true;}else std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
     const auto hold=ns();
     try {
       if(!im.plane[0]||!im.size[0]||im.width<=0||im.height<=0||im.stride<=0 ||
          (!stream&&(im.width!=4096||im.height!=256||im.stride<4096||im.size[0]<size_t(im.stride)*im.height)) ||
          (stream&&(im.width!=1632||im.height!=1224||im.stride<3264||im.size[0]<size_t(im.stride)*im.height))) throw std::runtime_error("Invalid VIN plane layout");
       if(received<begin) ++s.warmup;
       else if(received<end&&!stop.load()) {
         s.getNs+=received-getBegin;s.waitNs+=im.wait_ns;s.cacheNs+=im.cache_ns;s.received(im,received);
         if(o.diagnostic!="receive") {
           MemoryArena::Header h{im.size[0],received,received-getBegin,0,uint32_t(stream),uint32_t(im.width),uint32_t(im.height),uint32_t(im.stride),uint32_t(im.frame_id),uint32_t(im.format)};
           const auto copy=ns();
           if(!arena.append(h,im.plane[0])) {++s.overflow;full=true;stop=true;}
           s.copyNs+=ns()-copy;
         }
       }
     } catch(const std::exception& e) {errors[stream]=e.what();stop=true;}
     const auto release=ns();
     if(x5_release(camera,stream,&im)) {errors[stream]="VIN release failure";stop=true;}
     s.releaseNs+=ns()-release;s.workMax=std::max(s.workMax,ns()-hold);
   }
 };
 std::thread evs(receive,0),aps;
 try {aps=std::thread(receive,1);} catch(...) {stop=true;evs.join();throw;}
 std::string error,reason="duration";
 uint64_t nextReport=begin,nextVerify=begin;
 try {
 while(!stop) {
   const auto now=ns();
   if(interrupted||fs::exists(o.output/"stop.request")) {reason="requested";break;}
   if(now>=end) break;
   if(now>=nextReport) {
     std::cout<<"VIN received APS="<<stats[1].frames<<" EVS packets="<<stats[0].frames<<" buffered MiB="<<arena.used()/1048576.0<<std::endl;nextReport=now+1000000000ULL;
   }
   if(now>=begin) for(int i=0;i<2;++i) {
     const uint64_t last=stats[i].last.load();
     const uint64_t reference=last?last:begin;
     // A producer may publish a timestamp newer than this monitor iteration.
     if(now>reference&&double(now-reference)/1e9>(last&&i?o.stall:o.timeout)) throw std::runtime_error(i?"APS VIN stalled":"EVS VIN stalled");
   }
   if(sensor&&now>=nextVerify) {apx::verify(*sensor,regs);nextVerify=now+1000000000ULL;}
   std::this_thread::sleep_for(std::chrono::milliseconds(10));
 }
 } catch(const std::exception& e) {error=e.what();}
 const uint64_t stopped=std::min(ns(),end);
 stop=true;evs.join();aps.join(); // no DMA leases survive these joins
 if(sensor) {try {apx::verify(*sensor,regs);} catch(const std::exception& e) {error+=e.what();}}
 owner.reset();sensor.reset(); // stop hardware before any bulk output
 for(auto& e:errors) if(!e.empty()) error+="; "+e;
 if(full) reason="capacity";
 if(!stats[0].frames||!stats[1].frames) error+="; one stream absent";
 const double seconds=stopped>begin?(stopped-begin)/1e9:0;
 std::ostringstream report;
 report<<std::setprecision(12)<<"status="<<(error.empty()?"complete":"failed")<<"\nreason="<<(error.empty()?reason:error)
   <<"\nbackend=native_dual_vin_v1\nstorage=process_memory\ndiagnostic="<<o.diagnostic
   <<"\ncapture_seconds="<<seconds<<"\nwarmup_seconds="<<o.warmup<<"\nmemory_budget_bytes="<<o.maxBytes
   <<"\nbuffer_capacity_bytes="<<arena.capacity()<<"\nbuffer_peak_bytes="<<arena.used()<<"\nmlocked="<<locked
   <<"\nqueue_depth=0\nencoding_during_capture=0\nwriting_during_capture=0\ncopy_count_per_plane="<<(o.diagnostic=="receive"?0:1)
   <<"\nupstream_sensor_loss=unknown\nVIN_frame_ids_not_sensor_counters=true\nAE=absent_no_ISP_node"
   <<"\naps_exposure_requested_us="<<o.us<<"\naps_exposure_submitted_lines="<<o.lines
   <<"\naps_line_time_us_user_verified="<<o.lineUs<<"\naps_gain_requested_db="<<o.gain
   <<"\naps_gain_submitted_db="<<(o.gain>=0?apx::gainIndex(o.gain)*0.375:-1)
   <<"\naps_sdk_exposure_readback=unavailable_direct_register_control\naps_frame_exposure=unavailable"
   <<"\naps_control_bus="<<o.bus<<"\naps_configured_bus="<<configuredBus<<"\naps_control_address="<<o.address<<"\nvts_readback="<<vts<<'\n';
 for(auto r:regs) report<<"aps_register_0x"<<std::hex<<r.first<<"=0x"<<unsigned(r.second)<<std::dec<<'\n';
 report<<"aps_register_verification="<<(regs.empty()?"not_requested":error.empty()?"matched_after_set_periodic_and_before_stop":"see_reason")<<'\n';
 statsOut(report,"aps",stats[1],seconds);statsOut(report,"evs",stats[0],seconds);
 std::cout<<report.str();
 if(o.diagnostic!="record") {
   std::ofstream f(o.output/"diagnostic.tmp");f<<report.str();checkedClose(f);
   fs::rename(o.output/"diagnostic.tmp",o.output/"diagnostic.txt");
   fs::remove(o.output/"recording.storage");
   std::cout<<"Diagnostic only: payload intentionally discarded; no complete recording marker\n";
 } else {
   for(;;) {
     try {save(o,arena,stats[1].fps(),report.str());break;}
     catch(const std::exception& e) {
       std::cerr<<"SAVE FAILED: "<<e.what()<<"\nAll input retained in this process. Repair destination, then: touch '"<<(o.output/"retry.request").string()<<"'\nDo NOT kill the process; pure RAM cannot survive power loss/SIGKILL.\n";
       // Signals do not discard the sole copy. A filesystem request retries
       // the complete save transaction, including files partly published.
       while(!fs::exists(o.output/"retry.request")) std::this_thread::sleep_for(std::chrono::milliseconds(200));
       fs::remove(o.output/"retry.request");
     }
   }
 }
#ifdef __linux__
 if(locked) munlock(arena.data(),arena.capacity());
 ::close(captureLock);
#endif
 return error.empty()?0:2;
}
int main(int argc,char** argv) {
 try {return run(parse(argc,argv));}catch(const std::exception& e) {std::cerr<<"ERROR: "<<e.what()<<'\n';return 1;}
}
