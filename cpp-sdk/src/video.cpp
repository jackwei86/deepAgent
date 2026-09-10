// 视频美颜：VideoCapture 逐帧处理 + VideoWriter 编码输出
#include "deepagent/beauty.h"

#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace deepagent {

int beauty_video(const std::string& input_path, const std::string& output_path,
                 const BeautyParams& params, const ProgressReporter& reporter) {
    report_progress(reporter, 2, "打开视频");
    cv::VideoCapture cap;
    if (!cap.open(input_path)) {
        report_log(reporter, "无法打开输入视频: " + input_path);
        return 20;
    }
    const int width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    const int height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    double fps = cap.get(cv::CAP_PROP_FPS);
    if (fps <= 1.0 || fps > 240.0) fps = 25.0; // 部分容器读不到帧率时兜底
    const int total = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));

    cv::VideoWriter writer(output_path, cv::VideoWriter::fourcc('a', 'v', 'c', '1'), fps,
                           cv::Size(width, height));
    if (!writer.isOpened())
        writer.open(output_path, cv::VideoWriter::fourcc('m', 'p', '4', 'v'), fps,
                    cv::Size(width, height));
    if (!writer.isOpened()) {
        report_log(reporter, "无法创建输出视频: " + output_path);
        return 21;
    }
    report_log(reporter, "打开视频 " + std::to_string(width) + "x" + std::to_string(height) +
                "@" + std::to_string(static_cast<int>(std::lround(fps))) + "fps" +
                (total > 0 ? (" 共" + std::to_string(total) + "帧") : ""));

    cv::Mat frame, processed;
    int index = 0;
    int lastReported = -5;
    while (cap.read(frame)) {
        processed = detail::beautify_mat(frame, params);
        writer.write(processed);
        ++index;
        // 每帧或进度步进≥5%时上报，避免事件洪泛
        int percent = total > 0 ? static_cast<int>(index * 100.0 / total)
                                : static_cast<int>((index % 50) * 2);
        if (total > 0 && percent - lastReported >= 5) {
            report_progress(reporter, percent,
                            "处理帧 " + std::to_string(index) + "/" + std::to_string(total));
            lastReported = percent;
        }
    }
    if (index == 0) {
        report_log(reporter, "未读取到任何视频帧");
        return 22;
    }
    report_progress(reporter, 98, "封装输出");
    writer.release();
    cap.release();
    report_progress(reporter, 100, "完成 共处理" + std::to_string(index) + "帧");
    return 0;
}

} // namespace deepagent
