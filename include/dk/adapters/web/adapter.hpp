#pragma once
#include <boost/beast/http/file_body.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <memory>

#include "./protocal.hpp"
#include "./websocket.hpp"
#include "dk/adapters/base.hpp"
#include "spdlog/spdlog.h"

namespace fs = boost::filesystem;
namespace dk {

inline boost::beast::string_view get_mime_type(boost::beast::string_view path) {
    using boost::beast::iequals;
    auto const ext = [&path] {
        auto const pos = path.rfind(".");
        if (pos == boost::beast::string_view::npos) return boost::beast::string_view{};
        return path.substr(pos);
    }();
    if (iequals(ext, ".htm") || iequals(ext, ".html")) return "text/html";
    if (iequals(ext, ".js")) return "application/javascript";
    if (iequals(ext, ".css")) return "text/css";
    if (iequals(ext, ".json")) return "application/json";
    if (iequals(ext, ".png")) return "image/png";
    if (iequals(ext, ".jpg") || iequals(ext, ".jpeg")) return "image/jpeg";
    if (iequals(ext, ".gif")) return "image/gif";
    if (iequals(ext, ".svg")) return "image/svg+xml";
    if (iequals(ext, ".ico")) return "image/x-icon";
    if (iequals(ext, ".txt")) return "text/plain";
    return "application/octet-stream";
}

struct WsOpenEvent {
    std::shared_ptr<WsConnection> conn;
};

inline uint MAX_LOG_LENGTH = 500;  // 最多记录500个字符

template <typename Context, typename DerivedEngine>
class WebAdapter : public BaseAdapter<Context, DerivedEngine> {
   private:
    net::io_context& ioc_;
    tcp::acceptor acceptor_;

    std::shared_ptr<ConnectionManager> conn_manager_;
    // 路由表：{HTTP方法, 路径} -> IProtocolHandler 接口
    std::map<std::pair<boost::beast::http::verb, std::string>, std::shared_ptr<IProtocolHandler<WebAdapter>>> routes_;

    // 静态目录路由表：{ URL前缀 -> 本地绝对/相对路径 }
    std::map<std::string, std::string, std::greater<std::string>> static_routes_;

    // CORS相关
    bool cors_enabled_ = false;
    std::string cors_origin_ = "*";
    std::string cors_methods_ = "GET, POST, PUT, DELETE, OPTIONS";
    std::string cors_headers_ = "Content-Type, Authorization";

