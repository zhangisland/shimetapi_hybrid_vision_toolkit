#pragma once
// Application-side APS ISP. No SDK ABI changes and no sensor register guesses.
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>
namespace hv_aps {
using Gains = std::array<double,3>; // RGB
struct Config {
    double black = 0; // uncalibrated default, in input DN (not an assumed sensor pedestal)
    double gamma = 2.2;
    Gains gains{1,1,1};
    std::string mode = "continuous";
    double minimum = 0.25, maximum = 4;
    void validate() const {
        if (!std::isfinite(black) || black<0 || black>=255 || !std::isfinite(gamma) || gamma<=0 || gamma>10 ||
            !std::isfinite(minimum) || !std::isfinite(maximum) || minimum<=0 || minimum>1 || maximum<1 || maximum>16)
            throw std::invalid_argument("Invalid ISP black/gamma/gain limits");
        if (mode!="off" && mode!="manual" && mode!="once" && mode!="continuous")
            throw std::invalid_argument("WB mode must be off/manual/once/continuous");
        for (double v:gains) if (!std::isfinite(v) || v<minimum || v>maximum)
            throw std::invalid_argument("Manual RGB gains outside configured limits");
    }
};
struct Estimate { Gains gains{1,1,1}; size_t samples=0; double confidence=0; bool valid=false; };
inline double median(std::vector<double> v) {
    auto mid=v.begin()+v.size()/2; std::nth_element(v.begin(),mid,v.end()); return *mid;
}
// Conservative near-neutral log-chromaticity estimator. Reject monochrome scenes,
// dark/clipped cells, and estimates with too little spatial/intensity support.
inline Estimate estimate(const std::vector<Gains>& cells, const Config& cfg) {
    Estimate e; std::vector<double> r,b,y;
    for (const auto& c:cells) {
        double lo=*std::min_element(c.begin(),c.end()), hi=*std::max_element(c.begin(),c.end());
        if (!std::isfinite(lo) || !std::isfinite(hi) || lo<0.03 || hi>0.96 || hi/lo>2.5) continue;
        r.push_back(std::log(c[1]/c[0])); b.push_back(std::log(c[1]/c[2])); y.push_back(c[1]);
    }
    e.samples=r.size();
    if (r.size()<64 || r.size()<cells.size()/20) return e;
    auto limits=std::minmax_element(y.begin(),y.end());
    if (*limits.second-*limits.first<0.04) return e; // uniform colored wall: abstain
    double mr=median(r), mb=median(b); std::vector<double> dr,db;
    for(size_t i=0;i<r.size();++i) { dr.push_back(std::abs(r[i]-mr)); db.push_back(std::abs(b[i]-mb)); }
    double spread=std::max(median(dr),median(db));
    // A textured surface with one chromaticity is still a monochrome scene.
    // Strong uniform illuminant and colored surface are indistinguishable without a reference: abstain.
    if(spread<0.015 && std::max(std::abs(mr),std::abs(mb))>0.12) return e;
    e.confidence=double(r.size())/std::max(size_t(1),cells.size())*std::max(0.0,1-spread/0.25);
    if(e.confidence<0.15) return e;
    e.gains={std::exp(mr),1,std::exp(mb)};
    for(double v:e.gains) if(v<cfg.minimum || v>cfg.maximum) return e;
    e.valid=true; return e;
}
struct WhiteBalance {
    Gains gains{1,1,1}; std::string mode="off", status="off";
    unsigned attempts=0, stable=0; Estimate last;
    void setMode(const std::string& next, const Config& c, bool preserve=true) {
        Config checked=c; checked.mode=next; checked.validate();
        if(!preserve) gains=c.gains;
        mode=next; attempts=stable=0; status=next;
    }
    void update(const Estimate& e) {
        last=e;
        if(mode!="once" && mode!="continuous") return;
        ++attempts;
        if(e.valid) {
            double delta=0;
            for(int i=0;i<3;++i) {
                delta=std::max(delta,std::abs(std::log(e.gains[i]/gains[i])));
                gains[i]=std::exp(0.8*std::log(gains[i])+0.2*std::log(e.gains[i]));
            }
            stable=delta<0.015?stable+1:0; status="estimating";
        } else { stable=0; status="held: insufficient/ambiguous samples"; }
        if(mode=="once" && (stable>=5 || attempts>=90)) {
            status=stable>=5?"once converged -> manual":"once timeout (90 fresh frames) -> manual, gains held";
            mode="manual";
        }
    }
    Gains applied() const { return mode=="off"?Gains{1,1,1}:gains; }
};
inline std::string capabilities() {
    return "Legacy Camera interface (native live has separate ISP controls): SetExposure ignores its argument; "
           "no exposure range/unit/readback; SetFrameRate/GetFrameRate are EVS-only. "
           "APS FPS, native RAW8 selection, analog/digital gain, hardware AE/AWB: no public control/readback. "
           "Software AE disabled. NV12: hardware ISP output; Gray8 VIN bypass: RAW10 >> 2, derived Bayer8. "
           "ADC precision/stride/frame exposure metadata unavailable; drop count unknown. "
           "Software WB only for explicit Bayer input; identity CCM and uncalibrated black=0.";
}
inline void rejectHardwareRequest(const std::string& key) {
    throw std::invalid_argument(key+": unavailable; "+capabilities());
}
}
