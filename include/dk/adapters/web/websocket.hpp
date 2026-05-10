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
    size_t id_;
    inline static std::atomic<size_t> id_counter_{0};

    // Struct to hold pending message data and its timer
    struct PendingMsg {
        std::string payload;
        std::shared_ptr<boost::asio::steady_timer> timer;
        int retry_count;
    };
    // Auto-increment sequence for this specific connection
    std::atomic<uint64_t> msg_seq_{0};

    // Mutex to protect pending_msgs_ map
    std::mutex pending_mutex_;

    // Map of msg_id to pending messages
    std::map<uint64_t, PendingMsg> pending_msgs_;

   public:
    WsSessionImpl(tcp::socket&& socket, WsEndpoint endpoint)
        : ws_(std::move(socket)), endpoint_(std::move(endpoint)), id_(++id_counter_) {}

    std::shared_ptr<WsSessionImpl> derived_from_this() {
        return std::static_pointer_cast<WsSessionImpl>(shared_from_this());
    }

    size_t get_id() const override { return id_; }

    template <class Body, class Allocator>
    void accept(http::request<Body, http::basic_fields<Allocator>> req) {
        auto self = derived_from_this();
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.async_accept(req, [self](beast::error_code ec) {
            if (!ec) {
                if (self->endpoint_.on_open) {
                    self->endpoint_.on_open(self);
                }
                self->do_read();
            }
        });
    }

    void send(nlohmann::json msg_json) override {
        auto self = derived_from_this();
        net::post(ws_.get_executor(), [self, msg_json]() {
            self->write_queue_.push_back(msg_json.dump());
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

    void send_reliable(nlohmann::json msg_json) override {
        // 1. Auto generate unique ID for this connection
        uint64_t current_id = ++msg_seq_;

        // 2. Inject msg_id into json
        msg_json["msg_id"] = current_id;

        // 3. Serialize to string
        std::string payload = msg_json.dump();

        // 4. Start the reliable send loop
        do_send_and_wait(current_id, payload, 0);
    }

    void handle_ack(uint64_t ack_id) override {
        std::lock_guard<std::mutex> lock(pending_mutex_);

        auto it = pending_msgs_.find(ack_id);
        if (it != pending_msgs_.end()) {
            // Cancel timer
            boost::system::error_code ec;
            it->second.timer->cancel(ec);

            // Remove from waiting queue
            pending_msgs_.erase(it);
        }
    }

   private:
    // Core function to send and start timeout timer
    void do_send_and_wait(uint64_t msg_id, const std::string& payload, int retry_count) {
        // Max 3 retries, then give up (you can adjust this)
        if (retry_count > 3) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_msgs_.erase(msg_id);
            return;
        }
        // 1. Actually send the message via WebSocket
        send(payload);
        // 2. Create timer for this message
        auto timer = std::make_shared<boost::asio::steady_timer>(ws_.get_executor());
        timer->expires_after(std::chrono::milliseconds(1000));  // 1 second timeout
        // 3. Store in pending map safely
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_msgs_[msg_id] = {payload, timer, retry_count};
        }
        // 4. Start async wait
        // Use weak_ptr to avoid capturing a deleted session if client disconnects
        std::weak_ptr<WsSessionImpl> weak_self = std::static_pointer_cast<WsSessionImpl>(shared_from_this());
        timer->async_wait([weak_self, msg_id, payload, retry_count](boost::system::error_code ec) {
            if (ec) return;  // Timer cancelled or error
            if (auto self = weak_self.lock()) {
                // Check if message is still pending (not acked yet)
                bool needs_retry = false;
                {
                    std::lock_guard<std::mutex> lock(self->pending_mutex_);
                    if (self->pending_msgs_.count(msg_id) > 0) {
                        needs_retry = true;
                    }
                }

                // If no ACK received, try again
                if (needs_retry) {
                    self->do_send_and_wait(msg_id, payload, retry_count + 1);
                }
            }
        });
    }

    void do_read() {
        auto self = derived_from_this();
        ws_.async_read(buffer_, [self](beast::error_code ec, std::size_t bytes_transferred) {
            if (ec == websocket::error::closed || ec) {
                if (self->endpoint_.on_close) {
                    self->endpoint_.on_close(self);
                }
                return;
            }
            std::string msg = beast::buffers_to_string(self->buffer_.data());
            self->buffer_.consume(self->buffer_.size());

            if (self->endpoint_.on_message) {
                self->endpoint_.on_message(self, std::move(msg));
            }
            self->do_read();
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
            } else {
                self->is_writing_ = false;
            }
        });
    }
};
}  // namespace dk