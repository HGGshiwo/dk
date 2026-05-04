#pragma once
#include "./protocal.hpp"
#include "./websocket.hpp"

namespace dk {
template <typename Event, typename Context, typename DerivedEngine>
class WebAdapter : public BaseAdapter<Event, Context, DerivedEngine> {
   private:
    net::io_context& ioc_;
    tcp::acceptor acceptor_;

    // 路由表：{HTTP方法, 路径} -> IProtocolHandler 接口
    std::map<std::pair<boost::beast::http::verb, std::string>, std::shared_ptr<IProtocolHandler<WebAdapter>>> routes_;

   public:
    WebAdapter(net::io_context& ioc, unsigned short port, std::shared_ptr<DerivedEngine> engine)
        : BaseAdapter<Event, Context, DerivedEngine>(engine), ioc_(ioc), acceptor_(ioc, {tcp::v4(), port}) {
        do_accept();
    }

    // --- 1. 注册基础协议处理器 (供内部扩展使用) ---
    void register_handler(boost::beast::http::verb method, const std::string& path,
                          std::shared_ptr<IProtocolHandler<WebAdapter>> handler) {
        routes_[{method, path}] = std::move(handler);
    }

    // --- 2. 现有的 HTTP JSON 路由 (API 保持不变) ---
    template <typename SpecificEvent, typename SpecificResult>
    void register_route(boost::beast::http::verb method, const std::string& path, uint32_t timeout_ms = 5000) {
        // HTTP 专用的 Handler 实现
        class HttpRouteHandler : public IProtocolHandler<WebAdapter> {
            WebAdapter* adapter_;
            uint32_t timeout_ms_;

           public:
            HttpRouteHandler(WebAdapter* adapter, uint32_t timeout_ms) : adapter_(adapter), timeout_ms_(timeout_ms) {}

            void handle(std::shared_ptr<HttpSession<WebAdapter>> session,
                        http::request<http::string_body> req) override {
                try {
                    SpecificEvent event = json::parse(req.body()).template get<SpecificEvent>();
                    auto future_res = adapter_->dispatch_async(event, timeout_ms_);

                    std::move(future_res)
                        .then([session](SpecificResult result) {
                            json j_res = result;
                            session->send_http_response(http::status::ok, j_res.dump(), "application/json");
                        })
                        .catch_error([session](std::exception_ptr e) {
                            session->send_http_response(http::status::internal_server_error,
                                                        "{\"error\":\"Internal Error\"}");
                        });
                } catch (const std::exception& ex) {
                    session->send_http_response(http::status::bad_request, "{\"error\":\"Bad Request\"}");
                }
            }
        };
        register_handler(method, path, std::make_shared<HttpRouteHandler>(this, timeout_ms));
    }

    // --- 3. 新增的 WebSocket 路由 API ---
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

                // 2. 创建 WS Session 并开始握手
                auto ws_session = std::make_shared<WsSessionImpl>(std::move(socket), std::move(endpoint_));
                ws_session->accept(std::move(req));

                // 注：此时原 HttpSession 的生命周期自然结束，不再接收后续 HTTP 请求
            }
        };
        // WebSocket 升级请求一定是 GET 方法
        register_handler(http::verb::get, path, std::make_shared<WsRouteHandler>(std::move(endpoint)));
    }

   private:
    void do_accept() {
        acceptor_.async_accept(net::make_strand(ioc_), [this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<HttpSession<WebAdapter>>(std::move(socket), this)->start();
            }
            do_accept();
        });
    }
    friend class HttpSession<WebAdapter>;
};

// ==========================================
// 4. 重构后的 HttpSession
// ==========================================
template <typename AdapterType>
class HttpSession : public std::enable_shared_from_this<HttpSession<AdapterType>> {
    tcp::socket socket_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    std::shared_ptr<http::response<http::string_body>> res_;
    AdapterType* adapter_;
    bool socket_released_ = false;

   public:
    HttpSession(tcp::socket socket, AdapterType* adapter) : socket_(std::move(socket)), adapter_(adapter) {}

    void start() { read_request(); }

    // 提供给 Handler 回复标准 HTTP 响应的接口
    void send_http_response(http::status status, std::string body, std::string content_type = "application/json") {
        if (socket_released_) return;

        res_ = std::make_shared<http::response<http::string_body>>(status, req_.version());
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

   private:
    void read_request() {
        if (socket_released_) return;
        auto self = this->shared_from_this();
        http::async_read(socket_, buffer_, req_, [self](beast::error_code ec, std::size_t) {
            if (!ec) self->process_request();
        });
    }

    void process_request() {
        std::string target(req_.target().data(), req_.target().size());
        auto it = adapter_->routes_.find({req_.method(), target});

        if (it != adapter_->routes_.end()) {
            // 统一调用抽象接口的 handle 方法
            it->second->handle(this->shared_from_this(), std::move(req_));
        } else {
            send_http_response(http::status::not_found, "{\"error\":\"Not Found\"}");
        }
    }
};
}  // namespace dk