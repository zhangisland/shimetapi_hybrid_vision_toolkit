#pragma once
#include "../common/aps_core.h"
#include <shimetapi/core/frame.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace hv_player {
inline bool validApsBayerMode(const std::string& m) {
    return m=="none" || m=="rggb" || m=="bggr" || m=="grbg" || m=="gbrg" || m=="compare";
}
inline int apsBayerCode(const std::string& m) {
    if(m=="rggb") return cv::COLOR_BayerBG2BGR;
    if(m=="bggr") return cv::COLOR_BayerRG2BGR;
    if(m=="grbg") return cv::COLOR_BayerGB2BGR;
    if(m=="gbrg") return cv::COLOR_BayerGR2BGR;
    throw std::invalid_argument("Invalid Bayer pattern: "+m);
}
inline int channel(const std::string& p,int y,int x) {
    static const int c[4][4]={{0,1,1,2},{2,1,1,0},{1,0,2,1},{1,2,0,1}};
    int n=p=="rggb"?0:p=="bggr"?1:p=="grbg"?2:3;
    return c[n][(y%2)*2+x%2];
}
struct ApsIsp {
    hv_aps::Config config;
    hv_aps::WhiteBalance wb;
    std::string pattern="none", geometry;
    cv::Mat raw,black,balanced,linear,display;
    bool ispOutputCorrection=false; // NV12 residual WB only; ISP owns CCM/gamma
    double saturated=0; uint64_t frames=0;
    ApsIsp() { wb.setMode(config.mode,config,false); }
    void load(const std::string& path) {
        cv::FileStorage f(path,cv::FileStorage::READ);
        if(!f.isOpened()) throw std::runtime_error("Cannot read ISP config: "+path);
        const std::vector<std::string> allowed={"schema_version","black_level","gamma","wb_mode","wb_gains","wb_gain_min","wb_gain_max","wb_patch","brightness","saturation","ccm"};
        for(auto it=f.root().begin();it!=f.root().end();++it)
            if(std::find(allowed.begin(),allowed.end(),(*it).name())==allowed.end())
                throw std::invalid_argument("Unknown ISP config key: "+(*it).name());
        if(!f["schema_version"].empty() && int(f["schema_version"])!=1) throw std::invalid_argument("Unsupported ISP config version");
        auto c=config;
        if(!f["black_level"].empty()) f["black_level"]>>c.black;
        if(!f["gamma"].empty()) f["gamma"]>>c.gamma;
        if(!f["wb_mode"].empty()) f["wb_mode"]>>c.mode;
        if(c.mode=="gray_world") c.mode="continuous"; // old tuner configuration
        if(!f["wb_gain_min"].empty()) f["wb_gain_min"]>>c.minimum;
        if(!f["wb_gain_max"].empty()) f["wb_gain_max"]>>c.maximum;
        if(!f["wb_gains"].empty()) {
            std::vector<double> v; f["wb_gains"]>>v;
            if(v.size()!=3) throw std::invalid_argument("wb_gains must contain RGB");
            std::copy(v.begin(),v.end(),c.gains.begin());
        }
        // Do not silently ignore legacy artistic/calibration adjustments.
        for(const char* key:{"brightness","saturation"}) if(!f[key].empty() && double(f[key])!=1)
            throw std::invalid_argument(std::string(key)+" is only supported by the offline tuner");
        if(!f["ccm"].empty()) {
            auto rows=f["ccm"];
            if(rows.size()!=3) throw std::invalid_argument("CCM must be 3x3");
            for(int y=0;y<3;++y) {
                if(rows[y].size()!=3) throw std::invalid_argument("CCM must be 3x3");
                for(int x=0;x<3;++x) if(double(rows[y][x])!=(x==y?1.:0.))
                    throw std::invalid_argument("Non-identity CCM requires offline calibrated processing");
            }
        }
        c.validate(); config=c; wb.setMode(c.mode,c,false);
    }
    void mode(const std::string& m) {
        if((pattern=="none" && !ispOutputCorrection) || pattern=="compare") { std::cerr<<"Software WB requires one explicit Bayer pattern; not applied"<<std::endl; return; }
        wb.setMode(m,config); config.mode=m;
        if(m=="manual") config.gains=wb.gains;
    }
    cv::Mat process(const Shimeta::Frame& f) {
        config.validate();
        if(!validApsBayerMode(pattern) || f.width<4 || f.height<4 || f.width%2 || f.height%2 || !f.aps.data)
            throw std::runtime_error("Invalid APS geometry/Bayer mode");
        size_t n=size_t(f.width)*size_t(f.height);
        bool gray=f.format==Shimeta::PixelFormat::Gray8;
        if((!gray && f.format!=Shimeta::PixelFormat::NV12) || f.aps.size!=(gray?n:n*3/2))
            throw std::runtime_error("APS requires exact packed Gray8/NV12; stride/padding unknown");
        std::string next=std::to_string(f.width)+"x"+std::to_string(f.height)+(gray?" Gray8":" NV12")+pattern;
        if(next!=geometry) { geometry=next; wb.setMode(config.mode,config,false); raw.release(); }
        cv::Mat y(f.height,f.width,CV_8UC1,const_cast<uint8_t*>(f.aps.data));
        if(pattern=="none") {
            raw.release(); black.release(); balanced.release(); linear.release();
            if(gray) cv::cvtColor(y,display,cv::COLOR_GRAY2BGR);
            else {
                cv::Mat uv(f.height/2,f.width/2,CV_8UC2,const_cast<uint8_t*>(f.aps.data+n));
                cv::cvtColorTwoPlane(y,uv,display,cv::COLOR_YUV2BGR_NV12);
            }
            if(ispOutputCorrection) {
                if(gray) throw std::runtime_error("ISP residual correction requires NV12");
                // ISP output is encoded BGR8, not linear RAW. Reuse the existing
                // conservative RGB estimator; do not apply RAW black/CCM/gamma.
                std::vector<hv_aps::Gains> cells;
                const int step=std::max(1,f.width/128);
                for(int r=0;r<display.rows;r+=step) for(int c=0;c<display.cols;c+=step) {
                    const auto v=display.at<cv::Vec3b>(r,c);
                    cells.push_back({v[2]/255.,v[1]/255.,v[0]/255.});
                }
                wb.update(hv_aps::estimate(cells,config));
                const auto gains=wb.applied();
                cv::Mat lut(1,256,CV_8UC3);
                for(int i=0;i<256;++i) for(int c=0;c<3;++c)
                    lut.at<cv::Vec3b>(0,i)[c]=cv::saturate_cast<uint8_t>(i*gains[2-c]);
                cv::LUT(display,lut,display);
                ++frames;
            }
            return display;
        }
        if(!gray && !std::all_of(f.aps.data+n,f.aps.data+n*3/2,[](uint8_t v){return v==128;}))
            throw std::runtime_error("Bayer requires preserved Y samples with neutral UV; hardware ISP output rejected");
        if(pattern=="compare") {
            cv::Mat result=cv::Mat::zeros(f.height,f.width,CV_8UC3);
            const char* patterns[]={"rggb","bggr","grbg","gbrg"};
            for(int i=0;i<4;++i) {
                ApsIsp candidate; candidate.config=config; candidate.config.mode="off"; candidate.pattern=patterns[i];
                cv::Mat tile; cv::resize(candidate.process(f),tile,cv::Size(f.width/2,f.height/2));
                cv::putText(tile,patterns[i],cv::Point(8,24),cv::FONT_HERSHEY_SIMPLEX,.6,cv::Scalar(255,255,255),1);
                tile.copyTo(result(cv::Rect(i%2*f.width/2,i/2*f.height/2,f.width/2,f.height/2)));
            }
            raw.release(); display=result; return display;
        }
        raw=y.clone(); // never mutate pool/recorded bytes
        raw.convertTo(black,CV_32F,1/(255-config.black),-config.black/(255-config.black));
        cv::max(black,0,black);
        std::vector<hv_aps::Gains> cells;
        int step=std::max(2,(f.width/128/2)*2);
        for(int r=0;r<f.height-1;r+=step) for(int c=0;c<f.width-1;c+=step) {
            hv_aps::Gains v{0,0,0}; bool clipped=false;
            for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx) {
                int ch=channel(pattern,r+dy,c+dx);
                float sample=black.at<float>(r+dy,c+dx);
                clipped=clipped || sample>.96 || sample<.03;
                v[ch]+=sample*(ch==1?.5:1);
            }
            if(clipped) v={0,0,0};
            cells.push_back(v);
        }
        wb.update(hv_aps::estimate(cells,config));
        auto gains=wb.applied(); balanced=black.clone();
        for(int r=0;r<f.height;++r) for(int c=0;c<f.width;++c)
            balanced.at<float>(r,c)*=float(gains[channel(pattern,r,c)]);
        // Headroom scaling preserves highlights through the uint16 demosaicer.
        double headroom=std::max(1.,*std::max_element(gains.begin(),gains.end()));
        cv::Mat b16,bgr16; balanced.convertTo(b16,CV_16U,65535/headroom);
        cv::demosaicing(b16,bgr16,apsBayerCode(pattern));
        bgr16.convertTo(linear,CV_32F,headroom/65535); // BGR, identity CCM (uncalibrated)
        saturated=double(cv::countNonZero(raw>=250))/n;
        cv::Mat encoded; cv::max(linear,0,encoded); cv::min(encoded,1,encoded);
        cv::pow(encoded,1/config.gamma,encoded); encoded.convertTo(display,CV_8UC3,255);
        ++frames; return display;
    }
    std::string status() const {
        if((pattern=="none" && !ispOutputCorrection) || pattern=="compare") return geometry+" SW-WB not applied; exposure/gain/device FPS unknown";
        auto g=wb.applied();
        return geometry+" SW-WB="+wb.mode+" RGB="+std::to_string(g[0])+","+std::to_string(g[1])+","+std::to_string(g[2])+
            " samples="+std::to_string(wb.last.samples)+" confidence="+std::to_string(wb.last.confidence)+
            " sat="+std::to_string(saturated)+" "+wb.status;
    }
    void dump(const std::string& directory) const {
        if(raw.empty()) throw std::runtime_error("Diagnostics require one explicit Bayer pattern and a processed frame");
        if(!std::filesystem::create_directory(directory)) throw std::runtime_error("Diagnostic directory must be new");
        auto path=[&](const char* s){return (std::filesystem::path(directory)/s).string();};
        std::ofstream out(path("derived_bayer8.raw"),std::ios::binary);
        out.write(reinterpret_cast<const char*>(raw.data),std::streamsize(raw.total())); out.close();
        if(!out || !cv::imwrite(path("display.png"),display)) throw std::runtime_error("Diagnostic image write failed");
        cv::FileStorage f(path("stages.yml"),cv::FileStorage::WRITE);
        if(!f.isOpened()) throw std::runtime_error("Diagnostic write failed");
        f<<"black_corrected_bayer"<<black<<"white_balanced_bayer"<<balanced<<"linear_bgr_identity_ccm"<<linear;
        std::ofstream report(path("metadata.txt"));
        report<<status()<<"\nrequested_WB="<<config.mode<<" black_DN="<<config.black<<" gamma="<<config.gamma<<"\ninput_bits=8; sensor_ADC_bits=unknown; VIN_RAW10_transport_reduced_before_application\n"
              <<"exposure/gain/device_fps=unknown; not frame metadata\n"<<hv_aps::capabilities()<<'\n';
        report<<"histogram_DN,count\n";
        std::array<size_t,256> hist{};
        for(size_t i=0;i<raw.total();++i) ++hist[raw.data[i]];
        for(int i=0;i<256;++i) report<<i<<","<<hist[i]<<"\n";
        report.close(); if(!report) throw std::runtime_error("Diagnostic report write failed");
    }
};
// Options shared by live preview and playback. Unsupported hardware requests fail before Init.
inline bool apsOption(const std::string& key,int& i,int argc,char** argv,ApsIsp& isp) {
    if(key=="--aps-capabilities") { std::cout<<hv_aps::capabilities()<<std::endl; return true; }
    for(const char* k:{"--aps-exposure-us","--aps-fps","--aps-format","--aps-gain","--aps-ae","--aps-hardware-wb"})
        if(key==k) hv_aps::rejectHardwareRequest(key);
    if(key!="--aps-bayer" && key!="--aps-config" && key!="--aps-wb") return false;
    if(++i>=argc) throw std::invalid_argument("Missing value: "+key);
    if(key=="--aps-bayer") { isp.pattern=argv[i]; if(!validApsBayerMode(isp.pattern)) throw std::invalid_argument("Invalid Bayer mode"); }
    if(key=="--aps-config") isp.load(argv[i]);
    if(key=="--aps-wb") { isp.config.mode=argv[i]; isp.config.validate(); isp.wb.setMode(isp.config.mode,isp.config,false); }
    return true;
}
}
