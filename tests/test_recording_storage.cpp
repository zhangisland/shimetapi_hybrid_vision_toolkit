#include "../samples/cpp/hvs_record/recording_storage.h"
#include <chrono>
namespace fs = std::filesystem;
void require(bool ok) { if (!ok) throw std::runtime_error("storage assertion failed"); }
int main() {
    auto root=fs::temp_directory_path() / ("hvs-storage-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    for (bool fail : {false,true}) {
        auto out=root/(fail?"fail":"ok"); auto ram=root/(fail?"ram-fail":"ram-ok");
        fs::create_directory(out); fs::create_directory(ram);
        RecordingStorage storage(out,{},1024);
        // Exercise publication separately from Linux-only tmpfs mount checks.
        storage.memory=true; storage.data=ram;
        std::ofstream(ram/"events.raw") << "events";
        std::ofstream(ram/"aps.avi") << "images";
        std::ofstream(ram/"aps.frames.jsonl") << "metadata";
        std::ofstream(out/"recording.storage") << ram.string();
        if (fail) std::ofstream(out/"aps.avi.partial") << "keep";
        if (fail) {
            try { storage.publish(); return 1; } catch(const fs::filesystem_error&) {}
            require(fs::exists(ram/"events.raw") && fs::exists(ram/"aps.avi"));
            require(fs::exists(out/"recording.storage") && !fs::exists(out/"events.raw"));
        } else {
            storage.publish(); require(fs::file_size(out/"events.raw")==6);
            require(fs::file_size(out/"aps.avi")==6 && !fs::exists(ram));
            require(!fs::exists(out/"recording.storage"));
            require(fs::file_size(out/"aps.frames.jsonl")==8);
        }
    }
    // Only remove this test's uniquely created directory.
    fs::remove_all(root);
}
