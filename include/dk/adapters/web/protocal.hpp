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
#include "spdlog/spdlog.h"

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
    virtual void send(nlohmann::json msg_json) = 0;
    // 主动关闭连接
    virtual void close() = 0;

    virtual size_t get_id() const = 0;

    // Send reliable message to this specific client
    virtual void send_reliable(nlohmann::json msg_json) = 0;

    // Handle received ACK from client
    virtual void handle_ack(uint64_t msg_id) = 0;
};

// ==========================================
// 3. WebSocket 事件回调结构体
// ==========================================
struct WsEndpoint {
    std::function<void(std::shared_ptr<WsConnection>)> on_open = [](auto) {};
    std::function<void(std::shared_ptr<WsConnection>, std::string)> on_message = [](auto, auto) {};
    std::function<void(std::shared_ptr<WsConnection>)> on_close = [](auto) {};
};

// ==========================================
// Connection & Reliable Message Manager
// ==========================================
class ConnectionManager {
   private:
    net::io_context& ioc_;
    std::mutex mutex_;
    std::unordered_map<size_t, std::shared_ptr<WsConnection>> connections_;
    struct PendingMsg {
        std::string msg_id;
        std::string payload;
        std::weak_ptr<WsConnection> conn;
        std::shared_ptr<net::steady_timer> timer;
    };
    std::unordered_map<std::string, std::shared_ptr<PendingMsg>> pending_msgs_;

   public:
    explicit ConnectionManager(net::io_context& ioc) : ioc_(ioc) {}

    void add(std::shared_ptr<WsConnection> conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_[conn->get_id()] = conn;
        spdlog::info("[ConnectionManager] Client connected. Total: {}", connections_.size());
    }

    void remove(std::shared_ptr<WsConnection> conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(conn->get_id());
        spdlog::info("[ConnectionManager] Client disconnected. Total: {}", connections_.size());
    }

    void publish(const nlohmann::json& msg_json) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, conn] : connections_) {
            conn->send(msg_json);
        }
    }

    // Broadcast a reliable message to all connected clients
    void publish_reliable(const nlohmann::json& msg_json) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, conn] : connections_) {
            // Each connection will copy the JSON, assign its own msg_id, and track it
            conn->send_reliable(msg_json);
        }
    }
};
}  // namespace dk