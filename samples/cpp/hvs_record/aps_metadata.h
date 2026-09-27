#pragma once
#include <shimetapi/core/frame.h>
#include <ostream>
#include <cmath>
#include <iomanip>
#include <locale>
#include <optional>
#include <stdexcept>

// Separate from Frame: appending fields to a struct constructed by a prebuilt
// library would read past its allocation. Only a frame-associated native backend
// may supply an actual exposure value here; a requested setting is insufficient.
struct ApsExposure {
    std::optional<double> actualUs;
};
inline void writeApsMetadata(std::ostream& out, uint64_t index,
                             const Shimeta::Frame& source,
                             const ApsExposure& exposure = {},
                             int64_t hostReceiveNs = -1) {
    if (exposure.actualUs && (!std::isfinite(*exposure.actualUs) || *exposure.actualUs <= 0))
        throw std::invalid_argument("Invalid frame exposure duration");
    out.imbue(std::locale::classic());
    out << std::setprecision(17)
        << "{\"schema_version\":1,\"aps_frame_index\":" << index
        << ",\"host_receive_monotonic_ns\":";
    if (hostReceiveNs >= 0) out << hostReceiveNs; else out << "null";
    out << ",\"timestamp_source\":\"host_callback_not_sensor_exposure\""
        << ",\"width\":" << source.width << ",\"height\":" << source.height
        << ",\"source_format\":\"" << (source.format == Shimeta::PixelFormat::Gray8 ? "Gray8" : "NV12")
        << "\",\"storage_format\":\"NV12\",\"exposure_time_us\":";
    if (exposure.actualUs) out << *exposure.actualUs;
    else out << "null";
    out << ",\"exposure_status\":\"" << (exposure.actualUs ? "frame_associated" : "unavailable")
        << "\",\"exposure_source\":\"" << (exposure.actualUs ? "native_frame_metadata" : "sdk_frame_has_no_exposure")
        << "\",\"paired_evs_timestamp_us\":";
    if (source.aps_evs_ts.valid) out << source.aps_evs_ts.processed_timestamp;
    else out << "null";
    out << "}\n";
    if (!out) throw std::runtime_error("Cannot write APS metadata");
}
inline bool apsStalled(uint64_t frames, double ageSeconds, double stallTimeout) {
    return frames > 0 && ageSeconds > stallTimeout;
}
