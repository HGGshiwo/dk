#pragma once

#include "utils/ros.hpp"

// 1. 头文件隔离
#ifdef USE_ROS1
#include <ros/ros.h>
#include <std_msgs/String.h>

#include <boost/shared_ptr.hpp>
#elif defined(USE_ROS2)
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#endif
#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "../utils.hpp"
#include "dk/adapters/base.hpp"

namespace dk {

#ifdef USE_ROS1
using DefaultStringMsg = std_msgs::String;
#elif defined(USE_ROS2)
using DefaultStringMsg = std_msgs::msg::String;
#endif

template <typename Context, typename EngineType>
class RosAdapter : public dk::BaseAdapter<Context, EngineType> {
   private:
// 2. 成员变量隔离
#ifdef USE_ROS1
    ros::NodeHandle nh_;
    std::vector<ros::Subscriber> subs_;  // 自动管理生命周期
#elif defined(USE_ROS2)
    rclcpp::Node::SharedPtr node_;
    std::vector<std::shared_ptr<rclcpp::SubscriptionBase>> subs_;
#endif

   public:
// 3. 构造函数隔离
#ifdef USE_ROS1
    RosAdapter(std::shared_ptr<EngineType> engine, ros::NodeHandle nh)
        : dk::BaseAdapter<Context, EngineType>(engine), nh_(nh) {}
#elif defined(USE_ROS2)
    RosAdapter(std::shared_ptr<EngineType> engine, rclcpp::Node::SharedPtr node)
        : dk::BaseAdapter<Context, EngineType>(engine), node_(node) {}
#endif

    /*
    ## 将rostopic消息同步到Context，使用示例：
    ```
    bind_context(
        "/vehicle/speed",                   // 参数 1: ROS Topic 名字
        [](auto data, Context ctx) -> void {ctx.speed = data.twist.linear.x;}
    );
    ```
    */
    template <typename F>
    void bind_context(const std::string& topic_name, F&& cb) {
        // 自动推导出 MsgType
        using Traits = meta_utils::callable_traits<decltype(cb)>;
        using MsgType = typename Traits::template arg_type<0>;
        using ExactType = typename Traits::template exact_arg_type<0>;
        using RosMsgType = typename meta_utils::extract_ros_msg<MsgType>::type;

#ifdef USE_ROS1
        auto sub = nh_.subscribe<RosMsgType>(
            topic_name, 1,
            [this, cb = std::forward<F>(cb)](
                const typename RosMsgType::ConstPtr& data) -> void {
                static_assert(
                    std::is_convertible<const typename RosMsgType::ConstPtr&,
                                        ExactType>::value,
                    "Lambda arguments has wrong type: change MsgType to const "
                    "MsgType::ConstPtr& instead!");
                cb(data, this->engine_->get_context());
            });
        subs_.push_back(sub);
#elif defined(USE_ROS2)
        // 4. ROS2 下直接使用 node_
        auto sub = node_->create_subscription<RosMsgType>(
            topic_name, 10,
            [this, cb = std::forward<F>(cb)](ExactType data) -> void {
                cb(data, this->engine_->get_context());
            });
        subs_.push_back(sub);
#endif
    }

    /*
    ## 将rostopic消息绑定到事件，并触发事件，使用示例：
    ```
    bind_event(
        "/vehicle/speed",                   // 参数 1: ROS Topic 名字
        [](std_msg::string msg) { return TakeoffEvent(); } // 参数 2:
    ros消息转为一个event
    );
    ```
    */
    template <typename Callable>
    void bind_event(const std::string& topic_name, Callable&& translator) {
        // 1. 提取返回值类型（即转换后的 Event 类型）
        using Trails = meta_utils::callable_traits<decltype(translator)>;
        using ReturnType = typename Trails::return_type;

        // 2. 提取参数类型（比如 std_msgs::String::ConstPtr）
        using ArgType = typename Trails::template arg_type<0>;

        // 3. 从 ArgType 反向提取出原始 of RosMsgType（比如 std_msgs::String）
        using RosMsgType = typename meta_utils::extract_ros_msg<ArgType>::type;

#ifdef USE_ROS1
        // 订阅 ROS 消息
        auto sub = nh_.subscribe<RosMsgType>(
            topic_name, 10,
            [this, translator = std::forward<Callable>(translator)](
                const typename RosMsgType::ConstPtr& msg) {
                static_assert(
                    std::is_invocable_v<decltype(translator), decltype(msg)>,
                    "Lambda arguments has wrong type: change MsgType to const "
                    "MsgType::ConstPtr& instead!");
                // 显式指定 EventType，确保类型安全
                ReturnType e = translator(msg);

                // 推入引擎队列
                this->engine_->dispatch(e);
            });
        subs_.push_back(sub);
#elif defined(USE_ROS2)
        // 4. ROS2 下直接使用 node_
        auto sub = node_->create_subscription<RosMsgType>(
            topic_name, 10,
            [this,
             translator = std::forward<Callable>(translator)](ArgType msg) {
                ReturnType e = translator(msg);
                this->engine_->dispatch(e);
            });
        subs_.push_back(sub);
#endif
    }

    /*
    ## 将 JSON 格式的 rostopic 消息直接转为事件并触发，使用示例：
    ```
    bind_json_event<TakeoffEvent>(
        "/vehicle/takeoff"                  // 参数 1: ROS Topic 名字
    );
    ```
    */
    template <typename SpecificEvent, typename MsgType = DefaultStringMsg>
    void bind_json_event(
        const std::string& topic_name,
        std::function<void(SpecificEvent& e)> post_processor =
            [](SpecificEvent& e) {}) {
#ifdef USE_ROS1
        auto sub = nh_.subscribe<MsgType>(
            topic_name, 10,
            [this, post_processor = std::move(post_processor),
             topic_name](const typename MsgType::ConstPtr& msg) {
                try {
                    SpecificEvent event =
                        json::parse(msg->data).template get<SpecificEvent>();
                    post_processor(event);
                    this->engine_->dispatch(event);
                } catch (const std::exception& e) {
                    spdlog::error("Failed to parse json event from {}: {}",
                                  topic_name, e.what());
                }
            });
        subs_.push_back(sub);
#elif defined(USE_ROS2)
        auto sub = node_->create_subscription<MsgType>(
            topic_name, 10,
            [this, post_processor = std::move(post_processor),
             topic_name](const std::shared_ptr<const MsgType> msg) {
                try {
                    SpecificEvent event =
                        json::parse(msg->data).template get<SpecificEvent>();
                    post_processor(event);
                    this->engine_->dispatch(event);
                } catch (const std::exception& e) {
                    spdlog::error("Failed to parse json event from {}: {}",
                                  topic_name, e.what());
                }
            });
        subs_.push_back(sub);
#endif
    }
};
}  // namespace dk