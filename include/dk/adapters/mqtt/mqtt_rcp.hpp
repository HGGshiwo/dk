#pragma once
#include <boost/asio.hpp>
#include <chrono>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "dk/future.hpp"
#include "protocal.hpp"
#include "spdlog/spdlog.h"

namespace dk {

namespace net = boost::asio;

struct PendingRpcRequest {
    std::function<void(const std::string&)> on_success;
    std::function<void(const std::string&)> on_timeout;
    std::shared_ptr<net::steady_timer> timer;
    std::string expected_resp_topic;
};

class MqttRpcInvoker {
   private:
    net::io_context& ioc_;
    std::weak_ptr<IAsyncRuntime> rt_;
    std::atomic<uint64_t> next_msg_id_{1};
    std::map<uint64_t, std::shared_ptr<PendingRpcRequest>> pending_requests_;
    std::function<void(const std::string&, const std::string&, uint8_t, bool,
                       const std::string&, const std::string&)>
        publish_fn_;

   public:
    explicit MqttRpcInvoker(net::io_context& ioc,
                            std::shared_ptr<IAsyncRuntime> rt)
        : ioc_(ioc), rt_(rt) {}

    void set_publish_fn(
        std::function<void(const std::string&, const std::string&, uint8_t,
                           bool, const std::string&, const std::string&)>
            fn) {
        publish_fn_ = std::move(fn);
    }

    dk::Future<nlohmann::json> request(
        const std::string& topic, const std::string& req_payload_json_str,
        uint32_t timeout_ms = 5000, const std::string& expect_resp_topic = "") {
        auto promise =
            std::make_shared<dk::Promise<nlohmann::json>>(rt_.lock());
        auto future = promise->get_future();

        net::post(ioc_, [this, topic, req_payload_json_str, timeout_ms,
                         expect_resp_topic, promise]() mutable {
            uint64_t current_msg_id = next_msg_id_++;
            std::string payload = req_payload_json_str;

            try {
                if (!payload.empty() && payload[0] == '{') {
                    nlohmann::json j_req = nlohmann::json::parse(payload);
                    j_req["msg_id"] = current_msg_id;
                    payload = j_req.dump();
                }
            } catch (...) {
            }

            auto pending_task = std::make_shared<PendingRpcRequest>();
            pending_task->timer = std::make_shared<net::steady_timer>(
                ioc_, std::chrono::milliseconds(timeout_ms));
            std::string resp_topic =
                expect_resp_topic.empty() ? topic + "/resp" : expect_resp_topic;
            pending_task->expected_resp_topic = resp_topic;

            pending_task->on_success =
                [promise](const std::string& resp_json_str) {
                    try {
                        promise->resolve(nlohmann::json::parse(resp_json_str));
                    } catch (const std::exception& ex) {
                        promise->reject(std::make_exception_ptr(ex));
                    }
                };

            pending_task->on_timeout = [this, current_msg_id,
                                        promise](const std::string&) {
                pending_requests_.erase(current_msg_id);
                promise->reject(std::make_exception_ptr(
                    std::runtime_error("MQTT RPC Timeout for msg_id=" +
                                       std::to_string(current_msg_id))));
            };

            pending_task->timer->async_wait(
                [task = pending_task](boost::system::error_code ec) {
                    if (!ec) task->on_timeout("timeout");
                });

            pending_requests_[current_msg_id] = pending_task;
            if (publish_fn_) {
                publish_fn_(topic, payload, 0, false, resp_topic,
                            std::to_string(current_msg_id));
            }
        });

        return future;
    }

    bool try_intercept_rpc_response(const MqttMessage& msg) {
        if (msg.correlation_data.empty()) {
            if (msg.payload.empty() || msg.payload[0] != '{') return false;
            try {
                auto j = nlohmann::json::parse(msg.payload);
                if (!j.contains("msg_id")) return false;

                uint64_t msg_id = j["msg_id"].get<uint64_t>();
                auto it = pending_requests_.find(msg_id);
                if (it == pending_requests_.end()) return false;

                auto task = it->second;
                if (task->expected_resp_topic != msg.topic) return false;

                task->timer->cancel();
                task->on_success(msg.payload);
                pending_requests_.erase(it);
                return true;
            } catch (...) {
                return false;
            }
        }

        try {
            uint64_t msg_id = std::stoull(msg.correlation_data);
            auto it = pending_requests_.find(msg_id);
            if (it == pending_requests_.end()) return false;

            auto task = it->second;
            if (task->expected_resp_topic != msg.topic) return false;

            task->timer->cancel();
            task->on_success(msg.payload);
            pending_requests_.erase(it);
            return true;
        } catch (...) {
            return false;
        }
    }
};

}  // namespace dk