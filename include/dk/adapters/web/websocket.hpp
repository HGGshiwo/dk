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
        json payload;
        std::shared_ptr<boost::asio::steady_timer> timer;
        int retry_count;
    };

    // Struct to hold pending state data
    struct PendingState {
        uint64_t current_msg_id;
        std::shared_ptr<boost::asio::steady_timer> timer;
        int retry_count;
    };

    // Auto-increment sequence for this specific connection
    std::atomic<uint64_t> msg_seq_{0};

    // Mutex to protect pending_msgs_ map
    std::mutex pending_mutex_;

    // Map of msg_id to pending messages
    std::map<uint64_t, PendingMsg> pending_msgs_;

    // State-based reliable messages tracking
    std::map<std::string, nlohmann::json> latest_states_;
    std::map<std::string, PendingState> pending_states_;
    std::map<uint64_t, std::string> state_ack_map_;

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

        // 4. Start the reliable send loop
        do_send_and_wait(current_id, msg_json, 0);
    }

    void send_state(const std::string& state_key, nlohmann::json msg_json) override {
        std::lock_guard<std::mutex> lock(pending_mutex_);

        // Always store the latest data for this key
        latest_states_[state_key] = msg_json;
        auto it = pending_states_.find(state_key);
        if (it != pending_states_.end()) {
            // State is already waiting for ACK. Update payload and send immediately.
            // We keep the same msg_id and timer, so it acts like a transparent update.
            uint64_t msg_id = it->second.current_msg_id;
            msg_json["msg_id"] = msg_id;
            // msg_json["state_key"] = state_key;
            send(msg_json);
        } else {
            // New state tracking
            uint64_t current_id = ++msg_seq_;
            state_ack_map_[current_id] = state_key;
            msg_json["msg_id"] = current_id;
            // msg_json["state_key"] = state_key;
            send(msg_json);
            auto timer = std::make_shared<boost::asio::steady_timer>(ws_.get_executor());
            timer->expires_after(std::chrono::milliseconds(1000));
            pending_states_[state_key] = {current_id, timer, 0};
            do_state_wait(state_key, current_id, timer, 0);
        }
    }

    void handle_ack(uint64_t ack_id) override {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        // 1. Check normal reliable messages
        auto it = pending_msgs_.find(ack_id);
        if (it != pending_msgs_.end()) {
            boost::system::error_code ec;
            it->second.timer->cancel(ec);
            pending_msgs_.erase(it);
            return;
        }
        // 2. Check state-based reliable messages
        auto ack_it = state_ack_map_.find(ack_id);
        if (ack_it != state_ack_map_.end()) {
            std::string state_key = ack_it->second;
            auto state_it = pending_states_.find(state_key);

            // If the ACK matches the active msg_id for this state
            if (state_it != pending_states_.end() && state_it->second.current_msg_id == ack_id) {
                boost::system::error_code ec;
                state_it->second.timer->cancel(ec);
                pending_states_.erase(state_it);
            }
            state_ack_map_.erase(ack_it);
        }
    }

   private:
    // Core function to send and start timeout timer
    void do_send_and_wait(uint64_t msg_id, const json& payload, int retry_count) {
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

    void do_state_wait(std::string state_key, uint64_t msg_id, std::shared_ptr<boost::asio::steady_timer> timer,
                       int retry_count) {
        std::weak_ptr<WsSessionImpl> weak_self = derived_from_this();
        timer->async_wait([weak_self, state_key, msg_id, retry_count](boost::system::error_code ec) {
            if (ec) return;
            if (auto self = weak_self.lock()) {
                std::lock_guard<std::mutex> lock(self->pending_mutex_);
                auto it = self->pending_states_.find(state_key);

                // Ensure this state is still active and hasn't been replaced by a new lifecycle
                if (it != self->pending_states_.end() && it->second.current_msg_id == msg_id) {
                    if (retry_count >= 3) {
                        self->pending_states_.erase(it);
                        self->state_ack_map_.erase(msg_id);
                        return;
                    }
                    // Dynamically fetch the LATEST data for this key
                    nlohmann::json latest_msg = self->latest_states_[state_key];
                    latest_msg["msg_id"] = msg_id;
                    // latest_msg["state_key"] = state_key;

                    self->send(latest_msg);
                    // Setup next retry timer
                    auto new_timer = std::make_shared<boost::asio::steady_timer>(self->ws_.get_executor());
                    new_timer->expires_after(std::chrono::milliseconds(1000));
                    it->second.timer = new_timer;
                    it->second.retry_count = retry_count + 1;

                    self->do_state_wait(state_key, msg_id, new_timer, retry_count + 1);
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