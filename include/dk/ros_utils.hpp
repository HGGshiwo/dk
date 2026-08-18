#pragma once

#ifdef USE_ROS1
#include <ros/ros.h>
#include <spdlog/spdlog.h>

template <typename MsgType>
class ServiceClient {
    ros::NodeHandle nh_;
    ros::ServiceClient srv_client_;
    std::string srv_name_;
    std::function<bool(MsgType)> check_;

   public:
    ServiceClient(std::string srv_name, std::function<bool(MsgType)> check)
        : srv_name_(srv_name), check_(check) {
        srv_client_ = nh_.serviceClient<MsgType>(srv_name);
    }

    ServiceClient(ros::NodeHandle nh, std::string srv_name,
                  std::function<bool(MsgType)> check)
        : nh_(nh), srv_name_(srv_name), check_(check) {
        srv_client_ = nh_.serviceClient<MsgType>(srv_name);
    }

    bool call(MsgType& srv) {
        if (!srv_client_.call(srv)) {
            spdlog::error("{} service call failed!", srv_name_);
            return false;
        }
        if (check_(srv)) {
            spdlog::info("{} service call success!", srv_name_);
            return true;
        }
        spdlog::info("{} service call return false!", srv_name_);
        return false;
    }
};
#elif defined(USE_ROS2)
#include <spdlog/spdlog.h>

#include <any>
#include <memory>
#include <rcl_interfaces/srv/get_parameters.hpp>
#include <rcl_interfaces/srv/set_parameters.hpp>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <type_traits>
#include <vector>

// Split helper
inline std::pair<std::string, std::string> split_ros2_param(
    const std::string& full_path) {
    size_t last_slash = full_path.find_last_of('/');
    if (last_slash == std::string::npos) {
        return {"", full_path};
    }
    return {full_path.substr(0, last_slash), full_path.substr(last_slash + 1)};
}

// Remote parameters setters/getters
template <typename T>
bool set_remote_parameter(std::shared_ptr<rclcpp::Node> node,
                          const std::string& full_path, const T& value) {
    auto [node_name, param_name] = split_ros2_param(full_path);
    auto client = node->create_client<rcl_interfaces::srv::SetParameters>(
        node_name + "/set_parameters");
    if (!client->wait_for_service(std::chrono::seconds(1))) {
        return false;
    }
    auto req = std::make_shared<rcl_interfaces::srv::SetParameters::Request>();
    rcl_interfaces::msg::Parameter param;
    param.name = param_name;
    if constexpr (std::is_same_v<T, bool>) {
        param.value.type = rcl_interfaces::msg::ParameterType::PARAMETER_BOOL;
        param.value.bool_value = value;
    } else if constexpr (std::is_integral_v<T>) {
        param.value.type =
            rcl_interfaces::msg::ParameterType::PARAMETER_INTEGER;
        param.value.integer_value = value;
    } else if constexpr (std::is_floating_point_v<T>) {
        param.value.type = rcl_interfaces::msg::ParameterType::PARAMETER_DOUBLE;
        param.value.double_value = value;
    } else if constexpr (std::is_same_v<T, std::string>) {
        param.value.type = rcl_interfaces::msg::ParameterType::PARAMETER_STRING;
        param.value.string_value = value;
    }
    req->parameters.push_back(param);
    auto future = client->async_send_request(req);
    if (future.wait_for(std::chrono::seconds(1)) == std::future_status::ready) {
        auto res = future.get();
        return !res->results.empty() && res->results[0].successful;
    }
    return false;
}

