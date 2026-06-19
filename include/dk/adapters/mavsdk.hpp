#pragma once

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/action/action.h>
#include <mavsdk/plugins/mavlink_passthrough/mavlink_passthrough.h>
#include <mavsdk/plugins/offboard/offboard.h>
#include <mavsdk/plugins/param/param.h>
#include <mavsdk/plugins/telemetry/telemetry.h>

#include <functional>
#include <memory>

#include "dk/adapters/base.hpp"

namespace dk {

template <typename Context, typename DerivedEngine>
class MavsdkAdapter : public BaseAdapter<Context, DerivedEngine> {
   private:
    std::shared_ptr<mavsdk::System> system_;
    std::shared_ptr<mavsdk::Telemetry> telemetry_;
    std::shared_ptr<mavsdk::Action> action_;
    std::shared_ptr<mavsdk::Offboard> offboard_;
    std::shared_ptr<mavsdk::Param> param_;
    std::shared_ptr<mavsdk::MavlinkPassthrough> passthrough_;
    std::vector<std::any> subscription_handles_;

   public:
    MavsdkAdapter(std::shared_ptr<DerivedEngine> engine, std::shared_ptr<mavsdk::System> system)
        : BaseAdapter<Context, DerivedEngine>(engine), system_(system) {
        telemetry_ = std::make_shared<mavsdk::Telemetry>(system_);
        action_ = std::make_shared<mavsdk::Action>(system_);
        offboard_ = std::make_shared<mavsdk::Offboard>(system_);
        param_ = std::make_shared<mavsdk::Param>(system_);
        passthrough_ = std::make_shared<mavsdk::MavlinkPassthrough>(system_);
    }

    std::shared_ptr<mavsdk::System> get_system() { return system_; }
    std::shared_ptr<mavsdk::Telemetry> get_telemetry() { return telemetry_; }
    std::shared_ptr<mavsdk::Action> get_action() { return action_; }
    std::shared_ptr<mavsdk::Offboard> get_offboard() { return offboard_; }
    std::shared_ptr<mavsdk::Param> get_param() { return param_; }

    template <typename SubscribeFunc, typename Callback>
    void bind_system_context(SubscribeFunc sub_func, Callback cb) {
        if (!system_) {
            spdlog::error("System is null, cannot bind system context");
            return;
        }

        auto handle = (system_.get()->*sub_func)(
            [this, cb](auto&&... args) { cb(std::forward<decltype(args)>(args)..., this->engine_->get_context()); });
        subscription_handles_.push_back(std::move(handle));
    }

    template <typename SubscribeFunc, typename Callback>
    void bind_system_event(SubscribeFunc sub_func, Callback cb) {
        if (!system_) {
            spdlog::error("System is null, cannot bind system event");
            return;
        }

        auto handle = (system_.get()->*sub_func)([this, cb](auto&&... args) {
            auto event = cb(std::forward<decltype(args)>(args)...);
            this->engine_->dispatch_internal(event);
        });
        subscription_handles_.push_back(std::move(handle));
    }

    template <typename SubscribeFunc, typename CallbackFunc>
    void bind_telemetry_context(SubscribeFunc subscribe_func, CallbackFunc callback) {
        auto handle = (telemetry_.get()->*subscribe_func)(
            [this, callback](auto... args) { callback(args..., this->engine_->get_context()); });
        subscription_handles_.push_back(std::move(handle));
    }

    template <typename SubscribeFunc, typename CallbackFunc>
    void bind_telemetry_event(SubscribeFunc subscribe_func, CallbackFunc callback) {
        auto handle = (telemetry_.get()->*subscribe_func)([this, callback](auto... args) {
            auto event = callback(args...);
            this->engine_->dispatch_internal(event);
        });
        subscription_handles_.push_back(std::move(handle));
    }

    template <typename Callback>
    void bind_passthrough_context(uint16_t message_id, Callback cb) {
        if (!passthrough_) {
            spdlog::error("MavlinkPassthrough plugin is null, cannot bind message!");
            return;
        }

        auto handle = passthrough_->subscribe_message(message_id, [this, cb](const mavlink_message_t& msg) {
            // 将原始的 mavlink_message_t 和 ctx 一并传给业务层的 lambda
            cb(msg, this->engine_->get_context());
        });
        subscription_handles_.push_back(std::move(handle));
    }

    template <typename Callback>
    void bind_passthrough_event(uint16_t message_id, Callback cb) {
        if (!passthrough_) {
            spdlog::error("MavlinkPassthrough plugin is null, cannot bind event!");
            return;
        }

        auto handle = passthrough_->subscribe_message(message_id, [this, cb](const mavlink_message_t& msg) {
            // 调用业务层 lambda，将 mavlink_message_t 转换为自定义 Event
            auto event = cb(msg);
            // 发送到状态机引擎
            this->engine_->dispatch_internal(event);
        });
        subscription_handles_.push_back(std::move(handle));
    }
};

}  // namespace dk