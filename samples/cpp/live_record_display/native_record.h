#pragma once
#include "frame_sync.h"
#include "../hvs_record/dual_stream_writer.h"
#include "../hvs_record/avi_timing.h"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
namespace hv_live {
// Disk IO runs exclusively on this worker. Producers only retain owned frames.
class NativeRecorder {
public:
    using Item=Timed<Shimeta::Frame>;
    NativeRecorder(): worker([this]{run();}) {}
    ~NativeRecorder() { shutdown(); }
    void shutdown() { {std::lock_guard<std::mutex> l(m); accepting=false; quitting=true;} cv.notify_one(); if(worker.joinable()) worker.join(); }
    bool failed() {std::lock_guard<std::mutex> l(m); return !error.empty();}
    bool start(const std::string& base) {
        std::lock_guard<std::mutex> l(m);
        if(session || accepting || quitting) return false;
        directory=base+"_"+std::to_string(hostNs());
        error.clear(); dropped=0; bytes=0; accepting=true; session=true; cv.notify_one(); return true;
    }
    void stop() {std::lock_guard<std::mutex> l(m); accepting=false; cv.notify_one();}
    bool recording() {std::lock_guard<std::mutex> l(m); return accepting;}
    std::string status() {std::lock_guard<std::mutex> l(m); return error.empty()?(session?"recording/draining":"idle"):error;}
    void push(const Item& item) {
        std::lock_guard<std::mutex> l(m);
        if(!accepting) return;
        const auto n=item.value.aps.size+item.value.evs.size;
        if(bytes+n>64ULL*1024*1024) {++dropped; return;} // drop-new, never block capture
        q.push_back(item); bytes+=n; cv.notify_one();
    }
    std::atomic<uint64_t> dropped{0}, written{0};
    std::atomic<int64_t> workNs{0}, maxWorkNs{0};
private:
    void run() noexcept {
        DualStreamWriter writer; std::ofstream metadata; uint64_t frames=0,payload=0,evsPackets=0;
        int width=0,height=0; int64_t first=0,last=0; bool opened=false, initialized=false;
        std::string path;
        auto finish=[&] {
            if(opened) writer.close();
            const double fps=frames>1 && last>first ? double(frames-1)*1e9/double(last-first) : 0;
            if(fps>0) setAviFrameRate(std::filesystem::path(path)/"aps.avi",fps);
            if(metadata.is_open()) {metadata.close(); if(!metadata) throw std::runtime_error("metadata close failed");}
            if(!frames) throw std::runtime_error("Recording contains no APS frames");
            if(initialized) {
                std::ofstream summary(std::filesystem::path(path)/"summary.txt");
                summary<<"status=complete\naps_frames="<<frames<<"\naps_observed_fps="<<fps
                    <<"\nreceive_span_seconds="<<(last-first)/1e9
                    <<"\navi_duration_seconds="<<(fps>0?frames/fps:0)
                    <<"\naps_width="<<width<<"\naps_height="<<height
                    <<"\nevs_width=768\nevs_height=608\ngray8_frames=0\naps_paired_timestamps=0"
                    <<"\nevs_packets="<<evsPackets<<"\nqueue_dropped="<<dropped.load()
                    <<"\naps_storage_format=ISP_NV12_uncorrected\n"
                    <<"timeline=constant_average_rate; irregular intervals retained in aps.frames.csv\n"
                    <<"timing_status="<<(fps>0?"host_receive_average":"insufficient_frames_unverified")<<"\n";
                if(!summary) throw std::runtime_error("summary write failed");
            }
            std::cout<<"Recording finalized: "<<path<<" APS="<<frames<<" measured FPS="<<fps<<"\n";
            opened=initialized=false; frames=payload=evsPackets=0; first=last=0;
        };
        while(true) {
            Item item; bool have=false, finalize=false;
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l,[&]{return quitting || !q.empty() || (session&&!accepting);});
                if(!q.empty()) {item=std::move(q.front()); q.pop_front(); bytes-=item.value.aps.size+item.value.evs.size; have=true; path=directory;}
                else if(session&&!accepting) {finalize=true; path=directory;}
                else if(quitting) break;
            }
            try {
                if(finalize) {
                    finish(); std::lock_guard<std::mutex> l(m); session=false; if(quitting) break; continue;
                }
                if(!have) continue;
                auto begin=hostNs(); auto& f=item.value;
                if(!opened) {
                    if(!f.aps.size) continue; // consistent with original: begin at first APS
                    if(!std::filesystem::create_directory(path)) throw std::runtime_error("Recording directory exists");
                    initialized=true; width=f.width; height=f.height;
                    if(!writer.open(path+"/events.raw",path+"/aps.avi",768,608,width,height)) throw std::runtime_error("open recording failed");
                    opened=true; metadata.open(path+"/aps.frames.csv");
                    metadata<<"index,host_receive_ns,write_work_ns,exposure_actual_us\n";
                    if(!metadata) throw std::runtime_error("open metadata failed");
                }
                if(f.aps.size && (f.width!=width || f.height!=height || f.format!=Shimeta::PixelFormat::NV12))
                    throw std::runtime_error("Recording geometry/format changed");
                const auto n=f.aps.size+f.evs.size;
                if(payload+n>1024ULL*1024*1024) {++dropped; stop(); continue;} // bounded RIFF segment
                if(!writer.writeFrame(f,nullptr)) throw std::runtime_error("recording write failed");
                payload+=n; if(f.evs.size) ++evsPackets;
                if(f.aps.size) {
                    if(!frames) first=item.ns; last=item.ns;
                    metadata<<frames++<<','<<item.ns<<','<<(hostNs()-begin)<<",unknown\n";
                    if(!metadata) throw std::runtime_error("metadata write failed");
                }
                ++written; auto cost=hostNs()-begin; workNs+=cost;
                if(cost>maxWorkNs.load()) maxWorkNs=cost;
            } catch(const std::exception& e) {
                const bool ownedDirectory=initialized;
                writer.close(); if(metadata.is_open()) metadata.close(); opened=initialized=false;
                frames=payload=evsPackets=0; first=last=0;
                const std::string message=e.what();
                {std::lock_guard<std::mutex> l(m); error=message; accepting=session=false; q.clear(); bytes=0;}
                std::error_code ec;
                if(ownedDirectory && !path.empty() && std::filesystem::is_directory(path,ec)) {
                    std::ofstream failure(std::filesystem::path(path)/"summary.txt");failure<<"status=failed\nreason="<<message<<'\n';
                }
                std::cerr<<"Recording FAILED (partial files retained): "<<message<<"\n";
            }
        }
    }
    std::mutex m; std::condition_variable cv; std::deque<Item> q;
    size_t bytes=0; bool accepting=false,session=false,quitting=false;
    std::string directory,error; std::thread worker;
};
}
