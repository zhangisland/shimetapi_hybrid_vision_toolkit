#pragma once
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <utility>
#include <sstream>

namespace apx {
// Identity is checked by vp_sensor_fixed_mipi_host BEFORE sensor init.
// 0x3428 changes after init on the tested X5; do not assert 0x0808 in-stream.
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
   // APS coarse integration time in lines (big-endian u16: 0x3503=hi, 0x3504=lo).
   // Factory 0x0410=1040. Init-time override 1040->920 darkens raw_mean ~12% linearly
   // (probe_exposure_init_sweep.py, 2026-10-04). Fewer lines = shorter exposure =
   // darker, which fixes bright-field overexposure at gain 0. Runtime (post-open)
   // write retention is unverified on the board: readback below confirms the value
   // holds, and brightness must still be compared against the factory 1040 frame.
   regs.insert(regs.end(),{{0x3503,uint8_t((lines>>8)&0xFF)},{0x3504,uint8_t(lines&0xFF)}});
 }
 if(gain>=0) {
   const int i=gainIndex(gain);
   regs.insert(regs.end(),{{0x3603,7},{0x3660,1},{0x3602,analog[i]}, {0x3661,1},{0x3662,0}});
 }
 for(auto r:regs) io.write(r.first,r.second);
 if(gain>=0) for(auto v:{0,1,0}) io.write(0x342c,uint8_t(v)); // gain latch only
 for(auto r:regs) {const auto actual=io.read(r.first);if(actual!=r.second) {
   std::ostringstream msg;msg<<"APS register readback mismatch: reg=0x"<<std::hex<<r.first<<" requested=0x"<<unsigned(r.second)<<" read=0x"<<unsigned(actual);
   if(r.first==0x3503||r.first==0x3504) msg<<"; integration-time write did not retain after stream-open (may be init-latched)";
   throw std::runtime_error(msg.str());
 }}
 return regs;
}
template<class IO> void verify(IO& io,const Registers& regs) {
 for(auto r:regs) {const auto actual=io.read(r.first);if(actual!=r.second) {
  std::ostringstream msg;msg<<"APS settings overwritten during capture: reg=0x"<<std::hex<<r.first<<" expected=0x"<<unsigned(r.second)<<" read=0x"<<unsigned(actual);
  throw std::runtime_error(msg.str());
 }}
}
}
