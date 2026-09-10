cmake_minimum_required(VERSION 3.20)

project(deepagent-cv VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OpenCV 位置优先取环境变量/缓存变量 OpenCV_DIR，可在构建脚本中覆盖
if(NOT DEFINED OpenCV_DIR OR OpenCV_DIR STREQUAL "")
    set(OpenCV_DIR "G:/openv4.11.0._install" CACHE PATH "OpenCVConfig.cmake 所在目录")
endif()

# 桩模块：绕过自定义 OpenCV 包的 CUDA EXACT 版本校验（本项目不使用 CUDA）
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# 纯 Debug 链路(AGENTS.md)：与 OpenCV Debug DLL 的 CRT/STL ABI 完全一致。
# 不得映射到 Release 库——MSVC Debug/Release 的 std::string 布局不同，
# 跨 DLL 传 cv::String 会导致路径错乱(debug_build_guard 即为此而设)。
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio photo)

add_executable(deepagent-cv
    cli/main.cpp
    src/beauty.cpp
    src/matting.cpp
    src/composite.cpp
    src/video.cpp
)

target_include_directories(deepagent-cv PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(deepagent-cv PRIVATE ${OpenCV_LIBS})

# /utf-8: 源码与执行字符集均为 UTF-8(进度/日志含中文)
target_compile_options(deepagent-cv PRIVATE /utf-8 /W3 /MP)

# 本项目按 AGENTS.md 仅做 Debug 编译，链接 OpenCV Debug 导入库

# 构建后将 OpenCV Release 运行时 DLL 复制到可执行文件目录，免去部署时配置 PATH
add_custom_command(TARGET deepagent-cv POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DOPENCV_BIN_DIR=${OpenCV_DIR}/x64/vc16/bin
        -DEXE_DIR=$<TARGET_FILE_DIR:deepagent-cv>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/copy_opencv_dlls.cmake
    COMMENT "Copying OpenCV runtime DLLs"
    VERBATIM)
