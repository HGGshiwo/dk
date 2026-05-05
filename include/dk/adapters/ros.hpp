#pragma once
#include <ros/ros.h>

#include <boost/shared_ptr.hpp>
#include <memory>
#include <string>
#include <vector>

#include "../core.hpp"
#include "../utils.hpp"

namespace dk {
template <typename EventType, typename Context, typename EngineType>
class RosAdapter : public dk::BaseAdapter<EventType, Context, EngineType> {
   private:
    ros::NodeHandle nh_;
    std::vector<ros::Subscriber> subs_;  // 自动管理生命周期

   public:
    RosAdapter(std::shared_ptr<EngineType> engine, ros::NodeHandle& nh)
        : dk::BaseAdapter<EventType, Context, EngineType>(engine), nh_(nh) {};

    /*
    ## 将rostopic消息同步到Context，使用示例：
    ```
    bind_context<geometry_msgs::Twist, double>(
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

        // 假设 ctx 是类的成员变量，或者你可以从某个地方获取到它
        auto sub = nh_.subscribe<RosMsgType>(
            topic_name, 1, [this, cb = std::forward<F>(cb)](const typename RosMsgType::ConstPtr& data) -> void {
                static_assert(std::is_convertible_v<const typename RosMsgType::ConstPtr&, ExactType>,
                              "\n\n=======================================================\n"
                              "[DANKONG ERROR] 回调函数参数类型错误！\n"
                              "ROS 底层传递的是智能指针，而你的 Lambda 期望的是普通对象。\n"
                              "请将你的 Lambda 参数改为: 'const auto&' 或 'const MsgType::ConstPtr&'\n"
                              "=======================================================\n\n");
                cb(data, this->engine_->get_context());
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
    template <typename Callable>
    void bind_event(const std::string& topic_name, Callable&& translator) {
        // 1. 提取返回值类型（即转换后的 Event 类型）
        using Trails = meta_utils::callable_traits<decltype(translator)>;
        using ReturnType = typename Trails::return_type;

        // 2. 提取参数类型（比如 std_msgs::String::ConstPtr）
        using ArgType = typename Trails::template arg_type<0>;

        // 3. 从 ArgType 反向提取出原始的 RosMsgType（比如 std_msgs::String）
        using RosMsgType = typename meta_utils::extract_ros_msg<ArgType>::type;

        // 订阅 ROS 消息
        auto sub = nh_.subscribe<RosMsgType>(
            topic_name, 10,
            [this, translator = std::forward<Callable>(translator)](const typename RosMsgType::ConstPtr& msg) {
                static_assert(std::is_invocable_v<decltype(translator), decltype(msg)>,
                              "\n\n=======================================================\n"
                              "[DANKONG ERROR] 回调函数参数类型错误！\n"
                              "ROS 底层传递的是智能指针，而你的 Lambda 期望的是普通对象。\n"
                              "请将你的 Lambda 参数改为: 'const auto&' 或 'const MsgType::ConstPtr&'\n"
                              "=======================================================\n\n");
                // 显式指定 EventType，确保类型安全
                ReturnType e = translator(msg);

                // 推入引擎队列
                this->engine_->dispatch(e);
            });

        subs_.push_back(sub);
    }
};
}  // namespace dk