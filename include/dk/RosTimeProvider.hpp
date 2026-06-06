#pragma once

#include <ros/ros.h>

#include <boost/asio.hpp>
#include <functional>
#include <memory>

#include "./ITimeProvider.hpp"

namespace dk {

class RosTimeProvider : public ITimeProvider {
   private:
    ros::NodeHandle& nh_;
    boost::asio::io_context& io_;

   public:
    RosTimeProvider(ros::NodeHandle& nh, boost::asio::io_context& io) : nh_(nh), io_(io) {}

    double now() override { return ros::Time::now().toSec(); }

    void sleep_for(double seconds) override { ros::Duration(seconds).sleep(); }

    std::function<void()> set_timeout(double seconds, std::function<void()> callback) override {
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
    }

    std::function<void()> start_ticker(double interval_seconds, std::function<void()> callback) override {
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
    }
};

}  // namespace dk
