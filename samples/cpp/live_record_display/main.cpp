// live_record_display: MIPI-HVS 后端实时预览 + 按键开关录制。
// 本文件只保留参数解析与采集/显示主循环；可视化/录制/解码辅助见 live_widgets.{h,cpp}。
#include "live_widgets.h"
#include "../player/aps_color.h"
#include <iostream>

#include <shimetapi/codec/mipi_raw8_codec.h>
#include <shimetapi/hv/camera.h>
#include <shimetapi/hv/device_config.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <conio.h>
static bool stdinReady() { return _kbhit()!=0; }
#else
#include <sys/select.h>
#include <unistd.h>
static bool stdinReady() {
    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO,&fds); timeval tv={0,0};
    return select(STDIN_FILENO+1,&fds,nullptr,nullptr,&tv)>0;
}
#endif

using namespace hv_live;   // 引入 live_widgets 的全部符号，主循环代码保持原写法

// ---- 全局运行标志（主循环 + 键盘退出共用）----
std::atomic<bool> g_running(true);

static int run(int argc, char** argv) {
    // ---- 解析参数 ----
    bool noDisplay = false;
    std::string evsPrefix = "live_events";
    std::string apsPrefix = "live_video";

    hv_player::ApsIsp isp;
    try {
    for (int i = 1; i < argc; ++i) {
        if (hv_player::apsOption(argv[i],i,argc,argv,isp)) continue;
        if (std::strcmp(argv[i], "--no-display") == 0) {
            noDisplay = true;
        } else if (std::strcmp(argv[i], "--evs-prefix") == 0 && i + 1 < argc) {
            evsPrefix = argv[++i];
        } else if (std::strcmp(argv[i], "--aps-prefix") == 0 && i + 1 < argc) {
            apsPrefix = argv[++i];
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else { throw std::invalid_argument(std::string("Unknown option: ")+argv[i]); }
    }

    } catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 1; }
    if(argc==2 && std::string(argv[1])=="--aps-capabilities") return 0;
    std::cout<<hv_aps::capabilities()<<std::endl;
    std::printf("=== HV Toolkit live_record_display (MIPI-HVS) ===\n");

    // ---- 初始化 Camera ----
    // Callback receive counter is independent of UI/ISP polling. Retaining the
    // last owner prevents allocator address reuse from looking like a duplicate.
    std::mutex receiveMutex;
    std::shared_ptr<uint8_t[]> receiveOwner;
    const uint8_t* receivePtr=nullptr;
    std::atomic<uint64_t> callbackAps{0};
    std::atomic<bool> receiveReliable{true};
    Shimeta::hv::Camera cam;
    Shimeta::hv::DeviceConfig cfg;
    cfg.backend = Shimeta::hv::Backend::MipiHvs;
    if (!cam.Init(cfg)) { std::cerr<<"Camera.Init failed"<<std::endl; return 1; }
    struct SessionGuard {
        Shimeta::hv::Camera& camera;
        ~SessionGuard() { camera.StopStream(); camera.Destroy(); }
    } session{cam};
    cam.SetFrameCallback([&](const Shimeta::Frame& f) {
        if(!f.aps.data || !f.aps.size) return;
        std::lock_guard<std::mutex> lock(receiveMutex);
        if(!f.aps_owner) {receiveReliable=false; return;}
        if(f.aps.data!=receivePtr) {receiveOwner=f.aps_owner; receivePtr=f.aps.data; ++callbackAps;}
    });
    if (!cam.StartStream()) {
        std::fprintf(stderr, "无法启动 MIPI-HVS 设备\n");
        return 1;
    }
    std::printf("MIPI-HVS 设备已启动\n");

    // ---- 组件 ----
    Shimeta::codec::MipiRaw8Decoder decoder;
    EvsVisualizer visualizer;
    RecordManager recorder;

    std::atomic<bool> displayEnabled(true);
    cv::Mat evsDisplay, apsDisplay;

    if (!noDisplay) {
        cv::namedWindow(kWindowName, cv::WINDOW_NORMAL);
        std::printf("\n按 'r' 开始/停止录制，按 'd' 开关显示，按 'q/ESC' 退出\n");
    } else {
        displayEnabled = false;
        std::printf("\n显示已禁用（--no-display）。输入 r 回车录制，q 回车退出。\n");
    }

    // ---- 主循环 ----
    auto lastDisplayTime = std::chrono::steady_clock::now();
    const auto displayInterval = std::chrono::milliseconds(1000 / kDefaultDisplayFps);

    // 边沿触发去重：GetFrame 为电平触发（谓词只看 evs.size>0），同一 EVS 包
    // 会被重复返回；按 slab 指针识别新包，重复快照只刷显示、不重复喂
    // 可视化/录制（否则事件被重复累积、RAW 重复落盘）。
    const uint8_t* lastEvsPtr = nullptr;
    const uint8_t* lastApsPtr = nullptr;
    std::shared_ptr<uint8_t[]> lastEvsOwner, lastApsOwner;
    auto reportTime = std::chrono::steady_clock::now();
    auto lastApsTime = reportTime;
    uint64_t received=0, previewed=0, lastCallback=0;
    unsigned dumpIndex=0;
    bool failed=false;

    while (g_running) {
        // Poll commands even when frames arrive continuously.
        if(noDisplay) {
            if(stdinReady()) {
                std::string line;
                if(std::getline(std::cin,line) && !line.empty()) {
                    char k=line[0];
                    if(k=='q') g_running=false;
                    else if(k=='r') { if(recorder.isRecording()) recorder.stop(); else recorder.start(evsPrefix,apsPrefix); }
                    else if(k=='w') isp.mode("once"); else if(k=='c') isp.mode("continuous");
                    else if(k=='m') isp.mode("manual"); else if(k=='o') isp.mode("off");
                    else if(k=='s') { try { isp.dump("aps_diagnostic_"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())); }
                        catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; } }
                }
            }
        }
        if(!g_running) break;
        // 拉一帧
        Shimeta::Frame frame;
        if (!cam.GetFrame(frame, noDisplay ? 100 : 16)) {
            if(std::chrono::steady_clock::now()-lastApsTime>std::chrono::seconds(10)) {
                std::cerr<<"APS disconnected/stalled; restart after reconnect"<<std::endl; failed=true; break;
            }
            if(!noDisplay && (cv::waitKey(1)&0xFF)==27) g_running=false;
            continue;
        }

        if ((frame.evs.size && !frame.evs_owner) || (frame.aps.size && !frame.aps_owner)) {
            std::cerr<<"SDK frame owners missing: cannot safely deduplicate snapshots"<<std::endl;
            failed=true; break;
        }
        const bool newEvsPacket = frame.evs.size > 0 && frame.evs.data != lastEvsPtr;
        const bool newAps = frame.aps.size > 0 && frame.aps.data != lastApsPtr;
        if (newEvsPacket) { lastEvsPtr = frame.evs.data; lastEvsOwner=frame.evs_owner; }
        if (newAps) { lastApsPtr=frame.aps.data; lastApsOwner=frame.aps_owner; ++received; lastApsTime=std::chrono::steady_clock::now(); }

        // Decode EVS independently; recording uses only SDK-provided APS pairing.
        if (newEvsPacket) {
            std::vector<Shimeta::EventCD> events;
            decoder.Decode(frame.evs.data, frame.evs.size, events);
            if (!events.empty()) {
                visualizer.addEvents(events);
            }
        }

        // Preserve independent APS/EVS arrivals and the SDK's timestamp pairing.
        auto fresh=frame;
        if(!newEvsPacket) fresh.evs={};
        if(!newAps) fresh.aps={};
        try { if(newEvsPacket || newAps) recorder.writeFrame(fresh,frame.aps_evs_ts.valid?&frame.aps_evs_ts:nullptr); }
        catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; failed=true; break; }
        if(newAps) {
            try { apsDisplay=isp.process(frame); }
            catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; failed=true; break; }
        }

        // 显示刷新
        auto now = std::chrono::steady_clock::now();
        double elapsed=std::chrono::duration<double>(now-reportTime).count();
        if(elapsed>=1) {
            const auto callbacks=callbackAps.load();
            std::cout<<"APS callback receive/s="<<(receiveReliable?std::to_string((callbacks-lastCallback)/elapsed):"unknown")
                     <<" consumed unique snapshots/s="<<received/elapsed<<" preview/s="<<previewed/elapsed
                     <<" device FPS=unknown dropped=unknown "<<isp.status()<<std::endl;
            received=previewed=0; lastCallback=callbacks; reportTime=now;
        }
        if(now-lastApsTime>std::chrono::seconds(10)) { std::cerr<<"APS stalled; stop and restart after reconnect"<<std::endl; failed=true; break; }
        if (!noDisplay &&
            now - lastDisplayTime >= displayInterval) {
            lastDisplayTime = now;
            evsDisplay = visualizer.getFrame();

            // 拼接：左侧 EVS 可视化，右侧 APS（缩放到一半宽度）
            cv::Mat evsPanel = evsDisplay.empty()
                ? cv::Mat::zeros(kDefaultEvsHeight, kDefaultEvsWidth, CV_8UC3)
                : evsDisplay;
            cv::Mat apsPanel;
            if (!apsDisplay.empty()) {
                cv::resize(apsDisplay, apsPanel,
                           cv::Size(apsDisplay.cols / 2, apsDisplay.rows / 2));
            } else {
                apsPanel = cv::Mat::zeros(kDefaultApsHeight / 2, kDefaultApsWidth / 2, CV_8UC3);
            }

            int cw = evsPanel.cols + apsPanel.cols;
            int ch = std::max(evsPanel.rows, apsPanel.rows);
            cv::Mat combined(ch, cw, CV_8UC3, cv::Scalar(0, 0, 0));
            evsPanel.copyTo(combined(cv::Rect(0, 0, evsPanel.cols, evsPanel.rows)));
            apsPanel.copyTo(combined(cv::Rect(evsPanel.cols, 0, apsPanel.cols, apsPanel.rows)));

            // 录制状态指示
            std::string title = kWindowName;
            if (recorder.isRecording()) title += " [REC]";
            cv::setWindowTitle(kWindowName, title);
            if(displayEnabled) { cv::imshow(kWindowName, combined); ++previewed; }

            int key = cv::waitKey(1) & 0xFF;
            if (key == 27 || key == 'q' || key == 'Q') {
                g_running = false;
            } else if(key=='w') { isp.mode("once"); }
            else if(key=='c') { isp.mode("continuous"); }
            else if(key=='m') { isp.mode("manual"); }
            else if(key=='o') { isp.mode("off"); }
            else if(key=='s') {
                try { isp.dump("aps_diagnostic_"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+"_"+std::to_string(dumpIndex++)); }
                catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; }
            } else if (key == 'd'  || key == 'D') {
                displayEnabled = !displayEnabled;
                std::printf("显示 %s\n", displayEnabled ? "开启" : "关闭");
            } else if (key == 'r' || key == 'R') {
                if (recorder.isRecording())
                    recorder.stop();
                else
                    recorder.start(evsPrefix, apsPrefix);
            }
        }
    }

    // ---- 清理 ----
    recorder.stop();
    if (!noDisplay) cv::destroyAllWindows();
    std::printf("\n=== 程序结束 ===\n");
    return failed?2:0;
}

#ifdef HV_X5_NATIVE
int runNativeLive(int argc,char** argv);
#endif
int main(int argc,char** argv) {
#ifdef HV_X5_NATIVE
    try { return runNativeLive(argc,argv); }
    catch(const std::exception& e) { std::cerr<<"Native live failed: "<<e.what()<<std::endl; return 2; }
#else
    try { return run(argc,argv); }
    catch(const std::exception& e) { std::cerr<<"Live capture failed: "<<e.what()<<std::endl; return 2; }
#endif
}
