// 图像合成接口：前景(可带 Alpha 通道)贴到底图指定位置
#pragma once

#include <string>
#include "progress.h"

namespace deepagent {

struct CompositeParams {
    int x = -1;          // 前景左上角 X(像素)；<0 且 center=true 时自动居中
    int y = -1;          // 前景左上角 Y(像素)
    double scale = 1.0;  // 前景缩放比例，>0
    bool center = true;  // 未显式给出 x/y 时是否自动居中
};

// 图像合成：fg_path 支持 8UC3/8UC4(带 Alpha，如抠图产物)，
// 按 Alpha 混合写到 base_path 副本的指定区域后输出。
int composite_image(const std::string& base_path, const std::string& fg_path,
                    const std::string& output_path, const CompositeParams& params,
                    const ProgressReporter& reporter);

} // namespace deepagent
