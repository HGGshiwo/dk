#pragma once
#include <ros/ros.h>

#include <memory>
#include <string>
#include <vector>

#include "../core.hpp"

// --- 步骤 A：提取函数的 返回值 和 参数类型 ---
template <typename T>
struct lambda_traits : public lambda_traits<decltype(&T::operator())> {};
// 针对 Lambda 的特化（const 成员函数）
template <typename ClassType, typename ReturnType, typename ArgType>
struct lambda_traits<ReturnType (ClassType::*)(ArgType) const> {
    using return_type = ReturnType;
    using arg_type = ArgType;
};
// --- 步骤 B：从 ConstPtr 中提取出原始的 ROS 消息类型 ---
// ROS 的 ConstPtr 等价于 const boost::shared_ptr<const T>&
template <typename T>
struct extract_ros_msg;
// 剥离 const 引用和 boost::shared_ptr
template <typename T>
struct extract_ros_msg<const boost::shared_ptr<const T>&> {
    using type = T;
};

template <typename EventType, typename EngineType, typename Context>
class RosAdapter : public dk::BaseAdapter<EventType, EngineType> {
   private:
    ros::NodeHandle nh_;
    std::vector<ros::Subscriber> subs_;  // 自动管理生命周期
   public:
    RosAdapter(std::shared_ptr<TEngine> engine, ros::NodeHandle& nh) : nh_(nh), engine_(engine) {}

    /*
    ## 将rostopic消息同步到Context，使用示例：
    ```
    bind_context<geometry_msgs::Twist, double>(
        "/vehicle/speed",                   // 参数 1: ROS Topic 名字
        &geometry_msgs::Twist::linear_x,    // 参数 2: MsgType::* msg_field (提取源)
        &Context::current_speed             // 参数 3: Context::* ctx_field (存储目标)
    );
    ```
    */
    template <typename MsgType, typename T>
    void bind_context(const std::string& topic_name,
                      T MsgType::*msg_field,               // 提取 ROS 消息的哪个字段
                      std::atomic<T> Context::*ctx_field)  // 存入 Context 的哪个字段
    {
        auto sub =
            nh_.subscribe<MsgType>(topic_name, 1, [this, msg_field, ctx_field](const typename MsgType::ConstPtr& msg) {
                // 收到消息后，自动提取并写入黑板！
                (this->engine_->get_context().*ctx_field).store(msg->*msg_field);
            });
        subs_.push_back(sub);
    }

    /*
    ## 将rostopic消息绑定到事件，并触发事件，使用示例：
    ```
    bind_event<TakeoffEvent>(
        "/vehicle/speed",                   // 参数 1: ROS Topic 名字
        [](std_msg::string msg) { return TakeoffEvent(); } // 参数 2: ros消息转为一个event
    );
    ```
    */
    template <typename Event>
    void bind_event(const std::string& topic_name, Callbale translator) {
        // 1. 通过 traits 提取出 Lambda 的返回值 (PureCppType) 和 参数 (ArgType)
        using ArgType = typename lambda_traits<Callable>::arg_type;

        // 2. 从 ArgType (ConstPtr) 中反向提取出原始的 RosMsgType
        using RosMsgType = typename extract_ros_msg<ArgType>::type;

        auto sub = nh_.subscribe<RosMsgType>(
            topic_name, 10, [this, internal_event_id, translator](const typename RosMsgType::ConstPtr& msg) {
                Event e = translator(msg);
                // 推入引擎队列
                this->engine_->dispatch(e);
            });
        subs_.push_back(sub);
    }
};