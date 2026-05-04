cmake_minimum_required(VERSION 3.10)
project(dk)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(THIRDPARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(AUTO_JSON_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_json.py")

# 用法: dk_create_json_library(LIB_NAME <虚拟库名> SCAN_DIR <扫描目录>)
function(dk_create_json_library LIB_NAME SCAN_DIR)
    # 0. 安全检查：确保所需变量和环境存在
    if(NOT DEFINED AUTO_JSON_SCRIPT OR NOT EXISTS "${AUTO_JSON_SCRIPT}")
        message(FATAL_ERROR "[Framework] AUTO_JSON_SCRIPT 未定义或文件不存在: ${AUTO_JSON_SCRIPT}")
    endif()
    
    if(NOT TARGET Python3::Interpreter)
        message(FATAL_ERROR "[Framework] 找不到 Python3::Interpreter，请先调用 find_package(Python3 COMPONENTS Interpreter REQUIRED)")
    endif()

    # 1. 设置生成的路径
    set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/dk_json_${LIB_NAME}")
    set(GENERATED_HEADER "${GENERATED_DIR}/dk_auto_json.hpp")

    # 2. 全局搜索 (使用 CONFIGURE_DEPENDS 确保增量编译感知文件变化)
    file(GLOB_RECURSE USER_HEADERS 
         CONFIGURE_DEPENDS 
         "${SCAN_DIR}/*.hpp" "${SCAN_DIR}/*.h"
    )

    # 打印调试信息
    message(STATUS "[Framework] 准备为 ${LIB_NAME} 生成 JSON 头文件，扫描目录: ${SCAN_DIR}")

    # 3. 自定义命令：调用 Python 生成头文件
    # 注意：这里直接在 COMMAND 后写完整命令，并用双引号包裹路径以防止路径带空格导致报错
    add_custom_command(
        OUTPUT "${GENERATED_HEADER}"
        COMMAND Python3::Interpreter "${AUTO_JSON_SCRIPT}" 
                -d "${SCAN_DIR}" 
                -o "${GENERATED_HEADER}" 
                -j "<nlohmann/json.hpp>"
        DEPENDS ${USER_HEADERS} "${AUTO_JSON_SCRIPT}"
        COMMENT "[Framework] 正在为 ${LIB_NAME} 扫描生成全局 JSON 宏..."
        VERBATIM # 强烈建议加上 VERBATIM，确保跨平台参数转义正确
    )

    # 4. 【核心黑魔法】创建一个自定义 Target 来驱动文件的生成
    add_custom_target(${LIB_NAME}_generator DEPENDS "${GENERATED_HEADER}")

    # 5. 创建一个现代 CMake 的 INTERFACE (接口) 库
    add_library(${LIB_NAME} INTERFACE)
    
    # 将接口库与生成器绑定，确保编译依赖此库的代码前，先运行 Python 脚本
    add_dependencies(${LIB_NAME} ${LIB_NAME}_generator)
    
    # 暴露出生成的头文件目录，任何链接这个库的程序都能直接 #include
    target_include_directories(${LIB_NAME} INTERFACE "${GENERATED_DIR}")

endfunction()

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
target_include_directories(dk 
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include 
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)

target_compile_features(dk INTERFACE cxx_std_17)


# 2. 开关：默认不编译测试 (当被 ROS 引入时)
option(DK_TEST "Build DK framework tests" OFF)
message(STATUS "DK_TEST = ${DK_TEST}")
if(DK_TEST)
    enable_testing()
    add_subdirectory(tests)
endif()