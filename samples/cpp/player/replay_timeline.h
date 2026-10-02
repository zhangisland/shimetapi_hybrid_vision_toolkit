#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace hv_player {
// The native recorder has no paired sensor timestamps. Host receive times are
// an approximate bridge, NOT exposure timestamps. Use one robust offset rather
// than bending the sensor timeline independently for every arriving packet.
struct ReplayTimeline {
    std::vector<uint64_t> aps;
    bool host=false;
    int64_t offsetUs=0;
    uint64_t offsetSpreadUs=0;
    static uint64_t field(const std::string& line,const std::string& key) {
        std::smatch m;
        if(!std::regex_search(line,m,std::regex("\""+key+"\"\\s*:\\s*([0-9]+)")))
            throw std::runtime_error("Missing native replay metadata field: "+key);
        return std::stoull(m[1]);
    }
    bool load(const std::string& avi,const std::vector<uint64_t>& packetEnds,size_t count) {
        aps.clear();host=false;
        std::ifstream in(std::filesystem::path(avi).parent_path()/"vin.frames.jsonl");
        if(!in) return false;
        std::vector<uint64_t> apsHost,evsHost;
        std::string line;
        const std::regex apsTag("\"stream\"\\s*:\\s*\"aps\"");
        const std::regex evsTag("\"stream\"\\s*:\\s*\"evs\"");
        while(std::getline(in,line)) {
            if(std::regex_search(line,apsTag)) apsHost.push_back(field(line,"host_ns")/1000);
            else if(std::regex_search(line,evsTag)) evsHost.push_back(field(line,"host_ns")/1000);
        }
        if(apsHost.size()!=count||evsHost.size()!=packetEnds.size()||apsHost.empty()||evsHost.empty())
            throw std::runtime_error("Native replay metadata count mismatch; keep aps.avi, events.raw and vin.frames.jsonl from the SAME session");
        if(!std::is_sorted(apsHost.begin(),apsHost.end())||!std::is_sorted(evsHost.begin(),evsHost.end()))
            throw std::runtime_error("Nonmonotonic native host timestamps");
        std::vector<int64_t> offsets;
        for(size_t i=0;i<evsHost.size();++i) offsets.push_back(int64_t(evsHost[i])-int64_t(packetEnds[i]));
        std::sort(offsets.begin(),offsets.end());offsetUs=offsets[offsets.size()/2];
        offsetSpreadUs=uint64_t(offsets.back()-offsets.front());
        for(auto h:apsHost) {
            const int64_t t=int64_t(h)-offsetUs;
            if(t<0) throw std::runtime_error("Native clock bridge produced negative sensor time");
            aps.push_back(uint64_t(t));
        }
        host=true;return true;
    }
    uint64_t time(size_t index,double fps) const {
        return aps.empty()?uint64_t(double(index)*1e6/fps):aps[std::min(index,aps.size()-1)];
    }
    uint64_t elapsed(size_t index,double fps) const {return time(index,fps)-time(0,fps);}
    size_t index(uint64_t elapsedUs,double fps) const {
        if(aps.empty()) return size_t(double(elapsedUs)*fps/1e6);
        auto it=std::upper_bound(aps.begin(),aps.end(),aps.front()+elapsedUs);
        return it==aps.begin()?0:size_t(it-aps.begin()-1);
    }
};
}
