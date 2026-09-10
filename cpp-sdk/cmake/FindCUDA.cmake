# FindCUDA 桩模块（仅本项目使用）
#
# 本项目不编译、不链接任何 CUDA 代码；但所用自定义 OpenCV 包（带 CUDA 支持的预编译版）
# 的 OpenCVConfig.cmake 内部会执行 find_host_package(CUDA <ver> EXACT REQUIRED)，
# 目的只是版本校验。本桩令该校验直接通过，从而：
#   1) 不强制安装特定版本的 CUDA Toolkit；
#   2) 避免机器上多版本 CUDA（11.6/11.8/12.1/12.3/12.8）被 CMake 误检。
#
# 注意：若将来需要链接 OpenCV 的 CUDA 模块（cudaarithm/cudafilters 等），
# 请删除本桩并安装与 OpenCV 构建一致的 CUDA Toolkit。
set(CUDA_FOUND TRUE)
if(CUDA_FIND_VERSION)
    set(CUDA_VERSION_STRING "${CUDA_FIND_VERSION}")
else()
    set(CUDA_VERSION_STRING "11.6")
endif()
set(CUDA_TOOLKIT_ROOT_DIR "CUDA-STUB-NOT-USED")
set(CUDA_NVCC_EXECUTABLE "CUDA-STUB-NOT-USED")
set(CUDA_INCLUDE_DIRS "")
set(CUDA_LIBRARIES "")
set(CUDA_CUDART_LIBRARY "")
