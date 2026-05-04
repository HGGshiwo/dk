#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>  // 新增
#include <deque>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>

#include "dk/future.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

namespace dk {

// 前置声明
template <typename AdapterType>
class HttpSession;

// ==========================================
// 1. 统一协议接口 (支持 HTTP, WS, SSE 等)
// ==========================================
template <typename AdapterType>
class IProtocolHandler {
   public:
    virtual ~IProtocolHandler() = default;

    // session: 当前的 HTTP 会话 (可以用来回传响应，或者移出 Socket 升级协议)
    // req: 初始的 HTTP 请求
    virtual void handle(std::shared_ptr<HttpSession<AdapterType>> session, http::request<http::string_body> req) = 0;
};

// ==========================================
// 2. WebSocket 连接句柄 (暴露给业务层)
// ==========================================
class WsConnection : public std::enable_shared_from_this<WsConnection> {
   public:
    virtual ~WsConnection() = default;
    // 线程安全地发送消息
    virtual void send(const std::string& msg) = 0;
    // 主动关闭连接
    virtual void close() = 0;
};

// ==========================================
// 3. WebSocket 事件回调结构体
// ==========================================
struct WsEndpoint {
    std::function<void(std::shared_ptr<WsConnection>)> on_open = [](auto) {};
    std::function<void(std::shared_ptr<WsConnection>, std::string)> on_message = [](auto, auto) {};
    std::function<void(std::shared_ptr<WsConnection>)> on_close = [](auto) {};
};
}  // namespace dk