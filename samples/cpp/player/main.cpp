// player: 离线回放 live_record_display 录制的 .raw + .avi 文件。
// 本文件只保留信号处理与播放编排；数据/回放/UI 辅助代码见 player_widgets.{h,cpp}。
#include "player_widgets.h"
#include "replay_timeline.h"
#include <tuple>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <opencv2/opencv.hpp>

// execinfo.h / backtrace 在某些交叉工具链中不可用
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#else
// 模拟 backtrace 以避免链接错误
static int backtrace(void**, int) { return 0; }
static void backtrace_symbols_fd(void*, int, int) {}
#endif

using namespace hv_player;   // 引入 player_widgets 的全部符号，编排代码保持原写法

// ---- 全局运行标志（信号处理 + 主循环共用；UI 状态在 hv_player 命名空间内）----
std::atomic<bool> g_running(true);

// ---- 信号处理 ----
static void signalHandler(int) { g_running = false; }

#ifndef _WIN32
static void crashSignalHandler(int signum, siginfo_t*, void*) {
    constexpr char header[] = "\nFatal signal received, backtrace:\n";
    if (write(STDERR_FILENO, header, sizeof(header) - 1) > 0) {}
    void* frames[64];
    int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    signal(signum, SIG_DFL);
    raise(signum);
}
#endif

static void installSignalHandlers() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
#ifndef _WIN32
    void* frames[1]; backtrace(frames, 1);
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_sigaction = crashSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
#endif
}

