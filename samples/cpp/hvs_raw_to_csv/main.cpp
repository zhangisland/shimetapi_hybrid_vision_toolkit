// Export hvs_record RAW8 payloads with the same SDK decoder as the player.
#include <shimetapi/io/hybrid_reader.h>
#include <shimetapi/codec/mipi_raw8_codec.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <iomanip>
#include <array>
#include <algorithm>

class Progress {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start_ = Clock::now(), last_ = start_;
public:
    void update(uint64_t done, uint64_t total, bool final = false) {
        auto now = Clock::now();
        if (!final && done && now - last_ < std::chrono::milliseconds(250)) return;
        last_ = now;
        double ratio = total ? std::min(1.0, double(done) / total) : 1.0;
        if (!final) ratio = std::min(ratio, 0.999);
        int filled = int(ratio * 30);
        double elapsed = std::chrono::duration<double>(now - start_).count();
        std::cerr << "\r[" << std::string(filled, '=') << std::string(30-filled, ' ')
                  << "] " << std::fixed << std::setprecision(1) << ratio * 100 << "%  ETA ";
        if (done) std::cerr << std::setprecision(0) << elapsed * (total > done ? total-done : 0) / done << "s";
        else std::cerr << "--";
        std::cerr << "          " << std::flush;
        if (final) std::cerr << '\n';
    }
};

// Measure the payload separately so a truncated final subframe cannot disappear
// behind HybridReader's boolean EOF result.
uint64_t payloadSize(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open input");
    std::string line;
    bool end = false;
    while (input.peek() == '%') {
        if (!std::getline(input, line) || line.size() > 65536)
            throw std::runtime_error("Invalid RAW header");
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "% end") { end = true; break; }
    }
    if (!end) throw std::runtime_error("Expected hvs_record RAW text header ending in % end");
    auto offset = input.tellg();
    if (offset < 0) throw std::runtime_error("Invalid RAW payload offset");
    auto size = std::filesystem::file_size(path) - uint64_t(offset);
    if (size % Shimeta::codec::MipiRaw8Layout::kSubframeBytes)
        throw std::runtime_error("Truncated RAW8 subframe");
    return size;
}

void littleEndian(std::ostream& out, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) out.put(char((value >> (8*i)) & 255));
}

int main(int argc, char** argv) {
    const bool columns = argc == 4 && std::string(argv[3]) == "--columns";
    if (argc != 3 && !columns) {
        std::cerr << "Usage: hv_hvs_raw_to_csv events.raw events.csv\n"
                     "Input: hvs_record output; timestamp: sensor microseconds.\n";
        return 1;
    }
    try {
        const auto total = payloadSize(argv[1]);
        if (std::filesystem::exists(argv[2]))
            throw std::runtime_error("Output exists; choose a new CSV path");
        Shimeta::io::HybridReader reader;
        if (!reader.open(argv[1], ""))
            throw std::runtime_error("Cannot open input recording");
        std::ofstream csv;
        std::array<std::ofstream, 4> arrays;
        if (columns) {
            std::filesystem::create_directory(argv[2]);
            const char* names[] = {"x", "y", "polarity", "timestamp"};
            for (int i = 0; i < 4; ++i) {
                arrays[i].open(std::filesystem::path(argv[2]) / names[i], std::ios::binary);
                if (!arrays[i]) throw std::runtime_error("Cannot create column file");
            }
        } else {
            csv.open(argv[2], std::ios::binary);
            if (!csv) throw std::runtime_error("Cannot create CSV");
            csv << "x,y,polarity,timestamp\n";
        }
        Progress progress;
        progress.update(0, total);
        Shimeta::codec::MipiRaw8Decoder decoder;
        Shimeta::Frame frame;
        std::vector<Shimeta::EventCD> events;
        uint64_t count = 0, subframes = 0;
        // Read one complete subframe at a time: independent of camera packet size.
        constexpr size_t bytes = Shimeta::codec::MipiRaw8Layout::kSubframeBytes;
        while (reader.readEvsPacket(frame, bytes)) {
            if (frame.evs.size != bytes)
                throw std::runtime_error("Truncated RAW8 subframe; CSV is incomplete");
            events.clear();
            decoder.Decode(frame.evs.data, frame.evs.size, events);
            for (const auto& e : events) {
                if (columns) {
                    littleEndian(arrays[0], e.x, 2);
                    littleEndian(arrays[1], e.y, 2);
                    littleEndian(arrays[2], e.polarity, 1);
                    littleEndian(arrays[3], uint64_t(e.t), 8);
                } else csv << e.x << ',' << e.y << ',' << int(e.polarity) << ',' << e.t << '\n';
            }
            if (columns) {
                for (auto& out : arrays) if (!out) throw std::runtime_error("Column write failed");
            } else if (!csv) throw std::runtime_error("CSV write failed (check disk space)");
            count += events.size();
            ++subframes;
            progress.update(subframes * bytes, total);
        }
        if (subframes * bytes != total) throw std::runtime_error("Incomplete RAW8 read");
        if (columns) {
            for (auto& out : arrays) {
                out.close();
                if (!out) throw std::runtime_error("Column close failed");
            }
        } else {
            csv.close();
            if (!csv) throw std::runtime_error("CSV close failed");
        }
        progress.update(total, total, true);
        return count ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}