#include "../samples/cpp/player/player_widgets.h"
#include <cassert>
#include <thread>
#include <iostream>
namespace fake_player {extern std::atomic<int> delayMs,reads;}
using namespace hv_player;
using Clock=std::chrono::steady_clock;
int main(int argc,char** argv) {
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
