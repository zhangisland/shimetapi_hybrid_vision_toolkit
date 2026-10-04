#include "../samples/cpp/hvs_record/memory_arena.h"
#include "../samples/cpp/hvs_record/apx_manual.h"
#include <cassert>
#include <map>
#include <thread>
#include <limits>
struct IO {
 std::map<uint16_t,uint8_t> regs{{0x015a,0xa0}};
 apx::Registers writes;
 int fail=-1;
 uint8_t read(uint16_t r) {return regs[r];}
 void write(uint16_t r,uint8_t v) {
   if(int(writes.size())==fail) throw std::runtime_error("injected I2C failure");
   writes.push_back({r,v});regs[r]=v;
 }
};
template<class F> void rejects(F f) {bool failed=false;try {f();}catch(const std::exception&) {failed=true;}assert(failed);}
int main() {
 assert(MemoryArena::payloadCapacity(64ULL<<20,320ULL<<20)==32ULL<<20);
 rejects([]{MemoryArena::payloadCapacity(64ULL<<20,(320ULL<<20)-1);});
 rejects([]{MemoryArena::payloadCapacity(1ULL<<20,UINT64_MAX);});
 // Capacity accounts for headers/alignment; copies own storage after DMA reuse.
 MemoryArena a(1024*1024);
 auto producer=[&](int stream) {
   uint8_t data[129];std::memset(data,stream+1,sizeof(data));
   for(int i=0;i<100;++i) {
     MemoryArena::Header h{};h.bytes=sizeof(data);h.stream=stream;h.frameId=i;
     assert(a.append(h,data));
   }
   std::memset(data,0,sizeof(data));
 };
 std::thread p(producer,0),q(producer,1);p.join();q.join();
 int counts[2]={};
 a.each([&](const auto& h,const uint8_t* bytes) {++counts[h.stream];for(size_t i=0;i<h.bytes;++i) assert(bytes[i]==h.stream+1);});
 assert(counts[0]==100&&counts[1]==100);
 assert(a.used()==200*MemoryArena::extent(129));
 MemoryArena small(MemoryArena::extent(2));uint8_t bytes[]={1,2};MemoryArena::Header h{};h.bytes=2;
 assert(small.append(h,bytes));assert(!small.append(h,bytes));
 rejects([&]{MemoryArena::extent(SIZE_MAX);});
 rejects([&]{small.append(h,nullptr);});
 assert(apx::gainIndex(0)==0&&apx::gainIndex(6)==16&&apx::gainIndex(24)==64);
 assert(apx::gainIndex(0.1875)==1);
 rejects([]{apx::gainIndex(24.01);});rejects([]{apx::gainIndex(-1);});
 rejects([]{apx::gainIndex(std::numeric_limits<double>::quiet_NaN());});
 assert(apx::exposureLines(1000,10)==100);
 rejects([]{apx::exposureLines(1000,0);});rejects([]{apx::exposureLines(20000,10);});
 IO io;rejects([&]{apx::apply(io,500,6);});assert(io.writes.empty());
 auto regs=apx::apply(io,0,6);
 assert(io.regs[0x3602]==0x80&&io.regs[0x3603]==7&&io.regs[0x3660]==1&&io.regs[0x3661]==1&&io.regs[0x3662]==0);
 assert(io.writes[io.writes.size()-3]==std::make_pair(uint16_t(0x342c),uint8_t(0)));
 assert(io.writes[io.writes.size()-2]==std::make_pair(uint16_t(0x342c),uint8_t(1)));
 assert(io.writes.back()==std::make_pair(uint16_t(0x342c),uint8_t(0)));
 for(auto r:io.writes) assert(r.first!=0x0157&&r.first!=0x0158);
 apx::verify(io,regs);io.regs[0x3602]=0;rejects([&]{apx::verify(io,regs);});
 IO bad;bad.fail=1;rejects([&]{apx::apply(bad,0,0);});
 assert(bad.writes.size()==1); // abort on first failure, never pretend success
 return 0;
}
