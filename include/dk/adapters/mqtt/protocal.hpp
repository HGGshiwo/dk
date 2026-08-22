#pragma once
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "dk/future.hpp"
#include "spdlog/spdlog.h"

namespace dk {

struct MqttConnectEvent {};

struct MqttMessage {
    std::string topic;
    std::string payload;
    uint8_t qos = 0;
    uint16_t packet_id = 0;
    nlohmann::json properties = nlohmann::json::object();
    std::string response_topic;
    std::string correlation_data;
};

class IMqttSession;

// 底层消息路由处理接口
struct IMqttProtocolHandler {
    virtual ~IMqttProtocolHandler() = default;
    uint8_t qos = 0;
    virtual void handle(std::shared_ptr<IMqttSession> session,
                        const MqttMessage& msg) = 0;
};

// =================================================================
// IMqttSession - 会话发包接口（不含上下文依赖）
// =================================================================
class IMqttSession {
   public:
    virtual ~IMqttSession() = default;

    // --- Category 1: Direct Publish (直接发布，无返回) ---
    virtual void publish(const std::string& topic, const std::string& payload,
                         uint8_t qos = 0, bool retain = false,
                         const std::string& correlation_data = "") = 0;

    // JSON 发送版本
    virtual void publish(const std::string& topic,
                         const nlohmann::json& payload, uint8_t qos = 0,
                         bool retain = false,
                         const std::string& correlation_data = "") {
        publish(topic, payload.dump(), qos, retain, correlation_data);
    }
};

// =================================================================
// IMqttClient - 对外客户端总接口（直接使用，无需模板）
// =================================================================
class IMqttClient : public IMqttSession {
   protected:
    virtual void do_register_publish_handler(
        const std::string& topic,
        std::function<void(const nlohmann::json&)> internal_handler,
        uint8_t qos = 0) = 0;

   public:
    virtual ~IMqttClient() = default;

    // 手动连接接口，允许传入 client_id
    virtual void connect(const std::string& client_id = "") = 0;

    // 注册原始 MQTT 消息接收回调
    virtual void register_raw_handler(
        const std::string& topic,
        std::function<void(const MqttMessage&)> handler,
        uint8_t qos = 0) = 0;

    // --- Category 2: Request (请求-响应模型 RPC) ---
    virtual dk::Future<nlohmann::json> request(
        const std::string& topic, const nlohmann::json& req,
        uint32_t timeout_ms = 5000,
        const std::string& expect_resp_topic = "") = 0;

    // 发送请求(不需要响应)
    virtual void publish(const std::string& topic,
                         const nlohmann::json& payload, uint8_t qos = 0,
                         bool retain = false,
                         const std::string& correlation_data = "") override = 0;

    // 注册普通单向消息接收 (提供 SpecificEvent)
    template <typename SpecificEvent, typename Callback>
    void register_publish_handler(const std::string& topic,
                                  Callback&& business_handler,
                                  uint8_t qos = 0) {
        spdlog::info("[Mqtt] register to {}, qos={}", topic, qos);
        do_register_publish_handler(
            topic,
            [business_handler = std::forward<Callback>(business_handler),
             topic](const nlohmann::json& j_event) {
                try {
                    SpecificEvent event = j_event.template get<SpecificEvent>();
                    spdlog::info("[Mqtt] {} receive: {}", topic,
                                 j_event.dump());
                    business_handler(event);
                } catch (const std::exception& ex) {
                    spdlog::error("[Mqtt] handler error: {}", ex.what());
                }
            },
            qos);
    }
};

}  // namespace dk