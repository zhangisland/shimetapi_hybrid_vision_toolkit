#include "../samples/cpp/live_record_display/native_record.h"
#include <iterator>
#include <map>
std::atomic<bool> g_running{true};
namespace fake {extern bool files,fail;extern int delayMs;}
#define CHECK(x) do {if(!(x)) throw std::runtime_error(#x);} while(0)
int main() {
    namespace fs=std::filesystem;
    auto root=fs::temp_directory_path()/("native-record-test-"+std::to_string(hv_live::hostNs()));fs::create_directory(root);
    fake::files=true;
    {
        hv_live::NativeRecorder r;CHECK(r.start((root/"session").string()));
        Shimeta::Frame f;f.width=f.height=32;f.format=Shimeta::PixelFormat::NV12;
        f.aps_owner.reset(new uint8_t[1536]);f.aps={f.aps_owner.get(),1536};
        auto oversized=f; oversized.aps.size=65ULL*1024*1024;
        r.push({1,oversized}); // must reject before any payload access
        for(int i=0;i<55;++i) r.push({1000000000+int64_t(i*1e9/13.75),f});
        r.shutdown();CHECK(!r.failed());CHECK(r.dropped==1);
    }
    auto session=fs::directory_iterator(root)->path();
    std::ifstream input(session/"summary.txt");std::map<std::string,std::string> summary;std::string line;
    while(std::getline(input,line)){auto eq=line.find('=');if(eq!=std::string::npos)summary[line.substr(0,eq)]=line.substr(eq+1);}
    CHECK(summary["status"]=="complete" && summary["queue_dropped"]=="1");
    CHECK(summary["aps_frames"]=="55");CHECK(std::abs(std::stod(summary["aps_observed_fps"])-13.75)<1e-4);
    CHECK(std::abs(std::stod(summary["avi_duration_seconds"])-4)<1e-4);
    std::ifstream avi(session/"aps.avi",std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(avi)),{});avi.close();
    auto read=[&](size_t at){uint32_t n=0;for(int i=0;i<4;++i)n|=uint32_t(uint8_t(bytes[at+i]))<<(8*i);return n;};
    auto at=bytes.find("strh")+8;CHECK(read(at+24)==1375000 && read(at+20)==100000 && read(at+32)==55);
    CHECK(std::abs(double(read(at+32))*read(at+20)/read(at+24)-4)<1e-6);
    {
        hv_live::NativeRecorder r;fake::fail=true;CHECK(r.start((root/"failure").string()));
        Shimeta::Frame f;f.width=f.height=32;f.format=Shimeta::PixelFormat::NV12;f.aps_owner.reset(new uint8_t[1536]);f.aps={f.aps_owner.get(),1536};
        r.push({1,f});r.shutdown();CHECK(r.failed());fake::fail=false;
    }
    input.close();fs::remove_all(root);
    std::cout<<"Async recording: 55 frames at measured 13.75 FPS -> 4s AVI; write failure propagated\n";
}
