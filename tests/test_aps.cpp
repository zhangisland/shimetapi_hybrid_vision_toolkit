#include "../samples/cpp/player/aps_color.h"
#include <iostream>
#include <chrono>
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(0)
template<class F> void rejects(F f) { bool caught=false; try{f();}catch(const std::exception&){caught=true;} CHECK(caught); }
int main() {
 try {
    hv_aps::Config c; c.validate(); c.black=255; rejects([&]{c.validate();}); c.black=0;
    c.gains[0]=NAN; rejects([&]{c.validate();}); c.gains={1,1,1};
    rejects([]{hv_aps::rejectHardwareRequest("--aps-exposure-us");});
    std::vector<hv_aps::Gains> cells;
    for(int i=0;i<256;++i) { double y=.15+.5*i/256; cells.push_back({y*.7*(1+.08*std::sin(i)),y,y*.8*(1+.08*std::cos(i))}); }
    auto e=hv_aps::estimate(cells,c); CHECK(e.valid); CHECK(std::abs(e.gains[0]-1/.7)<.02);
    hv_aps::WhiteBalance wb; wb.setMode("once",c,false);
    for(int i=0;i<90;++i) wb.update(e);
    CHECK(wb.mode=="manual"); CHECK(std::abs(wb.gains[0]-1/.7)<.03);
    auto retained=wb.gains; wb.setMode("continuous",c); wb.update({}); CHECK(wb.gains==retained);
    wb.setMode("manual",c); CHECK(wb.gains==retained);
    wb.setMode("once",c); for(int i=0;i<90;++i) wb.update({}); CHECK(wb.mode=="manual"); CHECK(wb.gains==retained);
    CHECK(!hv_aps::estimate(std::vector<hv_aps::Gains>(128,{.3,.3,.3}),c).valid);
    CHECK(!hv_aps::estimate(std::vector<hv_aps::Gains>(128,{.8,.1,.1}),c).valid);
    CHECK(!hv_aps::estimate(std::vector<hv_aps::Gains>(128,{0,0,0}),c).valid);
    CHECK(!hv_aps::estimate(std::vector<hv_aps::Gains>(128,{1,1,1}),c).valid);
    for(const char* pattern:{"rggb","bggr","grbg","gbrg"}) {
        std::vector<uint8_t> bytes(32*32);
        const int values[]={80,120,160};
        for(int y=0;y<32;++y) for(int x=0;x<32;++x) bytes[y*32+x]=values[hv_player::channel(pattern,y,x)];
        auto original=bytes; Shimeta::Frame frame; frame.width=frame.height=32; frame.format=Shimeta::PixelFormat::Gray8;
        frame.aps={bytes.data(),bytes.size()};
        hv_player::ApsIsp isp; isp.pattern=pattern; isp.config.mode="off"; isp.config.gamma=1;
        auto output=isp.process(frame); auto p=output.at<cv::Vec3b>(16,16);
        CHECK(std::abs(p[0]-160)<=1 && std::abs(p[1]-120)<=1 && std::abs(p[2]-80)<=1); CHECK(bytes==original);
        isp.config.mode="manual"; isp.config.gains={4,1,1}; isp.geometry.clear(); isp.process(frame);
        CHECK(isp.linear.at<cv::Vec3f>(16,16)[2]>1.2f); CHECK(bytes==original);
        frame.aps.size--; rejects([&]{isp.process(frame);}); frame.aps.size++;
        std::vector<uint8_t> nv12(bytes); nv12.resize(32*32*3/2,128); frame.aps={nv12.data(),nv12.size()}; frame.format=Shimeta::PixelFormat::NV12;
        isp.process(frame); nv12.back()=127; rejects([&]{isp.process(frame);});
        isp.pattern="none"; CHECK(!isp.process(frame).empty()); CHECK(isp.raw.empty());
    }
    {
        std::vector<uint8_t> nv12(32*32*3/2,128); std::fill_n(nv12.data(),32*32,100);
        auto original=nv12; Shimeta::Frame f; f.width=f.height=32;f.format=Shimeta::PixelFormat::NV12;f.aps={nv12.data(),nv12.size()};
        hv_player::ApsIsp plain; auto baseline=plain.process(f).clone();
        hv_player::ApsIsp residual; residual.ispOutputCorrection=true;
        residual.config.black=200;residual.config.gamma=8;residual.config.mode="manual";residual.config.gains={2,1,1};
        auto corrected=residual.process(f).clone();
        auto b=baseline.at<cv::Vec3b>(16,16),c=corrected.at<cv::Vec3b>(16,16);
        CHECK(c[0]==b[0] && c[1]==b[1] && std::abs(int(c[2])-std::min(255,int(b[2])*2))<=1);
        residual.ispOutputCorrection=false;CHECK(cv::norm(baseline,residual.process(f),cv::NORM_INF)==0);
        CHECK(nv12==original);
    }
    std::vector<hv_aps::Gains> mono;
    for(int i=0;i<256;++i) {double y=.15+.4*i/256;mono.push_back({y*.7,y,y*.8});}
    CHECK(!hv_aps::estimate(mono,c).valid);
    // Old JSON is accepted; non-finite/out-of-range/manual artistic settings fail explicitly.
    std::ofstream cfg("legacy.json"); cfg<<R"({"wb_mode":"gray_world","wb_gains":[1,1,1],"black_level":16,"gamma":2.2})"; cfg.close();
    hv_player::ApsIsp isp; isp.load("legacy.json"); CHECK(isp.config.mode=="continuous" && isp.config.black==16);
    std::filesystem::remove("legacy.json");
    isp.pattern="gbrg";
    std::vector<uint8_t> bytes(32*32,100); Shimeta::Frame frame; frame.width=frame.height=32;
    frame.format=Shimeta::PixelFormat::Gray8; frame.aps={bytes.data(),bytes.size()}; isp.process(frame);
    const auto dir="diagnostic_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    isp.dump(dir); rejects([&]{isp.dump(dir);});
    std::ifstream raw(std::filesystem::path(dir)/"derived_bayer8.raw",std::ios::binary);
    std::vector<uint8_t> saved((std::istreambuf_iterator<char>(raw)),{}); CHECK(saved==bytes); raw.close();
    cv::FileStorage stages((std::filesystem::path(dir)/"stages.yml").string(),cv::FileStorage::READ);
    cv::Mat lin; stages["linear_bgr_identity_ccm"]>>lin; CHECK(lin.type()==CV_32FC3); stages.release();
    for(const char* file:{"derived_bayer8.raw","display.png","stages.yml","metadata.txt"}) std::filesystem::remove(std::filesystem::path(dir)/file);
    std::filesystem::remove(dir);
    std::cout<<"APS numerical/configuration/fake-frame tests passed\n";
    return 0;
 } catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 1; }
}
