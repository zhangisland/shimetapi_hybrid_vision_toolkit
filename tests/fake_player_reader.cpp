#include <shimetapi/io/hybrid_reader.h>
#include <shimetapi/codec/mipi_raw8_codec.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
namespace fake_player {std::atomic<int> delayMs{30},reads{0};std::map<const void*,bool> legacy;}
namespace Shimeta::io {
HybridReader::HybridReader()=default;
HybridReader::~HybridReader() {fake_player::legacy.erase(this);}
bool HybridReader::open(const std::string& evs,const std::string&) {next_aps_index_=0;evs_open_=std::filesystem::path(evs).filename()=="synthetic";fake_player::legacy[this]=evs_open_&&std::filesystem::path(evs).has_parent_path();return true;}
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
bool HybridReader::readEvsPacket(Frame& f,size_t) {
    if(!evs_open_||next_aps_index_>=2) return false;
    const size_t size=32*codec::MipiRaw8Layout::kSubframeBytes;
    f.evs_owner.reset(new uint8_t[size]{});f.evs={f.evs_owner.get(),size};
    for(size_t sub=0;sub<32;++sub) {
        const uint64_t ts=12000000+next_aps_index_*32000+sub*1000;
        const uint64_t word=((ts*200)<<24)|((fake_player::legacy[this]&&sub==0)?29+next_aps_index_:codec::MipiRaw8Layout::kHeaderMask);
        std::memcpy(f.evs_owner.get()+sub*codec::MipiRaw8Layout::kSubframeBytes,&word,8);
    }
    ++next_aps_index_;return true;
}
}
namespace Shimeta::codec {
size_t MipiRaw8Decoder::Decode(const uint8_t*,size_t,std::vector<EventCD>&,int) {return 0;}
}
