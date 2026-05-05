#pragma once
#define SPDLOG_COMPILED
#define FMT_HEADER_ONLY
#include <iostream>
#include <memory>

#include "spdlog/sinks/basic_file_sink.h"     // 基础文件输出
#include "spdlog/sinks/daily_file_sink.h"     // 按天轮转的文件输出
#include "spdlog/sinks/stdout_color_sinks.h"  // 终端带颜色的输出
#include "spdlog/spdlog.h"

namespace dk {
inline void init_logger() {
    try {
        // 1. 创建控制台 Sink（多线程安全版）
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::info);         // 控制台只显示 info 及以上
        console_sink->set_pattern("[%H:%M:%S] [%^%l%$] %v");  // 设置带颜色的格式
        // 2. 创建文件 Sink（按天轮转，多线程安全版）
        auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>("logs/my_log.txt", 23, 59);
        file_sink->set_level(spdlog::level::trace);  // 文件中记录所有细节
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [thread %t] %v");
        // 3. 将多个 Sink 组合成一个 Logger
        std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
        auto logger = std::make_shared<spdlog::logger>("multi_sink", sinks.begin(), sinks.end());

        // 4. 设置为全局默认 Logger
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::trace);  // 全局最低级别
        spdlog::flush_on(spdlog::level::err);     // 遇到 error 时立即刷新到磁盘
    } catch (const spdlog::spdlog_ex& ex) {
        std::cout << "Log initialization failed: " << ex.what() << std::endl;
    }
}
}  // namespace dk