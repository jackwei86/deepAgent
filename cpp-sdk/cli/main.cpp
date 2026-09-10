// deepagent-cv 命令行入口
//
// 两种调用方式：
//  1) 参数模式:  deepagent-cv.exe beauty --input a.jpg --output b.jpg --strength 0.7
//  2) 任务单模式: deepagent-cv.exe run --task task.json   (规范见 docs/任务信息JSON格式.md)
//
// stdout 按行输出 JSON 事件(start/progress/log/done/error)，供上层解析展示进度：
//  {"event":"progress","percent":35,"stage":"处理帧 88/250"}
//  {"event":"done","output":"...","elapsed_ms":1200}
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include "deepagent/beauty.h"
#include "deepagent/composite.h"
#include "deepagent/matting.h"
#include "deepagent/version.h"

using json = nlohmann::json;

namespace {

enum ExitCode {
    OK = 0,
    ERR_USAGE = 1,
    ERR_INPUT_READ = 20,
    ERR_OUTPUT_WRITE = 21,
    ERR_INVALID_PARAM = 22,
    ERR_TASK_PARSE = 30,
    ERR_UNSUPPORTED_TASK = 31,
};

struct CliContext {
    std::string task_id; // 任务单模式时回填，事件中回传
};

std::string now_hhmmss() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void emit(const json& j) {
    std::cout << j.dump() << "\n";
    std::cout.flush();
}

void emit_log(const CliContext& ctx, const std::string& message) {
    json e = {{"event", "log"}, {"message", "[" + now_hhmmss() + "] " + message}};
    if (!ctx.task_id.empty()) e["task_id"] = ctx.task_id;
    emit(e);
}

void emit_progress(const CliContext& ctx, int percent, const std::string& stage) {
    json e = {{"event", "progress"}, {"percent", percent}, {"stage", stage}};
    if (!ctx.task_id.empty()) e["task_id"] = ctx.task_id;
    emit(e);
}

void emit_start(const CliContext& ctx, const std::string& task, const std::string& input,
                const std::string& output) {
    json e = {{"event", "start"}, {"task", task}, {"input", input}, {"output", output}};
    if (!ctx.task_id.empty()) e["task_id"] = ctx.task_id;
    emit(e);
}

deepagent::ProgressReporter make_reporter(const CliContext& ctx) {
    return {[&ctx](int p, const std::string& s) { emit_progress(ctx, p, s); },
            [&ctx](const std::string& m) { emit_log(ctx, m); }};
}

struct TaskSpec {
    std::string command; // beauty | matting | composite | video-beauty
    std::string input;   // 主输入(composite 时为底图)
    std::string input2;  // composite 前景
    std::string output;
    deepagent::BeautyParams beauty;
    deepagent::MattingParams matting;
    deepagent::CompositeParams composite;
};

double get_num(const json& obj, const char* key, double def) {
    if (obj.contains(key) && obj.at(key).is_number()) return obj.at(key).get<double>();
    return def;
}

std::string get_str(const json& obj, const char* key, const std::string& def) {
    if (obj.contains(key) && obj.at(key).is_string()) return obj.at(key).get<std::string>();
    return def;
}

// 从任务单 JSON 构造执行规格
int spec_from_task_json(const std::string& task_file, TaskSpec& spec, CliContext& ctx) {
    FILE* f = nullptr;
    if (fopen_s(&f, task_file.c_str(), "rb") != 0 || !f) {
        std::cerr << "cannot open task file: " << task_file << std::endl;
        return ERR_TASK_PARSE;
    }
    std::string content;
    char buf[8192];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
    fclose(f);

    json task;
    try {
        task = json::parse(content);
    } catch (const std::exception& ex) {
        std::cerr << "task json parse error: " << ex.what() << std::endl;
        return ERR_TASK_PARSE;
    }
    if (task.contains("task_id") && task.at("task_id").is_string())
        ctx.task_id = task.at("task_id").get<std::string>();

    const json& t = task.contains("task") ? task.at("task") : json::object();
    const std::string ttype = get_str(t, "type", "");
    const json& params = t.contains("parameters") ? t.at("parameters") : json::object();

    if (ttype == "image_beauty" || ttype == "video_beauty") {
        spec.command = (ttype == "image_beauty") ? "beauty" : "video-beauty";
        spec.beauty.strength = get_num(params, "strength", 0.7);
        if (params.contains("denoise") && params.at("denoise").is_boolean())
            spec.beauty.denoise = params.at("denoise").get<bool>();
        spec.beauty.whiten = get_num(params, "whiten", 0.0);
    } else if (ttype == "image_matting") {
        spec.command = "matting";
        const std::string mode = get_str(params, "mode", "grabcut");
        spec.matting.mode = (mode == "chroma") ? deepagent::MattingMode::CHROMA
                                               : deepagent::MattingMode::GRABCUT;
        spec.matting.background = get_str(params, "background", "transparent");
        if (params.contains("rect") && params.at("rect").is_object()) {
            const json& r = params.at("rect");
            spec.matting.rect_x = static_cast<int>(get_num(r, "x", -1));
            spec.matting.rect_y = static_cast<int>(get_num(r, "y", -1));
            spec.matting.rect_w = static_cast<int>(get_num(r, "width", -1));
            spec.matting.rect_h = static_cast<int>(get_num(r, "height", -1));
        }
    } else if (ttype == "image_composite") {
        spec.command = "composite";
        spec.composite.x = static_cast<int>(get_num(params, "x", -1));
        spec.composite.y = static_cast<int>(get_num(params, "y", -1));
        spec.composite.scale = get_num(params, "scale", 1.0);
        if (params.contains("center") && params.at("center").is_boolean())
            spec.composite.center = params.at("center").get<bool>();
    } else {
        std::cerr << "unsupported task.type: " << ttype << std::endl;
        return ERR_UNSUPPORTED_TASK;
    }

    if (!task.contains("inputs") || !task.at("inputs").is_array() || task.at("inputs").empty()) {
        std::cerr << "task.inputs missing" << std::endl;
        return ERR_TASK_PARSE;
    }
    for (const auto& in : task.at("inputs")) {
        const std::string role = get_str(in, "role", "source");
        const std::string path = get_str(in, "path", "");
        if (role == "source" || (spec.command != "composite" && spec.input.empty()))
            spec.input = path;
        else if (role == "background" && spec.command == "composite")
            spec.input = path;
        else if (role == "foreground" && spec.command == "composite")
            spec.input2 = path;
    }
    if (spec.command == "composite" && spec.input2.empty()) {
        std::cerr << "composite task requires a foreground input" << std::endl;
        return ERR_TASK_PARSE;
    }

    if (task.contains("output"))
        spec.output = get_str(task.at("output"), "path", "");
    if (spec.output.empty()) {
        std::cerr << "task.output.path missing" << std::endl;
        return ERR_TASK_PARSE;
    }
    return OK;
}

int execute_spec(const TaskSpec& spec, const CliContext& ctx) {
    const auto t0 = std::chrono::steady_clock::now();
    emit_start(ctx, spec.command, spec.input, spec.output);
    const deepagent::ProgressReporter reporter = make_reporter(ctx);

    int rc = ERR_USAGE;
    if (spec.command == "beauty") {
        rc = deepagent::beautify_image(spec.input, spec.output, spec.beauty, reporter);
    } else if (spec.command == "matting") {
        rc = deepagent::matting_image(spec.input, spec.output, spec.matting, reporter);
    } else if (spec.command == "composite") {
        rc = deepagent::composite_image(spec.input, spec.input2, spec.output, spec.composite,
                                        reporter);
    } else if (spec.command == "video-beauty") {
        rc = deepagent::beauty_video(spec.input, spec.output, spec.beauty, reporter);
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    if (rc == 0) {
        emit({{"event", "done"}, {"output", spec.output}, {"elapsed_ms", ms},
              {"exit_code", 0}});
    } else {
        json e = {{"event", "error"}, {"code", rc}, {"elapsed_ms", ms}};
        if (!ctx.task_id.empty()) e["task_id"] = ctx.task_id;
        emit(e);
    }
    return rc;
}

void print_usage() {
    std::cout <<
        "deepagent-cv " DEEPAGENT_CV_VERSION_STRING " - 图像/视频处理演示 SDK 命令行\n"
        "用法:\n"
        "  deepagent-cv beauty --input <图> --output <图> [--strength 0.7] [--denoise 1] [--whiten 0]\n"
        "  deepagent-cv matting --input <图> --output <png> [--mode grabcut|chroma] [--background transparent|white|green]\n"
        "  deepagent-cv composite --base <底图> --fg <前景> --output <图> [--x N] [--y N] [--scale 1.0] [--center 1]\n"
        "  deepagent-cv video-beauty --input <视频> --output <mp4> [--strength 0.7]\n"
        "  deepagent-cv run --task <task.json>     # 按《任务信息JSON格式》执行任务单\n"
        "  deepagent-cv version\n";
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    if (argc < 2) {
        print_usage();
        return ERR_USAGE;
    }
    const std::string cmd = argv[1];
    if (cmd == "version") {
        emit({{"event", "version"}, {"version", DEEPAGENT_CV_VERSION_STRING}});
        return OK;
    }
    if (cmd == "info") {
        // 探测素材元数据: deepagent-cv info --input <file>
        std::string in;
        for (int i = 2; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], "--input") == 0) { in = argv[i + 1]; break; }
        if (in.empty()) return ERR_USAGE;
        cv::Mat img = cv::imread(in, cv::IMREAD_UNCHANGED);
        if (!img.empty()) {
            emit({{"event", "info"}, {"kind", "image"}, {"path", in},
                  {"width", img.cols}, {"height", img.rows}, {"channels", img.channels()}});
            return OK;
        }
        cv::VideoCapture cap;
        if (cap.open(in)) {
            json e = {{"event", "info"}, {"kind", "video"}, {"path", in},
                      {"width", (int)cap.get(cv::CAP_PROP_FRAME_WIDTH)},
                      {"height", (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT)},
                      {"fps", cap.get(cv::CAP_PROP_FPS)}};
            const int frames = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
            e["frame_count"] = frames;
            if (frames > 0 && cap.get(cv::CAP_PROP_FPS) > 0)
                e["duration_ms"] = static_cast<int>(frames * 1000.0 / cap.get(cv::CAP_PROP_FPS));
            emit(e);
            return OK;
        }
        emit({{"event", "info"}, {"kind", "unknown"}, {"path", in}});
        return ERR_INPUT_READ;
    }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_usage();
        return OK;
    }

    TaskSpec spec;
    CliContext ctx;

    if (cmd == "run") {
        std::string task_file;
        for (int i = 2; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--task") == 0) {
                task_file = argv[i + 1];
                break;
            }
        }
        if (task_file.empty()) {
            std::cerr << "run 需要 --task <task.json>" << std::endl;
            return ERR_USAGE;
        }
        const int prc = spec_from_task_json(task_file, spec, ctx);
        if (prc != OK) return prc;
        return execute_spec(spec, ctx);
    }

    // ---- 参数模式解析 ----
    spec.command = cmd;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << std::endl;
                std::exit(ERR_USAGE);
            }
            return argv[++i];
        };
        if (a == "--input") spec.input = next("--input");
        else if (a == "--output") spec.output = next("--output");
        else if (a == "--base") spec.input = next("--base");
        else if (a == "--fg") spec.input2 = next("--fg");
        else if (a == "--strength") spec.beauty.strength = std::stod(next("--strength"));
        else if (a == "--denoise") spec.beauty.denoise = std::stoi(next("--denoise")) != 0;
        else if (a == "--whiten") spec.beauty.whiten = std::stod(next("--whiten"));
        else if (a == "--mode") spec.matting.mode = (next("--mode") == "chroma")
                                       ? deepagent::MattingMode::CHROMA
                                       : deepagent::MattingMode::GRABCUT;
        else if (a == "--background") spec.matting.background = next("--background");
        else if (a == "--x") spec.composite.x = std::stoi(next("--x"));
        else if (a == "--y") spec.composite.y = std::stoi(next("--y"));
        else if (a == "--scale") spec.composite.scale = std::stod(next("--scale"));
        else if (a == "--center") spec.composite.center = std::stoi(next("--center")) != 0;
        else {
            std::cerr << "unknown argument: " << a << std::endl;
            print_usage();
            return ERR_USAGE;
        }
    }

    if (spec.input.empty() || spec.output.empty() ||
        (spec.command == "composite" && spec.input2.empty())) {
        print_usage();
        return ERR_USAGE;
    }
    if (cmd != "beauty" && cmd != "matting" && cmd != "composite" && cmd != "video-beauty") {
        std::cerr << "unknown command: " << cmd << std::endl;
        print_usage();
        return ERR_USAGE;
    }
    return execute_spec(spec, ctx);
}
