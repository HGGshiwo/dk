#pragma once

#include "ros_utils.hpp"

// 根据宏定义引入对应的 ROS 头文件
#ifdef USE_ROS1
#include <ros/ros.h>
#elif defined(USE_ROS2)
#include <rclcpp/rclcpp.hpp>
#endif

#include <boost/asio.hpp>
#include <chrono>  // 确保引入 chrono 库供 ROS 2 的定时器使用
#include <functional>
#include <memory>

#include "./ITimeProvider.hpp"

namespace dk {

class RosTimeProvider : public ITimeProvider {
   private:
    boost::asio::io_context& io_;

// 根据 ROS 版本定义不同的节点/句柄成员
#ifdef USE_ROS1
    ros::NodeHandle nh_;
#elif defined(USE_ROS2)
    rclcpp::Node::SharedPtr node_;
#endif

   public:
// 根据 ROS 版本定义不同的构造函数
#ifdef USE_ROS1
    RosTimeProvider(ros::NodeHandle nh, boost::asio::io_context& io)
        : nh_(nh), io_(io) {}
#elif defined(USE_ROS2)
    RosTimeProvider(rclcpp::Node::SharedPtr node, boost::asio::io_context& io)
        : node_(node), io_(io) {}
#endif

    double now() override {
#ifdef USE_ROS1
        return ros::Time::now().toSec();
#elif defined(USE_ROS2)
        return node_->now().seconds();
#else
        return 0.0;
#endif
    }

    void sleep_for(double seconds) override {
#ifdef USE_ROS1
        ros::Duration(seconds).sleep();
#elif defined(USE_ROS2)
        rclcpp::sleep_for(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(seconds)));
#endif
    }

    std::function<void()> set_timeout(double seconds,
                                      std::function<void()> callback) override {
#ifdef USE_ROS1
        // 创建 oneshot = true 的 Timer
        auto timer = std::make_shared<ros::Timer>();
        *timer = nh_.createTimer(
            ros::Duration(seconds),
            [this, callback](const ros::TimerEvent&) {
                // 极度重要：从 ROS 线程投递到 Asio IO 队列，保证线程安全！
                boost::asio::post(this->io_, callback);
            },
            true);

        return [timer]() { timer->stop(); };
#elif defined(USE_ROS2)
        struct TimerHolder {
            rclcpp::TimerBase::SharedPtr timer;
        };
        auto holder = std::make_shared<TimerHolder>();
        std::weak_ptr<TimerHolder> weak_holder = holder;

        holder->timer = node_->create_wall_timer(
            std::chrono::duration<double>(seconds),
            [this, callback, weak_holder]() {
                if (auto shared_holder = weak_holder.lock()) {
                    if (shared_holder->timer) {
                        shared_holder->timer->cancel();
                    }
                }
                boost::asio::post(this->io_, callback);
            });

        return [holder]() {
            if (holder->timer) {
                holder->timer->cancel();
            }
        };
#endif
    }

    std::function<void()> start_ticker(
        double interval_seconds, std::function<void()> callback) override {
#ifdef USE_ROS1
        // 创建 oneshot = false 的 Timer
        auto timer = std::make_shared<ros::Timer>();
        *timer = nh_.createTimer(
            ros::Duration(interval_seconds),
            [this, callback](const ros::TimerEvent&) {
                // 极度重要：从 ROS 线程投递到 Asio IO 队列，保证线程安全！
                boost::asio::post(this->io_, callback);
            },
            false);

        return [timer]() { timer->stop(); };
#elif defined(USE_ROS2)
        auto timer = node_->create_wall_timer(
            std::chrono::duration<double>(interval_seconds),
            [this, callback]() { boost::asio::post(this->io_, callback); });

        return [timer]() { timer->cancel(); };
#endif
    }
};

}  // namespace dk