template <typename T>
T get_remote_parameter(std::shared_ptr<rclcpp::Node> node,
                       const std::string& full_path, const T& default_val) {
    auto [node_name, param_name] = split_ros2_param(full_path);
    auto client = node->create_client<rcl_interfaces::srv::GetParameters>(
        node_name + "/get_parameters");
    if (!client->wait_for_service(std::chrono::seconds(1))) {
        return default_val;
    }
    auto req = std::make_shared<rcl_interfaces::srv::GetParameters::Request>();
    req->names.push_back(param_name);
    auto future = client->async_send_request(req);
    if (future.wait_for(std::chrono::seconds(1)) == std::future_status::ready) {
        auto res = future.get();
        if (!res->values.empty()) {
            auto& val = res->values[0];
            if constexpr (std::is_same_v<T, bool>) {
                if (val.type ==
                    rcl_interfaces::msg::ParameterType::PARAMETER_BOOL)
                    return val.bool_value;
            } else if constexpr (std::is_integral_v<T>) {
                if (val.type ==
                    rcl_interfaces::msg::ParameterType::PARAMETER_INTEGER)
                    return val.integer_value;
            } else if constexpr (std::is_floating_point_v<T>) {
                if (val.type ==
                    rcl_interfaces::msg::ParameterType::PARAMETER_DOUBLE)
                    return val.double_value;
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (val.type ==
                    rcl_interfaces::msg::ParameterType::PARAMETER_STRING)
                    return val.string_value;
            }
        }
    }
    return default_val;
}

class RosPublisher {
    std::shared_ptr<void> pub_;
    std::function<void(const void*)> publish_fn_;

   public:
    RosPublisher() = default;

    template <typename T>
    RosPublisher(std::shared_ptr<rclcpp::Publisher<T>> pub)
        : pub_(pub), publish_fn_([pub](const void* msg_ptr) {
              pub->publish(*static_cast<const T*>(msg_ptr));
          }) {}

    template <typename T>
    void publish(const T& msg) {
        if (publish_fn_) {
            publish_fn_(&msg);
        }
    }

    template <typename T>
    void publish(const std::shared_ptr<const T>& msg) {
        if (publish_fn_ && msg) {
            publish_fn_(msg.get());
        }
    }
};

class RosNodeHandle {
    std::shared_ptr<rclcpp::Node> node_;

   public:
    RosNodeHandle() = default;
    RosNodeHandle(std::shared_ptr<rclcpp::Node> node) : node_(node) {}

    std::shared_ptr<rclcpp::Node> get_node() const { return node_; }

    template <typename T>
    void setParam(const std::string& name, const T& val) {
        if (node_) set_remote_parameter(node_, name, val);
    }

    template <typename T>
    void param(const std::string& name, T& val, const T& default_val) {
        if (node_)
            val = get_remote_parameter(node_, name, default_val);
        else
            val = default_val;
    }

    template <typename T>
    RosPublisher advertise(const std::string& topic, size_t queue_size) {
        if (node_) {
            return RosPublisher(node_->create_publisher<T>(topic, queue_size));
        }
        return RosPublisher();
    }
};

// Service wrapper
template <typename T>
struct Ros2ServiceWrapper {
    using ServiceType = T;
    typename T::Request request;
    typename T::Response response;
};

// ServiceClient for ROS 2
template <typename MsgWrapperType>
class ServiceClient {
    using ServiceType = typename MsgWrapperType::ServiceType;
    std::shared_ptr<rclcpp::Node> node_;
    typename rclcpp::Client<ServiceType>::SharedPtr srv_client_;
    std::string srv_name_;
    std::function<bool(MsgWrapperType&)> check_;

   public:
    ServiceClient(std::shared_ptr<rclcpp::Node> node, std::string srv_name,
                  std::function<bool(MsgWrapperType)> check)
        : node_(node), srv_name_(srv_name), check_(check) {
        if (node_) {
            srv_client_ = node_->create_client<ServiceType>(srv_name);
        }
    }

    bool call(MsgWrapperType& srv) {
        if (!srv_client_) return false;
        if (!srv_client_->wait_for_service(std::chrono::seconds(1))) {
            spdlog::error("Service {} not available!", srv_name_);
            return false;
        }
        auto req_ptr =
            std::make_shared<typename ServiceType::Request>(srv.request);
        auto future = srv_client_->async_send_request(req_ptr);
        if (future.wait_for(std::chrono::seconds(3)) ==
            std::future_status::ready) {
            auto response = future.get();
            srv.response = *response;
            if (check_(srv)) {
                spdlog::info("{} service call success!", srv_name_);
                return true;
            }
            spdlog::info("{} service call return false!", srv_name_);
            return false;
        } else {
            spdlog::error("{} service call timeout!", srv_name_);
            return false;
        }
    }
};

#endif
