#include "deepagent/beauty.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace deepagent {

namespace {
constexpr int kMaxWorkingSide = 1280; // 工作分辨率上限，控制滤波耗时

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
} // namespace

namespace detail {

cv::Mat beautify_mat(const cv::Mat& src, const BeautyParams& params) {
    CV_Assert(!src.empty() && src.type() == CV_8UC3);

    // 下采样到工作分辨率加速双边滤波
    double scale = 1.0;
    const int longSide = std::max(src.cols, src.rows);
    if (longSide > kMaxWorkingSide) scale = static_cast<double>(kMaxWorkingSide) / longSide;

    cv::Mat working = src;
    if (scale < 1.0)
        cv::resize(src, working, cv::Size(), scale, scale, cv::INTER_AREA);

    const double s = clamp01(params.strength);
    // 双边滤波直径 5..21，sigma 随强度增大；直径取奇数
    const int d = (2 * static_cast<int>(std::lround(2.0 + s * 8.0))) + 1;
    const double sigma = 40.0 + s * 90.0;

    cv::Mat smooth;
    cv::bilateralFilter(working, smooth, d, sigma, sigma);

    if (params.denoise) {
        cv::Mat denoised;
        cv::medianBlur(smooth, denoised, 3);
        smooth = denoised;
    }

    // 磨皮融合：保留原图边缘细节，避免整体发糊
    const double alpha = 0.55 + s * 0.35; // 滤波结果权重
    cv::Mat blended;
    cv::addWeighted(working, 1.0 - alpha, smooth, alpha, 0.0, blended);

    // 美白：轻度提升亮度与对比度
    const double w = clamp01(params.whiten);
    if (w > 0.0) {
        cv::Mat brightened;
        blended.convertTo(brightened, -1, 1.0 + w * 0.12, w * 10.0);
        blended = brightened;
    }

    // 放回原始尺寸
    if (scale < 1.0)
        cv::resize(blended, blended, src.size(), 0, 0, cv::INTER_LINEAR);
    return blended;
}

} // namespace detail

int beautify_image(const std::string& input_path, const std::string& output_path,
                   const BeautyParams& params, const ProgressReporter& reporter) {
    report_progress(reporter, 5, "加载图像");
    cv::Mat src = cv::imread(input_path, cv::IMREAD_COLOR);
    if (src.empty()) {
        report_log(reporter, "无法读取输入图像: " + input_path);
        return 20; // ERR_INPUT_READ
    }
    report_log(reporter, "加载图像 " + std::to_string(src.cols) + "x" + std::to_string(src.rows));

    report_progress(reporter, 40, "双边滤波磨皮 strength=" +
                    std::to_string(static_cast<int>(clamp01(params.strength) * 100)) + "%");
    cv::Mat result = detail::beautify_mat(src, params);

    report_progress(reporter, 85, "写出结果文件");
    std::vector<int> write_params;
    if (output_path.size() >= 4 &&
        (output_path.compare(output_path.size() - 4, 4, ".jpg") == 0 ||
         output_path.compare(output_path.size() - 5, 5, ".jpeg") == 0)) {
        write_params = {cv::IMWRITE_JPEG_QUALITY, 95};
    }
    if (!cv::imwrite(output_path, result, write_params)) {
        report_log(reporter, "无法写出结果图像: " + output_path);
        return 21; // ERR_OUTPUT_WRITE
    }
    report_progress(reporter, 100, "完成");
    return 0;
}

} // namespace deepagent
