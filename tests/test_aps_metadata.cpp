#include "../samples/cpp/hvs_record/aps_metadata.h"
#include <sstream>
#include <limits>
void require(bool condition) { if (!condition) throw std::runtime_error("metadata assertion"); }
int main() {
    Shimeta::Frame f; f.width=1632; f.height=1224; f.format=Shimeta::PixelFormat::NV12;
    std::ostringstream unknown; writeApsMetadata(unknown,0,f);
    require(unknown.str().find("\"exposure_time_us\":null")!=std::string::npos);
    require(unknown.str().find("sdk_frame_has_no_exposure")!=std::string::npos);
    require(unknown.str().find("\"paired_evs_timestamp_us\":null")!=std::string::npos);
    f.format=Shimeta::PixelFormat::Gray8; f.aps_evs_ts.valid=true; f.aps_evs_ts.processed_timestamp=123456;
    std::ostringstream known; writeApsMetadata(known,1,f,ApsExposure{5000.25});
    require(known.str().find("\"exposure_time_us\":5000.25")!=std::string::npos);
    require(known.str().find("\"aps_frame_index\":1")!=std::string::npos);
    require(known.str().find("Gray8")!=std::string::npos);
    require(known.str().find("123456")!=std::string::npos);
    try { writeApsMetadata(known,2,f,ApsExposure{std::numeric_limits<double>::quiet_NaN()}); return 1; }
    catch(const std::invalid_argument&) {}
    std::ostringstream failed; failed.setstate(std::ios::badbit);
    try { writeApsMetadata(failed,2,f); return 2; } catch(const std::runtime_error&) {}
    require(apsStalled(1,3.9,2)); // User's 4-second recording with one initial frame.
    require(!apsStalled(120,0.03,2));
    require(!apsStalled(0,3.9,2)); // Startup uses its separate timeout.
    require(!apsStalled(1,3.9,5)); // Explicit low-rate tolerance.
}
