#pragma once

#include "./protocal.hpp"
namespace dk {
// 内部类：接管 Socket 并处理 WebSocket 生命周期
class WsSessionImpl : public WsConnection {
    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    WsEndpoint endpoint_;
    std::deque<std::string> write_queue_;
    bool is_writing_ = false;

   public:
    WsSessionImpl(tcp::socket&& socket, WsEndpoint endpoint) : ws_(std::move(socket)), endpoint_(std::move(endpoint)) {}

    // 🌟 核心技巧：封装一个获取派生类智能指针的函数
    std::shared_ptr<WsSessionImpl> derived_from_this() {
        return std::static_pointer_cast<WsSessionImpl>(shared_from_this());
    }

    template <class Body, class Allocator>
    void accept(http::request<Body, http::basic_fields<Allocator>> req) {
        // 使用 derived_from_this 获取派生类指针
        auto self = derived_from_this();

        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.async_accept(req, [self](beast::error_code ec) {
            if (!ec) {
                // 传给用户的依然是基类接口
                self->endpoint_.on_open(self);
                self->do_read();
            }
        });
    }

    void send(const std::string& msg) override {
        auto self = derived_from_this();
        net::post(ws_.get_executor(), [self, msg]() {
            self->write_queue_.push_back(msg);
            if (!self->is_writing_) {
                self->do_write();
            }
        });
    }

    void close() override {
        auto self = derived_from_this();
        net::post(ws_.get_executor(), [self]() {
            self->ws_.async_close(websocket::close_reason(websocket::close_code::normal), [self](beast::error_code) {});
        });
    }

   private:
    void do_read() {
        auto self = derived_from_this();
        // 这里必须用派生类指针，因为 buffer_ 和 ws_ 是派生类的私有成员
        ws_.async_read(buffer_, [self](beast::error_code ec, std::size_t bytes_transferred) {
            if (ec == websocket::error::closed) {
                self->endpoint_.on_close(self);
                return;
            }
            if (!ec) {
                std::string msg = beast::buffers_to_string(self->buffer_.data());
                self->buffer_.consume(self->buffer_.size());

                // 将 self 隐式转为 shared_ptr<WsConnection> 传给业务层
                self->endpoint_.on_message(self, std::move(msg));
                self->do_read();
            }
        });
    }

    void do_write() {
        if (write_queue_.empty()) {
            is_writing_ = false;
            return;
        }
        is_writing_ = true;
        auto self = derived_from_this();
        ws_.text(true);
        ws_.async_write(net::buffer(write_queue_.front()), [self](beast::error_code ec, std::size_t) {
            if (!ec) {
                self->write_queue_.pop_front();
                self->do_write();
            }
        });
    }
};
}  // namespace dk