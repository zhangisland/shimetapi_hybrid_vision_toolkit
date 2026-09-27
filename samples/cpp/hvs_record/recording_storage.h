#pragma once
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <cstdlib>
#include <cstdint>
#ifdef __linux__
#include <sys/vfs.h>
#include <unistd.h>
#include <fcntl.h>
#endif

// Failures retain RAM files and the recovery marker; never recursively delete.
class RecordingStorage {
public:
    std::filesystem::path data, output;
    bool memory;
    RecordingStorage(const std::filesystem::path& destination,
                     const std::filesystem::path& ramRoot, uint64_t maxBytes)
        : data(destination), output(destination), memory(!ramRoot.empty()) {
        namespace fs = std::filesystem;
        constexpr uint64_t margin = 64ULL * 1024 * 1024;
        if (fs::space(output).available < maxBytes + margin)
            throw std::runtime_error("Insufficient destination space");
        if (!memory) return;
#ifdef __linux__
        struct statfs info {};
        if (statfs(ramRoot.c_str(), &info) != 0 || info.f_type != 0x01021994)
            throw std::runtime_error("RAM directory must be a mounted tmpfs");
        if (fs::space(ramRoot).available < maxBytes + margin || availableMemory() < maxBytes + margin)
            throw std::runtime_error("Insufficient tmpfs space; reduce --max-mib");
        std::string pattern = (ramRoot / "hvs-XXXXXX").string();
        if (!mkdtemp(pattern.data())) throw std::runtime_error("Cannot create RAM session");
        data = pattern;
        std::ofstream marker(output / "recording.storage");
        marker << data.string() << '\n';
        marker.close();
        if (!marker) throw std::runtime_error("Cannot write RAM recovery marker: " + data.string());
#else
        throw std::runtime_error("RAM recording requires Linux tmpfs");
#endif
    }
    static uint64_t availableMemory() {
#ifdef __linux__
        std::ifstream mem("/proc/meminfo");
        std::string key, rest;
        uint64_t kib;
        while (mem >> key) {
            if (key == "MemAvailable:") {
                if (mem >> kib) return kib * 1024;
                break;
            }
            std::getline(mem, rest);
        }
        throw std::runtime_error("Cannot read MemAvailable");
#else
        return UINT64_MAX;
#endif
    }
    static void syncFile(const std::filesystem::path& path) {
#ifdef __linux__
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("Cannot open for sync: " + path.string());
        int result = ::fsync(fd);
        ::close(fd);
        if (result != 0) throw std::runtime_error("fsync failed: " + path.string());
#endif
    }
    void publish() {
        namespace fs = std::filesystem;
        if (!memory) return;
        for (const auto* name : {"events.raw", "aps.avi", "aps.frames.jsonl"}) {
            auto source = data / name;
            auto temporary = output / (std::string(name) + ".partial");
            if (!fs::copy_file(source, temporary, fs::copy_options::none) ||
                fs::file_size(source) != fs::file_size(temporary))
                throw std::runtime_error("RAM copy verification failed");
            syncFile(temporary);
        }
        for (const auto* name : {"events.raw", "aps.avi", "aps.frames.jsonl"}) {
            if (fs::exists(output / name)) throw std::runtime_error("Destination file exists");
            fs::rename(output / (std::string(name) + ".partial"), output / name);
        }
        syncFile(output);
        fs::remove(data / "events.raw");
        fs::remove(data / "aps.avi");
        fs::remove(data / "aps.frames.jsonl");
        fs::remove(data);
        fs::remove(output / "recording.storage");
    }
};
