#pragma once
#include <mqtt/async_client.h>

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../base.hpp"
#include "./mqtt_rcp.hpp"
#include "nlohmann/json_fwd.hpp"
#include "protocal.hpp"
#include "spdlog/spdlog.h"

namespace dk {
template <typename Context, typename DerivedEngine>
class MqttClientAdapter : public dk::BaseAdapter<Context, DerivedEngine>,
                          public IMqttClient,
                          public std::enable_shared_from_this<
                              MqttClientAdapter<Context, DerivedEngine>> {
   private:
    net::io_context& ioc_;
    std::string host_;
    unsigned short port_;
    std::unique_ptr<mqtt::async_client> client_;
    std::map<std::string, std::shared_ptr<IMqttProtocolHandler>> routes_;
    MqttRpcInvoker rpc_invoker_;

    class MqttPahoCallback : public virtual mqtt::callback {
        net::io_context& ioc_;
        std::weak_ptr<MqttClientAdapter> adapter_weak_;

       public:
        MqttPahoCallback(net::io_context& ioc,
                         std::shared_ptr<MqttClientAdapter> adapter)
            : ioc_(ioc), adapter_weak_(adapter) {}

        void connected(const std::string& cause) override {
            net::post(ioc_, [weak = adapter_weak_, cause]() {
                if (auto adapter = weak.lock()) adapter->on_connected(cause);
            });
        }
        void connection_lost(const std::string& cause) override {
            net::post(ioc_, [weak = adapter_weak_, cause]() {
                if (auto adapter = weak.lock())
                    adapter->on_connection_lost(cause);
            });
        }
        void message_arrived(mqtt::const_message_ptr msg) override {
            std::string topic = msg->get_topic();
            std::string payload = msg->get_payload_str();
            int qos = msg->get_qos();

            std::string response_topic;
            std::string correlation_data;

            const mqtt::properties& props = msg->get_properties();
            if (props.contains(mqtt::property::RESPONSE_TOPIC)) {
                response_topic = mqtt::get<std::string>(
                    props, mqtt::property::RESPONSE_TOPIC);
            }
            if (props.contains(mqtt::property::CORRELATION_DATA)) {
                correlation_data = mqtt::get<std::string>(
                    props, mqtt::property::CORRELATION_DATA);
            }

            net::post(ioc_, [weak = adapter_weak_, topic = std::move(topic),
                             payload = std::move(payload), qos,
                             response_topic = std::move(response_topic),
                             correlation_data =
                                 std::move(correlation_data)]() mutable {
                if (auto adapter = weak.lock())
                    adapter->on_message(topic, payload, qos, response_topic,
                                        correlation_data);
            });
        }
        void delivery_complete(mqtt::delivery_token_ptr tok) override {}
    };

    std::shared_ptr<MqttPahoCallback> cb_;
    bool is_connected_ = false;
    net::steady_timer reconnect_timer_;
    std::atomic<bool> should_reconnect_{true};
    std::string client_id_;

    void do_send_publish(const std::string& topic, const std::string& payload,
                         uint8_t qos_level, bool retain,
                         const std::string& response_topic = "",
                         const std::string& correlation_data = "") {
        if (!is_connected_ || !client_) {
            spdlog::warn("[MqttClientAdapter] Drop publish (offline): topic={}",
                         topic);
            return;
        }
        try {
            mqtt::properties props;
            if (!response_topic.empty()) {
                props.add({mqtt::property::RESPONSE_TOPIC, response_topic});
            }
            if (!correlation_data.empty()) {
                props.add({mqtt::property::CORRELATION_DATA, correlation_data});
            }
            auto msg =
                mqtt::message::create(topic, payload.data(), payload.size(),
                                      qos_level, retain, props);
            client_->publish(msg);
        } catch (const std::exception& ex) {
            spdlog::warn(
                "[MqttClientAdapter] Publish failed: topic={} error={}", topic,
                ex.what());
        }
    }

    void on_connected(const std::string& cause) {
        spdlog::info(
            "[MqttClientAdapter] MQTT connected successfully. Cause: {}",
            cause);
        is_connected_ = true;
        subscribe_to_routes();
        this->dispatch(MqttConnectEvent{});
    }

    class MqttConnectActionListener
        : public virtual mqtt::iaction_listener,
          public std::enable_shared_from_this<MqttConnectActionListener> {
        net::io_context& ioc_;
        std::weak_ptr<MqttClientAdapter> adapter_weak_;

       public:
        MqttConnectActionListener(net::io_context& ioc,
                                  std::shared_ptr<MqttClientAdapter> adapter)
            : ioc_(ioc), adapter_weak_(adapter) {}

        void on_failure(const mqtt::token& tok) override {
            std::string err_msg =
                "rc=" + std::to_string(tok.get_return_code()) +
                " (reason=" + std::to_string(tok.get_reason_code()) + ")";
            net::post(ioc_, [weak = adapter_weak_, err_msg,
                             self = this->shared_from_this()]() {
                if (auto adapter = weak.lock()) {
                    adapter->on_connect_failed(err_msg);
                    adapter->remove_listener(self);
                }
            });
        }

        void on_success(const mqtt::token& tok) override {
            net::post(ioc_, [weak = adapter_weak_,
                             self = this->shared_from_this()]() {
                if (auto adapter = weak.lock()) {
                    adapter->remove_listener(self);
                }
            });
        }
    };

    std::vector<std::shared_ptr<MqttConnectActionListener>> pending_listeners_;

    void remove_listener(std::shared_ptr<MqttConnectActionListener> l) {
        auto it =
            std::find(pending_listeners_.begin(), pending_listeners_.end(), l);
        if (it != pending_listeners_.end()) {
            pending_listeners_.erase(it);
        }
    }

    void on_connect_failed(const std::string& reason) {
        spdlog::error(
            "[MqttClientAdapter] MQTT connection failed: {}. Scheduling "
            "reconnect...",
            reason);
        is_connected_ = false;
        if (should_reconnect_) schedule_reconnect();
    }

    void on_connection_lost(const std::string& cause) {
        spdlog::warn("[MqttClientAdapter] Connection lost: {}", cause);
        is_connected_ = false;
        // 依赖 paho 底层自动重连，此处不需要主动调用 schedule_reconnect()
    }

    void schedule_reconnect() {
        boost::system::error_code ec;
        reconnect_timer_.cancel(ec);
        reconnect_timer_.expires_after(std::chrono::seconds(5));
        reconnect_timer_.async_wait([this, self = this->shared_from_this()](
                                        boost::system::error_code ec) {
            if (!ec && should_reconnect_) do_connect();
        });
    }

    void do_connect() {
        const char* proxy_envs[] = {"http_proxy",  "HTTP_PROXY", "https_proxy",
                                    "HTTPS_PROXY", "all_proxy",  "ALL_PROXY"};
        for (const char* env_name : proxy_envs) {
            const char* val = ::getenv(env_name);
            if (val && std::strlen(val) == 0) {
                ::unsetenv(env_name);
            }
        }

        if (!client_) {
            std::string server_address =
                "tcp://" + host_ + ":" + std::to_string(port_);

            if (client_id_.empty()) {
                client_id_ = "dk_client_" +
                             std::to_string(std::chrono::steady_clock::now()
                                                .time_since_epoch()
                                                .count());
            }

            try {
                // 1. 设置创建选项：允许在未连接/断连期间发送消息（Offline
                // Buffering）
                auto create_opts =
                    mqtt::create_options_builder()
                        .send_while_disconnected(
                            true, true)  // 缓存开启，持久化=true，最多存1000条
                        .finalize();

                client_ = std::make_unique<mqtt::async_client>(
                    server_address, client_id_, create_opts);
                cb_ = std::make_shared<MqttPahoCallback>(
                    ioc_, this->shared_from_this());
                client_->set_callback(*cb_);
            } catch (const std::exception& ex) {
                spdlog::error(
                    "[MqttClientAdapter] Failed to create async client: {}",
                    ex.what());
                if (should_reconnect_) schedule_reconnect();
                return;
            }
        }

        try {
            // 2. 设置连接选项：开启自动重连 (min 1s, max 10s Exponential
            // Backoff)
            mqtt::connect_options conn_opts;
            conn_opts.set_mqtt_version(MQTTVERSION_5);
            conn_opts.set_keep_alive_interval(20);
            conn_opts.set_connect_timeout(30);
            conn_opts.set_clean_start(true);
            conn_opts.set_automatic_reconnect(1, 10);  // <-- 底层自动断线重连！

            auto listener = std::make_shared<MqttConnectActionListener>(
                ioc_, this->shared_from_this());
            pending_listeners_.push_back(listener);

            spdlog::info(
                "[MqttClientAdapter] Connecting to MQTT Broker at tcp://{}:{} "
                "with "
                "client_id: {}...",
                host_, port_, client_id_);
            client_->connect(conn_opts, nullptr, *listener);
        } catch (const std::exception& ex) {
            spdlog::error("[MqttClientAdapter] Failed to start connection: {}",
                          ex.what());
            if (should_reconnect_) schedule_reconnect();
        }
    }

    void subscribe_to_routes() {
        if (routes_.empty() || !client_) return;
        try {
            for (const auto& [topic, handler] : routes_) {
                client_->subscribe(topic, handler->qos);
            }
        } catch (const std::exception& ex) {
            spdlog::error("[MqttClientAdapter] Subscribe failed: {}",
                          ex.what());
        }
    }

    // 1. 去除 MQTT 扩展订阅前缀，提取真实业务主题
    static std::string normalize_topic(const std::string& topic) {
        // 处理 $exclusive/xxx -> xxx
        const std::string exclusive_prefix = "$exclusive/";
        if (topic.rfind(exclusive_prefix, 0) == 0) {
            return topic.substr(exclusive_prefix.length());
        }

        // 兼容处理共享订阅 $share/group_name/xxx -> xxx
        const std::string share_prefix = "$share/";
        if (topic.rfind(share_prefix, 0) == 0) {
            size_t second_slash = topic.find('/', share_prefix.length());
            if (second_slash != std::string::npos) {
                return topic.substr(second_slash + 1);
            }
        }

        // 处理队列订阅 $queue/xxx -> xxx
        const std::string queue_prefix = "$queue/";
        if (topic.rfind(queue_prefix, 0) == 0) {
            return topic.substr(queue_prefix.length());
        }

        return topic;  // 没有特殊前缀，原样返回
    }

    void on_message(const std::string& topic, const std::string& payload,
                    int qos, const std::string& response_topic,
                    const std::string& correlation_data) {
        MqttMessage msg{topic,
                        payload,
                        static_cast<uint8_t>(qos),
                        0,
                        nlohmann::json::object(),
                        response_topic,
                        correlation_data};

        if (rpc_invoker_.try_intercept_rpc_response(msg)) return;

        auto it = routes_.find(topic);
        if (it != routes_.end()) {
            it->second->handle(this->shared_from_this(), msg);
        } else {
            spdlog::warn("[MqttClientAdapter] No route registered for: {}",
                         topic);
        }
    }

   protected:
    // 连接到底层 RPC Invoker
    dk::Future<nlohmann::json> do_request_base_json(
        const std::string& topic, const std::string& req_payload,
        uint32_t timeout_ms, const std::string& expect_resp_topic) {
        return rpc_invoker_.request(topic, req_payload, timeout_ms,
                                    expect_resp_topic);
    }

   public:
    MqttClientAdapter(std::shared_ptr<DerivedEngine> engine,
                      const std::string& host, unsigned short port)
        : BaseAdapter<Context, DerivedEngine>(engine),
          ioc_(engine->get_ioc()),
          host_(host),
          port_(port),
          rpc_invoker_(ioc_, engine),
          reconnect_timer_(ioc_) {
        rpc_invoker_.set_publish_fn(
            [this](const std::string& topic, const std::string& payload,
                   uint8_t qos, bool retain, const std::string& response_topic,
                   const std::string& correlation_data) {
                this->do_send_publish(topic, payload, qos, retain,
                                      response_topic, correlation_data);
            });
    }

    // 手动连接接口，允许传入 client_id
    void connect(const std::string& client_id = "") override {
        net::post(ioc_, [this, self = this->shared_from_this(), client_id]() {
            should_reconnect_ = true;
            if (!client_id.empty()) {
                client_id_ = client_id;
            }
            do_connect();
        });
    }

    void register_raw_handler(const std::string& topic,
                              std::function<void(const MqttMessage&)> handler,
                              uint8_t qos = 0) override {
        class RawMqttHandler : public IMqttProtocolHandler {
            std::function<void(const MqttMessage&)> handler_;

           public:
            RawMqttHandler(std::function<void(const MqttMessage&)> h, uint8_t q)
                : handler_(std::move(h)) {
                this->qos = q;
            }

            void handle(std::shared_ptr<IMqttSession>,
                        const MqttMessage& msg) override {
                if (handler_) {
                    handler_(msg);
                }
            }
        };

        register_handler(
            topic, std::make_shared<RawMqttHandler>(std::move(handler), qos));
        spdlog::info("MqttAdapter register raw route: topic={}", topic);
    }

    ~MqttClientAdapter() override {
        should_reconnect_ = false;
        boost::system::error_code ec;
        reconnect_timer_.cancel(ec);
        if (client_) {
            try {
                if (client_->is_connected()) client_->disconnect();
            } catch (...) {
            }
        }
    }

    // --- 接口实现：直接 Publish ---
    void publish(const std::string& topic, const std::string& payload,
                 uint8_t qos = 0, bool retain = false,
                 const std::string& correlation_data = "") override {
        do_send_publish(topic, payload, qos, retain, "", correlation_data);
    }

    void publish(const std::string& topic, const nlohmann::json& payload,
                 uint8_t qos = 0, bool retain = false,
                 const std::string& correlation_data = "") override {
        std::string data = payload.dump();
        do_send_publish(topic, data, qos, retain, "", correlation_data);
    }

    dk::Future<nlohmann::json> request(
        const std::string& topic, const nlohmann::json& req,
        uint32_t timeout_ms = 5000,
        const std::string& expect_resp_topic = "") override {
        return rpc_invoker_.request(topic, req.dump(), timeout_ms,
                                    expect_resp_topic);
    }

    // --- 注册处理函数 ---
    void register_handler(const std::string& topic,
                          std::shared_ptr<IMqttProtocolHandler> handler) {
        std::string normalized_topic = normalize_topic(topic);
        routes_[normalized_topic] = std::move(handler);
        if (is_connected_ && client_) {
            try {
                client_->subscribe(topic, routes_[normalized_topic]->qos);
            } catch (...) {
            }
        }
    }

   protected:
    void do_register_publish_handler(
        const std::string& topic,
        std::function<void(const nlohmann::json&)> internal_handler,
        uint8_t qos = 0) override {
        class SimplePublishHandler : public IMqttProtocolHandler {
            std::function<void(const nlohmann::json&)> handler_;

           public:
            explicit SimplePublishHandler(
                std::function<void(const nlohmann::json&)> h, uint8_t q)
                : handler_(std::move(h)) {
                this->qos = q;
            }

            void handle(std::shared_ptr<IMqttSession>,
                        const MqttMessage& msg) override {
                try {
                    nlohmann::json j_event =
                        msg.payload.empty()
                            ? nlohmann::json()
                            : nlohmann::json::parse(msg.payload);
                    if (handler_) handler_(j_event);
                } catch (const std::exception& ex) {
                    spdlog::error("[Mqtt] {} processing error: {}", msg.topic,
                                  ex.what());
                }
            }
        };

        register_handler(topic, std::make_shared<SimplePublishHandler>(
                                    std::move(internal_handler), qos));
        spdlog::info("MqttAdapter register publish route: topic={}", topic);
    }

    // 3. 注册 RPC 服务端处理中心（将原本名称 register_route 改为更见名知意的
    // register_rpc_handler）
    template <typename SpecificEvent, typename SpecificResult>
    void register_rpc_handler(
        const std::string& topic, uint32_t timeout_ms = 5000,
        std::function<void(SpecificEvent& e)> post_processor =
            [](SpecificEvent&) {}) {
        class MqttRpcHandler : public IMqttProtocolHandler {
            MqttClientAdapter* adapter_;
            uint32_t timeout_ms_;
            std::string topic_;
            std::string resp_topic_;
            std::function<void(SpecificEvent& e)> post_processor_;

           public:
            MqttRpcHandler(MqttClientAdapter* adapter, uint32_t timeout_ms,
                           std::string topic,
                           std::function<void(SpecificEvent& e)> pp)
                : IMqttProtocolHandler(qos),
                  adapter_(adapter),
                  timeout_ms_(timeout_ms),
                  topic_(std::move(topic)),
                  resp_topic_(topic_ + "/resp"),
                  post_processor_(std::move(pp)) {}

            void handle(std::shared_ptr<IMqttSession> session,
                        const MqttMessage& msg) override {
                try {
                    SpecificEvent event =
                        msg.payload.empty()
                            ? SpecificEvent{}
                            : nlohmann::json::parse(msg.payload)
                                  .template get<SpecificEvent>();

                    post_processor_(event);

                    std::string resp_topic = msg.response_topic.empty()
                                                 ? resp_topic_
                                                 : msg.response_topic;
                    std::string correlation_id = msg.correlation_data;
                    if (correlation_id.empty()) {
                        try {
                            auto j = nlohmann::json::parse(msg.payload);
                            if (j.contains("msg_id")) {
                                correlation_id =
                                    std::to_string(j["msg_id"].get<uint64_t>());
                            }
                        } catch (...) {
                        }
                    }

                    adapter_->dispatch_async(event, timeout_ms_)
                        .then([session, resp_topic,
                               correlation_id](SpecificResult result) {
                            nlohmann::json j_res = result;
                            if (!correlation_id.empty()) {
                                try {
                                    j_res["msg_id"] =
                                        std::stoull(correlation_id);
                                } catch (...) {
                                }
                            }
                            session->publish(resp_topic, j_res, 0, false,
                                             correlation_id);
                        })
                        .catch_error([session, resp_topic,
                                      correlation_id](std::exception_ptr e) {
                            std::string err_msg = "Unknown internal error";
                            try {
                                if (e) std::rethrow_exception(e);
                            } catch (const std::exception& ex) {
                                err_msg = ex.what();
                            } catch (...) {
                            }

                            nlohmann::json j_err = {{"status", "error"},
                                                    {"msg", err_msg}};
                            if (!correlation_id.empty()) {
                                try {
                                    j_err["msg_id"] =
                                        std::stoull(correlation_id);
                                } catch (...) {
                                }
                            }
                            session->publish(resp_topic, j_err, 0, false,
                                             correlation_id);
                        });
                } catch (const std::exception& ex) {
                    spdlog::error("[MQTT Parse Error] Topic {}: {}", topic_,
                                  ex.what());

                    std::string resp_topic = msg.response_topic.empty()
                                                 ? resp_topic_
                                                 : msg.response_topic;
                    std::string correlation_id = msg.correlation_data;
                    if (correlation_id.empty()) {
                        try {
                            auto j = nlohmann::json::parse(msg.payload);
                            if (j.contains("msg_id")) {
                                correlation_id =
                                    std::to_string(j["msg_id"].get<uint64_t>());
                            }
                        } catch (...) {
                        }
                    }

                    nlohmann::json j_err = {
                        {"status", "error"},
                        {"msg", "Bad Request / JSON Parse Error"}};
                    if (!correlation_id.empty()) {
                        try {
                            j_err["msg_id"] = std::stoull(correlation_id);
                        } catch (...) {
                        }
                    }
                    session->publish(resp_topic, j_err, 0, false,
                                     correlation_id);
                }
            }
        };

        register_handler(topic, std::make_shared<MqttRpcHandler>(
                                    this, timeout_ms, topic, post_processor));
        spdlog::info("MqttAdapter register RPC route: topic={} -> resp={}/resp",
                     topic, topic);
    }
};

}  // namespace dk