   public:
    WebAdapter(std::shared_ptr<DerivedEngine> engine, unsigned short port)
        : BaseAdapter<Context, DerivedEngine>(engine),
          ioc_(engine->get_ioc()),
          acceptor_(engine->get_ioc()),
          conn_manager_(std::make_shared<ConnectionManager>(engine->get_ioc())) {
        beast::error_code ec;
        // 2. 打开 Acceptor
        acceptor_.open(tcp::v4(), ec);
        if (ec) {
            std::string err = "Acceptor open error: " + ec.message();
            spdlog::error("[WebAdapter Error] {}", err);
            throw std::runtime_error(err);
        }
        // 3. 设置端口复用 (非常重要：防止服务器重启时报 Address already in use)
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            std::string err = "Acceptor set reuse_address error: " + ec.message();
            spdlog::error("[WebAdapter Error] {}", err);
            throw std::runtime_error(err);
        }
        // 4. 绑定端口
        acceptor_.bind({tcp::v4(), port}, ec);
        if (ec) {
            std::string err = "Bind port " + std::to_string(port) + " failed: " + ec.message();
            spdlog::error("[WebAdapter Error] {}", err);
            throw std::runtime_error(err);  // 抛出异常，阻止程序带病启动
        }
        // 5. 开始监听
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            std::string err = "Listen on port " + std::to_string(port) + " failed: " + ec.message();
            spdlog::error("[WebAdapter Error] {}", err);
            throw std::runtime_error(err);
        }
        spdlog::info("[WebAdapter Info] Successfully listening on port {}", port);
        do_accept();
    }

    std::shared_ptr<ConnectionManager> get_manager() { return conn_manager_; }

    // --- 1. 注册基础协议处理器 (供内部扩展使用) ---
    void register_handler(boost::beast::http::verb method, const std::string& path,
                          std::shared_ptr<IProtocolHandler<WebAdapter>> handler) {
        routes_[{method, path}] = std::move(handler);
    }

    // --- 2. 现有的 HTTP JSON 路由 (API 保持不变) ---
    template <typename SpecificEvent, typename SpecificResult>
    void register_route(
        boost::beast::http::verb method, const std::string& path, uint32_t timeout_ms = 5000,
        std::function<void(SpecificEvent& e)> post_processor = [](SpecificEvent& e) {}) {
        // HTTP 专用的 Handler 实现
        class HttpRouteHandler : public IProtocolHandler<WebAdapter> {
            WebAdapter* adapter_;
            uint32_t timeout_ms_;
            std::string method_;
            std::string path_;
            std::function<void(SpecificEvent& e)> post_processor_;

            void log_requst(const std::string& data) {
                spdlog::info("receive request: method={} path={} body={}", method_, path_,
                             data.substr(0, MAX_LOG_LENGTH));
            }
            void log_result(const std::string& data) {
                spdlog::info("send request: method={} path={} body={}", method_, path_, data.substr(0, MAX_LOG_LENGTH));
            };

           public:
            HttpRouteHandler(WebAdapter* adapter, uint32_t timeout_ms, const std::string method, const std::string path,
                             std::function<void(SpecificEvent& e)> post_processor)
                : adapter_(adapter),
                  timeout_ms_(timeout_ms),
                  method_(method),
                  path_(path),
                  post_processor_(post_processor) {}

            void handle(std::shared_ptr<HttpSession<WebAdapter>> session,
                        http::request<http::string_body> req) override {
                try {
                    log_requst(req.body());
                    SpecificEvent event =
                        req.body().empty() ? SpecificEvent{} : json::parse(req.body()).template get<SpecificEvent>();
                    post_processor_(event);
                    auto future_res = adapter_->dispatch_async(event, timeout_ms_);

                    std::move(future_res)
                        .then([session, this](SpecificResult result) {
                            json j_res = result;
                            auto data = j_res.dump();

                            session->send_http_response(http::status::ok, data, "application/json");
                            this->log_result(data);
                        })
                        .catch_error([session, this](std::exception_ptr e) {
                            std::string err_msg = "Unknown internal error";
                            std::string detail = "";
                            // 1. 通过重新抛出来解析具体的异常信息
                            try {
                                if (e) {
                                    std::rethrow_exception(e);
                                }
                            } catch (const TraceableException& ex) {
                                err_msg = ex.what();
                                // detail = ex.format_exc();
                            } catch (const std::exception& ex) {
                                err_msg = ex.what();  // 获取具体的报错字符串
                            } catch (...) {
                                err_msg = "Unknown non-standard exception";
                            }
                            // 2. 构造安全的 JSON 响应（自动处理转义字符，防止 JSON 注入/破坏）
                            json j_err;
                            j_err["status"] = "error";
                            j_err["msg"] = err_msg;
                            // j_err["detail"] = detail;
                            // 3. 发送带具体错误信息的 HTTP响应
                            auto data = j_err.dump();
                            session->send_http_response(http::status::ok, data);
                            this->log_result(data);
                        });
                } catch (const std::exception& ex) {
                    session->send_http_response(http::status::bad_request, "{\"error\":\"Bad Request\"}");
                    log_result(std::string(ex.what()));
                }
            }
        };

        auto method_str = std::string(boost::beast::http::to_string(method));
        register_handler(method, path,
                         std::make_shared<HttpRouteHandler>(this, timeout_ms, method_str, path, post_processor));
        spdlog::info("WebAdapter register route: method={} path={}", method_str, path);
    }

    // --- 3. WebSocket 路由 API ---
    void register_ws_route(const std::string& path, WsEndpoint endpoint) {
        // WebSocket 专用的 Handler 实现
        class WsRouteHandler : public IProtocolHandler<WebAdapter> {
            WsEndpoint endpoint_;

           public:
            WsRouteHandler(WsEndpoint endpoint) : endpoint_(std::move(endpoint)) {}

            void handle(std::shared_ptr<HttpSession<WebAdapter>> session,
                        http::request<http::string_body> req) override {
                if (!websocket::is_upgrade(req)) {
                    session->send_http_response(http::status::bad_request, "Expected WebSocket Upgrade");
                    return;
                }
                // 1. 从 HttpSession 中接管/移出 socket
                tcp::socket socket = session->release_socket();

                // Copy endpoint_ to the new session, do not use std::move
                auto ws_session = std::make_shared<WsSessionImpl>(std::move(socket), endpoint_);
                ws_session->accept(std::move(req));

                // 注：此时原 HttpSession 的生命周期自然结束，不再接收后续 HTTP 请求
            }
        };
        // WebSocket 升级请求一定是 GET 方法
        register_handler(http::verb::get, path, std::make_shared<WsRouteHandler>(std::move(endpoint)));
    }

    // Advanced Managed WebSocket Route
    // This route automatically hooks into ConnectionManager and intercepts ACK messages
    void register_managed_ws_route(
        const std::string& path,
        std::function<void(std::shared_ptr<WsConnection>, const std::string&)> business_message_handler) {
        WsEndpoint managed_endpoint;

        managed_endpoint.on_open = [this](std::shared_ptr<WsConnection> conn) {
            this->engine_->dispatch(WsOpenEvent{conn});
            conn_manager_->add(conn);
        };
        managed_endpoint.on_close = [this](std::shared_ptr<WsConnection> conn) { conn_manager_->remove(conn); };
        managed_endpoint.on_message = [this, business_message_handler](std::shared_ptr<WsConnection> conn,
                                                                       std::string msg) {
            try {
                auto json_data = json::parse(msg);
                // Intercept ACK messages automatically
                if (json_data.contains("msg_id")) {
                    uint64_t acked_id = json_data["msg_id"].get<uint64_t>();
                    conn->handle_ack(acked_id);
                    return;  // If it's pure ACK, we can stop processing here
                }
            } catch (...) {
                // Not a JSON or parse error, just ignore and pass to business logic
            }

            // Dispatch to business logic
            if (business_message_handler) {
                business_message_handler(conn, std::move(msg));
            }
        };
        register_ws_route(path, std::move(managed_endpoint));
    }

    void register_static_dir(const std::string& url_prefix, const boost::filesystem::path& local_dir) {
        register_static_dir(url_prefix, local_dir.string());
    }

    // --- 4. 注册静态目录路由 ---
    // url_prefix: 比如 "/static"
    // local_dir: 可以传绝对路径，或者基于运行目录的相对路径 "./public"
    void register_static_dir(const std::string& url_prefix, const std::string& local_dir) {
        // 使用绝对路径以防止运行期间工作目录切换导致找不到文件
        fs::path abs_dir = fs::absolute(local_dir);

        if (!fs::exists(abs_dir) || !fs::is_directory(abs_dir)) {
            spdlog::warn("[WebAdapter] Static dir warning: {} does not exist or is not a directory.", abs_dir.string());
            // 视需求决定这里是否抛出异常
        }
        static_routes_[url_prefix] = abs_dir.string();
        spdlog::info("[WebAdapter] Mount static route: {} -> {}", url_prefix, abs_dir.string());
    }

    // 读取并返回一个json
    void register_file_route(boost::beast::http::verb method, const std::string& path,
                             const boost::filesystem::path& local_file_path,
                             const std::string& content_type = "application/json") {
        register_file_route(method, path, local_file_path.string(), content_type);
    }

    void register_file_route(boost::beast::http::verb method, const std::string& path,
                             const std::string& local_file_path, const std::string& content_type = "application/json") {
        class SingleFileHandler : public IProtocolHandler<WebAdapter> {
            std::string file_path_;
            std::string content_type_;

           public:
            SingleFileHandler(std::string fp, std::string ct)
                : file_path_(std::move(fp)), content_type_(std::move(ct)) {}
            void handle(std::shared_ptr<HttpSession<WebAdapter>> session,
                        http::request<http::string_body> req) override {
                // 每次请求动态读取文件（支持热修改）
                std::ifstream ifs(file_path_, std::ios::binary);
                if (!ifs.is_open()) {
                    spdlog::error("[WebAdapter] FileRoute failed to open file: {}", file_path_);
                    session->send_http_response(http::status::internal_server_error,
                                                "{\"error\":\"File not found or unreadable\"}", "application/json");
                    return;
                }
                // 将文件内容全部读入 string
                std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

                // 复用现有的 send_http_response 返回内容
                session->send_http_response(http::status::ok, std::move(content), content_type_);
            }
        };
        // 转换为绝对路径，防止程序运行期间 Current Working Directory 发生变化导致找不到文件
        std::string abs_path = fs::absolute(local_file_path).string();

        // 注册到现有的精确路由表 routes_ 中
        register_handler(method, path, std::make_shared<SingleFileHandler>(abs_path, content_type));

        spdlog::info("[WebAdapter] Mount file route: {} {} -> {}", std::string(boost::beast::http::to_string(method)),
                     path, abs_path);
    }

    // 开启全局 CORS 的方法
    void enable_cors(const std::string& allow_origin = "*",
                     const std::string& allow_methods = "GET, POST, PUT, DELETE, OPTIONS",
                     const std::string& allow_headers = "Content-Type, Authorization") {
        cors_enabled_ = true;
        cors_origin_ = allow_origin;
        cors_methods_ = allow_methods;
        cors_headers_ = allow_headers;
        spdlog::info("[WebAdapter] CORS enabled globally (Origin: {})", allow_origin);
    }

   private:
    void do_accept() {
        acceptor_.async_accept(net::make_strand(ioc_), [this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<HttpSession<WebAdapter>>(std::move(socket), this)->start();
            } else {
                // 增加运行时的 accept 错误提示
                spdlog::error("[WebAdapter Error] async_accept failed: {}", ec.message());

                // 如果是致命错误（比如 acceptor 被关闭），应该停止 accept 循环
                if (ec == net::error::operation_aborted || ec == net::error::bad_descriptor) {
                    return;
                }
            }
            // 只要不是致命错误，继续接收下一个连接
            do_accept();
        });
    }
    friend class HttpSession<WebAdapter>;
};

