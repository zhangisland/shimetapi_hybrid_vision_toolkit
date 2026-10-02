#include "../samples/cpp/player/player_widgets.h"
#include <cassert>
#include <thread>
#include <iostream>
#include "../samples/cpp/player/replay_timeline.h"
namespace fake_player {extern std::atomic<int> delayMs,reads;}
using namespace hv_player;
using Clock=std::chrono::steady_clock;
int main(int argc,char** argv) {
    fake_player::delayMs=0;
    VideoReader sequential;assert(sequential.open("fake",30));
    cv::Mat selected;Shimeta::EvsTimestamp selectedTs{};
    assert(sequential.readFrameAt(20,selected,&selectedTs));
    assert(sequential.processedCount()==1); // 21 raw reads, ONE ISP invocation.
    assert(selectedTs.processed_timestamp==1020);
    fake_player::delayMs=30;
    EvsFrameSequence evsSequence;assert(evsSequence.open("synthetic"));
    assert(evsSequence.frameCount()==16); // All-empty events still span 64 ms.
    assert(evsSequence.processedTimestampAt(0)==12000000);
    assert(evsSequence.processedTimestampAt(15)==12060000);
    assert(evsSequence.frameIndexForTimestamp(12032000,EvsStepMode::Single)==8);
    const auto temp=std::filesystem::temp_directory_path()/
        ("hvs-replay-"+std::to_string(Clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(temp);
    {
        std::ofstream meta(temp/"vin.frames.jsonl");
        meta<<"{\"stream\":\"evs\",\"host_ns\":9000031000000}\n"
            <<"{\"stream\":\"aps\",\"host_ns\":9000032000000}\n"
            <<"{\"stream\":\"evs\",\"host_ns\":9000063000000}\n"
            <<"{\"stream\":\"aps\",\"host_ns\":9000065000000}\n";
    }
    ReplayTimeline timeline;assert(timeline.load((temp/"aps.avi").string(),evsSequence.packetEnds(),2));
    assert(timeline.host&&timeline.offsetSpreadUs==0);
    assert(timeline.time(0,30)==12032000); // Host epoch differs by billions of us.
    assert(timeline.elapsed(1,30)==33000);
    assert(timeline.index(32999,30)==0&&timeline.index(33000,30)==1);
    assert(evsSequence.frameIndexForTimestamp(timeline.time(0,30),EvsStepMode::Single)==8);
    bool rejected=false;
    try {timeline.load((temp/"aps.avi").string(),evsSequence.packetEnds(),3);}
    catch(const std::exception&) {rejected=true;}
    assert(rejected);
    std::filesystem::remove(temp/"vin.frames.jsonl");std::filesystem::remove(temp);
    ApsFrameCache cache;assert(cache.open("fake",30));
    cv::Mat frame;uint64_t actual=0;
    const auto begin=Clock::now();
    // A far seek would previously decode 24 slow frames on the GUI thread.
    cache.frameAt(23,frame,&actual);
    assert(Clock::now()-begin<std::chrono::milliseconds(100));
    auto waitFor=[&](uint64_t target) {
        auto deadline=Clock::now()+std::chrono::seconds(5);
        while(Clock::now()<deadline) {
            if(cache.frameAt(target,frame,&actual)&&actual==target) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    assert(waitFor(23));assert(cache.cachedFrameCount()<=8);
    // Repeated presentation of a paused target must not advance its image or
    // timestamp, even when an earlier background request has been in flight.
    const auto before=cache.decodeStats().frames;
    for(int i=0;i<5;++i) {
        Shimeta::EvsTimestamp ts{};
        assert(cache.frameAt(23,frame,&actual,&ts)&&actual==23&&ts.processed_timestamp==1023);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(cache.decodeStats().frames==before);
    assert(isApsAtKnownEnd(23,cache));assert(!isApsAtKnownEnd(5,cache));
    // Evicted backwards frame must reopen and decode, without blocking UI.
    assert(waitFor(0));assert(cache.timestampAt(0).processed_timestamp==1000);
    const auto immutable=frame.clone();cache.frameAt(20,frame,&actual);
    assert(cv::mean(immutable)[0]>0);
    cv::Mat evs(608,768,CV_8UC3,cv::Scalar(20,40,80));
    cv::Mat aps(1224,1632,CV_8UC3,cv::Scalar(70,140,210));
    auto canvas=composeSideBySide(evs,aps,1,"host",0,2,0,24,100,EvsStepMode::Aps,EvsColorMode::BlueRed,false,1,true,true);
    auto small=fitPlayerCanvas(canvas,1024,600);
    assert(small.cols<=1024&&small.rows<=600);
    assert(!g_ui_buttons.empty());
    for(const auto& button:g_ui_buttons) {
        assert((button.rect & cv::Rect(0,0,small.cols,small.rows))==button.rect);
        const auto center=button.rect.tl()+cv::Point(button.rect.width/2,button.rect.height/2);
        g_pending_action=int(UiAction::None);
        mouseCallback(cv::EVENT_LBUTTONDOWN,center.x,center.y,0,nullptr);
        assert(g_pending_action==int(button.action));
    }
    if(argc>1) cv::imwrite(argv[1],small);
    std::cout<<"Nonblocking decode, bounded cache, rewind, scaled complete UI and button hits passed\n";
}
