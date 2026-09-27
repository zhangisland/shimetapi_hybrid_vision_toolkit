#pragma once
#include <filesystem>
#include <fstream>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <cstdint>
#include <cstring>

// Update only AVI timing fields after the SDK has closed its writer. Walk RIFF
// chunks, never search payload bytes for header names. Reject unsupported layouts.
inline void setAviFrameRate(const std::filesystem::path& path, double fps) {
    if (!std::isfinite(fps) || fps < 0.001 || fps > 10000)
        throw std::runtime_error("Invalid observed AVI frame rate");
    std::fstream f(path, std::ios::in|std::ios::out|std::ios::binary);
    auto read32=[&](uint64_t at) { unsigned char b[4]; f.seekg(at); f.read(reinterpret_cast<char*>(b),4);
        if (!f) throw std::runtime_error("Truncated AVI header");
        return uint32_t(b[0]) | uint32_t(b[1])<<8 | uint32_t(b[2])<<16 | uint32_t(b[3])<<24; };
    auto tag=[&](uint64_t at,const char* expected) { char b[4]; f.seekg(at); f.read(b,4);
        if (!f) throw std::runtime_error("Truncated AVI tag"); return std::memcmp(b,expected,4)==0; };
    if (!f || !tag(0,"RIFF") || !tag(8,"AVI ")) throw std::runtime_error("Not a RIFF AVI");
    uint64_t size=std::filesystem::file_size(path);
    std::vector<std::pair<uint64_t,uint32_t>> patches;
    unsigned avih=0, video=0;
    auto walk=[&](auto&& self,uint64_t start,uint64_t end,int depth)->void {
        if (depth>3) throw std::runtime_error("Invalid AVI nesting");
        for(uint64_t pos=start;pos+8<=end;) {
            uint64_t n=read32(pos+4), next=pos+8+n+(n&1);
            if(pos+8+n>end || next>end) throw std::runtime_error("Invalid AVI chunk size");
            if(tag(pos,"LIST") && n>=4 && (tag(pos+8,"hdrl") || tag(pos+8,"strl")))
                self(self,pos+12,pos+8+n,depth+1);
            else if(tag(pos,"avih") && n>=56) { ++avih; patches.push_back({pos+8,uint32_t(std::llround(1e6/fps))}); }
            else if(tag(pos,"strh") && n>=56 && tag(pos+8,"vids")) {
                ++video; patches.push_back({pos+8+20,100000});
                patches.push_back({pos+8+24,uint32_t(std::llround(fps*100000))});
            }
            pos=next;
        }
    };
    if(uint64_t(read32(4))+8 != size) throw std::runtime_error("Unfinalized AVI size");
    walk(walk,12,size,0);
    if(avih!=1 || video!=1) throw std::runtime_error("Expected one AVI video stream");
    for(auto [at,value]:patches) {
        unsigned char b[4]={uint8_t(value),uint8_t(value>>8),uint8_t(value>>16),uint8_t(value>>24)};
        f.seekp(at); f.write(reinterpret_cast<char*>(b),4);
    }
    f.flush(); if(!f) throw std::runtime_error("AVI timing update failed");
}
