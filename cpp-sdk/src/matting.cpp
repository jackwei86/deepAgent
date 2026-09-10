#include "deepagent/matting.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace deepagent {

namespace {
cv::Rect initial_rect(const cv::Mat& img, const MattingParams& p) {
    if (p.rect_x >= 0 && p.rect_y >= 0 && p.rect_w > 0 && p.rect_h > 0) {
        cv::Rect r(p.rect_x, p.rect_y, p.rect_w, p.rect_h);
        r &= cv::Rect(0, 0, img.cols, img.rows);
        return r;
    }
    // 默认取画面中央 60% 区域为可能的前景
    const int w = static_cast<int>(img.cols * 0.6);
    const int h = static_cast<int>(img.rows * 0.6);
    return cv::Rect((img.cols - w) / 2, (img.rows - h) / 2, w, h);
}

cv::Mat grabcut_mask(const cv::Mat& img, const MattingParams& p,
                     const ProgressReporter& reporter) {
    cv::Mat mask(img.size(), CV_8UC1, cv::Scalar(cv::GC_BGD));
    const cv::Rect rect = initial_rect(img, p);
    cv::Mat bgd, fgd;
    report_log(reporter, "GrabCut 分割，初始矩形 [" + std::to_string(rect.x) + "," +
                std::to_string(rect.y) + " " + std::to_string(rect.width) + "x" +
                std::to_string(rect.height) + "]");
    report_progress(reporter, 45, "GrabCut 迭代分割");
    cv::grabCut(img, mask, rect, bgd, fgd, 5, cv::GC_INIT_WITH_RECT);

    cv::Mat binMask;
    mask.copyTo(binMask);
    binMask = (mask == cv::GC_FGD) | (mask == cv::GC_PR_FGD);
    return binMask;
}

cv::Mat chroma_mask(const cv::Mat& img, const ProgressReporter& reporter) {
    report_progress(reporter, 45, "HSV 绿幕键控");
    cv::Mat hsv;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    std::vector<cv::Mat> ch;
    cv::split(hsv, ch);
    // 绿幕: H∈[35,85] 且饱和度较高；对亮度极端的像素保守处理
    cv::Mat mask;
    cv::inRange(hsv, cv::Scalar(35, 60, 40), cv::Scalar(85, 255, 255), mask);
    // 轻度形态学平滑边缘
    cv::erode(mask, mask, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));
    cv::dilate(mask, mask, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));
    cv::bitwise_not(mask, mask); // 1 = 前景
    return mask;
}
} // namespace

int matting_image(const std::string& input_path, const std::string& output_path,
                  const MattingParams& params, const ProgressReporter& reporter) {
    report_progress(reporter, 5, "加载图像");
    // 以 UNCHANGED 读取，保留可能的 Alpha
    cv::Mat src = cv::imread(input_path, cv::IMREAD_UNCHANGED);
    if (src.empty()) {
        report_log(reporter, "无法读取输入图像: " + input_path);
        return 20;
    }
    if (src.channels() == 4)
        cv::cvtColor(src, src, cv::COLOR_BGRA2BGR);
    else if (src.channels() == 1)
        cv::cvtColor(src, src, cv::COLOR_GRAY2BGR);

    cv::Mat fgMask = (params.mode == MattingMode::CHROMA) ? chroma_mask(src, reporter)
                                                          : grabcut_mask(src, params, reporter);
    report_log(reporter, "前景像素占比 " +
                std::to_string(static_cast<int>(100.0 * cv::countNonZero(fgMask) /
                                                (fgMask.rows * fgMask.cols))) + "%");

    report_progress(reporter, 80, "合成输出");
    bool ok = false;
    if (params.background == "transparent") {
        std::vector<cv::Mat> bgr;
        cv::split(src, bgr);
        std::vector<cv::Mat> outCh = {bgr[0], bgr[1], bgr[2], fgMask};
        cv::Mat out;
        cv::merge(outCh, out);
        ok = cv::imwrite(output_path, out);
    } else {
        cv::Mat out = src.clone();
        const cv::Scalar bgColor = (params.background == "green") ? cv::Scalar(0, 255, 0)
                                                                  : cv::Scalar(255, 255, 255);
        out.setTo(bgColor, ~fgMask);
        ok = cv::imwrite(output_path, out);
    }
    if (!ok) {
        report_log(reporter, "无法写出结果图像: " + output_path);
        return 21;
    }
    report_progress(reporter, 100, "完成");
    return 0;
}

} // namespace deepagent
