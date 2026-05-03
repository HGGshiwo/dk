cmake_minimum_required(VERSION 3.10)
project(dk)
add_compile_options(-g -fno-omit-frame-pointer)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# ==========================================================
# 核心技巧：创建一个专门用于开启 ASan 的“虚拟库” (INTERFACE 库)
# ==========================================================
add_library(asan_config INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # 全部用 INTERFACE 关键字绑定到 asan_config 这个虚拟库上
    target_compile_options(asan_config INTERFACE 
        -fsanitize=address 
        -fno-omit-frame-pointer
    )
    target_link_options(asan_config INTERFACE 
        -fsanitize=address
    )
endif()


# 1. 声明为一个纯头文件库 (INTERFACE)
add_library(dk INTERFACE)
target_include_directories(dk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(dk INTERFACE cxx_std_17)


# 2. 开关：默认不编译测试 (当被 ROS 引入时)
option(DK_TEST "Build DK framework tests" OFF)
message(STATUS "DK_TEST = ${DK_TEST}")
if(DK_TEST)
    enable_testing()
    add_subdirectory(tests)
endif()