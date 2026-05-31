cmake_minimum_required(VERSION 3.14)
project(dk)

# ==============================================================================
# 编译选项与 ASAN 配置
# ==============================================================================
add_library(asan_config INTERFACE)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(asan_config INTERFACE 
            -fsanitize=address
            -fno-omit-frame-pointer
        )
        target_link_options(asan_config INTERFACE 
            -fsanitize=address
        )
    endif()
endif()

# ==============================================================================
# 依赖查找与拉取
# ==============================================================================
# 1. 系统级依赖与庞大库 (保留 find_package)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Boost REQUIRED)

# 2. 轻量级/纯头文件第三方库 (使用 FetchContent 拉取)
include(FetchContent)

# 拉取 spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0 # 目前非常稳定的 release 版本
)

# 拉取 nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)

# 一键让上述库可用
FetchContent_MakeAvailable(spdlog nlohmann_json)

# ==============================================================================
# JSON 自动生成宏函数
# ==============================================================================
set(AUTO_JSON_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_json.py")
set_property(GLOBAL PROPERTY AUTO_JSON_SCRIPT "${AUTO_JSON_SCRIPT}")

# 用法: dk_create_json_library(LIB_NAME <虚拟库名> SCAN_DIR <扫描目录>)
function(dk_create_json_library LIB_NAME SCAN_DIR)    
    get_property(AUTO_JSON_SCRIPT GLOBAL PROPERTY AUTO_JSON_SCRIPT)
    if(NOT DEFINED AUTO_JSON_SCRIPT OR NOT EXISTS "${AUTO_JSON_SCRIPT}")
        message(FATAL_ERROR "[Framework] AUTO_JSON_SCRIPT 未定义或文件不存在: ${AUTO_JSON_SCRIPT}")
    endif()
    
    if(NOT TARGET Python3::Interpreter)
        message(FATAL_ERROR "[Framework] 找不到 Python3::Interpreter")
    endif()

    set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/dk_json_${LIB_NAME}")
    set(GENERATED_HEADER "${GENERATED_DIR}/dk_auto_json.hpp")

    file(GLOB_RECURSE USER_HEADERS 
         CONFIGURE_DEPENDS 
         "${SCAN_DIR}/*.hpp" "${SCAN_DIR}/*.h"
    )

    message(STATUS "[Framework] 准备为 ${LIB_NAME} 生成 JSON 头文件，扫描目录: ${SCAN_DIR}")

    add_custom_command(
        OUTPUT "${GENERATED_HEADER}"
        COMMAND Python3::Interpreter "${AUTO_JSON_SCRIPT}" 
                -d "${SCAN_DIR}" 
                -o "${GENERATED_HEADER}" 
                -j "<nlohmann/json.hpp>"
        DEPENDS ${USER_HEADERS} "${AUTO_JSON_SCRIPT}"
        COMMENT "[Framework] 正在为 ${LIB_NAME} 扫描生成全局 JSON 宏..."
        VERBATIM
    )

    add_custom_target(${LIB_NAME}_generator DEPENDS "${GENERATED_HEADER}")
    add_library(${LIB_NAME} INTERFACE)
    add_dependencies(${LIB_NAME} ${LIB_NAME}_generator)
    target_include_directories(${LIB_NAME} INTERFACE "${GENERATED_DIR}")
endfunction()

# ==============================================================================
# DK 核心框架目标
# ==============================================================================
# 1. 声明为一个纯头文件库 (INTERFACE)
add_library(dk INTERFACE)

# 配置 Include 目录
target_include_directories(dk INTERFACE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include 
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty 
)

# 2. 链接依赖 (核心！)
# 注意：链接 spdlog::spdlog，使用者只要链接了 dk 就能直接拥有高亮日志功能
target_link_libraries(dk INTERFACE 
    Boost::headers 
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

# 3. 开启 C++17 特性
target_compile_features(dk INTERFACE cxx_std_17)

# 4. 根据不同操作系统，传递底层的系统链接要求给最终的使用者
if(UNIX AND NOT APPLE)
    target_link_libraries(dk INTERFACE dl)
elseif(WIN32)
    target_link_libraries(dk INTERFACE Dbgeng)
endif()

# ==============================================================================
# 测试模块配置
# ==============================================================================
option(DK_TEST "Build DK framework tests" OFF)
message(STATUS "DK_TEST = ${DK_TEST}")
if(DK_TEST)
    enable_testing()
    add_subdirectory(tests)
endif()