// ==========================================
// HttpSession
// ==========================================
template <typename AdapterType>
class HttpSession : public std::enable_shared_from_this<HttpSession<AdapterType>> {
    tcp::socket socket_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    std::shared_ptr<http::response<http::string_body>> res_;
    AdapterType* adapter_;
    bool socket_released_ = false;

    // 专用于发送 file_body 的响应指针，保持发送期间生命周期存活
    std::shared_ptr<http::response<http::file_body>> file_res_;

   public:
    HttpSession(tcp::socket socket, AdapterType* adapter) : socket_(std::move(socket)), adapter_(adapter) {}

    void start() { read_request(); }

    // 提供给 Handler 回复标准 HTTP 响应的接口
    void send_http_response(http::status status, std::string body, std::string content_type = "application/json") {
        if (socket_released_) return;

        res_ = std::make_shared<http::response<http::string_body>>(status, req_.version());

        if (adapter_->cors_enabled_) {
            res_->set(http::field::access_control_allow_origin, adapter_->cors_origin_);
        }

        res_->set(http::field::server, "WebAdapter");
        res_->set(http::field::content_type, content_type);
        res_->keep_alive(req_.keep_alive());
        res_->body() = std::move(body);
        res_->prepare_payload();

        auto self = this->shared_from_this();
        http::async_write(socket_, *res_, [self](beast::error_code ec, std::size_t) {
            if (!self->res_->keep_alive()) {
                beast::error_code ignored_ec;
                self->socket_.shutdown(tcp::socket::shutdown_send, ignored_ec);
            } else if (!ec) {
                self->res_.reset();
                self->req_ = {};
                self->read_request();
            }
        });
    }

