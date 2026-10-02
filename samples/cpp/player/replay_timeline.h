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
    double sensorUnitsPerHostUs=1;
    std::vector<uint64_t> hostElapsed;
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
        // Fit rate as well as origin. The board's nominal /200 codec clock
        // ran at ~0.8 units/host-us; offset-only mapping drifts by ~1 s in 5 s.
        const double x0=double(evsHost.front()),y0=double(packetEnds.front());
        double sx=0,sy=0,sxx=0,sxy=0;
        for(size_t i=0;i<evsHost.size();++i) {
            const double x=double(evsHost[i])-x0,y=double(packetEnds[i])-y0;
            sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;
        }
        const double n=double(evsHost.size()),den=n*sxx-sx*sx;
        sensorUnitsPerHostUs=den>0?(n*sxy-sx*sy)/den:1;
        if(!std::isfinite(sensorUnitsPerHostUs)||sensorUnitsPerHostUs<0.1||sensorUnitsPerHostUs>10)
            throw std::runtime_error("Invalid EVS/host clock rate fit");
        const double intercept=(sy-sensorUnitsPerHostUs*sx)/n;
        double lo=1e100,hi=-1e100;
        for(size_t i=0;i<evsHost.size();++i) {
            const double residual=double(packetEnds[i])-y0-intercept-sensorUnitsPerHostUs*(double(evsHost[i])-x0);
            lo=std::min(lo,residual);hi=std::max(hi,residual);
        }
        offsetUs=int64_t(x0-y0);offsetSpreadUs=uint64_t(std::llround((hi-lo)/sensorUnitsPerHostUs));
        hostElapsed.clear();
        for(auto h:apsHost) {
            const double t=y0+intercept+sensorUnitsPerHostUs*(double(h)-x0);
            if(t<0) throw std::runtime_error("Native clock bridge produced negative sensor time");
            aps.push_back(uint64_t(std::llround(t)));hostElapsed.push_back(h-apsHost.front());
        }
        host=true;return true;
    }
    uint64_t time(size_t index,double fps) const {
        return aps.empty()?uint64_t(double(index)*1e6/fps):aps[std::min(index,aps.size()-1)];
    }
    uint64_t elapsed(size_t index,double fps) const {return hostElapsed.empty()?time(index,fps)-time(0,fps):hostElapsed[std::min(index,hostElapsed.size()-1)];}
    size_t index(uint64_t elapsedUs,double fps) const {
        if(aps.empty()) return size_t(double(elapsedUs)*fps/1e6);
        const auto& times=hostElapsed.empty()?aps:hostElapsed;
        auto it=std::upper_bound(times.begin(),times.end(),times.front()+elapsedUs);
        return it==times.begin()?0:size_t(it-times.begin()-1);
    }
};
}
