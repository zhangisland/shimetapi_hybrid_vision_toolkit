#include <shimetapi/io/hybrid_reader.h>
#include <shimetapi/codec/mipi_raw8_codec.h>
#include <chrono>
#include <thread>
#include <atomic>
namespace fake_player {std::atomic<int> delayMs{30},reads{0};}
namespace Shimeta::io {
HybridReader::HybridReader()=default;
HybridReader::~HybridReader()=default;
bool HybridReader::open(const std::string&,const std::string&) {next_aps_index_=0;return true;}
double HybridReader::apsFps() const {return 29.79;}
uint32_t HybridReader::apsFrameCount() const {return 24;}
bool HybridReader::readApsFrame(Frame& f,EvsTimestamp* ts) {
    if(next_aps_index_>=24) return false;
    ++fake_player::reads;
    std::this_thread::sleep_for(std::chrono::milliseconds(fake_player::delayMs.load()));
    f.width=f.height=32;f.format=PixelFormat::NV12;
    f.aps_owner.reset(new uint8_t[32*32*3/2]);
    std::fill_n(f.aps_owner.get(),32*32,uint8_t(40+next_aps_index_));
    std::fill_n(f.aps_owner.get()+32*32,32*32/2,uint8_t(128));
    f.aps={f.aps_owner.get(),32*32*3/2};
    if(ts) {*ts={};ts->valid=true;ts->processed_timestamp=1000+next_aps_index_;}
    ++next_aps_index_;return true;
}
bool HybridReader::readEvsPacket(Frame&,size_t) {return false;}
}
namespace Shimeta::codec {
size_t MipiRaw8Decoder::Decode(const uint8_t*,size_t,std::vector<EventCD>&,int) {return 0;}
}
