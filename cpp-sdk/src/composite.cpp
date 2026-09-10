#include "deepagent/composite.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace deepagent {

int composite_image(const std::string& base_path, const std::string& fg_path,
                    const std::string& output_path, const CompositeParams& params,
                    const ProgressReporter& reporter) {
    report_progress(reporter, 10, "加载底图");
    cv::Mat base = cv::imread(base_path, cv::IMREAD_COLOR);
    if (base.empty()) {
        report_log(reporter, "无法读取底图: " + base_path);
        return 20;
    }

    report_progress(reporter, 30, "加载前景");
    // UNCHANGED 读取：兼容抠图产物等带 Alpha 的 PNG
    cv::Mat fg = cv::imread(fg_path, cv::IMREAD_UNCHANGED);
    if (fg.empty()) {
        report_log(reporter, "无法读取前景图: " + fg_path);
        return 20;
    }
    if (fg.channels() == 1)
        cv::cvtColor(fg, fg, cv::COLOR_GRAY2BGR);
    cv::Mat fgBGR, fgAlpha;
    if (fg.channels() == 4) {
        std::vector<cv::Mat> ch;
        cv::split(fg, ch);
        cv::merge(std::vector<cv::Mat>{ch[0], ch[1], ch[2]}, fgBGR);
        ch[3].convertTo(fgAlpha, CV_32F, 1.0 / 255.0);
    } else {
        fgBGR = fg;
        fgAlpha = cv::Mat(fg.size(), CV_32F, cv::Scalar(1.0));
    }

    report_progress(reporter, 50, "缩放前景");
    double scale = params.scale > 0.0 ? params.scale : 1.0;
    if (std::abs(scale - 1.0) > 1e-6) {
        cv::resize(fgBGR, fgBGR, cv::Size(), scale, scale, cv::INTER_AREA);
        cv::resize(fgAlpha, fgAlpha, cv::Size(), scale, scale, cv::INTER_AREA);
    }

    int x = params.x, y = params.y;
    if (x < 0 || y < 0) {
        if (params.center) {
            x = (base.cols - fgBGR.cols) / 2;
            y = (base.rows - fgBGR.rows) / 2;
        } else {
            x = 0;
            y = 0;
        }
    }

    // 裁剪前景超出底图边界的部分
    cv::Rect fgRect(0, 0, fgBGR.cols, fgBGR.rows);
    cv::Rect dstRect(x, y, fgBGR.cols, fgBGR.rows);
    const cv::Rect baseRect(0, 0, base.cols, base.rows);
    const cv::Rect visible = dstRect & baseRect;
    if (visible.empty()) {
        report_log(reporter, "前景完全位于底图之外，请检查 x/y 参数");
        return 22; // ERR_INVALID_PARAM
    }
    dstRect = visible;
    fgRect = cv::Rect(visible.x - x, visible.y - y, visible.width, visible.height);
    report_log(reporter, "贴图位置 [" + std::to_string(dstRect.x) + "," +
                std::to_string(dstRect.y) + "] 尺寸 " + std::to_string(dstRect.width) + "x" +
                std::to_string(dstRect.height));

    report_progress(reporter, 75, "Alpha 混合");
    cv::Mat roi = base(dstRect);
    cv::Mat fg32, base32;
    fgBGR(fgRect).convertTo(fg32, CV_32F);
    roi.convertTo(base32, CV_32F);
    cv::Mat alpha = fgAlpha(fgRect);
    std::vector<cv::Mat> a3 = {alpha, alpha, alpha};
    cv::Mat alpha3;
    cv::merge(a3, alpha3);
    cv::Mat blended = fg32.mul(alpha3) + base32.mul(cv::Scalar::all(1.0) - alpha3);
    blended.convertTo(roi, CV_8U);

    report_progress(reporter, 90, "写出结果文件");
    if (!cv::imwrite(output_path, base)) {
        report_log(reporter, "无法写出结果图像: " + output_path);
        return 21;
    }
    report_progress(reporter, 100, "完成");
    return 0;
}

} // namespace deepagent