    // 提供给 WS/SSE Handler 接管底层 Socket 的接口
    tcp::socket release_socket() {
        socket_released_ = true;
        return std::move(socket_);
    }

    // 安全处理和发送静态文件
    void serve_static_file(const std::string& base_dir, std::string req_path) {
        // 安全检查：防止类似 /static/../../../etc/passwd 的目录穿越攻击
        if (req_path.find("..") != std::string::npos) {
            send_http_response(http::status::bad_request, "Illegal path");
            return;
        }
        // 拼接完整的本地路径
        if (!req_path.empty() && req_path[0] == '/') req_path = req_path.substr(1);
        fs::path full_path = fs::path(base_dir) / req_path;
        // 如果请求的是一个目录，尝试寻找 index.html
        if (fs::is_directory(full_path)) {
            full_path /= "index.html";
        }
        beast::error_code ec;
        http::file_body::value_type file;
        file.open(full_path.string().c_str(), beast::file_mode::scan, ec);
        // 文件打开失败 (不存在或无权限)
        if (ec) {
            spdlog::error("[WebAdapter] file {} not available: {}", full_path.string(), ec.message());
            send_http_response(http::status::not_found, "File not found");
            return;
        }
        // 构造文件响应体
        file_res_ = std::make_shared<http::response<http::file_body>>(
            std::piecewise_construct, std::make_tuple(std::move(file)),
            std::make_tuple(http::status::ok, req_.version()));

        if (adapter_->cors_enabled_) {
            file_res_->set(http::field::access_control_allow_origin, adapter_->cors_origin_);
        }
        file_res_->set(http::field::server, "WebAdapter");
        file_res_->set(http::field::content_type, get_mime_type(full_path.string()));
        file_res_->keep_alive(req_.keep_alive());
        file_res_->prepare_payload();
        // 异步写出文件
        auto self = this->shared_from_this();
        http::async_write(socket_, *file_res_, [self](beast::error_code ec, std::size_t) {
            if (!self->file_res_->keep_alive()) {
                beast::error_code ignored_ec;
                self->socket_.shutdown(tcp::socket::shutdown_send, ignored_ec);
            } else if (!ec) {
                self->file_res_.reset();
                self->req_ = {};
                self->read_request();
            }
        });
    }