int main(int argc, char** argv) {
    installSignalHandlers();
    cv::setNumThreads(2);
    if (argc < 3) { printUsage(argv[0]); return 1; }

    std::string raw_path = argv[1];
    std::string avi_path = argv[2];
    bool dump_timestamps = false;
    double fallback_fps = 30.0, speed = 1.0;
    int window_width=1280,window_height=720;
    ApsIsp isp;
    int numeric_arg = 0;
    try {
    for (int i = 3; i < argc; ++i) {
        const std::string option=argv[i];
        if(option=="--window-width"||option=="--window-height") {
            if(++i>=argc) throw std::invalid_argument("Missing viewport size");
            size_t consumed=0;const int value=std::stoi(argv[i],&consumed);
            if(consumed!=std::strlen(argv[i])||value<320||value>4096) throw std::invalid_argument("Viewport size must be 320..4096");
            (option=="--window-width"?window_width:window_height)=value;continue;
        }
        if (apsOption(argv[i], i, argc, argv, isp)) continue;
        if (std::strcmp(argv[i], "--dump-timestamps") == 0) {
            dump_timestamps = true;
        } else if (numeric_arg++ == 0) {
            fallback_fps = std::atof(argv[i]);
        } else if (numeric_arg == 2) {
            speed = std::atof(argv[i]);
        } else {
            printUsage(argv[0]);
            return 1;
        }
    }
    } catch(const std::exception& e) { std::cerr << e.what() << std::endl; return 1; }
    if (fallback_fps <= 0.0 || speed <= 0.0) { printUsage(argv[0]); return 1; }
    if (dump_timestamps) return dumpTimestamps(raw_path, avi_path, std::cout) ? 0 : 1;

    // ---- 加载 EVS ----
    EvsFrameSequence evs_seq;
    if (!evs_seq.open(raw_path)) return 1;

    // ---- 加载 APS ----
    ApsFrameCache video_cache;
    video_cache.setIsp(isp);
    if (!video_cache.open(avi_path, fallback_fps,isp.pattern!="none")) {
        std::cerr << "Failed to open AVI: " << avi_path << std::endl;
        return 1;
    }
    TimestampSyncMap ts_sync;
    bool has_ts_sync = ts_sync.open(raw_path, avi_path);

    ReplayTimeline timeline;
    try {timeline.load(avi_path,evs_seq.packetEnds(),video_cache.frameCount());}
    catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;return 1;}
    if(timeline.host) std::cout<<"SYNC: host receive approximation; offset="<<timeline.offsetUs
        <<" us, sensor-units/host-us="<<timeline.sensorUnitsPerHostUs<<", residual spread="<<timeline.offsetSpreadUs<<" us. Not per-frame sensor sync.\n";
    auto img_size = evs_seq.imageSize();
    std::cout << "EVS raw: " << raw_path << " (" << img_size.first << "x" << img_size.second << ")\n";
    std::cout << "APS AVI: " << avi_path << ", fps=" << video_cache.fps() << "\n";
    std::cout << "Speed: " << speed << "x\n";
    std::cout << "Keys: q/ESC quit, Space pause, a/d step, click buttons\n";

    // ---- GUI 窗口 ----
    cv::Mat last_video_frame=makeBlank(1632,1224);
    const std::string win_name = "HV Player (toolkit)";
    try {
        cv::namedWindow(win_name, cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO);
        cv::setMouseCallback(win_name, mouseCallback);
        // Paint the FULL UI and pump the X11 event loop before waiting for any
        // APS reconstruction. This also establishes the intended window size.
        auto initial=fitPlayerCanvas(composeSideBySide(evs_seq.frameAt(0,EvsColorMode::BlueRed),last_video_frame,
            0,"loading APS",0,evs_seq.processedTimestampAt(0),0,video_cache.frameCount(),evs_seq.frameCount(),
            EvsStepMode::Aps,EvsColorMode::BlueRed,false,speed,true,true),window_width,window_height);
        cv::imshow(win_name,initial);
        cv::resizeWindow(win_name,initial.cols,initial.rows);
        cv::waitKey(1);
    } catch (const cv::Exception& e) { printGuiStartupError(e); return 1; }

    using Clock=std::chrono::steady_clock;
    auto apsStart=Clock::now(),evsStart=apsStart,reportStart=apsStart;
    bool evs_playing=true,aps_playing=true,sync_enabled=true,ready=false;
    uint64_t evs_frame_index=0,aps_frame_index=0,displayed_aps_index=0;
    EvsStepMode evs_step_mode=EvsStepMode::Aps;
    EvsColorMode evs_color_mode=EvsColorMode::BlueRed;
    uint64_t shown=0,skipped=0;
    double maxPresentMs=0;
    auto resetClocks=[&] {
        const auto now=Clock::now();
        apsStart=now-std::chrono::microseconds(int64_t(timeline.elapsed(aps_frame_index,video_cache.fps())/speed));
        evsStart=now-std::chrono::microseconds(int64_t((evs_seq.processedTimestampAt(evs_frame_index)-evs_seq.processedTimestampAt(0))/(speed*timeline.sensorUnitsPerHostUs)));
    };
    Shimeta::EvsTimestamp displayedTimestamp{};
    auto sensorForAps=[&](uint64_t index) {
        const auto ts=displayedTimestamp;
        if(ts.valid) return ts.processed_timestamp?ts.processed_timestamp:ts.raw_timestamp/200;
        if(timeline.host) return timeline.time(index,video_cache.fps());
        if(has_ts_sync) return ts_sync.apsSensorUs(index);
        // Unknown offset: align recording starts explicitly, never compare
        // relative APS frame/fps directly against absolute EVS sensor time.
        return evs_seq.processedTimestampAt(0)+apsPlaybackTimestampUs(index,video_cache.fps());
    };
    using PaintState=std::tuple<uint64_t,uint64_t,bool,bool,bool,int,int,double>;
    PaintState painted{};bool havePaint=false;
    uint64_t previousPrepared=UINT64_MAX;
    while(g_running) {
        if(!video_cache.error().empty()) {std::cerr<<video_cache.error()<<std::endl;return 1;}
        if(!ready&&video_cache.preparedFrames()!=previousPrepared) {
            previousPrepared=video_cache.preparedFrames();
            auto progress=composeSideBySide(evs_seq.frameAt(0,evs_color_mode),last_video_frame,0,
                "prepare "+std::to_string(previousPrepared)+"/"+std::to_string(video_cache.frameCount()),
                0,0,0,video_cache.frameCount(),evs_seq.frameCount(),evs_step_mode,evs_color_mode,true,speed,false,false);
            cv::imshow(win_name,fitPlayerCanvas(progress,window_width,window_height));
        }
        UiAction action=UiAction(g_pending_action.exchange(int(UiAction::None)));
        if(action==UiAction::SyncOn||action==UiAction::SyncOff) {
            sync_enabled=action==UiAction::SyncOn;aps_frame_index=displayed_aps_index;
            if(sync_enabled) evs_playing=aps_playing;
            resetClocks();
        } else if(action==UiAction::ApsTogglePlay||action==UiAction::EvsTogglePlay) {
            if(sync_enabled) {
                aps_frame_index=displayed_aps_index;
                aps_playing=!aps_playing;evs_playing=aps_playing;
                if(aps_playing&&isApsAtKnownEnd(aps_frame_index,video_cache)) aps_frame_index=0;
            } else if(action==UiAction::ApsTogglePlay) {
                aps_frame_index=displayed_aps_index;aps_playing=!aps_playing;
                if(aps_playing&&isApsAtKnownEnd(aps_frame_index,video_cache)) aps_frame_index=0;
            } else {
                evs_playing=!evs_playing;
                if(evs_playing&&evs_frame_index+1>=evs_seq.frameCount()) evs_frame_index=0;
            }
            resetClocks();
        } else if(action==UiAction::ApsPrev||action==UiAction::ApsNext||action==UiAction::EvsPrev||action==UiAction::EvsNext) {
            const bool next=action==UiAction::ApsNext||action==UiAction::EvsNext;
            if(sync_enabled||action==UiAction::ApsPrev||action==UiAction::ApsNext) {
                aps_playing=false;if(sync_enabled) evs_playing=false;
                aps_frame_index=next?std::min<uint64_t>(displayed_aps_index+1,video_cache.frameCount()-1):
                    (displayed_aps_index?displayed_aps_index-1:0);
            } else {
                evs_playing=false;
                const uint64_t step=evs_step_mode==EvsStepMode::Single?1:kEvsPerApsFrame;
                evs_frame_index=next?std::min<uint64_t>(evs_frame_index+step,evs_seq.frameCount()-1):
                    (evs_frame_index>step?evs_frame_index-step:0);
            }
            resetClocks();
        } else if(action==UiAction::EvsStepSingle) evs_step_mode=EvsStepMode::Single;
        else if(action==UiAction::EvsStepAps) evs_step_mode=EvsStepMode::Aps;
        else if(action==UiAction::EvsColorBlueRed) evs_color_mode=EvsColorMode::BlueRed;
        else if(action==UiAction::EvsColorOrangeYellow) evs_color_mode=EvsColorMode::OrangeYellow;
        else if(speedForAction(action)>0) {
            speed=speedForAction(action);aps_frame_index=displayed_aps_index;resetClocks();
        }
        const auto now=Clock::now();
        if(ready&&aps_playing) {
            const auto us=std::chrono::duration_cast<std::chrono::microseconds>(now-apsStart).count();
            aps_frame_index=timeline.index(uint64_t(std::max(0.0,double(us)*speed)),video_cache.fps());
        }
        if(ready&&evs_playing&&!sync_enabled) {
            const auto us=std::chrono::duration_cast<std::chrono::microseconds>(now-evsStart).count();
            const auto target=evs_seq.processedTimestampAt(0)+uint64_t(std::max(0.0,double(us)*speed*timeline.sensorUnitsPerHostUs));
            evs_frame_index=evs_seq.frameIndexForTimestamp(target,EvsStepMode::Single);
            if(target>=evs_seq.processedTimestampAt(evs_seq.frameCount()-1)) evs_playing=false;
        }
        cv::Mat frame;uint64_t actual=0;Shimeta::EvsTimestamp frameTimestamp{};
        if(video_cache.frameAt(aps_frame_index,frame,&actual,&frameTimestamp)) {
            // During an asynchronous backwards seek keep the previous COMPLETE
            // pair until the requested frame is ready; don't show a future cache entry.
            if(actual<=aps_frame_index) {
                if(!ready||actual!=displayed_aps_index) {
                    if(ready&&actual>displayed_aps_index+1) skipped+=actual-displayed_aps_index-1;
                    ++shown;displayed_aps_index=actual;last_video_frame=frame;displayedTimestamp=frameTimestamp;
                }
                if(!ready) {
                    ready=true;aps_frame_index=actual;resetClocks();reportStart=Clock::now();
                    if(!timeline.host&&!has_ts_sync&&!displayedTimestamp.valid)
                        std::cerr<<"SYNC fallback: recording starts aligned; original clock offset UNKNOWN. Keep native vin.frames.jsonl for host alignment.\n";
                }
                if(isApsAtKnownEnd(actual,video_cache)) {aps_playing=false;if(sync_enabled) evs_playing=false;}
            }
        }
        const auto apsTs=sensorForAps(displayed_aps_index);
        std::string source="start aligned / approx";
        if(displayedTimestamp.valid) source="AVI sensor tsmp";
        else if(timeline.host) source="host bridge / approx";
        else if(has_ts_sync) source="CSV clock bridge";
        if(sync_enabled&&ready) evs_frame_index=evs_seq.frameIndexForTimestamp(apsTs,EvsStepMode::Single);
        PaintState state{displayed_aps_index,evs_frame_index,sync_enabled,aps_playing,evs_playing,int(evs_step_mode),int(evs_color_mode),speed};
        if(ready&&(!havePaint||state!=painted)) {
            const auto paintStart=Clock::now();
            // Accumulate a TIME window, not eight nonempty event frames.
            const auto begin=evs_seq.processedTimestampAt(evs_frame_index);
            const auto end=begin+uint64_t(1e6*timeline.sensorUnitsPerHostUs/video_cache.fps());
            size_t count=1;
            if(evs_step_mode==EvsStepMode::Aps) while(evs_frame_index+count<evs_seq.frameCount()&&
                evs_seq.processedTimestampAt(evs_frame_index+count)<end) ++count;
            const auto evs=evs_seq.accumulatedFrameAt(evs_frame_index,count,evs_color_mode);
            cv::imshow(win_name,fitPlayerCanvas(composeSideBySide(evs,last_video_frame,timeline.host?timeline.elapsed(displayed_aps_index,video_cache.fps()):apsTs,source,
                displayed_aps_index,timeline.host?uint64_t(std::max(0.0,(double(begin)-double(timeline.aps.front()))/timeline.sensorUnitsPerHostUs)):begin,evs_frame_index,video_cache.frameCount(),evs_seq.frameCount(),
                evs_step_mode,evs_color_mode,sync_enabled,speed,evs_playing,aps_playing),window_width,window_height));
            maxPresentMs=std::max(maxPresentMs,std::chrono::duration<double,std::milli>(Clock::now()-paintStart).count());
            painted=state;havePaint=true;
        }
        if(ready&&Clock::now()-reportStart>=std::chrono::seconds(5)) {
            const double elapsed=std::chrono::duration<double>(Clock::now()-reportStart).count();
            const auto decode=video_cache.decodeStats();
            std::cout<<"Replay presented APS="<<shown<<" ("<<shown/elapsed<<"/s), skipped previews="<<skipped
                <<", max compose+imshow ms="<<maxPresentMs
                <<", cumulative preview-fetch mean/max ms="<<(decode.frames?decode.totalMs/decode.frames:0)<<'/'<<decode.maxMs
                <<"; capture data unchanged\n";
            shown=skipped=0;maxPresentMs=0;reportStart=Clock::now();
        }
        const int key=cv::waitKey(1)&0xff;
        if(key==27||key=='q'||key=='Q') g_running=false;
        else if(key==' ') g_pending_action=int(UiAction::ApsTogglePlay);
        else if(key=='a'||key=='A') g_pending_action=int(sync_enabled?UiAction::ApsPrev:UiAction::EvsPrev);
        else if(key=='d'||key=='D') g_pending_action=int(sync_enabled?UiAction::ApsNext:UiAction::EvsNext);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    cv::destroyAllWindows();
    return 0;
}
