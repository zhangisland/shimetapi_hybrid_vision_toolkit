#include <shimetapi/hv/camera.h>
#include "dual_stream_writer.h"
#include "recording_storage.h"
#include "aps_metadata.h"
#include "avi_timing.h"
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static volatile std::sig_atomic_t stopping = 0;
static void onSignal(int) { stopping = 1; }
struct Options {
    fs::path output;
    fs::path ramRoot = "/dev/shm";
    bool memory = true;
    double seconds = 0, timeout = 10, apsStallTimeout = 2;
    int width = 1632, height = 1224;
    int evsWidth = 768, evsHeight = 608;
    uint64_t maxBytes = 1024ULL * 1024 * 1024;
};
static Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--help") {
            std::cout << "hv_hvs_record --output NEW_DIRECTORY [--seconds 0] [--timeout 10] [--aps-stall-timeout 2] "
                         "[--aps-width 1632 --aps-height 1224] [--evs-width 768 --evs-height 608] [--max-mib 1024] [--storage memory|disk] [--ram-dir /dev/shm]\n"
                         "Stop: SIGINT, SIGTERM, or create NEW_DIRECTORY/stop.request\n";
            std::exit(0);
        }
        if (++i == argc) throw std::runtime_error("Missing value: " + key);
        const std::string value = argv[i];
        size_t used = 0;
        if (key == "--output") { o.output = value; continue; }
        if (key == "--ram-dir") { o.ramRoot = value; continue; }
        if (key == "--storage") {
            if (value == "disk") o.memory = false;
            else if (value == "memory") o.memory = true;
            else throw std::runtime_error("storage must be memory or disk");
            continue;
        }
        const double n = std::stod(value, &used);
        if (used != value.size() || !std::isfinite(n)) throw std::runtime_error("Invalid number: " + value);
        if (key == "--seconds") o.seconds = n;
        else if (key == "--timeout") o.timeout = n;
        else if (key == "--aps-stall-timeout") o.apsStallTimeout = n;
        else if (key == "--width" || key == "--height" || key == "--aps-width" ||
                 key == "--aps-height" || key == "--evs-width" || key == "--evs-height") {
            if (n < 2 || n > 8192 || n != std::floor(n) || int(n) % 2)
                throw std::runtime_error("Dimensions must be even integers in [2,8192]");
            if (key == "--evs-width") o.evsWidth = int(n);
            else if (key == "--evs-height") o.evsHeight = int(n);
            else if (key == "--width" || key == "--aps-width") o.width = int(n);
            else o.height = int(n);
        } else if (key == "--max-mib") {
            if (n < 1 || n > 2048) throw std::runtime_error("max-mib must be in [1,2048] (AVI RIFF limit)");
            o.maxBytes = uint64_t(n * 1024 * 1024);
        } else throw std::runtime_error("Unknown option: " + key);
    }
    if (o.output.empty() || o.seconds < 0 || o.timeout <= 0 || o.apsStallTimeout <= 0)
        throw std::runtime_error("Require --output, seconds >= 0, timeout > 0");
    if (!o.memory) o.ramRoot.clear();
    else if (o.ramRoot.empty()) throw std::runtime_error("RAM directory cannot be empty");
    return o;
}
struct Recorder {
    const Options& opt;
    DualStreamWriter writer;
    std::ofstream metadata;
    std::mutex mutex;
    bool active = false, limit = false;
    std::string error;
    uint64_t packets = 0, aps = 0, payload = 0, evsBytes = 0, paired = 0, gray = 0;
    std::vector<uint8_t> nv12;
    Clock::time_point firstApsReceived{}, lastApsReceived{};
    double callbackWorkMs = 0, callbackWorkMaxMs = 0;
    Clock::time_point lastEvs = Clock::now(), lastAps = Clock::now();
    // Retained owners prevent the pool from reusing the previous buffer address.
    std::shared_ptr<uint8_t[]> lastEvsOwner, lastApsOwner;
    const uint8_t* lastEvsPtr = nullptr;
    const uint8_t* lastApsPtr = nullptr;
    explicit Recorder(const Options& o) : opt(o) {}
    void consume(const Shimeta::Frame& source) noexcept {
        const auto received = Clock::now();
        std::lock_guard<std::mutex> lock(mutex);
        if (!active || !error.empty() || limit) return;
        try {
            auto f = source;
            const bool newEvs = f.evs.data && f.evs.size &&
                (f.evs.data != lastEvsPtr || !f.evs_owner);
            const bool newAps = f.aps.data && f.aps.size &&
                (f.aps.data != lastApsPtr || !f.aps_owner);
            if (!newEvs) f.evs = {};
            if (!newAps) f.aps = {};
            if (!newEvs && !newAps) return;

            if (newAps) {
                if (f.width != opt.width || f.height != opt.height)
                    throw std::runtime_error("APS dimensions differ from --aps-width/--aps-height: " +
                        std::to_string(f.width) + "x" + std::to_string(f.height));
                const size_t pixels = size_t(f.width) * f.height;
                if (f.format == Shimeta::PixelFormat::Gray8 && f.aps.size == pixels) {
                    nv12.resize(pixels * 3 / 2, 128);
                    std::copy_n(f.aps.data, pixels, nv12.data());
                    f.aps = {nv12.data(), nv12.size()};
                    f.format = Shimeta::PixelFormat::NV12;

                } else if (f.format != Shimeta::PixelFormat::NV12 || f.aps.size != pixels * 3 / 2) {
                    throw std::runtime_error("APS must be packed NV12 or Gray8; padded/unknown format rejected");
                }
            }
            const uint64_t bytes = f.evs.size + f.aps.size;
            if (payload + bytes > opt.maxBytes) { limit = true; return; }
            // Preserve SDK pairing. Never invent exact sync from the current EVS packet.
            const auto* ts = f.aps_evs_ts.valid ? &f.aps_evs_ts : nullptr;
            if (!writer.writeFrame(f, ts)) throw std::runtime_error("DualStreamWriter.writeFrame failed");
            payload += bytes;
            const auto now = Clock::now();
            if (newEvs) {
                ++packets; evsBytes += f.evs.size; lastEvs = now;
                lastEvsOwner = source.evs_owner; lastEvsPtr = source.evs.data;
            }
            if (newAps) {
                const auto count = writer.apsFrameCount();
                if (count != aps + 1) throw std::runtime_error("Writer did not append exactly one APS frame");
                aps = count; lastAps = now;
                if (aps == 1) firstApsReceived = received;
                lastApsReceived = received;
                if (source.format == Shimeta::PixelFormat::Gray8) ++gray;
                writeApsMetadata(metadata, aps - 1, source, {},
                    std::chrono::duration_cast<std::chrono::nanoseconds>(received.time_since_epoch()).count());
                if (ts) ++paired;
                lastApsOwner = source.aps_owner; lastApsPtr = source.aps.data;
            }
            const double work = std::chrono::duration<double,std::milli>(Clock::now()-received).count();
            callbackWorkMs += work;
            callbackWorkMaxMs = std::max(callbackWorkMaxMs, work);
        } catch (const std::exception& e) { error = e.what(); }
        catch (...) { error = "Unknown exception in recording callback"; }
    }
};
static int run(const Options& o) {
    if (!fs::create_directory(o.output)) throw std::runtime_error("Output directory already exists");
    RecordingStorage storage(o.output, o.ramRoot, o.maxBytes);
    std::cout << "Recording data directory: " << storage.data << std::endl;
    Recorder r(o);
    Shimeta::hv::Camera camera;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    bool initialized = false, startAttempted = false;
    std::string reason = "stop";
    try {
        if (!r.writer.open((storage.data / "events.raw").string(), (storage.data / "aps.avi").string(),
                           o.evsWidth, o.evsHeight, o.width, o.height)) throw std::runtime_error("Cannot open recording files");
        std::cout << "EVS geometry=" << o.evsWidth << "x" << o.evsHeight
                  << ", APS geometry=" << o.width << "x" << o.height << std::endl;
        r.metadata.open(storage.data / "aps.frames.jsonl");
        if (!r.metadata) throw std::runtime_error("Cannot open APS metadata");
        std::cout << "APS exposure unavailable in bundled Frame API; recording null with provenance.\n";
        Shimeta::hv::DeviceConfig config;
        config.backend = Shimeta::hv::Backend::MipiHvs;
        if (!camera.Init(config)) throw std::runtime_error("Camera.Init failed");
        initialized = true;
        camera.SetFrameCallback([&r](const Shimeta::Frame& f) { r.consume(f); });
        { std::lock_guard<std::mutex> lock(r.mutex); r.active = true; }
        startAttempted = true;
        if (!camera.StartStream()) throw std::runtime_error("Camera.StartStream failed");
        const auto begin = Clock::now();
        { std::lock_guard<std::mutex> lock(r.mutex); r.lastEvs = r.lastAps = begin; }
        auto report = begin;
        std::cout << "Recording MipiHvs. Ctrl+C or hvs.py stop --output <session> to finish.\n";
        while (!stopping && !fs::exists(o.output / "stop.request")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto now = Clock::now();
            std::lock_guard<std::mutex> lock(r.mutex);
            if (!r.error.empty()) break;
            if (r.limit) { reason = "size_limit"; break; }
            if (storage.memory && (fs::space(storage.data).available < 32ULL * 1024 * 1024 ||
                                   RecordingStorage::availableMemory() < 64ULL * 1024 * 1024)) {
                reason = "ram_space_limit"; break;
            }
            if (apsStalled(r.aps, std::chrono::duration<double>(now - r.lastAps).count(), o.apsStallTimeout)) {
                r.error = "APS stalled after first frame; partial data retained; inspect ISP pipeline";
                break;
            }
            if (std::chrono::duration<double>(now - r.lastEvs).count() > o.timeout ||
                std::chrono::duration<double>(now - r.lastAps).count() > o.timeout) {
                r.error = "APS or EVS absent/stalled; inspect MIPI/ISP logs and X5 runtime libraries";
                break;
            }
            if (now - report >= std::chrono::seconds(1)) {
                std::cout << "EVS packets=" << r.packets << " APS frames=" << r.aps
                          << " payload MiB=" << r.payload / (1024 * 1024) << std::endl;
                report = now;
            }
            if (o.seconds > 0 && std::chrono::duration<double>(now - begin).count() >= o.seconds) {
                reason = "duration"; break;
            }
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(r.mutex); r.error = e.what();
    }
    { std::lock_guard<std::mutex> lock(r.mutex); r.active = false; }
    if (startAttempted) camera.StopStream();
    if (initialized) camera.Destroy();
    r.writer.close();
    double measuredFps = 0;
    if (r.aps > 1) {
        const double span = std::chrono::duration<double>(r.lastApsReceived-r.firstApsReceived).count();
        if (span > 0) measuredFps = double(r.aps-1)/span;
    }
    if (measuredFps > 0) {
        try {
            setAviFrameRate(storage.data / "aps.avi", measuredFps);
            std::cout << "APS observed callback FPS=" << measuredFps
                      << "; AVI timing corrected (host timing, not sensor timing)" << std::endl;
        } catch (const std::exception& e) { r.error += std::string("; ") + e.what(); }
    }
    if (r.metadata.is_open()) {
        r.metadata.close();
        if (!r.metadata) r.error += "; APS metadata close failed";
    }
    if (r.error.empty() && apsStalled(r.aps,
            std::chrono::duration<double>(Clock::now() - r.lastAps).count(), o.apsStallTimeout))
        r.error = "APS stalled at capture end; partial data retained";
    if (r.error.empty() && (!r.packets || !r.aps)) r.error = "Incomplete HVS recording: one or both streams have zero frames";
    if (r.error.empty() && (fs::file_size(storage.data / "events.raw") <= r.evsBytes ||
        fs::file_size(storage.data / "aps.avi") < r.aps * uint64_t(o.width) * o.height * 3 / 2))
        r.error = "Output size check failed";
    try {
        std::cout << "Capture stopped; finalizing recording files..." << std::endl;
        storage.publish();
    } catch (const std::exception& e) {
        r.error += std::string("; finalization failed: ") + e.what() +
                   "; recovery data: " + storage.data.string();
    }
    std::ofstream summary(o.output / "summary.tmp");
    summary << "status=" << (r.error.empty() ? "complete" : "failed") << '\n'
            << "storage=" << (storage.memory ? "memory" : "disk") << '\n'
            << "reason=" << (r.error.empty() ? reason : r.error) << '\n'
            << "evs_packets=" << r.packets << "\naps_frames=" << r.aps
            << "\naps_paired_timestamps=" << r.paired << "\ngray8_frames=" << r.gray
            << "\nevs_width=" << o.evsWidth << "\nevs_height=" << o.evsHeight
            << "\naps_width=" << o.width << "\naps_height=" << o.height
            << "\naps_observed_fps=" << measuredFps
            << "\naps_avi_timing_source=" << (measuredFps > 0 ? "host_callback_average" : "default_30_insufficient_frames")
            << "\ncallback_work_total_ms=" << r.callbackWorkMs
            << "\ncallback_work_max_ms=" << r.callbackWorkMaxMs
            << "\naps_storage_format=NV12"
            << "\naps_metadata_file=aps.frames.jsonl"
            << "\naps_exposure_status=unavailable"
            << "\naps_exposure_source=sdk_frame_has_no_exposure"
            << "\naps_stall_timeout_seconds=" << o.apsStallTimeout
            << "\npayload_bytes=" << r.payload << '\n';
    summary.close();
    if (!summary) throw std::runtime_error("Failed to write summary.txt");
    fs::rename(o.output / "summary.tmp", o.output / "summary.txt");
    std::cout << "EVS packets=" << r.packets << ", APS frames=" << r.aps << '\n';
    if (!r.error.empty()) { std::cerr << "FAILED: " << r.error << '\n'; return 2; }
    std::cout << "Saved " << o.output << " (" << reason << ")\n";
    if (r.paired != r.aps) std::cout << "Some APS frames lack sensor pairing; exact synchronized replay is unavailable for those frames.\n";
    return 0;
}
int main(int argc, char** argv) {
    try { return run(parse(argc, argv)); }
    catch (const std::exception& e) { std::cerr << "ERROR: " << e.what() << '\n'; return 1; }
}
