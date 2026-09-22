#pragma once
#include <shimetapi/core/frame.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <stdexcept>
#include <string>

namespace hv_player {
inline bool validApsBayerMode(const std::string& mode) {
    return mode=="none" || mode=="rggb" || mode=="bggr" ||
           mode=="grbg" || mode=="gbrg" || mode=="compare";
}
inline int apsBayerCode(const std::string& mode) {
    // OpenCV's legacy two-letter BGR aliases are counterintuitive:
    // RGGB2BGR == BG2BGR. These aliases also work with the bundled older headers.
    if (mode=="rggb") return cv::COLOR_BayerBG2BGR;
    if (mode=="bggr") return cv::COLOR_BayerRG2BGR;
    if (mode=="grbg") return cv::COLOR_BayerGB2BGR;
    if (mode=="gbrg") return cv::COLOR_BayerGR2BGR;
    throw std::invalid_argument("Unknown Bayer pattern: " + mode);
}

// The recording stores unprocessed Bayer RAW (VIN RAW10 >> 2 as Gray8):
// no black-level subtraction, no white balance, no gamma. Demosaicing
// alone therefore shows a strongly tinted, dark image. These parameters
// approximate what a real ISP would do before color output.
struct ApsColorEnhance {
    double black_level = 16.0;   // sensor RAW10 BL ~64 after >>2
    bool gray_world_wb = true;   // per-frame gray-world white balance
    double wb_gain_min = 0.25;   // clamp per-channel gains
    double wb_gain_max = 4.0;
    double gamma = 0.4545;       // 1/2.2, lifts shadows
};

inline void enhanceApsColor(cv::Mat& img, const ApsColorEnhance& p) {
    if (img.empty() || img.type()!=CV_8UC3) return;
    // 1) Black-level subtraction + full-range stretch via 8-bit LUT.
    if (p.black_level > 0.0) {
        const double scale = 255.0 / (255.0 - p.black_level);
        cv::Mat bl_lut(1, 256, CV_8UC1);
        for (int i = 0; i < 256; ++i) {
            double v = (double(i) - p.black_level) * scale;
            bl_lut.at<uint8_t>(i) = cv::saturate_cast<uint8_t>(v < 0.0 ? 0 : v);
        }
        cv::LUT(img, bl_lut, img);
    }
    // 2) Gray-world white balance: scale each channel so all channel means
    // match the green mean. Gains are clamped so a monochrome scene cannot
    // blow up any channel. WB normalizes the global cast but cannot fix a
    // wrong Bayer phase (R/B swapped objects stay swapped) -- that is
    // exactly what makes the compare view a reliable phase test.
    if (p.gray_world_wb) {
        cv::Scalar means = cv::mean(img);
        cv::Mat channels[3];
        cv::split(img, channels);
        for (int c = 0; c < 3; ++c) {
            if (means[c] <= 1.0) continue;
            double gain = means[1] / means[c];  // normalize to green mean
            gain = std::min(std::max(gain, p.wb_gain_min), p.wb_gain_max);
            if (std::abs(gain - 1.0) < 0.01) continue;
            channels[c].convertTo(channels[c], -1, gain, 0.0);
        }
        cv::merge(channels, 3, img);
    }
    // 3) Gamma correction via LUT.
    if (p.gamma > 0.0 && std::abs(p.gamma - 1.0) > 1e-3) {
        cv::Mat gamma_lut(1, 256, CV_8UC1);
        for (int i = 0; i < 256; ++i)
            gamma_lut.at<uint8_t>(i) = cv::saturate_cast<uint8_t>(
                255.0 * std::pow(double(i) / 255.0, p.gamma));
        cv::LUT(img, gamma_lut, img);
    }
}
inline cv::Mat decodeApsForDisplay(const Shimeta::Frame& f, const std::string& mode) {
    if (!validApsBayerMode(mode)) throw std::invalid_argument("Invalid APS Bayer mode");
    if (f.width<4 || f.height<4 || f.width%2 || f.height%2 ||
        f.format!=Shimeta::PixelFormat::NV12 || !f.aps.data)
        throw std::runtime_error("Invalid APS NV12 frame");
    const size_t pixels=size_t(f.width)*f.height;
    if (f.aps.size<pixels*3/2) throw std::runtime_error("Truncated APS NV12 frame");
    cv::Mat y(f.height,f.width,CV_8UC1,const_cast<uint8_t*>(f.aps.data));
    cv::Mat result;
    if (mode=="none") {
        cv::Mat uv(f.height/2,f.width/2,CV_8UC2,const_cast<uint8_t*>(f.aps.data+pixels));
        cv::cvtColorTwoPlane(y,uv,result,cv::COLOR_YUV2BGR_NV12);
        return result;
    }
    // Our Gray8 recorder copies Bayer8 bytes directly to Y and sets UV to 128.
    // Demosaic that unscaled Y, before YUV range conversion or image resizing.
    if (!std::all_of(f.aps.data+pixels,f.aps.data+pixels*3/2,
                     [](uint8_t v){return v==128;}))
        throw std::runtime_error("Bayer mode requires untouched Gray8-in-NV12 (neutral UV)");
    if (mode!="compare") {
        cv::demosaicing(y,result,apsBayerCode(mode));
        enhanceApsColor(result, ApsColorEnhance{});
        return result;
    }
    const char* patterns[]={"rggb","bggr","grbg","gbrg"};
    result=cv::Mat::zeros(f.height,f.width,CV_8UC3);
    for (int i=0;i<4;++i) {
        cv::Mat color,tile;
        cv::demosaicing(y,color,apsBayerCode(patterns[i]));
        cv::resize(color,tile,cv::Size(f.width/2,f.height/2),0,0,cv::INTER_AREA);
        enhanceApsColor(tile, ApsColorEnhance{});
        const std::string label=patterns[i];
        cv::putText(tile,label,cv::Point(10,28),cv::FONT_HERSHEY_SIMPLEX,
                    0.7,cv::Scalar(0,0,0),4);
        cv::putText(tile,label,cv::Point(10,28),cv::FONT_HERSHEY_SIMPLEX,
                    0.7,cv::Scalar(255,255,255),1);
        tile.copyTo(result(cv::Rect((i%2)*(f.width/2),(i/2)*(f.height/2),f.width/2,f.height/2)));
    }
    return result;
}
}
