#pragma once
#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>
#include <variant>
// 包含自动生成的 JSON 宏头文件 (文档中提到的)

#include "dk/adapters/web.hpp"
#include "dk/engine.hpp"
#include "dk/future.hpp"

// ============================================================================
// 1. 定义数据结构 (使用框架要求的宏标记)
// ============================================================================

// @JSON_ENABLE
struct TestLoginResult {
    bool success;
    std::string token;
};

// @JSON_ENABLE
struct AsyncLoginEvent : public dk::AsyncEvent<TestLoginResult> {
    std::string username;
    std::string password;
};
