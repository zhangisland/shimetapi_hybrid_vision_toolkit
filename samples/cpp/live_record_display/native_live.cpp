#include "x5_capture.h"
#include "frame_sync.h"
#include "native_record.h"
#include "live_widgets.h"
#include "../player/aps_color.h"
#include <shimetapi/codec/mipi_raw8_codec.h>
#include <atomic>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <sys/select.h>
#include <unistd.h>
#endif
namespace {
volatile std::sig_atomic_t interrupted=0;
void signalStop(int) {interrupted=1;}
struct Options {
    double exposure=5100,again=1,dgain=1,toleranceMs=25,waitMs=40,seconds=0;
    int profile=1; bool noDisplay=false,record=false,correction=true;
    std::string output="live_native";
};
double number(const char* value) {
    size_t used=0; std::string s=value; double n=std::stod(s,&used);
    if(used!=s.size() || !std::isfinite(n)) throw std::invalid_argument("Invalid numeric value: "+s);
    return n;
}
uint64_t logicalFrames(const Shimeta::Frame& f) {
    uint64_t count=0;
    // Same four spatial subframes per logical EVS frame as the official sample.
    for(size_t group=0;group+4*32768<=f.evs.size;group+=4*32768) {
        unsigned mask=0;
        for(size_t sub=0;sub<4;++sub) {
            uint32_t header; uint64_t metadata;
            std::memcpy(&header,f.evs.data+group+sub*32768,4);
            std::memcpy(&metadata,f.evs.data+group+sub*32768+8,8);
            const unsigned index=unsigned((metadata>>44)&15);
            if((header&0xFFFFFF)==0xFFFF && index<4) mask|=1u<<index;
        }
        if(mask==15) ++count;
    }
    return count;
}
Shimeta::Frame owned(const x5_image& im,bool aps) {
    if(im.width<=0 || im.height<=0 || im.width>8192 || im.height>8192 || im.stride<im.width ||
       !im.plane[0] || size_t(im.stride)*im.height>im.size[0]) throw std::runtime_error("Invalid SDK plane/stride");
    if(aps && ((im.width%2)||(im.height%2)||!im.plane[1] || size_t(im.stride)*(im.height/2)>im.size[1]))
        throw std::runtime_error("Invalid ISP NV12 UV plane/stride");
    const size_t pixels=size_t(im.width)*im.height, n=aps?pixels*3/2:pixels;
    auto memory=std::shared_ptr<uint8_t[]>(new uint8_t[n],std::default_delete<uint8_t[]>());
    for(int y=0;y<im.height;++y) std::memcpy(memory.get()+size_t(y)*im.width,im.plane[0]+size_t(y)*im.stride,im.width);
    if(aps) for(int y=0;y<im.height/2;++y) std::memcpy(memory.get()+pixels+size_t(y)*im.width,im.plane[1]+size_t(y)*im.stride,im.width);
    Shimeta::Frame f; f.width=im.width; f.height=im.height; f.frame_id=im.frame_id;
    f.format=aps?Shimeta::PixelFormat::NV12:Shimeta::PixelFormat::Gray8;
    if(aps) {f.aps_owner=memory; f.aps={memory.get(),n};}
    else {f.evs_owner=memory; f.evs={memory.get(),n};}
    return f;
}
struct CaptureStats {
    std::atomic<uint64_t> frames{0},logical{0},errors{0};
    std::atomic<int64_t> getNs{0},copyNs{0},callbackNs{0},callbackMaxNs{0},lastNs{0};
};
}
int runNativeLive(int argc,char** argv) {
    using namespace hv_live;
    Options o; hv_player::ApsIsp color; color.ispOutputCorrection=true;
    for(int i=1;i<argc;++i) {
        std::string k=argv[i];
        if(k=="--help" || k=="-h") {
            std::cout<<"Native X5 EVS VC0 + APS VC1 offline ISP\n"
                <<"--profile 0..5 --aps-exposure-us 5100 --aps-gain 1 --aps-dgain 1 --aps-ae manual\n"
                <<"Exposure/gains are initialization-only; API/readback are not frame measurements.\n"
                <<"--sync-tolerance-ms 25 --sync-wait-ms 40 --aps-correction on|off --aps-wb off|manual|once|continuous\n"
                <<"--aps-config JSON (existing WB fields; RAW black/gamma not reapplied to ISP output)\n"
                <<"--no-display --seconds 10 --record --output NEW_SESSION_PREFIX\n"
                <<"Keys: q/ESC quit, r record, k correction on/off, w/c/m/o WB modes\n"
                <<"Recording: uncorrected ISP NV12 AVI + EVS raw; host average-rate timeline + per-frame CSV.\n";
            return 0;
        }
        if(k=="--no-display") {o.noDisplay=true;continue;}
        if(k=="--record") {o.record=true;continue;}
        if(k=="--aps-capabilities") {
            std::cout<<"Native ISP manual exposure (us converted to seconds), analog/digital gains; initialization only. "
                     <<"Attribute readback separate from frame measurement. HVS auto AE disabled per sample.\n"; return 0;
        }
        if(i+1>=argc) throw std::invalid_argument("Missing value: "+k);
        std::string v=argv[++i];
        if(k=="--output") o.output=v;
        else if(k=="--aps-wb") {color.config.mode=v; color.config.validate(); color.wb.setMode(v,color.config,false);}
        else if(k=="--aps-config") color.load(v);
        else if(k=="--aps-ae") {if(v!="manual") throw std::invalid_argument("HVS auto AE not enabled: official sensor_mode=2 path does not converge");}
        else if(k=="--aps-correction") {if(v!="on" && v!="off") throw std::invalid_argument("correction on|off");o.correction=v=="on";}
        else if(k=="--aps-bayer") {if(v!="none") throw std::invalid_argument("Native ISP is NV12, never Bayer");}
        else if(k=="--aps-exposure-us") o.exposure=number(v.c_str());
        else if(k=="--aps-gain") o.again=number(v.c_str());
        else if(k=="--aps-dgain") o.dgain=number(v.c_str());
        else if(k=="--sync-tolerance-ms") o.toleranceMs=number(v.c_str());
        else if(k=="--sync-wait-ms") o.waitMs=number(v.c_str());
        else if(k=="--seconds") o.seconds=number(v.c_str());
        else if(k=="--profile") {double n=number(v.c_str());if(n<0||n>5||n!=std::floor(n)) throw std::invalid_argument("profile 0..5");o.profile=int(n);}
        else throw std::invalid_argument("Unknown native option: "+k);
    }
    if(o.exposure<=0||o.again<=0||o.dgain<=0||o.seconds<0 || o.toleranceMs<0||o.toleranceMs>1000||o.waitMs<0||o.waitMs>1000)
        throw std::invalid_argument("Require positive exposure/gains; sync bounds 0..1000ms; seconds>=0");
    std::cout<<"HOST approximate pairing, not hardware sync. EVS packet receive times; ISP latency and packet duration unknown.\n"
             <<"ISP: demosaic/CCM/gamma, neutral manual AWB. Application: existing residual WB only.\n"
             <<"Exposure REQUEST us="<<o.exposure<<" again="<<o.again<<" dgain="<<o.dgain<<" (initialization only)\n";
    x5_capture* backend=nullptr;
    int ret=x5_open(&backend,o.profile,o.exposure,o.again,o.dgain);
    if(ret || !backend) throw std::runtime_error("Native pipeline initialization failed: "+std::to_string(ret));
    struct BackendGuard {x5_capture* p; ~BackendGuard(){x5_close(p);}} backendGuard{backend};
    FrameSync<Shimeta::Frame> sync(int64_t(o.toleranceMs*1e6),int64_t(o.waitMs*1e6));
    NativeRecorder recorder;
    if(o.record) recorder.start(o.output);
    std::atomic<bool> stop{false}; CaptureStats stats[2];
    std::mutex errorMutex; std::string captureError;
    auto capture=[&](int aps) {
        auto& st=stats[aps]; bool first=true;
        try {
            while(!stop) {
                x5_image image{}; auto before=hostNs(); int code=x5_get(backend,aps,&image); const auto received=hostNs();
                st.getNs+=received-before;
                if(code==X5_CAPTURE_FATAL) throw std::runtime_error("Native capture buffer validation/cache/allocation failed");
                if(code) {++st.errors; std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue;}
                struct Lease {x5_capture* p;int aps;x5_image& im;~Lease(){if(im.lease) {int r=x5_release(p,aps,&im);if(r) std::cerr<<"SDK release failed: "<<r<<'\n';}}} lease{backend,aps,image};
                if(first) {
                    std::cout<<(aps?"APS ISP NV12":"EVS VIN RAW8")<<" "<<image.width<<'x'<<image.height
                        <<" stride="<<image.stride<<" plane_bytes="<<image.size[0]<<','<<image.size[1]<<'\n'; first=false;
                }
                auto f=owned(image,aps); st.copyNs+=hostNs()-received;
                if((code=x5_release(backend,aps,&image))!=0) throw std::runtime_error("releaseframe failed: "+std::to_string(code));
                ++st.frames; st.logical+=aps?1:logicalFrames(f); st.lastNs=received;
                const auto callbackStart=hostNs();
                Timed<Shimeta::Frame> item{received,std::move(f)};
                recorder.push(item); sync.push(aps,std::move(item));
                const auto work=hostNs()-callbackStart; st.callbackNs+=work;
                if(work>st.callbackMaxNs.load()) st.callbackMaxNs=work;
            }
        } catch(const std::exception& e) {std::lock_guard<std::mutex> l(errorMutex);captureError=e.what();stop=true;}
    };
    std::thread evs,aps;
    struct Join {std::atomic<bool>& stop;std::thread &e,&a;~Join(){stop=true;if(e.joinable())e.join();if(a.joinable())a.join();}} join{stop,evs,aps};
    evs=std::thread(capture,0); aps=std::thread(capture,1);
    interrupted=0; std::signal(SIGINT,signalStop);std::signal(SIGTERM,signalStop);
    if(!o.noDisplay) cv::namedWindow(kWindowName,cv::WINDOW_NORMAL);
    struct WindowGuard {bool active;~WindowGuard(){if(active)cv::destroyAllWindows();}} windowGuard{!o.noDisplay};
    Shimeta::codec::MipiRaw8Decoder decoder;
    const auto begin=hostNs(); auto report=begin; int64_t displayedNs=0, correctionNs=0, displayNs=0,decodeNs=0;
    int64_t lastUi=0;
    uint64_t displayed=0,processed=0,previous[2]={0,0},previousLogical=0;
    cv::Mat lastCombined; std::string pairStatus="Waiting APS / EVS";
    while(!stop && !interrupted && !(o.seconds>0 && (hostNs()-begin)/1e9>=o.seconds)) {
        if(auto pair=sync.poll(hostNs())) {
            auto t=hostNs(); color.ispOutputCorrection=o.correction;
            auto apsImage=color.process(pair->aps.value); correctionNs+=hostNs()-t; ++processed;
            cv::Mat evsImage=cv::Mat::zeros(kDefaultEvsHeight,kDefaultEvsWidth,CV_8UC3);
            pairStatus="UNMATCHED: no EVS within tolerance";
            if(pair->evs) {
                t=hostNs(); std::vector<Shimeta::EventCD> events; const auto& f=pair->evs->value;
                decoder.Decode(f.evs.data,f.evs.size,events);
                EvsVisualizer packet; packet.addEvents(events); evsImage=packet.getFrame();
                decodeNs+=hostNs()-t;
                pairStatus="HOST packet pair delta="+std::to_string(pair->deltaNs/1e6)+"ms";
            }
            displayedNs=pair->aps.ns;
            t=hostNs(); cv::Mat small; cv::resize(apsImage,small,cv::Size(kDefaultEvsWidth,kDefaultEvsHeight));
            cv::hconcat(evsImage,small,lastCombined); displayNs+=hostNs()-t;
        }
        int key=-1;
        if(!o.noDisplay) {
            auto t=hostNs();
            if(!lastCombined.empty() && hostNs()-lastUi>=33000000) {
                lastUi=hostNs();
                auto canvas=lastCombined.clone();
                bool stale=hostNs()-displayedNs>500000000;
                const std::string label=stale?"STALE APS (>500ms), not synchronized":pairStatus;
                cv::putText(canvas,label,{10,24},cv::FONT_HERSHEY_SIMPLEX,.6,stale?cv::Scalar(0,0,255):cv::Scalar(0,255,255),1);
                cv::putText(canvas,std::string("Correction ")+(o.correction?"ON":"OFF")+" | "+recorder.status(),{10,50},cv::FONT_HERSHEY_SIMPLEX,.6,cv::Scalar(0,255,255),1);
                cv::imshow(kWindowName,canvas); ++displayed;
            }
            key=cv::waitKey(1)&255; displayNs+=hostNs()-t;
        } else {
#ifndef _WIN32
            fd_set fds;FD_ZERO(&fds);FD_SET(STDIN_FILENO,&fds);timeval tv{0,0};
            if(select(STDIN_FILENO+1,&fds,nullptr,nullptr,&tv)>0) {char ch;if(read(STDIN_FILENO,&ch,1)==1)key=ch;}
#endif
        }
        if(key=='q'||key==27) stop=true;
        else if(key=='r') {if(recorder.recording())recorder.stop();else recorder.start(o.output);}
        else if(key=='k') o.correction=!o.correction;
        else if(key=='w'||key=='c'||key=='m'||key=='o') {
            color.ispOutputCorrection=true; color.mode(key=='w'?"once":key=='c'?"continuous":key=='m'?"manual":"off");
        }
        const auto now=hostNs();
        if(now-report>=1000000000) {
            auto ss=sync.stats(); double elapsed=(now-report)/1e9;
            std::cout<<"FPS EVS packets="<<(stats[0].frames.load()-previous[0])/elapsed
                <<" logical="<<(stats[0].logical.load()-previousLogical)/elapsed
                <<" APS="<<(stats[1].frames.load()-previous[1])/elapsed
                <<" preview="<<processed/elapsed<<" UI="<<displayed/elapsed<<" | "<<pairStatus
                <<" age_ms EVS="<<(stats[0].lastNs? (now-stats[0].lastNs.load())/1e6:-1)
                <<" APS="<<(stats[1].lastNs?(now-stats[1].lastNs.load())/1e6:-1)
                <<" paired="<<ss.matched<<" unmatched="<<ss.unmatched
                <<" evicted EVS/APS="<<ss.evsDropped<<'/'<<ss.apsDropped<<" record_drop="<<recorder.dropped
                <<" get_errors EVS/APS="<<stats[0].errors<<'/'<<stats[1].errors
                <<" correction_ms="<<correctionNs/1e6<<" decode_ms="<<decodeNs/1e6<<" display_ms="<<displayNs/1e6
                <<" capture_copy_total_ms="<<(stats[0].copyNs+stats[1].copyNs)/1e6
                <<" get_wait_total_ms="<<(stats[0].getNs+stats[1].getNs)/1e6
                <<" callback_total_ms="<<(stats[0].callbackNs+stats[1].callbackNs)/1e6
                <<" callback_max_ms="<<std::max(stats[0].callbackMaxNs.load(),stats[1].callbackMaxNs.load())/1e6
                <<" record_total_ms="<<recorder.workNs.load()/1e6<<" record_max_ms="<<recorder.maxWorkNs.load()/1e6
                <<" | "<<color.status()<<" | "<<recorder.status()<<'\n';
            previous[0]=stats[0].frames; previous[1]=stats[1].frames; previousLogical=stats[0].logical;
            processed=displayed=0;correctionNs=decodeNs=displayNs=0;report=now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stop=true; if(evs.joinable())evs.join();if(aps.joinable())aps.join();recorder.shutdown();
    std::lock_guard<std::mutex> l(errorMutex);
    if(!captureError.empty()) throw std::runtime_error(captureError);
    if(recorder.failed()) throw std::runtime_error("Recording failed: "+recorder.status());
    if(!stats[0].frames || !stats[1].frames) throw std::runtime_error("One or both streams delivered zero frames");
    return 0;
}
