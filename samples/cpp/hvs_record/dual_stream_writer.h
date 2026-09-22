#pragma once
#include <shimetapi/io/event_writer.h>
#include <shimetapi/io/hybrid_writer.h>

// HybridWriter::open has only one geometry for both file headers. Keep EVS
// writing separate so an APS size change cannot corrupt the RAW geometry.
class DualStreamWriter {
public:
    bool open(const std::string& evsPath, const std::string& apsPath,
              uint32_t evsWidth, uint32_t evsHeight,
              uint32_t apsWidth, uint32_t apsHeight) {
        if (!events_.open(evsPath, evsWidth, evsHeight)) return false;
        // Use the SDK's AVI/timestamp writer unchanged. Its unused EVS header
        // goes to the Linux null device; no event payload is sent to it.
        if (!images_.open("/dev/null", apsPath, apsWidth, apsHeight)) {
            events_.close();
            return false;
        }
        return true;
    }
    bool writeFrame(const Shimeta::Frame& frame, const Shimeta::EvsTimestamp* ts) {
        if (frame.evs.size && (!frame.evs.data ||
            events_.writeRaw(frame.evs.data, frame.evs.size) != frame.evs.size)) return false;
        if (!frame.aps.size) return true;
        auto apsOnly = frame;
        apsOnly.evs = {};
        return images_.writeFrame(apsOnly, ts);
    }
    uint32_t apsFrameCount() const { return images_.apsFrameCount(); }
    void close() { events_.close(); images_.close(); }
private:
    Shimeta::io::EventWriter events_;
    Shimeta::io::HybridWriter images_;
};