   private:
    void read_request() {
        if (socket_released_) return;
        auto self = this->shared_from_this();
        http::async_read(socket_, buffer_, req_, [self](beast::error_code ec, std::size_t) {
            if (!ec) self->process_request();
        });
    }

    static bool is_valid_match(const std::string& path, const std::string& prefix) {
        if (prefix.empty() || path.empty()) return false;

        if (path.find(prefix) != 0) return false;
        if (path.length() == prefix.length()) return true;  // Exact match

        if (prefix == "/") return false;

        if (prefix.back() == '/') return true;  // e.g., "/home/" matches "/home/assets"
        return path[prefix.length()] == '/';    // e.g., "/home" matches "/home/assets"
    }

    void process_request() {
        if (req_.method() == http::verb::options && adapter_->cors_enabled_) {
            send_cors_preflight();
            return;
        }

        std::string target(req_.target().data(), req_.target().size());

        // 【新增】去除 URL 中的 Query 参数 (如 ?v=1.2.3)
        size_t query_pos = target.find('?');
        std::string pure_path = (query_pos != std::string::npos) ? target.substr(0, query_pos) : target;
        // 1. 先尝试精确匹配 API 和 WebSocket 路由
        auto it = adapter_->routes_.find({req_.method(), pure_path});
        if (it != adapter_->routes_.end()) {
            it->second->handle(this->shared_from_this(), std::move(req_));
            return;
        }
        // 2. 如果是 GET 或 HEAD 请求，尝试前缀匹配静态路由
        if (req_.method() == http::verb::get || req_.method() == http::verb::head) {
            for (const auto& [prefix, local_dir] : adapter_->static_routes_) {
                // 判断前缀匹配 (C++20 可以使用 pure_path.starts_with(prefix))
                if (is_valid_match(pure_path, prefix)) {
                    std::string rel_path = pure_path.substr(prefix.length());
                    serve_static_file(local_dir, rel_path);
                    return;
                }
            }
        }
        // 3. 都找不到，返回 404
        spdlog::error("[WebAdapter] {} {} not found, return 404!", boost::beast::http::to_string(req_.method()),
                      pure_path);
        send_http_response(http::status::not_found, "{\"error\":\"Not Found\"}");
    }

    void send_cors_preflight() {
        if (socket_released_) return;
        // 预检请求一般返回 204 No Content
        res_ = std::make_shared<http::response<http::string_body>>(http::status::no_content, req_.version());
        res_->set(http::field::server, "WebAdapter");

        // 告诉浏览器允许的跨域规则
        res_->set(http::field::access_control_allow_origin, adapter_->cors_origin_);
        res_->set(http::field::access_control_allow_methods, adapter_->cors_methods_);
        res_->set(http::field::access_control_allow_headers, adapter_->cors_headers_);
        res_->set(http::field::access_control_max_age, "86400");  // 浏览器缓存该规则 24 小时，不用每次都发 OPTIONS

        res_->keep_alive(req_.keep_alive());
        res_->prepare_payload();
        auto self = this->shared_from_this();
        http::async_write(socket_, *res_, [self](beast::error_code ec, std::size_t) {
            if (!self->res_->keep_alive()) {
                beast::error_code ignored_ec;
                self->socket_.shutdown(tcp::socket::shutdown_send, ignored_ec);
            } else if (!ec) {
                self->res_.reset();
                self->req_ = {};
                self->read_request();
            }
        });
    }
};
}  // namespace dk