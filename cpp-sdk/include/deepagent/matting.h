// 图像抠图(前景提取)接口
#pragma once

#include <string>
#include "progress.h"

namespace deepagent {

enum class MattingMode {
    GRABCUT, // 通用场景：以初始矩形进行 GrabCut 迭代分割，取画面中央主体
    CHROMA   // 绿幕场景：HSV 色域键控，适合纯色背景素材
};

struct MattingParams {
    MattingMode mode = MattingMode::GRABCUT;
    // GrabCut 初始矩形(像素)；均为负值时取画面中央 60% 区域
    int rect_x = -1, rect_y = -1, rect_w = -1, rect_h = -1;
    // 抠出前景的背景填充：transparent(RGBA PNG) / white / green
    std::string background = "transparent";
};

// 图像抠图：分离前景并按 params.background 指定背景输出。
// transparent 输出 8UC4 RGBA(建议 output 为 .png)；white/green 输出 8UC3。
int matting_image(const std::string& input_path, const std::string& output_path,
                  const MattingParams& params, const ProgressReporter& reporter);

} // namespace deepagent
