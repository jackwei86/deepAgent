// 图像/视频美颜接口
#pragma once

#include <string>
#include "progress.h"

#include <opencv2/core.hpp>

namespace deepagent {

struct BeautyParams {
    double strength = 0.7; // 美颜强度 0.0-1.0，映射双边滤波直径与混合权重
    bool   denoise  = true; // 附加轻度降噪(中值滤波去除孤立噪点)
    double whiten   = 0.0; // 美白程度 0.0-1.0，0 为关闭
};

// 图像美颜：双边滤波磨皮 + 可选降噪/美白，结果写出到 output_path。
// 返回 0 成功；非 0 为错误码(见 cli/main.cpp 错误码约定)。
int beautify_image(const std::string& input_path, const std::string& output_path,
                   const BeautyParams& params, const ProgressReporter& reporter);

// 视频美颜：逐帧执行与 beautify_image 相同的处理，按帧上报进度。
int beauty_video(const std::string& input_path, const std::string& output_path,
                 const BeautyParams& params, const ProgressReporter& reporter);

namespace detail {
// Mat 级美颜实现，供图像/视频两条路径复用；输入须为 8UC3 BGR。
cv::Mat beautify_mat(const cv::Mat& src, const BeautyParams& params);
} // namespace detail

} // namespace deepagent
