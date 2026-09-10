# 构建后复制 OpenCV Release 运行时 DLL 到可执行文件目录
# 用法: cmake -DOPENCV_BIN_DIR=<bin> -DEXE_DIR=<dir> -P copy_opencv_dlls.cmake

if(NOT DEFINED OPENCV_BIN_DIR OR NOT DEFINED EXE_DIR)
    message(FATAL_ERROR "copy_opencv_dlls.cmake 需要 OPENCV_BIN_DIR 与 EXE_DIR")
endif()

set(_modules core imgproc imgcodecs videoio photo)
set(_copied 0)
foreach(_mod IN LISTS _modules)
    file(GLOB _found "${OPENCV_BIN_DIR}/opencv_${_mod}4*.dll")
    # 本项目仅 Debug 构建：只保留 Debug 后缀(d) DLL，与所链接的 Debug 导入库一致
    list(FILTER _found INCLUDE REGEX "d[0-9]*\\.dll$")
    foreach(_dll IN LISTS _found)
        file(COPY "${_dll}" DESTINATION "${EXE_DIR}")
        math(EXPR _copied "${_copied} + 1")
    endforeach()
endforeach()

# videoio 的 ffmpeg 插件 DLL(Debug 版)
file(GLOB _ffmpeg "${OPENCV_BIN_DIR}/opencv_videoio_ffmpeg*_64d.dll")
foreach(_dll IN LISTS _ffmpeg)
    file(COPY "${_dll}" DESTINATION "${EXE_DIR}")
endforeach()

message(STATUS "Copied ${_copied} OpenCV debug DLLs to ${EXE_DIR}")
