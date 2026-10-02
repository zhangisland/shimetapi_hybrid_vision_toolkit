#pragma once
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <utility>
#include <sstream>

namespace apx {
// Match vp_sensors.c: reg16/data16 is ONE repeated-start read, not two
// independent reg16/data8 accesses. Exposure registers still use data8.
template<class IO> uint16_t verifyIdentity(IO& io, int bus, int address) {
 uint8_t bytes[2]{};
 io.readBytes(0x3428,bytes,2);
 const auto id=uint16_t((uint16_t(bytes[0])<<8)|bytes[1]);
 if(id!=0x0808) {
   std::ostringstream msg;
   msg<<"APX003CC chip identity mismatch: bus="<<bus<<" addr=0x"<<std::hex<<address
      <<" reg=0x3428 reg16/data16 read=0x"<<id<<" expected=0x0808; no exposure writes performed";
   throw std::runtime_error(msg.str());
 }
 return id;
}
// The supplied guide contains 146 rows, not 161. Restrict to the internally
// consistent analog segment 0..64; do not invent the contradictory digital LUT.
inline constexpr uint8_t analog[] = {
 255,244,234,224,215,205,197,188,181,173,166,159,152,145,139,133,
 128,122,117,112,108,103,99,94,90,87,83,79,76,73,70,67,
 64,61,59,56,54,52,49,47,45,43,42,40,38,37,35,34,
 32,31,29,28,27,26,25,24,23,22,21,20,20,19,18,17,16};
inline int gainIndex(double db) {
 if(!std::isfinite(db)||db<0||db>24) throw std::runtime_error("APS gain must be 0..24 dB (analog only)");
 return int(std::floor(db/0.375+0.5));
}
inline int exposureLines(double us, double lineUs) {
 if(!std::isfinite(us)||!std::isfinite(lineUs)||us<=0||lineUs<=0)
   throw std::runtime_error("microseconds require a verified --aps-line-time-us; VTS/fps alone is not a timing measurement");
 const double n=std::floor(us/lineUs+0.5);
 if(n<1||n>1162) throw std::runtime_error("exposure quantizes outside 1..1162 lines");
 return int(n);
}
using Registers=std::vector<std::pair<uint16_t,uint8_t>>;
template<class IO> Registers apply(IO& io,int lines,double gain) {
 Registers regs;
 if(lines) {
   if(lines<1||lines>1162) throw std::runtime_error("APS exposure outside 1..1162 lines");
   // Preserve unrelated high nibble.
   regs.push_back({0x015a,uint8_t((io.read(0x015a)&0xf0)|((lines>>8)&15))});
   regs.push_back({0x015b,uint8_t(lines)});
 }
 if(gain>=0) {
   const int i=gainIndex(gain);
   regs.insert(regs.end(),{{0x3603,7},{0x3660,1},{0x3602,analog[i]}, {0x3661,1},{0x3662,0}});
 }
 for(auto r:regs) io.write(r.first,r.second);
 if(!regs.empty()) for(auto v:{0,1,0}) io.write(0x342c,uint8_t(v));
 for(auto r:regs) if(io.read(r.first)!=r.second) throw std::runtime_error("APS register readback mismatch");
 return regs;
}
template<class IO> void verify(IO& io,const Registers& regs) {
 for(auto r:regs) if(io.read(r.first)!=r.second) throw std::runtime_error("APS settings overwritten during capture");
}
}
